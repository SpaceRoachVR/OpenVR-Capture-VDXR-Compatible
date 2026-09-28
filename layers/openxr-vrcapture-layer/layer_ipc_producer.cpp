#include "layer_ipc_producer.h"
#include "../../shared/vr_shared_texture.h"
#include <algorithm>

namespace vrcapture {

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
}

void LayerIpcProducer::InitWorkerLoop()
{
    for (;;) {
        ComPtr<ID3D11Device> device;
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool isD3D12 = false;

        {
            std::unique_lock<std::mutex> lock(m_initCvMutex);
            m_initCv.wait(lock, [this] { return m_initWorkerShouldExit || m_initRequestPending; });

            if (m_initWorkerShouldExit) {
                return;
            }

            device = m_pendingDevice;
            width = m_pendingWidth;
            height = m_pendingHeight;
            format = m_pendingFormat;
            isD3D12 = m_pendingIsD3D12;
            m_initRequestPending = false;
        }

        Initialize(device.Get(), width, height, format, isD3D12);
    }
}

void LayerIpcProducer::RequestAsyncInitialize(ID3D11Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool is_d3d12)
{
    std::lock_guard<std::mutex> lock(m_initCvMutex);

    // Start the worker on first real need -- i.e. only once OBS is actually
    // consuming frames from this application.
    if (!m_initWorkerStarted) {
        m_initWorkerShouldExit = false;
        m_initWorker = std::thread(&LayerIpcProducer::InitWorkerLoop, this);
        m_initWorkerStarted = true;
    }

    m_pendingDevice = device;
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_pendingFormat = format;
    m_pendingIsD3D12 = is_d3d12;
    m_initRequestPending = true;
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
        return false;
    }

    auto *header = reinterpret_cast<VRSharedFrameHeader *>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VRSharedFrameHeader)));
    if (!header) {
        CloseHandle(hMap);
        return false;
    }

    if (header->magic != VR_IPC_MAGIC || header->version != VR_IPC_VERSION) {
        // Not initialized yet, or a plugin/layer version mismatch.
        UnmapViewOfFile(header);
        CloseHandle(hMap);
        return false;
    }

    m_hMapFile = hMap;
    m_sharedHeader = header;

    m_sharedHeader->shared_handle = 0;
    m_sharedHeader->texture_width = 0;
    m_sharedHeader->texture_height = 0;
    m_sharedHeader->is_d3d12 = m_isD3D12 ? 1 : 0;
    m_sharedHeader->last_producer_heartbeat = GetTickCount64();
    m_sharedHeader->active_backend = VRBackendType::OpenXR_VDXR;

    return true;
}

bool LayerIpcProducer::Initialize(ID3D11Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool is_d3d12)
{
    if (!device || width == 0 || height == 0) {
        return false;
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_sharedHeader) {
            return false;
        }
        m_isD3D12 = is_d3d12;
        m_sharedHeader->is_d3d12 = is_d3d12 ? 1 : 0;
        if (m_sharedTexture && m_width == width && m_height == height && m_format == format) {
            return true; // Already matching
        }
    }

    // Build the replacement outside the lock (ID3D11Device is free-threaded),
    // so the app's xrEndFrame never waits on CreateTexture2D.
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    HANDLE sharedHandle = nullptr;
    if (!CreateSharedD3D11Texture(device, width, height, format, texture, keyedMutex, sharedHandle)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sharedHeader) {
        return false; // Shutdown raced us
    }

    // Swap atomically with respect to CopyFrame(); the old texture is
    // released when `texture` goes out of scope after the swap.
    m_device = device;
    m_sharedTexture.Swap(texture);
    m_keyedMutex.Swap(keyedMutex);
    m_sharedHandle = sharedHandle;
    m_width = width;
    m_height = height;
    m_format = format;

    // Publish under the seqlock so a reader never pairs a new handle with
    // old dimensions.
    InterlockedIncrement(&m_sharedHeader->seq);
    m_sharedHeader->shared_handle = reinterpret_cast<uint64_t>(m_sharedHandle);
    m_sharedHeader->texture_width = m_width;
    m_sharedHeader->texture_height = m_height;
    m_sharedHeader->dxgi_format = static_cast<uint32_t>(m_format);
    InterlockedIncrement(&m_sharedHeader->seq);

    return true;
}

