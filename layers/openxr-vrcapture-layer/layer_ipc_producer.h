#pragma once

#include "../../shared/vr_ipc_types.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

class LayerIpcProducer {
public:
    LayerIpcProducer();
    ~LayerIpcProducer();

    // Opens OBS's shared mapping if it exists. Cheap to call every frame: it
    // retries the OpenFileMappingW at most once per kAttachRetryMs.
    bool TryAttach();

    // Creates/resizes the shared texture. Requires a prior successful TryAttach().
    bool Initialize(ID3D11Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool is_d3d12 = false);
    void Shutdown();

    // Signals and joins the background worker. Call from a well-defined
    // teardown point (xrDestroySession) -- never from a static destructor,
    // where the join would run under the Windows loader lock and deadlock the
    // host application on exit.
    void StopWorker();

    // Non-blocking: hands the (possibly blocking) CreateTexture2D
    // work to a background worker thread so callers on latency-sensitive paths
    // (e.g. xrEndFrame) never stall on it. Safe to call every frame; only the
    // most recent request is honored if several arrive before the worker catches up.
    void RequestAsyncInitialize(ID3D11Device *device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool is_d3d12 = false);

    bool IsObsConnected();
    VREyeSelection GetRequestedEye();

    // True if the published texture already matches these parameters.
    bool HasTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) const;

    // Copies `box` of source[sourceSubresource] into the shared texture and
    // publishes frame metadata, as one atomic step with respect to texture
    // (re)creation. Returns false (frame dropped) if there is no texture yet,
    // a resize is still pending, or OBS hasn't released the previous frame.
    bool CopyFrame(ID3D11DeviceContext *context, ID3D11Texture2D *source, UINT sourceSubresource,
                   const D3D11_BOX &box, int64_t display_time_ns, const float fov[4],
                   const float orientation[4], const float position[3]);

private:
    void DestroySharedTexture();
    void InitWorkerLoop();

    mutable std::mutex m_mutex;

    // Shared memory IPC
    static constexpr uint64_t kAttachRetryMs = 1000;

    HANDLE m_hMapFile = nullptr;
    VRSharedFrameHeader *m_sharedHeader = nullptr;
    uint64_t m_lastAttachAttempt = 0;

    // D3D11 shared resources
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Texture2D> m_sharedTexture;
    ComPtr<IDXGIKeyedMutex> m_keyedMutex;
    HANDLE m_sharedHandle = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    bool m_isD3D12 = false;

    uint64_t m_frameIndex = 0;

    // Background init worker (see RequestAsyncInitialize)
    std::thread m_initWorker;
    std::mutex m_initCvMutex;
    std::condition_variable m_initCv;
    bool m_initRequestPending = false;
    bool m_initWorkerShouldExit = false;
    bool m_initWorkerStarted = false;

    ComPtr<ID3D11Device> m_pendingDevice;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;
    DXGI_FORMAT m_pendingFormat = DXGI_FORMAT_UNKNOWN;
    bool m_pendingIsD3D12 = false;
};

} // namespace vrcapture
