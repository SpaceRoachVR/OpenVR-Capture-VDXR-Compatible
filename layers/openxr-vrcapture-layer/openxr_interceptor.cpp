#include "openxr_interceptor.h"
#include "layer_log.h"
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

void OpenXRInterceptor::ResetGraphicsLocked()
{
    m_swapchains.clear();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();
    m_d3d12.Reset();
    m_isD3D12 = false;
}

XrResult OpenXRInterceptor::xrDestroyInstance(XrInstance instance)
{
    PFN_xrDestroyInstance destroyInstance = nullptr;
    ResolveProc(m_nextGetInstanceProcAddr, instance, "xrDestroyInstance", destroyInstance);

    // Normally xrDestroySession already did this; an app may destroy the
    // instance with a session still alive.
    m_ipc.StopWorker();
    m_ipc.Shutdown();
    {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        ResetGraphicsLocked();
    }

    // Function pointers are only valid for the instance they were resolved
    // from. Forget them so a later instance in the same process re-resolves
    // against itself instead of calling into a destroyed one.
    m_instance = nullptr;
    m_pfnCreateSession = nullptr;
    m_pfnDestroySession = nullptr;
    m_pfnCreateSwapchain = nullptr;
    m_pfnDestroySwapchain = nullptr;
    m_pfnEnumerateSwapchainImages = nullptr;
    m_pfnAcquireSwapchainImage = nullptr;
    m_pfnReleaseSwapchainImage = nullptr;
    m_pfnEndFrame = nullptr;

    return destroyInstance ? destroyInstance(instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult OpenXRInterceptor::xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session)
{
    if (m_instance == nullptr) {
        m_instance = instance;
    }
    ResolveProc(m_nextGetInstanceProcAddr, instance, "xrCreateSession", m_pfnCreateSession);

    if (!m_pfnCreateSession) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = m_pfnCreateSession(instance, createInfo, session);
    if (XR_FAILED(result) || !createInfo) {
        LayerLog("xrCreateSession: runtime returned %d", (int)result);
        return result;
    }

    // Record the graphics binding of the session that was actually created.
    const XrGraphicsBindingD3D11KHR *d3d11 = nullptr;
    const XrGraphicsBindingD3D12KHR *d3d12 = nullptr;
    const XrBaseInStructure *next = reinterpret_cast<const XrBaseInStructure *>(createInfo->next);
    // Bound the walk: a malformed or accidentally-cyclic extension chain
    // must not be able to hang the app in an infinite loop here.
    constexpr int kMaxChainLength = 64;
    for (int guard = 0; next && guard < kMaxChainLength; ++guard, next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(next);
        } else if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR *>(next);
        }
    }

    std::lock_guard<std::mutex> lock(m_swapchainMutex);
    ResetGraphicsLocked();
    if (d3d11 && d3d11->device) {
        m_d3d11Device = d3d11->device;
        m_d3d11Device->GetImmediateContext(m_d3d11Context.GetAddressOf());
        LayerLog("xrCreateSession: D3D11 session - capturable");
    } else if (d3d12 && d3d12->device && d3d12->queue) {
        // D3D12: layer a D3D11 device over the app's device and queue and
        // capture through it (see D3D12Interop). Other graphics APIs
        // (Vulkan, OpenGL) are not captured.
        if (m_d3d12.Initialize(d3d12->device, d3d12->queue)) {
            m_d3d11Device = m_d3d12.Device();
            m_d3d11Context = m_d3d12.Context();
            m_isD3D12 = true;
            LayerLog("xrCreateSession: D3D12 session - capturable via D3D11On12");
        } else {
            LayerLog("xrCreateSession: D3D12 session but D3D11On12 setup failed - not capturable");
        }
    } else {
        LayerLog("xrCreateSession: no D3D11/D3D12 graphics binding (Vulkan/OpenGL?) - not capturable");
    }
    return result;
}

