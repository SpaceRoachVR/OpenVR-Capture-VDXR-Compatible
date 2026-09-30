#include "openxr_ipc_consumer.h"
#include <cassert>

namespace vrcapture {

namespace {
// Seqlock-consistent read of frame_index (see VRSharedFrameHeader::seq). Retries
// while the producer is mid-write (odd seq) or the value changed across the read,
// so we never pair a stale frame_index with data from a different frame.
uint64_t ReadFrameIndexConsistent(const VRSharedFrameHeader *header)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        LONG s1 = header->seq;
        if (s1 & 1) continue; // producer mid-write, retry

        uint64_t frameIndex = header->frame_index;

        _ReadWriteBarrier();
        LONG s2 = header->seq;
        if (s1 == s2) {
            return frameIndex;
        }
    }
    // Producer is mid-write on every attempt (highly contended); fall back to a
    // direct read rather than blocking the render thread indefinitely.
    return header->frame_index;
}
}

OpenXrIpcConsumer::OpenXrIpcConsumer()
{
}

OpenXrIpcConsumer::~OpenXrIpcConsumer()
{
    Shutdown();
}

bool OpenXrIpcConsumer::Initialize()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_sharedHeader) {
        return true;
    }

    m_hMapFile = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        VR_IPC_SHARED_MEMORY_NAME);

    if (!m_hMapFile) {
        return false;
    }

    m_sharedHeader = reinterpret_cast<VRSharedFrameHeader *>(
        MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VRSharedFrameHeader)));

    if (!m_sharedHeader) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
        return false;
    }

    if (m_sharedHeader->magic != VR_IPC_MAGIC) {
        UnmapViewOfFile(m_sharedHeader);
        m_sharedHeader = nullptr;
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
        return false;
    }

    m_sharedHeader->obs_connected = 1;
    m_sharedHeader->last_consumer_heartbeat = GetTickCount64();

    return true;
}

void OpenXrIpcConsumer::DestroyTexture()
{
    m_obsKeyedMutex.Reset();
    if (m_obsTexture) {
        obs_enter_graphics();
        gs_texture_destroy(m_obsTexture);
        obs_leave_graphics();
        m_obsTexture = nullptr;
    }
}

void OpenXrIpcConsumer::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_sharedHeader) {
        m_sharedHeader->obs_connected = 0;
        UnmapViewOfFile(m_sharedHeader);
        m_sharedHeader = nullptr;
    }

    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }

    DestroyTexture();
    m_currentSharedHandle = 0;
    m_width = 0;
    m_height = 0;
}

bool OpenXrIpcConsumer::IsProducerActive()
{
    if (!m_sharedHeader) {
        Initialize();
    }

    if (!m_sharedHeader) {
        return false;
    }

    uint64_t now = GetTickCount64();
    if (m_sharedHeader->magic == VR_IPC_MAGIC &&
        m_sharedHeader->active_backend == VRBackendType::OpenXR_VDXR &&
        (now - m_sharedHeader->last_producer_heartbeat < 2000)) {
        return true;
    }

    // Producer is inactive or closed: release texture to prevent rendering a dead handle
    if (m_obsTexture) {
        std::lock_guard<std::mutex> lock(m_mutex);
        DestroyTexture();
        m_currentSharedHandle = 0;
        m_width = 0;
        m_height = 0;
    }

    return false;
}

void OpenXrIpcConsumer::SetConnected(bool connected)
{
    if (!m_sharedHeader) {
        Initialize();
    }

    if (m_sharedHeader) {
        m_sharedHeader->obs_connected = connected ? 1 : 0;
        m_sharedHeader->last_consumer_heartbeat = GetTickCount64();
    }
}

bool OpenXrIpcConsumer::UpdateTexture(VREyeSelection eye)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_sharedHeader) {
        Initialize();
    }

    if (!m_sharedHeader) {
        return false;
    }

    // Send heartbeat and eye preference
    m_sharedHeader->obs_connected = 1;
    m_sharedHeader->last_consumer_heartbeat = GetTickCount64();
    m_sharedHeader->requested_eye = static_cast<uint32_t>(eye);

    if (m_sharedHeader->shared_handle == 0 || m_sharedHeader->texture_width == 0 || m_sharedHeader->texture_height == 0) {
        return false;
    }

    // The header is written by another (untrusted-ish) process; reject
    // out-of-range dimensions rather than acting on them.
    if (m_sharedHeader->texture_width > VR_IPC_MAX_TEXTURE_DIM || m_sharedHeader->texture_height > VR_IPC_MAX_TEXTURE_DIM) {
        return false;
    }

    // Check if texture handle or dimensions changed
    if (m_sharedHeader->shared_handle != m_currentSharedHandle ||
        m_sharedHeader->texture_width != m_width ||
        m_sharedHeader->texture_height != m_height ||
        !m_obsTexture) {

        DestroyTexture();

        m_currentSharedHandle = m_sharedHeader->shared_handle;
        m_width = m_sharedHeader->texture_width;
        m_height = m_sharedHeader->texture_height;
        m_format = static_cast<DXGI_FORMAT>(m_sharedHeader->dxgi_format);

        assert((m_currentSharedHandle >> 32) == 0 && "DXGI shared handle unexpectedly exceeds 32 bits");
        uint32_t gsHandle = static_cast<uint32_t>(m_currentSharedHandle);

        obs_enter_graphics();
        m_obsTexture = gs_texture_open_shared(gsHandle);
        if (m_obsTexture) {
            ID3D11Texture2D *d3dTex = reinterpret_cast<ID3D11Texture2D *>(gs_texture_get_obj(m_obsTexture));
            if (d3dTex) {
                d3dTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(m_obsKeyedMutex.GetAddressOf()));
            }
        }
        obs_leave_graphics();

        if (!m_obsTexture) {
            return false;
        }
    }

    m_frameIndex = ReadFrameIndexConsistent(m_sharedHeader);
    return true;
}

void OpenXrIpcConsumer::Render(gs_effect_t *effect)
{
    if (!m_obsTexture) return;

    if (m_obsKeyedMutex) {
        // Non-blocking try to acquire key 1 (written by game process)
        if (m_obsKeyedMutex->AcquireSync(1, 0) == S_OK) {
            while (gs_effect_loop(effect, "Draw")) {
                obs_source_draw(m_obsTexture, 0, 0, 0, 0, false);
            }
            m_obsKeyedMutex->ReleaseSync(0); // Return to producer
        }
        // Do NOT draw in an else branch if AcquireSync failed: drawing an
        // unacquired keyed mutex resource causes DXGI_ERROR_DEVICE_REMOVED.
    } else {
        while (gs_effect_loop(effect, "Draw")) {
            obs_source_draw(m_obsTexture, 0, 0, 0, 0, false);
        }
    }
}

} // namespace vrcapture
