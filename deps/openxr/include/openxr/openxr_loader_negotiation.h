#pragma once

#ifndef OPENXR_LOADER_NEGOTIATION_H_
#define OPENXR_LOADER_NEGOTIATION_H_

#include <openxr/openxr.h>

#define XR_CURRENT_LOADER_API_LAYER_VERSION 1
#define XR_CURRENT_LOADER_RUNTIME_VERSION 1

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
} XrLoaderInterfaceStructs;

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateApiLayerRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrVoidFunction createApiLayerInstance;
} XrNegotiateApiLayerRequest;

typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult(XRAPI_PTR* PFN_xrNegotiateLoaderApiLayerInterface)(
    const XrNegotiateLoaderInfo* loaderInfo,
    const char* apiLayerName,
    XrNegotiateApiLayerRequest* apiLayerRequest);

typedef XrResult(XRAPI_PTR* PFN_xrNegotiateLoaderRuntimeInterface)(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest);

typedef XrResult(XRAPI_PTR* PFN_xrCreateApiLayerInstance)(
    const XrInstanceCreateInfo* info,
    const struct XrApiLayerCreateInfo* layerInfo,
    XrInstance* instance);

typedef struct XrApiLayerNextInfo {
    XrStructureType structType;
    const void* next;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrVoidFunction nextCreateApiLayerInstance;
} XrApiLayerNextInfo;

typedef struct XrApiLayerCreateInfo {
    XrStructureType structType;
    const void* next;
    void* loaderInfo;
    XrApiLayerNextInfo* nextInfo;
} XrApiLayerCreateInfo;

#endif // OPENXR_LOADER_NEGOTIATION_H_