XrResult OpenXRInterceptor::xrDestroySession(XrSession session)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrDestroySession", m_pfnDestroySession);

    // Safe teardown point for the worker: an ordinary app-driven call, not the
    // loader lock.
    LayerLog("xrDestroySession");
    m_ipc.StopWorker();
    m_ipc.Shutdown();
    m_loggedAttached = m_loggedConnected = false;
    m_loggedEyeMask = 0;

    {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        ResetGraphicsLocked();
    }

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
        info.arraySize = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
        info.mipCount = createInfo->mipCount > 0 ? createInfo->mipCount : 1;
        info.sampleCount = createInfo->sampleCount;
        info.format = static_cast<DXGI_FORMAT>(createInfo->format);
        info.usageFlags = createInfo->usageFlags;
        m_swapchains[*swapchain] = info;
        LayerLog("xrCreateSwapchain %p: %ux%u format %lld array %u mips %u samples %u usage 0x%llx",
                 (void *)*swapchain, info.width, info.height, (long long)createInfo->format, info.arraySize,
                 info.mipCount, info.sampleCount, (unsigned long long)info.usageFlags);
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
                LayerLog("xrEnumerateSwapchainImages %p: %u D3D11 images", (void *)swapchain, count);
            } else if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR && m_d3d12.IsValid() &&
                       (it->second.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
                // Only color swapchains can be projection views; wrapping
                // assumes their RENDER_TARGET hand-back state.
                auto *d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR *>(images);
                uint32_t count = imageCountOutput ? *imageCountOutput : imageCapacityInput;
                count = (std::min)(count, imageCapacityInput);
                it->second.d3d11_textures.resize(count);
                uint32_t wrappedOk = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    it->second.d3d11_textures[i] = m_d3d12.Wrap(d3d12Images[i].texture);
                    wrappedOk += it->second.d3d11_textures[i] ? 1 : 0;
                }
                LayerLog("xrEnumerateSwapchainImages %p: %u D3D12 images, %u wrapped", (void *)swapchain, count,
                         wrappedOk);
            } else {
                LayerLog("xrEnumerateSwapchainImages %p: image type %d not captured", (void *)swapchain,
                         (int)images->type);
            }
        } else {
            LayerLog("xrEnumerateSwapchainImages %p: swapchain was never seen by xrCreateSwapchain",
                     (void *)swapchain);
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

bool OpenXRInterceptor::CaptureView(VREyeSelection eye, const XrCompositionLayerProjectionView &view)
{
    const XrSwapchain swapchain = view.subImage.swapchain;
    const uint32_t arrayIndex = view.subImage.imageArrayIndex;

    // Resolve everything we need under the lock, take a strong reference to
    // the source texture, then release it before touching the GPU. Holding
    // m_swapchainMutex across the copy would stall the per-frame
    // Acquire/Release hooks on the app's render thread and cost frame time in
    // the headset.
    ComPtr<ID3D11Texture2D> srcTexRef;
    uint32_t swapWidth = 0, swapHeight = 0, swapMips = 1, swapArraySize = 1;
    DXGI_FORMAT swapFormat = DXGI_FORMAT_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(m_swapchainMutex);
        auto it = m_swapchains.find(swapchain);
        if (it == m_swapchains.end()) {
            LAYER_LOG_ONCE("capture: projection view uses swapchain %p that was never seen", (void *)swapchain);
        } else if (it->second.d3d11_textures.empty()) {
            LAYER_LOG_ONCE("capture: swapchain %p has no captured images (not enumerated / unsupported type)",
                           (void *)swapchain);
        } else if (it->second.sampleCount > 1) {
            LAYER_LOG_ONCE("capture: swapchain %p is MSAA (%u samples) - not captured", (void *)swapchain,
                           it->second.sampleCount);
        } else if (it->second.currentImageIndex >= it->second.d3d11_textures.size()) {
            LAYER_LOG_ONCE("capture: image index %u out of range (%zu images)", it->second.currentImageIndex,
                           it->second.d3d11_textures.size());
        }
        if (it != m_swapchains.end() && !it->second.d3d11_textures.empty() && it->second.sampleCount <= 1) {
            // Use the image actually submitted this frame (tracked via
            // xrAcquireSwapchainImage/xrReleaseSwapchainImage), not a fixed
            // slot -- swapchains are multi-buffered, so a fixed index would
            // frequently capture a stale/wrong buffer. MSAA swapchains are
            // skipped: CopySubresourceRegion can't read them (they'd need a
            // resolve).
            uint32_t imageIndex = it->second.currentImageIndex;
            if (imageIndex < it->second.d3d11_textures.size()) {
                srcTexRef = it->second.d3d11_textures[imageIndex];
                swapWidth = it->second.width;
                swapHeight = it->second.height;
                swapMips = it->second.mipCount;
                swapArraySize = it->second.arraySize;
                swapFormat = it->second.format;
            }
        }
    }
    if (!srcTexRef) {
        return false;
    }

    const XrRect2Di &rect = view.subImage.imageRect;
    const int64_t cropX = rect.offset.x;
    const int64_t cropY = rect.offset.y;
    const int64_t cropWidth = rect.extent.width > 0 ? rect.extent.width : swapWidth;
    const int64_t cropHeight = rect.extent.height > 0 ? rect.extent.height : swapHeight;

    // The rect comes from the app; never let it drive an out-of-bounds copy.
    if (cropX < 0 || cropY < 0 || cropWidth <= 0 || cropHeight <= 0 || cropX + cropWidth > swapWidth ||
        cropY + cropHeight > swapHeight || arrayIndex >= swapArraySize) {
        LAYER_LOG_ONCE("capture: view rect (%lld,%lld %lldx%lld) layer %u outside swapchain %ux%u array %u",
                       cropX, cropY, cropWidth, cropHeight, arrayIndex, swapWidth, swapHeight, swapArraySize);
        return false;
    }

    const uint32_t w = static_cast<uint32_t>(cropWidth);
    const uint32_t h = static_cast<uint32_t>(cropHeight);
    if (!m_ipc.HasTexture(eye, w, h, swapFormat)) {
        // Non-blocking: hands resource (re)creation to a background worker
        // instead of stalling this latency-sensitive frame-submission call.
        // Until it lands, CopyEye drops frames whose size doesn't match.
        m_ipc.RequestAsyncInitialize(eye, m_d3d11Device.Get(), w, h, swapFormat, false);
    }

    D3D11_BOX box = {};
    box.left = static_cast<UINT>(cropX);
    box.top = static_cast<UINT>(cropY);
    box.front = 0;
    box.right = static_cast<UINT>(cropX + cropWidth);
    box.bottom = static_cast<UINT>(cropY + cropHeight);
    box.back = 1;

    const float fov[4] = {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown};
    const float orientation[4] = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                                  view.pose.orientation.w};
    const float position[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};

    // Wrapped D3D12 images must be acquired around any D3D11 use.
    if (m_isD3D12) {
        m_d3d12.Acquire(srcTexRef.Get());
    }
    const bool copied = m_ipc.CopyEye(eye, m_d3d11Context.Get(), srcTexRef.Get(),
                                      D3D11CalcSubresource(0, arrayIndex, swapMips), box, fov, orientation, position);
    if (m_isD3D12) {
        m_d3d12.Release(srcTexRef.Get());
    }
    return copied;
}

