#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include "openxr_interceptor.h"
#include <string.h>

using namespace vrcapture;

static PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
static PFN_xrCreateApiLayerInstance g_nextCreateApiLayerInstance = nullptr;

static XrResult XRAPI_CALL Hook_xrGetInstanceProcAddr(
    XrInstance instance,
    const char *name,
    PFN_xrVoidFunction *function)
{
    if (!name || !function) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Check our hooked procedures first
    PFN_xrVoidFunction hooked = OpenXRInterceptor::Get().GetHookedProcAddr(name);
    if (hooked) {
        *function = hooked;
        return XR_SUCCESS;
    }

    if (g_nextGetInstanceProcAddr) {
        return g_nextGetInstanceProcAddr(instance, name, function);
    }

    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static XrResult XRAPI_CALL Hook_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo *info,
    const struct XrApiLayerCreateInfo *layerInfo,
    XrInstance *instance)
{
    if (!layerInfo || !layerInfo->nextInfo) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    g_nextGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    g_nextCreateApiLayerInstance = reinterpret_cast<PFN_xrCreateApiLayerInstance>(layerInfo->nextInfo->nextCreateApiLayerInstance);

    OpenXRInterceptor::Get().SetNextGetInstanceProcAddr(g_nextGetInstanceProcAddr);

    // Chain to next layer or runtime
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = const_cast<XrApiLayerNextInfo *>(reinterpret_cast<const XrApiLayerNextInfo *>(layerInfo->nextInfo->next));

    if (g_nextCreateApiLayerInstance) {
        XrResult result = g_nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
        if (XR_SUCCEEDED(result) && instance) {
            OpenXRInterceptor::Get().SetInstance(*instance);
        }
        return result;
    }

    return XR_ERROR_INITIALIZATION_FAILED;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loaderInfo,
    const char *apiLayerName,
    XrNegotiateApiLayerRequest *apiLayerRequest)
{
    (void)apiLayerName;

    if (!loaderInfo || !apiLayerRequest) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != 1 ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != 1 ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = Hook_xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateApiLayerInstance);

    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
