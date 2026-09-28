#include "openxr_ipc_consumer.h"
#include <algorithm>

namespace vrcapture {

namespace {
struct EyeSnapshot {
    uint64_t shared_handle;
    uint32_t width;
    uint32_t height;
    uint64_t frame_index;
};

bool ValidEye(VREyeSelection eye)
{
    return static_cast<uint32_t>(eye) < VR_IPC_EYE_COUNT;
}

// Seqlock-consistent read of one eye's producer-owned fields (see
// VRSharedFrameHeader::seq). Retries while the producer is mid-write (odd seq)
// or the value changed across the read, so a new handle is never paired with
// the previous texture's dimensions. Returns false if the producer stayed
// mid-write on every attempt; the caller just tries again next tick.
bool ReadEyeConsistent(const VRSharedFrameHeader *header, uint32_t eyeIndex, EyeSnapshot &out)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        LONG s1 = header->seq;
        if (s1 & 1) continue; // producer mid-write, retry
        _ReadWriteBarrier();

        const VRSharedEyeTexture &eye = header->eyes[eyeIndex];
        out.shared_handle = eye.shared_handle;
        out.width = eye.texture_width;
        out.height = eye.texture_height;
        out.frame_index = header->frame_index;

        _ReadWriteBarrier();
        if (header->seq == s1) {
            return true;
        }
    }
    return false;
}

std::mutex s_sharedMutex;
std::weak_ptr<OpenXrIpcConsumer> s_shared;
}

std::shared_ptr<OpenXrIpcConsumer> OpenXrIpcConsumer::Acquire()
{
    std::lock_guard<std::mutex> lock(s_sharedMutex);
    std::shared_ptr<OpenXrIpcConsumer> consumer = s_shared.lock();
    if (!consumer) {
        consumer = std::make_shared<OpenXrIpcConsumer>();
        s_shared = consumer;
    }
    return consumer;
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

    // OBS owns the mapping (see vr_ipc_types.h): create it, or open it if a
    // still-attached game from an earlier OBS run already has it.
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
    PublishInterestLocked();
    return true;
}

void OpenXrIpcConsumer::DestroySharedTexture(EyeState &eye)
{
    eye.keyedMutex.Reset();
    if (eye.sharedTexture) {
        obs_enter_graphics();
        gs_texture_destroy(eye.sharedTexture);
        obs_leave_graphics();
        eye.sharedTexture = nullptr;
    }
    eye.sharedHandle = 0;
}

void OpenXrIpcConsumer::DestroyEye(EyeState &eye)
{
    DestroySharedTexture(eye);
    if (eye.privateTexture) {
        obs_enter_graphics();
        gs_texture_destroy(eye.privateTexture);
        obs_leave_graphics();
        eye.privateTexture = nullptr;
    }
    eye.privateFormat = GS_UNKNOWN;
    eye.hasFrame = false;
    eye.width = 0;
    eye.height = 0;
}

void OpenXrIpcConsumer::DestroyAllTextures()
{
    for (EyeState &eye : m_eyes) {
        DestroyEye(eye);
    }
}

void OpenXrIpcConsumer::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_sharedHeader) {
        m_sharedHeader->obs_connected = 0;
        m_sharedHeader->requested_eye_mask = 0;
        UnmapViewOfFile(m_sharedHeader);
        m_sharedHeader = nullptr;
    }

    if (m_hMapFile) {
        CloseHandle(m_hMapFile);
        m_hMapFile = nullptr;
    }

    DestroyAllTextures();
    m_lastInitAttempt = 0;
}

uint32_t OpenXrIpcConsumer::AddClient()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint32_t id = m_nextClientId++;
    m_clients[id] = Client{};
    return id;
}

void OpenXrIpcConsumer::RemoveClient(uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clients.erase(id);
    if (m_sharedHeader) {
        PublishInterestLocked();
    }
}

void OpenXrIpcConsumer::SetInterest(uint32_t id, bool visible, VREyeSelection eye)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(id);
    if (it == m_clients.end()) {
        return;
    }
    it->second.visible = visible;
    it->second.eye = eye;

    // Only create the mapping once somebody actually wants OpenXR frames.
    if (m_sharedHeader || (visible && InitializeLocked())) {
        PublishInterestLocked();
    }
}

void OpenXrIpcConsumer::PublishInterestLocked()
{
    uint32_t mask = 0;
    for (const auto &entry : m_clients) {
        if (entry.second.visible && ValidEye(entry.second.eye)) {
            mask |= EyeBit(entry.second.eye);
        }
    }
    m_sharedHeader->requested_eye_mask = mask;
    m_sharedHeader->obs_connected = mask ? 1 : 0;
    if (mask) {
        m_sharedHeader->last_consumer_heartbeat = GetTickCount64();
    }
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
    for (EyeState &eye : m_eyes) {
        if (eye.sharedTexture || eye.privateTexture) {
            DestroyEye(eye);
        }
    }

    return false;
}

