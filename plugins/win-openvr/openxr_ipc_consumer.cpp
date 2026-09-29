#include "openxr_ipc_consumer.h"
#include "obs_draw_util.h"
#include <sddl.h>
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

    // OBS is commonly run as administrator (for game capture). An object
    // created by an elevated process gets a default DACL that only admits
    // Administrators and SYSTEM, so the non-elevated games that must open it
    // would get ACCESS_DENIED. Grant Authenticated Users access explicitly.
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)",
                                                             SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }

    // OBS owns the mapping (see vr_ipc_types.h): create it, or open it if a
    // still-attached game from an earlier OBS run already has it.
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        sd ? &sa : nullptr,
        PAGE_READWRITE,
        0,
        sizeof(VRSharedFrameHeader),
        VR_IPC_SHARED_MEMORY_NAME);
    const DWORD createError = GetLastError();
    if (sd) {
        LocalFree(sd);
    }
    if (!hMap) {
        return false;
    }
    const bool created = createError != ERROR_ALREADY_EXISTS;

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
    eye.sharedTexture.Reset();
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

    // The game's texture is opened directly on OBS's D3D11 device, not via
    // gs_texture_open_shared: OBS can't represent common swapchain formats
    // such as R8G8B8A8_UNORM_SRGB, and we only ever copy out of it anyway.
    auto *obsDevice = gs_get_device_type() == GS_DEVICE_DIRECT3D_11
                          ? static_cast<ID3D11Device *>(gs_get_device_obj())
                          : nullptr;

    if (obsDevice && (snap.shared_handle != eye.sharedHandle || !eye.sharedTexture)) {
        DestroySharedTexture(eye);

        HRESULT hr = obsDevice->OpenSharedResource(reinterpret_cast<HANDLE>(snap.shared_handle),
                                                   IID_PPV_ARGS(eye.sharedTexture.GetAddressOf()));
        if (FAILED(hr) || !eye.sharedTexture) {
            eye.sharedTexture.Reset();
            blog(LOG_WARNING,
                 "[win_vrcapture] could not open the game's shared texture for eye %u (handle 0x%llx, hr 0x%08lx)",
                 eyeIndex, (unsigned long long)snap.shared_handle, hr);
        } else {
            eye.sharedHandle = snap.shared_handle;
            eye.sharedTexture.As(&eye.keyedMutex); // absent on the no-keyed-mutex fallback path
        }
    }

    if (eye.sharedTexture) {
        // Size/format come from the texture actually opened, not the header,
        // so the private copy always matches what we copy from.
        D3D11_TEXTURE2D_DESC desc = {};
        eye.sharedTexture->GetDesc(&desc);
        const uint32_t w = desc.Width;
        const uint32_t h = desc.Height;
        const gs_color_format fmt = GsFormatForSwapchainContent(desc.Format);

        if (fmt == GS_UNKNOWN) {
            if (!m_loggedBadFormat) {
                blog(LOG_WARNING, "[win_vrcapture] OpenXR shared texture has an unsupported format (DXGI %d)",
                     static_cast<int>(desc.Format));
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

            // Same device as the shared texture, so this is a plain GPU copy on
            // OBS's own context (graphics thread, inside the graphics lock).
            auto *dst = eye.privateTexture ? static_cast<ID3D11Texture2D *>(gs_texture_get_obj(eye.privateTexture))
                                           : nullptr;
            ComPtr<ID3D11DeviceContext> obsContext;
            obsDevice->GetImmediateContext(obsContext.GetAddressOf());
            auto copyFrame = [&]() { obsContext->CopyResource(dst, eye.sharedTexture.Get()); };

            if (dst && obsContext) {
                if (eye.keyedMutex) {
                    // Take the frame only when the producer has handed us
                    // key 1 (i.e. there is a new one), copy it out, and give
                    // the key straight back so the game can write the next
                    // frame. Rendering reads privateTexture, never the shared
                    // texture, so every render call this OBS frame -- preview,
                    // program, projectors -- sees the same image.
                    if (eye.keyedMutex->AcquireSync(1, 0) == S_OK) {
                        copyFrame();
                        eye.keyedMutex->ReleaseSync(0);
                        eye.hasFrame = true;
                    }
                } else if (snap.frame_index != eye.frameIndex || !eye.hasFrame) {
                    // Fallback texture without a keyed mutex: no cross-process
                    // sync available, so copy whenever a new frame is published.
                    copyFrame();
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

    // The private copy keeps the producer's gs format, so sRGB swapchains are
    // decoded as sRGB and UNORM/float swapchains (linear per the OpenXR spec)
    // are sampled as linear.
    DrawTextureRegion(effect, eye->privateTexture, x, y, cx, cy);
    return true;
}

} // namespace vrcapture
