#include "layer_ipc_producer.h"
#include "../../shared/vr_shared_texture.h"
#include "layer_log.h"
#include <algorithm>

namespace vrcapture {

namespace {
bool ValidEye(VREyeSelection eye)
{
    return static_cast<uint32_t>(eye) < VR_IPC_EYE_COUNT;
}
}

LayerIpcProducer::LayerIpcProducer()
{
    // The worker is started lazily (see RequestAsyncInitialize), not here. This
    // layer is registered as an implicit OpenXR layer, so it loads into every
    // OpenXR application on the machine -- the vast majority of which never
    // capture to OBS and must not pay for a background thread they never use.
}

LayerIpcProducer::~LayerIpcProducer()
{
    StopWorker();
    Shutdown();
}

void LayerIpcProducer::StopWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_initCvMutex);
        if (!m_initWorkerStarted) {
            return;
        }
        m_initWorkerShouldExit = true;
    }
    m_initCv.notify_all();
    if (m_initWorker.joinable()) {
        m_initWorker.join();
    }

    std::lock_guard<std::mutex> lock(m_initCvMutex);
    m_initWorkerStarted = false;
    m_initWorkerShouldExit = false;
    for (PendingInit &p : m_pending) {
        p = PendingInit{};
    }
}

void LayerIpcProducer::InitWorkerLoop()
{
    for (;;) {
        PendingInit work[VR_IPC_EYE_COUNT];

        {
            std::unique_lock<std::mutex> lock(m_initCvMutex);
            m_initCv.wait(lock, [this] {
                return m_initWorkerShouldExit || m_pending[0].pending || m_pending[1].pending;
            });

            if (m_initWorkerShouldExit) {
                return;
            }

            for (uint32_t i = 0; i < VR_IPC_EYE_COUNT; ++i) {
                work[i] = std::move(m_pending[i]);
                m_pending[i] = PendingInit{};
            }
        }

        for (uint32_t i = 0; i < VR_IPC_EYE_COUNT; ++i) {
            if (work[i].pending) {
                Initialize(static_cast<VREyeSelection>(i), work[i].device.Get(), work[i].width, work[i].height,
                           work[i].format, work[i].isD3D12);
            }
        }
    }
}

void LayerIpcProducer::RequestAsyncInitialize(VREyeSelection eye, ID3D11Device *device, uint32_t width,
                                              uint32_t height, DXGI_FORMAT format, bool is_d3d12)
{
    if (!ValidEye(eye)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_initCvMutex);

    // Start the worker on first real need -- i.e. only once OBS is actually
    // consuming frames from this application.
    if (!m_initWorkerStarted) {
        m_initWorkerShouldExit = false;
        m_initWorker = std::thread(&LayerIpcProducer::InitWorkerLoop, this);
        m_initWorkerStarted = true;
    }

    PendingInit &p = m_pending[static_cast<uint32_t>(eye)];
    p.pending = true;
    p.device = device;
    p.width = width;
    p.height = height;
    p.format = format;
    p.isD3D12 = is_d3d12;
    m_initCv.notify_all();
}

