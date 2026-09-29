#pragma once

#include "../../shared/vr_ipc_types.h"
#include <obs-module.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <map>
#include <memory>
#include <mutex>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

// OBS side of the OpenXR layer IPC. There is one instance per OBS process,
// shared by every VR Capture source (see Acquire), because the shared memory
// header has a single "OBS" side: sources register as clients and the consumer
// advertises the union of what visible clients want (connected flag + eye
// mask), so hiding one source never switches off another.
//
// Threading: UpdateTexture / GetTexture / Render run on the OBS graphics thread
// (video_tick / video_render). Client registration may come from any thread.
class OpenXrIpcConsumer {
public:
    // Process-wide shared instance; created on first use, shut down (mapping
    // released, OBS marked disconnected) when the last reference goes away.
    static std::shared_ptr<OpenXrIpcConsumer> Acquire();

    OpenXrIpcConsumer();
    ~OpenXrIpcConsumer();

    bool Initialize();
    void Shutdown();

    // ---- Clients (one per OBS source) ----
    uint32_t AddClient();
    void RemoveClient(uint32_t id);
    // `visible` = the source is showing and may use OpenXR capture.
    void SetInterest(uint32_t id, bool visible, VREyeSelection eye);

    bool IsProducerActive();

    // Call once per OBS frame per interested source (video_tick). Heartbeats
    // the producer and, if a new frame for `eye` is available, copies it into
    // an OBS-owned texture. Several sources on the same eye are fine: the
    // first one per frame copies, the rest see no new frame and reuse it.
    // Returns true while there is a frame for this eye to render.
    bool UpdateTexture(VREyeSelection eye);

    // The OBS-owned copy of the eye's latest frame (null until the first frame).
    gs_texture_t *GetTexture(VREyeSelection eye) const;
    uint32_t GetWidth(VREyeSelection eye) const;
    uint32_t GetHeight(VREyeSelection eye) const;

    // Draws the (x, y, cx, cy) sub-rectangle of the eye's latest frame at the
    // origin. Graphics thread only. Returns false if there is nothing to draw.
    bool Render(VREyeSelection eye, gs_effect_t *effect, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy);

private:
    struct EyeState {
        // Producer's texture, opened on OBS's device. Only touched in
        // UpdateTexture while holding its keyed mutex (when it has one).
        ComPtr<ID3D11Texture2D> sharedTexture;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        uint64_t sharedHandle = 0;

        // OBS-owned copy that is actually rendered.
        gs_texture_t *privateTexture = nullptr;
        gs_color_format privateFormat = GS_UNKNOWN;
        bool hasFrame = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t frameIndex = 0;
    };

    struct Client {
        bool visible = false;
        VREyeSelection eye = VREyeSelection::Right;
    };

    // Callers of the *Locked / Destroy* helpers must hold m_mutex.
    bool InitializeLocked();
    void PublishInterestLocked();
    void DestroySharedTexture(EyeState &eye);
    void DestroyEye(EyeState &eye);
    void DestroyAllTextures();
    const EyeState *FindEye(VREyeSelection eye) const;

    mutable std::mutex m_mutex;

    HANDLE m_hMapFile = nullptr;
    VRSharedFrameHeader *m_sharedHeader = nullptr;
    uint64_t m_lastInitAttempt = 0;

    EyeState m_eyes[VR_IPC_EYE_COUNT];
    bool m_loggedBadFormat = false;

    std::map<uint32_t, Client> m_clients;
    uint32_t m_nextClientId = 1;
};

} // namespace vrcapture