bool OpenXrIpcConsumer::UpdateTexture(VREyeSelection eyeSel)
{
    if (!ValidEye(eyeSel)) {
        return false;
    }
    const uint32_t eyeIndex = static_cast<uint32_t>(eyeSel);

    std::lock_guard<std::mutex> lock(m_mutex);

    // m_mutex is not recursive: must call the Locked variant here.
    if (!InitializeLocked()) {
        return false;
    }

    // Heartbeat and interest (connected flag / eye mask come from all clients)
    PublishInterestLocked();
    m_sharedHeader->last_consumer_heartbeat = GetTickCount64();

    EyeState &eye = m_eyes[eyeIndex];

    EyeSnapshot snap = {};
    if (!ReadEyeConsistent(m_sharedHeader, eyeIndex, snap)) {
        return eye.hasFrame; // producer busy; keep showing the last frame
    }

    // The header is written by another (untrusted-ish) process; reject
    // missing or out-of-range textures rather than acting on them.
    if (snap.shared_handle == 0 || snap.width == 0 || snap.height == 0 ||
        snap.width > VR_IPC_MAX_TEXTURE_DIM || snap.height > VR_IPC_MAX_TEXTURE_DIM) {
        return eye.hasFrame;
    }

    obs_enter_graphics();

    if (snap.shared_handle != eye.sharedHandle || !eye.sharedTexture) {
        DestroySharedTexture(eye);

        // Legacy (GetSharedHandle) DXGI handles are 32-bit values by design.
        if ((snap.shared_handle >> 32) == 0) {
            eye.sharedTexture = gs_texture_open_shared(static_cast<uint32_t>(snap.shared_handle));
        }
        if (eye.sharedTexture) {
            eye.sharedHandle = snap.shared_handle;
            auto *d3dTex = reinterpret_cast<ID3D11Texture2D *>(gs_texture_get_obj(eye.sharedTexture));
            if (d3dTex) {
                d3dTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(eye.keyedMutex.GetAddressOf()));
            }
        }
    }

    if (eye.sharedTexture) {
        // Size/format come from the texture OBS actually opened, not the
        // header, so the private copy always matches what we copy from.
        const uint32_t w = gs_texture_get_width(eye.sharedTexture);
        const uint32_t h = gs_texture_get_height(eye.sharedTexture);
        const gs_color_format fmt = gs_texture_get_color_format(eye.sharedTexture);

        if (fmt == GS_UNKNOWN) {
            if (!m_loggedBadFormat) {
                blog(LOG_WARNING, "[win_vrcapture] OpenXR shared texture has an unsupported format (DXGI %u)",
                     m_sharedHeader->eyes[eyeIndex].dxgi_format);
                m_loggedBadFormat = true;
            }
        } else {
            if (!eye.privateTexture || eye.width != w || eye.height != h || eye.privateFormat != fmt) {
                if (eye.privateTexture) {
                    gs_texture_destroy(eye.privateTexture);
                }
                eye.privateTexture = gs_texture_create(w, h, fmt, 1, nullptr, 0);
                eye.privateFormat = fmt;
                eye.width = eye.privateTexture ? w : 0;
                eye.height = eye.privateTexture ? h : 0;
                eye.hasFrame = false;
            }

            if (eye.privateTexture) {
                if (eye.keyedMutex) {
                    // Take the frame only when the producer has handed us
                    // key 1 (i.e. there is a new one), copy it out, and give
                    // the key straight back so the game can write the next
                    // frame. Rendering reads privateTexture, never the shared
                    // texture, so every render call this OBS frame -- preview,
                    // program, projectors -- sees the same image.
                    if (eye.keyedMutex->AcquireSync(1, 0) == S_OK) {
                        gs_copy_texture(eye.privateTexture, eye.sharedTexture);
                        eye.keyedMutex->ReleaseSync(0);
                        eye.hasFrame = true;
                    }
                } else if (snap.frame_index != eye.frameIndex || !eye.hasFrame) {
                    // Fallback texture without a keyed mutex: no cross-process
                    // sync available, so copy whenever a new frame is published.
                    gs_copy_texture(eye.privateTexture, eye.sharedTexture);
                    eye.hasFrame = true;
                }
                eye.frameIndex = snap.frame_index;
            }
        }
    }

    obs_leave_graphics();
    return eye.hasFrame;
}

const OpenXrIpcConsumer::EyeState *OpenXrIpcConsumer::FindEye(VREyeSelection eye) const
{
    return ValidEye(eye) ? &m_eyes[static_cast<uint32_t>(eye)] : nullptr;
}

gs_texture_t *OpenXrIpcConsumer::GetTexture(VREyeSelection eye) const
{
    const EyeState *e = FindEye(eye);
    return (e && e->hasFrame) ? e->privateTexture : nullptr;
}

uint32_t OpenXrIpcConsumer::GetWidth(VREyeSelection eye) const
{
    const EyeState *e = FindEye(eye);
    return e ? e->width : 0;
}

uint32_t OpenXrIpcConsumer::GetHeight(VREyeSelection eye) const
{
    const EyeState *e = FindEye(eye);
    return e ? e->height : 0;
}

bool OpenXrIpcConsumer::Render(VREyeSelection eyeSel, gs_effect_t *effect, uint32_t x, uint32_t y, uint32_t cx,
                               uint32_t cy)
{
    const EyeState *eye = FindEye(eyeSel);
    if (!eye || !eye->privateTexture || !eye->hasFrame || !effect) {
        return false;
    }

    // Clamp the crop to the texture; a 0 extent means "to the edge".
    x = (std::min)(x, eye->width - 1);
    y = (std::min)(y, eye->height - 1);
    cx = (cx == 0 || x + cx > eye->width) ? eye->width - x : cx;
    cy = (cy == 0 || y + cy > eye->height) ? eye->height - y : cy;

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, eye->privateTexture);
    while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite_subregion(eye->privateTexture, 0, x, y, cx, cy);
    }
    return true;
}

} // namespace vrcapture
