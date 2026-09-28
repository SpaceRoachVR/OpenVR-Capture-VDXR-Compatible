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
    return InitializeLocked();
}

bool OpenXrIpcConsumer::InitializeLocked()
{
    if (m_sharedHeader) {
        return true;
    }

    // Only reached again if a previous attempt failed (e.g. an incompatible
    // layer version holds the mapping); don't retry on every video tick.
    const uint64_t now = GetTickCount64();
    if (m_lastInitAttempt != 0 && now - m_lastInitAttempt < 1000) {
        return false;
    }
    m_lastInitAttempt = now;

    // OBS owns the mapping (see vr_ipc_types.h): create it, or open it if
    // another source in this process -- or a still-attached game from an
    // earlier OBS run -- already has it.
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(VRSharedFrameHeader),
        VR_IPC_SHARED_MEMORY_NAME);
    if (!hMap) {
        return false;
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;

    auto *header = reinterpret_cast<VRSharedFrameHeader *>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VRSharedFrameHeader)));
    if (!header) {
        CloseHandle(hMap);
        return false;
    }

    if (created) {
        // Fresh pages are zeroed. Fill in the identity last so a layer that
        // opens the mapping mid-initialization rejects it and retries.
        header->active_backend = VRBackendType::Inactive;
        header->requested_eye = static_cast<uint32_t>(VREyeSelection::Right);
        header->version = VR_IPC_VERSION;
        MemoryBarrier();
        header->magic = VR_IPC_MAGIC;
    } else if (header->magic != VR_IPC_MAGIC || header->version != VR_IPC_VERSION) {
        // Created by an incompatible plugin/layer build; don't interpret it.
        UnmapViewOfFile(header);
        CloseHandle(hMap);
        return false;
    }

    m_hMapFile = hMap;
    m_sharedHeader = header;
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
    m_lastInitAttempt = 0;
}

bool OpenXrIpcConsumer::IsProducerActive()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!InitializeLocked()) {
        return false;
    }

    uint64_t now = GetTickCount64();
    if (m_sharedHeader->active_backend == VRBackendType::OpenXR_VDXR &&
        (now - m_sharedHeader->last_producer_heartbeat < 2000)) {
        return true;
    }

    // Producer is inactive or closed: release texture to prevent rendering a dead handle
    if (m_obsTexture) {
        DestroyTexture();
        m_currentSharedHandle = 0;
        m_width = 0;
        m_height = 0;
    }

    return false;
}

void OpenXrIpcConsumer::SetConnected(bool connected)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (InitializeLocked()) {
        m_sharedHeader->obs_connected = connected ? 1 : 0;
        m_sharedHeader->last_consumer_heartbeat = GetTickCount64();
    }
}

bool OpenXrIpcConsumer::UpdateTexture(VREyeSelection eye)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // m_mutex is not recursive: must call the Locked variant here.
    if (!InitializeLocked()) {
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
