#pragma once

// The official openxr_platform.h does not include the platform/graphics headers
// itself; they must precede it (XR_USE_* are defined in CMakeLists.txt).
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include "layer_ipc_producer.h"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <wrl/client.h>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

struct SwapchainInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t arraySize = 1;
    uint32_t mipCount = 1;
    uint32_t sampleCount = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<ComPtr<ID3D11Texture2D>> d3d11_textures;

    // Index most recently returned by xrAcquireSwapchainImage, promoted to
    // currentImageIndex once xrReleaseSwapchainImage confirms the app is done
    // rendering into it -- this is the image actually submitted this frame.
    uint32_t pendingAcquiredIndex = 0;
    uint32_t currentImageIndex = 0;
};

class OpenXRInterceptor {
public:
    static OpenXRInterceptor &Get();

    void SetNextGetInstanceProcAddr(PFN_xrGetInstanceProcAddr next) { m_nextGetInstanceProcAddr = next; }
    PFN_xrGetInstanceProcAddr GetNextGetInstanceProcAddr() const { return m_nextGetInstanceProcAddr; }
    void SetInstance(XrInstance instance) { m_instance = instance; }

    // Intercepted OpenXR functions
    XrResult xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session);
    XrResult xrDestroySession(XrSession session);
    XrResult xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain);
    XrResult xrDestroySwapchain(XrSwapchain swapchain);
    XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images);
    XrResult xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index);
    XrResult xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo);
    XrResult xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo);

    PFN_xrVoidFunction GetHookedProcAddr(const char *name);

private:
    OpenXRInterceptor() = default;
    ~OpenXRInterceptor() = default;

    PFN_xrGetInstanceProcAddr m_nextGetInstanceProcAddr = nullptr;
    XrInstance m_instance = nullptr;

    // Real function pointers, lazily resolved on first use via m_nextGetInstanceProcAddr
    PFN_xrDestroyInstance m_pfnDestroyInstance = nullptr;
    PFN_xrCreateSession m_pfnCreateSession = nullptr;
    PFN_xrDestroySession m_pfnDestroySession = nullptr;
    PFN_xrCreateSwapchain m_pfnCreateSwapchain = nullptr;
    PFN_xrDestroySwapchain m_pfnDestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages m_pfnEnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage m_pfnAcquireSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage m_pfnReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame m_pfnEndFrame = nullptr;

    // D3D11 Graphics State
    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    bool m_isD3D12 = false;

    // Swapchain registry
    std::mutex m_swapchainMutex;
    std::unordered_map<XrSwapchain, SwapchainInfo> m_swapchains;

    // Shared IPC Producer
    LayerIpcProducer m_ipc;
};

} // namespace vrcapture
