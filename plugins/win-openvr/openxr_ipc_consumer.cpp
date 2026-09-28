#include "openxr_ipc_consumer.h"
#include <algorithm>

namespace vrcapture {

namespace {
struct HeaderSnapshot {
    uint64_t shared_handle;
    uint32_t width;
    uint32_t height;
    uint64_t frame_index;
};

// Seqlock-consistent read of the producer-owned fields (see
// VRSharedFrameHeader::seq). Retries while the producer is mid-write (odd seq)
// or the value changed across the read, so a new handle is never paired with
// the previous texture's dimensions. Returns false if the producer stayed
// mid-write on every attempt; the caller just tries again next tick.
bool ReadHeaderConsistent(const VRSharedFrameHeader *header, HeaderSnapshot &out)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        LONG s1 = header->seq;
        if (s1 & 1) continue; // producer mid-write, retry
        _ReadWriteBarrier();

        out.shared_handle = header->shared_handle;
        out.width = header->texture_width;
        out.height = header->texture_height;
        out.frame_index = header->frame_index;

        _ReadWriteBarrier();
        if (header->seq == s1) {
            return true;
        }
    }
    return false;
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

void OpenXrIpcConsumer::DestroySharedTexture()
{
    m_obsKeyedMutex.Reset();
    if (m_sharedTexture) {
        obs_enter_graphics();
        gs_texture_destroy(m_sharedTexture);
        obs_leave_graphics();
        m_sharedTexture = nullptr;
    }
    m_currentSharedHandle = 0;
}

void OpenXrIpcConsumer::DestroyTexture()
{
    DestroySharedTexture();
    if (m_privateTexture) {
        obs_enter_graphics();
        gs_texture_destroy(m_privateTexture);
        obs_leave_graphics();
        m_privateTexture = nullptr;
    }
    m_hasFrame = false;
    m_width = 0;
    m_height = 0;
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

    // Producer is inactive or closed: release textures so a dead handle is
    // never rendered and the source stops showing the game's last frame.
    if (m_sharedTexture || m_privateTexture) {
        DestroyTexture();
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

    HeaderSnapshot snap = {};
    if (!ReadHeaderConsistent(m_sharedHeader, snap)) {
        return m_hasFrame; // producer busy; keep showing the last frame
    }

    // The header is written by another (untrusted-ish) process; reject
    // missing or out-of-range textures rather than acting on them.
    if (snap.shared_handle == 0 || snap.width == 0 || snap.height == 0 ||
        snap.width > VR_IPC_MAX_TEXTURE_DIM || snap.height > VR_IPC_MAX_TEXTURE_DIM) {
        return m_hasFrame;
    }

    obs_enter_graphics();

    if (snap.shared_handle != m_currentSharedHandle || !m_sharedTexture) {
        DestroySharedTexture();

        // Legacy (GetSharedHandle) DXGI handles are 32-bit values by design.
        if ((snap.shared_handle >> 32) == 0) {
            m_sharedTexture = gs_texture_open_shared(static_cast<uint32_t>(snap.shared_handle));
        }
        if (m_sharedTexture) {
            m_currentSharedHandle = snap.shared_handle;
            auto *d3dTex = reinterpret_cast<ID3D11Texture2D *>(gs_texture_get_obj(m_sharedTexture));
            if (d3dTex) {
                d3dTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(m_obsKeyedMutex.GetAddressOf()));
            }
        }
    }

    if (m_sharedTexture) {
        // Size/format come from the texture OBS actually opened, not the
        // header, so the private copy always matches what we copy from.
        const uint32_t w = gs_texture_get_width(m_sharedTexture);
        const uint32_t h = gs_texture_get_height(m_sharedTexture);
        const gs_color_format fmt = gs_texture_get_color_format(m_sharedTexture);

        if (fmt == GS_UNKNOWN) {
            if (!m_loggedBadFormat) {
                blog(LOG_WARNING, "[win_vrcapture] OpenXR shared texture has an unsupported format (DXGI %u)",
                     m_sharedHeader->dxgi_format);
                m_loggedBadFormat = true;
            }
        } else {
            if (!m_privateTexture || m_width != w || m_height != h || m_privateFormat != fmt) {
                if (m_privateTexture) {
                    gs_texture_destroy(m_privateTexture);
                }
                m_privateTexture = gs_texture_create(w, h, fmt, 1, nullptr, 0);
                m_privateFormat = fmt;
                m_width = m_privateTexture ? w : 0;
                m_height = m_privateTexture ? h : 0;
                m_hasFrame = false;
            }

            if (m_privateTexture) {
                if (m_obsKeyedMutex) {
                    // Take the frame only when the producer has handed us
                    // key 1 (i.e. there is a new one), copy it out, and give
                    // the key straight back so the game can write the next
                    // frame. Rendering reads m_privateTexture, never the
                    // shared texture, so every render call this OBS frame --
                    // preview, program, projectors -- sees the same image.
                    if (m_obsKeyedMutex->AcquireSync(1, 0) == S_OK) {
                        gs_copy_texture(m_privateTexture, m_sharedTexture);
                        m_obsKeyedMutex->ReleaseSync(0);
                        m_hasFrame = true;
                    }
                } else if (snap.frame_index != m_frameIndex || !m_hasFrame) {
                    // Fallback texture without a keyed mutex: no cross-process
                    // sync available, so copy whenever a new frame is published.
                    gs_copy_texture(m_privateTexture, m_sharedTexture);
                    m_hasFrame = true;
                }
                m_frameIndex = snap.frame_index;
            }
        }
    }

    obs_leave_graphics();
    return m_hasFrame;
}

bool OpenXrIpcConsumer::Render(gs_effect_t *effect, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    if (!m_privateTexture || !m_hasFrame || !effect) {
        return false;
    }

    // Clamp the crop to the texture; a 0 extent means "to the edge".
    x = (std::min)(x, m_width - 1);
    y = (std::min)(y, m_height - 1);
    cx = (cx == 0 || x + cx > m_width) ? m_width - x : cx;
    cy = (cy == 0 || y + cy > m_height) ? m_height - y : cy;

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, m_privateTexture);
    while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite_subregion(m_privateTexture, 0, x, y, cx, cy);
    }
    return true;
}

} // namespace vrcapture
