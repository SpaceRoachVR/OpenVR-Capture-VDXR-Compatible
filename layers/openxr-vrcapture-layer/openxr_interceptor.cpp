#include "openxr_interceptor.h"
#include <string.h>
#include <algorithm>

namespace vrcapture {

namespace {

// Lazily resolves a real (unhooked) OpenXR entry point once and caches it in
// `out`. Every intercepted method other than xrCreateSession previously left
// its m_pfnXxx pointer permanently null -- nothing chained the call through to
// the runtime, so the app would get XR_ERROR_FUNCTION_UNSUPPORTED back for e.g.
// xrCreateSwapchain/xrEndFrame the moment this layer was loaded.
template <typename PFN>
bool ResolveProc(PFN_xrGetInstanceProcAddr getProcAddr, XrInstance instance, const char *name, PFN &out)
{
    if (out) return true;
    if (!getProcAddr || instance == nullptr) return false;
    return XR_SUCCEEDED(getProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&out)));
}

} // namespace

OpenXRInterceptor &OpenXRInterceptor::Get()
{
    // Intentionally leaked: a static object here would run its destructor at
    // DLL unload, i.e. potentially under the Windows loader lock, where joining
    // the IPC worker thread would deadlock the host application on exit. Real
    // cleanup happens in xrDestroySession; the OS reclaims the rest at exit.
    static OpenXRInterceptor *s_instance = new OpenXRInterceptor();
    return *s_instance;
}

XrResult OpenXRInterceptor::xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session)
{
    if (m_instance == nullptr) {
        m_instance = instance;
    }
    ResolveProc(m_nextGetInstanceProcAddr, instance, "xrCreateSession", m_pfnCreateSession);

    if (createInfo && createInfo->next) {
        const XrBaseInStructure *next = reinterpret_cast<const XrBaseInStructure *>(createInfo->next);
        // Bound the walk: a malformed or accidentally-cyclic extension chain
        // must not be able to hang the app in an infinite loop here.
        constexpr int kMaxChainLength = 64;
        int guard = 0;
        while (next && guard++ < kMaxChainLength) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const auto *d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(next);
                if (d3d11 && d3d11->device) {
                    m_d3d11Device = d3d11->device;
                    m_d3d11Device->GetImmediateContext(m_d3d11Context.ReleaseAndGetAddressOf());
                    m_isD3D12 = false;
                }
            } else if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                m_isD3D12 = true;
            }
            next = next->next;
        }
    }

    if (m_pfnCreateSession) {
        return m_pfnCreateSession(instance, createInfo, session);
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult OpenXRInterceptor::xrDestroySession(XrSession session)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrDestroySession", m_pfnDestroySession);

    // Safe teardown point for the worker: an ordinary app-driven call, not the
    // loader lock.
    m_ipc.StopWorker();
    m_ipc.Shutdown();

    std::lock_guard<std::mutex> lock(m_swapchainMutex);
    m_swapchains.clear();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();

    if (m_pfnDestroySession) {
        return m_pfnDestroySession(session);
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult OpenXRInterceptor::xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrCreateSwapchain", m_pfnCreateSwapchain);

    XrResult result = m_pfnCreateSwapchain ? m_pfnCreateSwapchain(session, createInfo, swapchain) : XR_ERROR_FUNCTION_UNSUPPORTED;

    if (XR_SUCCEEDED(result) && swapchain && createInfo) {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        SwapchainInfo info = {};
        info.width = createInfo->width;
        info.height = createInfo->height;
        info.arraySize = createInfo->arraySize;
        info.format = static_cast<DXGI_FORMAT>(createInfo->format);
        m_swapchains[*swapchain] = info;
    }

    return result;
}

XrResult OpenXRInterceptor::xrDestroySwapchain(XrSwapchain swapchain)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrDestroySwapchain", m_pfnDestroySwapchain);

    {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        m_swapchains.erase(swapchain);
    }

    if (m_pfnDestroySwapchain) {
        return m_pfnDestroySwapchain(swapchain);
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult OpenXRInterceptor::xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrEnumerateSwapchainImages", m_pfnEnumerateSwapchainImages);

    XrResult result = m_pfnEnumerateSwapchainImages ?
        m_pfnEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images) :
        XR_ERROR_FUNCTION_UNSUPPORTED;

    if (XR_SUCCEEDED(result) && images && imageCapacityInput > 0) {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        auto it = m_swapchains.find(swapchain);
        if (it != m_swapchains.end()) {
            if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                auto *d3d11Images = reinterpret_cast<XrSwapchainImageD3D11KHR *>(images);
                // A runtime is only supposed to write imageCapacityInput entries
                // on the enumerate-only call and imageCountOutput<=imageCapacityInput
                // on the fill call, but clamp defensively so a misbehaving runtime
                // can't drive an out-of-bounds read of the caller's `images` array.
                uint32_t count = imageCountOutput ? *imageCountOutput : imageCapacityInput;
                count = (std::min)(count, imageCapacityInput);
                it->second.d3d11_textures.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    it->second.d3d11_textures[i] = d3d11Images[i].texture;
                }
            }
        }
    }

    return result;
}