bool LayerIpcProducer::TryAttach()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_sharedHeader) {
        return true;
    }

    // Called from xrEndFrame every frame; only hit the kernel about once a
    // second while OBS isn't running (the common case for most OpenXR apps).
    const uint64_t now = GetTickCount64();
    if (m_lastAttachAttempt != 0 && now - m_lastAttachAttempt < kAttachRetryMs) {
        return false;
    }
    m_lastAttachAttempt = now;

    // OBS owns the mapping; never create it here (see vr_ipc_types.h).
    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VR_IPC_SHARED_MEMORY_NAME);
    if (!hMap) {
        const DWORD err = GetLastError();
        if (err != m_lastAttachError) {
            // 2 = OBS not running / no VR Capture source; 5 = access denied.
            LayerLog("attach: OpenFileMapping failed, error %lu%s", err,
                     err == ERROR_FILE_NOT_FOUND ? " (OBS not running or no VR Capture source)"
                     : err == ERROR_ACCESS_DENIED ? " (access denied - permissions/elevation mismatch)"
                                                  : "");
            m_lastAttachError = err;
        }
        return false;
    }

    auto *header = reinterpret_cast<VRSharedFrameHeader *>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VRSharedFrameHeader)));
    if (!header) {
        LayerLog("attach: MapViewOfFile failed, error %lu", GetLastError());
        CloseHandle(hMap);
        return false;
    }

    if (header->magic != VR_IPC_MAGIC || header->version != VR_IPC_VERSION) {
        if (m_lastAttachError != 0xFFFFFFFF) {
            LayerLog("attach: OBS mapping has magic 0x%08x version %u, expected version %u (plugin/layer mismatch)",
                     header->magic, header->version, VR_IPC_VERSION);
            m_lastAttachError = 0xFFFFFFFF;
        }
        // Not initialized yet, or a plugin/layer version mismatch.
        UnmapViewOfFile(header);
        CloseHandle(hMap);
        return false;
    }

    m_hMapFile = hMap;
    m_sharedHeader = header;
    m_lastAttachError = 0;

    // A previous session may have left handles behind; start clean.
    InterlockedIncrement(&m_sharedHeader->seq);
    for (uint32_t i = 0; i < VR_IPC_EYE_COUNT; ++i) {
        m_sharedHeader->eyes[i] = VRSharedEyeTexture{};
    }
    m_sharedHeader->is_d3d12 = m_isD3D12 ? 1 : 0;
    m_sharedHeader->last_producer_heartbeat = GetTickCount64();
    m_sharedHeader->active_backend = VRBackendType::OpenXR_VDXR;
    InterlockedIncrement(&m_sharedHeader->seq);

    return true;
}

bool LayerIpcProducer::Initialize(VREyeSelection eye, ID3D11Device *device, uint32_t width, uint32_t height,
                                  DXGI_FORMAT format, bool is_d3d12)
{
    if (!ValidEye(eye) || !device || width == 0 || height == 0) {
        return false;
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    const uint32_t index = static_cast<uint32_t>(eye);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sharedHeader) {
            return false;
        }
        m_isD3D12 = is_d3d12;
        m_sharedHeader->is_d3d12 = is_d3d12 ? 1 : 0;
        const EyeSlot &slot = m_eyes[index];
        if (slot.texture && slot.width == width && slot.height == height && slot.format == format) {
            return true; // Already matching
        }
    }

    // Build the replacement outside the lock (ID3D11Device is free-threaded),
    // so the app's xrEndFrame never waits on CreateTexture2D.
    EyeSlot fresh;
    if (!CreateSharedD3D11Texture(device, width, height, format, fresh.texture, fresh.keyedMutex, fresh.sharedHandle)) {
        LayerLog("eye %u: creating %ux%u shared texture (DXGI format %d) failed", index, width, height, (int)format);
        return false;
    }
    LayerLog("eye %u: shared texture %ux%u DXGI format %d, keyed mutex %s", index, width, height, (int)format,
             fresh.keyedMutex ? "yes" : "no");
    fresh.width = width;
    fresh.height = height;
    fresh.format = format;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sharedHeader) {
        return false; // Shutdown raced us
    }

    // Swap atomically with respect to CopyEye(); the old texture is released
    // when `fresh` goes out of scope after the swap.
    m_device = device;
    std::swap(m_eyes[index], fresh);
    const EyeSlot &slot = m_eyes[index];

    // Publish under the seqlock so a reader never pairs a new handle with
    // old dimensions.
    InterlockedIncrement(&m_sharedHeader->seq);
    VRSharedEyeTexture &out = m_sharedHeader->eyes[index];
    out.shared_handle = reinterpret_cast<uint64_t>(slot.sharedHandle);
    out.texture_width = slot.width;
    out.texture_height = slot.height;
    out.dxgi_format = static_cast<uint32_t>(slot.format);
    InterlockedIncrement(&m_sharedHeader->seq);

    return true;
}

