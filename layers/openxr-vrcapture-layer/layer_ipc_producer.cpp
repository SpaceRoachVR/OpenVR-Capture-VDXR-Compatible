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
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!device || width == 0 || height == 0 || !m_sharedHeader) {
        return false;
    }

    m_device = device;
    m_isD3D12 = is_d3d12;
    m_sharedHeader->is_d3d12 = is_d3d12 ? 1 : 0;

    return CreateSharedTexture(device, width, height, format);
}

bool LayerIpcProducer::CreateSharedTexture(ID3D11Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    if (m_sharedTexture && m_width == width && m_height == height && m_format == format) {
        return true; // Already matching
    }

    DestroySharedTexture();

    m_width = width;
    m_height = height;
    m_format = (format != DXGI_FORMAT_UNKNOWN) ? format : DXGI_FORMAT_R8G8B8A8_UNORM;

    if (!CreateSharedD3D11Texture(device, m_width, m_height, m_format, m_sharedTexture, m_keyedMutex, m_sharedHandle)) {
        return false;
    }

    // Publish texture handle to shared memory
    if (m_sharedHeader) {
        m_sharedHeader->shared_handle = reinterpret_cast<uint64_t>(m_sharedHandle);
        m_sharedHeader->texture_width = m_width;
        m_sharedHeader->texture_height = m_height;
        m_sharedHeader->dxgi_format = static_cast<uint32_t>(m_format);
    }

    return true;
}

void LayerIpcProducer::DestroySharedTexture()
{
    m_keyedMutex.Reset();
    m_sharedTexture.Reset();
    m_sharedHandle = nullptr;
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

bool LayerIpcProducer::BeginFrameCopy(ID3D11DeviceContext *context)
{
    (void)context;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedTexture) return false;

    if (m_keyedMutex) {
        // Non-blocking try to acquire key 0 (released by OBS consumer)
        HRESULT hr = m_keyedMutex->AcquireSync(0, 0);
        if (hr == S_OK) {
            m_hasAcquiredLock = true;
            return true;
        } else {
            m_hasAcquiredLock = false;
            return false; // OBS is currently reading or busy
        }
    }

    m_hasAcquiredLock = true;
    return true;
}

void LayerIpcProducer::EndFrameCopy(int64_t display_time_ns, const float fov[4], const float orientation[4], const float position[3])
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_keyedMutex && m_hasAcquiredLock) {
        m_keyedMutex->ReleaseSync(1); // Release key 1 for OBS consumer
    }
    m_hasAcquiredLock = false;

    m_frameIndex++;

    if (m_sharedHeader) {
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
    }
}

} // namespace vrcapture