bool LayerIpcProducer::HasTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) const
{
    if (format == DXGI_FORMAT_UNKNOWN) {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sharedTexture && m_width == width && m_height == height && m_format == format;
}

void LayerIpcProducer::DestroySharedTexture()
{
    m_keyedMutex.Reset();
    m_sharedTexture.Reset();
    m_sharedHandle = nullptr;
    m_width = 0;
    m_height = 0;
    m_format = DXGI_FORMAT_UNKNOWN;
}

void LayerIpcProducer::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    DestroySharedTexture();
    m_device.Reset();

    if (m_sharedHeader) {
        m_sharedHeader->active_backend = VRBackendType::Inactive;
        m_sharedHeader->shared_handle = 0;
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

VREyeSelection LayerIpcProducer::GetRequestedEye()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedHeader) return VREyeSelection::Right;
    return static_cast<VREyeSelection>(m_sharedHeader->requested_eye);
}

bool LayerIpcProducer::CopyFrame(ID3D11DeviceContext *context, ID3D11Texture2D *source, UINT sourceSubresource,
                                 const D3D11_BOX &box, int64_t display_time_ns, const float fov[4],
                                 const float orientation[4], const float position[3])
{
    if (!context || !source || box.right <= box.left || box.bottom <= box.top) {
        return false;
    }

    // Held across acquire -> copy -> release -> publish so the worker can't
    // swap the texture out from under us mid-copy. CopySubresourceRegion only
    // records a command, so this is a short hold.
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedTexture || !m_sharedHeader) {
        return false;
    }

    // A resize is pending on the worker; don't write a box that doesn't fit.
    if (box.right - box.left != m_width || box.bottom - box.top != m_height) {
        return false;
    }

    if (m_keyedMutex) {
        // Non-blocking: key 0 is handed back by OBS once it has copied the
        // previous frame out. If it hasn't yet, drop this frame.
        if (m_keyedMutex->AcquireSync(0, 0) != S_OK) {
            return false;
        }
    }

    context->CopySubresourceRegion(m_sharedTexture.Get(), 0, 0, 0, 0, source, sourceSubresource, &box);

    if (m_keyedMutex) {
        m_keyedMutex->ReleaseSync(1); // Hand key 1 to the OBS consumer
    }

    m_frameIndex++;

    // Seqlock: odd = write in progress. Cross-process readers spin/retry
    // rather than ever observing a torn mix of these fields.
    InterlockedIncrement(&m_sharedHeader->seq);

    m_sharedHeader->frame_index = m_frameIndex;
    m_sharedHeader->display_time_ns = display_time_ns;
    m_sharedHeader->last_producer_heartbeat = GetTickCount64();

    if (fov) {
        m_sharedHeader->fov_left = fov[0];
        m_sharedHeader->fov_right = fov[1];
        m_sharedHeader->fov_up = fov[2];
        m_sharedHeader->fov_down = fov[3];
    }

    if (orientation) {
        m_sharedHeader->pose_orientation[0] = orientation[0];
        m_sharedHeader->pose_orientation[1] = orientation[1];
        m_sharedHeader->pose_orientation[2] = orientation[2];
        m_sharedHeader->pose_orientation[3] = orientation[3];
    }

    if (position) {
        m_sharedHeader->pose_position[0] = position[0];
        m_sharedHeader->pose_position[1] = position[1];
        m_sharedHeader->pose_position[2] = position[2];
    }

    InterlockedIncrement(&m_sharedHeader->seq); // back to even: stable
    return true;
}

} // namespace vrcapture