bool LayerIpcProducer::HasTexture(VREyeSelection eye, uint32_t width, uint32_t height, DXGI_FORMAT format) const
{
    if (!ValidEye(eye)) {
        return false;
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const EyeSlot &slot = m_eyes[static_cast<uint32_t>(eye)];
    return slot.texture && slot.width == width && slot.height == height && slot.format == format;
}

void LayerIpcProducer::ClearEyeLocked(uint32_t index)
{
    m_eyes[index] = EyeSlot{};
}

void LayerIpcProducer::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (uint32_t i = 0; i < VR_IPC_EYE_COUNT; ++i) {
        ClearEyeLocked(i);
    }
    m_device.Reset();

    if (m_sharedHeader) {
        InterlockedIncrement(&m_sharedHeader->seq);
        m_sharedHeader->active_backend = VRBackendType::Inactive;
        for (uint32_t i = 0; i < VR_IPC_EYE_COUNT; ++i) {
            m_sharedHeader->eyes[i].shared_handle = 0;
        }
        InterlockedIncrement(&m_sharedHeader->seq);
        UnmapViewOfFile(m_sharedHeader);
        m_sharedHeader = nullptr;
    }

    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }

    // Let the next session re-attach immediately rather than after the throttle.
    m_lastAttachAttempt = 0;
}

bool LayerIpcProducer::IsObsConnected()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedHeader) return false;
    // Update our heartbeat
    m_sharedHeader->last_producer_heartbeat = GetTickCount64();

    // Check if OBS heartbeat is fresh (within last 3 seconds)
    uint64_t now = GetTickCount64();
    if (m_sharedHeader->obs_connected && (now - m_sharedHeader->last_consumer_heartbeat < 3000)) {
        return true;
    }
    return false;
}

uint32_t LayerIpcProducer::GetRequestedEyeMask()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedHeader) return 0;
    return m_sharedHeader->requested_eye_mask & ((1u << VR_IPC_EYE_COUNT) - 1);
}

bool LayerIpcProducer::CopyEye(VREyeSelection eye, ID3D11DeviceContext *context, ID3D11Texture2D *source,
                               UINT sourceSubresource, const D3D11_BOX &box, const float fov[4],
                               const float orientation[4], const float position[3])
{
    if (!ValidEye(eye) || !context || !source || box.right <= box.left || box.bottom <= box.top) {
        return false;
    }
    const uint32_t index = static_cast<uint32_t>(eye);

    // Held across acquire -> copy -> release -> publish so the worker can't
    // swap the texture out from under us mid-copy. CopySubresourceRegion only
    // records a command, so this is a short hold.
    std::lock_guard<std::mutex> lock(m_mutex);

    EyeSlot &slot = m_eyes[index];
    if (!slot.texture || !m_sharedHeader) {
        return false;
    }

    // A resize is pending on the worker; don't write a box that doesn't fit.
    if (box.right - box.left != slot.width || box.bottom - box.top != slot.height) {
        return false;
    }

    if (slot.keyedMutex) {
        // Non-blocking: key 0 is handed back by OBS once it has copied the
        // previous frame out. If it hasn't yet, drop this frame.
        if (slot.keyedMutex->AcquireSync(0, 0) != S_OK) {
            return false;
        }
    }

    context->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, source, sourceSubresource, &box);

    if (slot.keyedMutex) {
        slot.keyedMutex->ReleaseSync(1); // Hand key 1 to the OBS consumer
    }
    if (!slot.loggedFirstCopy) {
        LayerLog("eye %u: first frame copied", index);
        slot.loggedFirstCopy = true;
    }

    InterlockedIncrement(&m_sharedHeader->seq);
    VRSharedEyeTexture &out = m_sharedHeader->eyes[index];
    for (int i = 0; i < 4; ++i) {
        out.fov[i] = fov ? fov[i] : 0.0f;
        out.pose_orientation[i] = orientation ? orientation[i] : 0.0f;
    }
    for (int i = 0; i < 3; ++i) {
        out.pose_position[i] = position ? position[i] : 0.0f;
    }
    InterlockedIncrement(&m_sharedHeader->seq);
    return true;
}

void LayerIpcProducer::PublishFrame(int64_t display_time_ns)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedHeader) {
        return;
    }

    m_frameIndex++;

    // Seqlock: odd = write in progress. Cross-process readers spin/retry
    // rather than ever observing a torn mix of these fields.
    InterlockedIncrement(&m_sharedHeader->seq);
    m_sharedHeader->frame_index = m_frameIndex;
    m_sharedHeader->display_time_ns = display_time_ns;
    m_sharedHeader->last_producer_heartbeat = GetTickCount64();
    InterlockedIncrement(&m_sharedHeader->seq); // back to even: stable
}

} // namespace vrcapture
