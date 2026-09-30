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
    bool UpdateTexture(VREyeSelection eye);

    gs_texture_t *GetTexture() const { return m_obsTexture; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint64_t GetFrameIndex() const { return m_frameIndex; }
    DXGI_FORMAT GetFormat() const { return m_format; }

    void SetConnected(bool connected);
    void Render(gs_effect_t *effect);

private:
    void DestroyTexture();

    std::mutex m_mutex;

    HANDLE m_hMapFile = nullptr;
    VRSharedFrameHeader *m_sharedHeader = nullptr;

    gs_texture_t *m_obsTexture = nullptr;
    ComPtr<IDXGIKeyedMutex> m_obsKeyedMutex;

    uint64_t m_currentSharedHandle = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    uint64_t m_frameIndex = 0;
};

} // namespace vrcapture