XrResult OpenXRInterceptor::xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrAcquireSwapchainImage", m_pfnAcquireSwapchainImage);

    XrResult result = m_pfnAcquireSwapchainImage ?
        m_pfnAcquireSwapchainImage(swapchain, acquireInfo, index) :
        XR_ERROR_FUNCTION_UNSUPPORTED;

    if (XR_SUCCEEDED(result) && index) {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        auto it = m_swapchains.find(swapchain);
        if (it != m_swapchains.end()) {
            it->second.pendingAcquiredIndex = *index;
        }
    }

    return result;
}

XrResult OpenXRInterceptor::xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrReleaseSwapchainImage", m_pfnReleaseSwapchainImage);

    XrResult result = m_pfnReleaseSwapchainImage ?
        m_pfnReleaseSwapchainImage(swapchain, releaseInfo) :
        XR_ERROR_FUNCTION_UNSUPPORTED;

    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        auto it = m_swapchains.find(swapchain);
        if (it != m_swapchains.end()) {
            // The app has finished rendering into the image it last acquired --
            // that's the one that will be referenced by the next xrEndFrame.
            it->second.currentImageIndex = it->second.pendingAcquiredIndex;
        }
    }

    return result;
}

XrResult OpenXRInterceptor::xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrEndFrame", m_pfnEndFrame);

    if (frameEndInfo && m_d3d11Device && m_d3d11Context && m_ipc.IsObsConnected()) {
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            if (!frameEndInfo->layers[i]) continue;

            if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto *proj = reinterpret_cast<const XrCompositionLayerProjection *>(frameEndInfo->layers[i]);
                if (proj && proj->viewCount > 0 && proj->views) {
                    // Determine which eye to capture (0 = Left, 1 = Right)
                    VREyeSelection reqEye = m_ipc.GetRequestedEye();
                    uint32_t viewIndex = (reqEye == VREyeSelection::Left) ? 0 : 1;
                    if (viewIndex >= proj->viewCount) {
                        viewIndex = 0;
                    }

                    const XrCompositionLayerProjectionView &view = proj->views[viewIndex];
                    XrSwapchain swapchain = view.subImage.swapchain;
                    uint32_t arrayIndex = view.subImage.imageArrayIndex;

                    // Resolve everything we need under the lock, take a strong
                    // reference to the source texture, then release it before
                    // touching the GPU. Holding m_swapchainMutex across the copy
                    // would stall the per-frame Acquire/Release hooks on the
                    // app's render thread and cost frame time in the headset.
                    ComPtr<ID3D11Texture2D> srcTexRef;
                    uint32_t swapWidth = 0, swapHeight = 0;
                    DXGI_FORMAT swapFormat = DXGI_FORMAT_UNKNOWN;
                    {
                        std::lock_guard<std::mutex> lock(m_swapchainMutex);
                        auto it = m_swapchains.find(swapchain);
                        if (it != m_swapchains.end() && !it->second.d3d11_textures.empty()) {
                            // Use the image actually submitted this frame (tracked via
                            // xrAcquireSwapchainImage/xrReleaseSwapchainImage), not a
                            // fixed slot -- swapchains are multi-buffered, so a fixed
                            // index would frequently capture a stale/wrong buffer.
                            uint32_t imageIndex = it->second.currentImageIndex;
                            if (imageIndex < it->second.d3d11_textures.size()) {
                                srcTexRef = it->second.d3d11_textures[imageIndex];
                                swapWidth = it->second.width;
                                swapHeight = it->second.height;
                                swapFormat = it->second.format;
                            }
                        }
                    }

                    {
                        {
                            ID3D11Texture2D *srcTex = srcTexRef.Get();
                            if (srcTex) {
                                uint32_t cropWidth = view.subImage.imageRect.extent.width;
                                uint32_t cropHeight = view.subImage.imageRect.extent.height;
                                if (cropWidth == 0) cropWidth = swapWidth;
                                if (cropHeight == 0) cropHeight = swapHeight;

                                if (m_ipc.GetWidth() != cropWidth || m_ipc.GetHeight() != cropHeight || !m_ipc.GetSharedTexture()) {
                                    // Non-blocking: hands resource (re)creation to a
                                    // background worker instead of stalling this
                                    // latency-sensitive frame-submission call.
                                    m_ipc.RequestAsyncInitialize(m_d3d11Device.Get(), cropWidth, cropHeight, swapFormat, false);
                                }

                                if (m_ipc.BeginFrameCopy(m_d3d11Context.Get())) {
                                    D3D11_BOX box = {};
                                    box.left = static_cast<UINT>(view.subImage.imageRect.offset.x);
                                    box.top = static_cast<UINT>(view.subImage.imageRect.offset.y);
                                    box.front = 0;
                                    box.right = static_cast<UINT>(box.left + cropWidth);
                                    box.bottom = static_cast<UINT>(box.top + cropHeight);
                                    box.back = 1;

                                    m_d3d11Context->CopySubresourceRegion(
                                        m_ipc.GetSharedTexture(),
                                        0,
                                        0,
                                        0,
                                        0,
                                        srcTex,
                                        arrayIndex,
                                        &box);

                                    float fov[4] = {
                                        view.fov.angleLeft,
                                        view.fov.angleRight,
                                        view.fov.angleUp,
                                        view.fov.angleDown
                                    };

                                    float orientation[4] = {
                                        view.pose.orientation.x,
                                        view.pose.orientation.y,
                                        view.pose.orientation.z,
                                        view.pose.orientation.w
                                    };

                                    float position[3] = {
                                        view.pose.position.x,
                                        view.pose.position.y,
                                        view.pose.position.z
                                    };

                                    m_ipc.EndFrameCopy(frameEndInfo->displayTime, fov, orientation, position);
                                }
                            }
                        }
                    }
                }
                break; // Handled projection layer
            }
        }
    }

    if (m_pfnEndFrame) {
        return m_pfnEndFrame(session, frameEndInfo);
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

PFN_xrVoidFunction OpenXRInterceptor::GetHookedProcAddr(const char *name)
{
    if (!name) return nullptr;

    if (strcmp(name, "xrCreateSession") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session) {
            return OpenXRInterceptor::Get().xrCreateSession(instance, createInfo, session);
        });
    }
    if (strcmp(name, "xrDestroySession") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSession session) {
            return OpenXRInterceptor::Get().xrDestroySession(session);
        });
    }
    if (strcmp(name, "xrCreateSwapchain") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain) {
            return OpenXRInterceptor::Get().xrCreateSwapchain(session, createInfo, swapchain);
        });
    }
    if (strcmp(name, "xrDestroySwapchain") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSwapchain swapchain) {
            return OpenXRInterceptor::Get().xrDestroySwapchain(swapchain);
        });
    }
    if (strcmp(name, "xrEnumerateSwapchainImages") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput, XrSwapchainImageBaseHeader *images) {
            return OpenXRInterceptor::Get().xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
        });
    }
    if (strcmp(name, "xrAcquireSwapchainImage") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo, uint32_t *index) {
            return OpenXRInterceptor::Get().xrAcquireSwapchainImage(swapchain, acquireInfo, index);
        });
    }
    if (strcmp(name, "xrReleaseSwapchainImage") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo) {
            return OpenXRInterceptor::Get().xrReleaseSwapchainImage(swapchain, releaseInfo);
        });
    }
    if (strcmp(name, "xrEndFrame") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrSession session, const XrFrameEndInfo *frameEndInfo) {
            return OpenXRInterceptor::Get().xrEndFrame(session, frameEndInfo);
        });
    }

    return nullptr;
}

} // namespace vrcapture
