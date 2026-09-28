#pragma once

#include "../../shared/vr_ipc_types.h"
#include <obs-module.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <mutex>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

class OpenXrIpcConsumer {
public:
    OpenXrIpcConsumer();
    ~OpenXrIpcConsumer();

    bool Initialize();
    void Shutdown();

    bool IsProducerActive();

    // Call once per OBS frame (video_tick). Heartbeats the producer and, if a
    // new frame is available, copies it into an OBS-owned texture. Returns true
    // while there is a frame to render.
    bool UpdateTexture(VREyeSelection eye);

    // The OBS-owned copy of the latest frame (null until the first frame).
    gs_texture_t *GetTexture() const { return m_hasFrame ? m_privateTexture : nullptr; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint64_t GetFrameIndex() const { return m_frameIndex; }

    void SetConnected(bool connected);

    // Draws the (x, y, cx, cy) sub-rectangle of the latest frame at the
    // origin. Graphics thread only. Returns false if there is nothing to draw.
    bool Render(gs_effect_t *effect, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy);

private:
    // Callers of the *Locked / Destroy* helpers must hold m_mutex.
    bool InitializeLocked();
    void DestroySharedTexture();
    void DestroyTexture();

    std::mutex m_mutex;

    HANDLE m_hMapFile = nullptr;
    VRSharedFrameHeader *m_sharedHeader = nullptr;
    uint64_t m_lastInitAttempt = 0;

    // Producer's texture, opened on OBS's device. Only touched in
    // UpdateTexture while holding its keyed mutex (when it has one).
    gs_texture_t *m_sharedTexture = nullptr;
    ComPtr<IDXGIKeyedMutex> m_obsKeyedMutex;
    uint64_t m_currentSharedHandle = 0;

    // OBS-owned copy that is actually rendered.
    gs_texture_t *m_privateTexture = nullptr;
    gs_color_format m_privateFormat = GS_UNKNOWN;
    bool m_hasFrame = false;
    bool m_loggedBadFormat = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_frameIndex = 0;
};

} // namespace vrcapture