XrResult OpenXRInterceptor::xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo)
{
    ResolveProc(m_nextGetInstanceProcAddr, m_instance, "xrEndFrame", m_pfnEndFrame);

    if (frameEndInfo) {
        LAYER_LOG_ONCE("first xrEndFrame: %u layer(s), first type %d, capturable session: %s",
                       frameEndInfo->layerCount,
                       (frameEndInfo->layerCount && frameEndInfo->layers[0]) ? (int)frameEndInfo->layers[0]->type : -1,
                       (m_d3d11Device && m_d3d11Context) ? "yes" : "no");
    }

    // Only D3D11 sessions are capturable, so only those attach to OBS's
    // mapping; TryAttach is throttled and costs nothing once attached.
    const bool attached = frameEndInfo && m_d3d11Device && m_d3d11Context && m_ipc.TryAttach();
    const bool connected = attached && m_ipc.IsObsConnected();
    if (attached != m_loggedAttached) {
        LayerLog(attached ? "attached to OBS shared memory" : "detached from OBS shared memory");
        m_loggedAttached = attached;
    }
    if (connected != m_loggedConnected) {
        LayerLog(connected ? "OBS has a visible VR Capture source: capturing" : "no visible OBS source: idle");
        m_loggedConnected = connected;
    }
    if (connected) {
        const uint32_t mask = m_ipc.GetRequestedEyeMask();
        if (mask != m_loggedEyeMask) {
            LayerLog("OBS requests eyes: %s%s", (mask & 1) ? "left " : "", (mask & 2) ? "right" : "");
            m_loggedEyeMask = mask;
        }
    }
    if (connected) {
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            if (!frameEndInfo->layers[i]) continue;

            if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto *proj = reinterpret_cast<const XrCompositionLayerProjection *>(frameEndInfo->layers[i]);
                if (proj && proj->viewCount > 0 && proj->views) {
                    bool anyCopied = false;
                    const uint32_t eyeMask = m_ipc.GetRequestedEyeMask();
                    for (uint32_t eyeIndex = 0; eyeIndex < VR_IPC_EYE_COUNT; ++eyeIndex) {
                        const VREyeSelection eye = static_cast<VREyeSelection>(eyeIndex);
                        if (!(eyeMask & EyeBit(eye))) {
                            continue; // no visible OBS source wants this eye
                        }
                        // Stereo: view 0 = left, 1 = right. Mono apps have a
                        // single view, which then serves both eyes.
                        const uint32_t viewIndex = (std::min)(eyeIndex, proj->viewCount - 1);
                        anyCopied |= CaptureView(eye, proj->views[viewIndex]);
                    }
                    if (anyCopied) {
                        LAYER_LOG_ONCE("first frame handed to OBS");
                        if (m_isD3D12) {
                            // Submit the copies to the app's queue, ordered
                            // after the frame's own rendering work.
                            m_d3d12.Flush();
                        }
                        m_ipc.PublishFrame(frameEndInfo->displayTime);
                    }
                }
                break; // Handled projection layer
            }
        }
        bool sawProjection = false;
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            sawProjection |= frameEndInfo->layers[i] && frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        }
        if (!sawProjection && frameEndInfo->layerCount > 0) {
            LAYER_LOG_ONCE("capture: frame has %u layer(s) but no projection layer (first type %d)",
                           frameEndInfo->layerCount,
                           frameEndInfo->layers[0] ? (int)frameEndInfo->layers[0]->type : -1);
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

    if (strcmp(name, "xrDestroyInstance") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(+[](XrInstance instance) {
            return OpenXRInterceptor::Get().xrDestroyInstance(instance);
        });
    }
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
