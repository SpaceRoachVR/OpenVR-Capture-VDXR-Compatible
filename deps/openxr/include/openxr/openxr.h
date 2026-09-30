#pragma once

#ifndef OPENXR_H_
#define OPENXR_H_

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#define XRAPI_ATTR
#define XRAPI_CALL __stdcall
#define XRAPI_PTR  XRAPI_CALL
#else
#define XRAPI_ATTR
#define XRAPI_CALL
#define XRAPI_PTR
#endif

#define XR_MAKE_VERSION(major, minor, patch) \
    ((((uint64_t)(major)) << 48) | (((uint64_t)(minor)) << 32) | ((uint64_t)(patch)))

#define XR_CURRENT_API_VERSION XR_MAKE_VERSION(1, 0, 34)

#define XR_DEFINE_HANDLE(object) typedef struct object##_T* object;

XR_DEFINE_HANDLE(XrInstance)
XR_DEFINE_HANDLE(XrSession)
XR_DEFINE_HANDLE(XrSpace)
XR_DEFINE_HANDLE(XrAction)
XR_DEFINE_HANDLE(XrSwapchain)
XR_DEFINE_HANDLE(XrActionSet)

typedef uint64_t XrVersion;
typedef uint64_t XrFlags64;
typedef int64_t XrTime;
typedef int64_t XrDuration;
typedef uint32_t XrBool32;

#define XR_TRUE 1
#define XR_FALSE 0

#define XR_MAX_EXTENSION_NAME_SIZE 128
#define XR_MAX_API_LAYER_NAME_SIZE 256
#define XR_MAX_API_LAYER_DESCRIPTION_SIZE 256
#define XR_MAX_SYSTEM_NAME_SIZE 256
#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE 128
#define XR_MAX_RUNTIME_NAME_SIZE 128
#define XR_MAX_PATH_LENGTH 256

typedef enum XrResult {
    XR_SUCCESS = 0,
    XR_TIMEOUT_EXPIRED = 1,
    XR_SESSION_LOSS_PENDING = 3,
    XR_EVENT_UNAVAILABLE = 4,
    XR_SPACE_BOUNDS_UNAVAILABLE = 7,
    XR_SESSION_NOT_FOCUSED = 8,
    XR_FRAME_DISCARDED = 9,
    XR_ERROR_VALIDATION_FAILURE = -1,
    XR_ERROR_RUNTIME_FAILURE = -2,
    XR_ERROR_OUT_OF_MEMORY = -3,
    XR_ERROR_API_VERSION_UNSUPPORTED = -4,
    XR_ERROR_INITIALIZATION_FAILED = -5,
    XR_ERROR_FUNCTION_UNSUPPORTED = -6,
    XR_ERROR_FEATURE_UNSUPPORTED = -7,
    XR_ERROR_EXTENSION_NOT_PRESENT = -8,
    XR_ERROR_LIMIT_REACHED = -9,
    XR_ERROR_SIZE_INSUFFICIENT = -10,
    XR_ERROR_HANDLE_INVALID = -11,
    XR_ERROR_INSTANCE_LOST = -12,
    XR_ERROR_SESSION_RUNNING = -14,
    XR_ERROR_SESSION_NOT_RUNNING = -16,
    XR_ERROR_SESSION_LOST = -17,
    XR_ERROR_SYSTEM_INVALID = -18,
    XR_ERROR_PATH_INVALID = -19,
    XR_ERROR_PATH_COUNT_EXCEEDED = -20,
    XR_ERROR_PATH_FORMAT_INVALID = -21,
    XR_ERROR_PATH_UNSUPPORTED = -22,
    XR_ERROR_LAYER_INVALID = -23,
    XR_ERROR_LAYER_LIMIT_EXCEEDED = -24,
    XR_ERROR_SWAPCHAIN_RECT_INVALID = -25,
    XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED = -26,
    XR_ERROR_ACTION_TYPE_MISMATCH = -27,
    XR_ERROR_SESSION_NOT_READY = -28,
    XR_ERROR_SESSION_NOT_STOPPING = -29,
    XR_ERROR_TIME_INVALID = -30,
    XR_ERROR_REFERENCE_SPACE_UNSUPPORTED = -31,
    XR_ERROR_FILE_ACCESS_ERROR = -32,
    XR_ERROR_FILE_CONTENTS_INVALID = -33,
    XR_ERROR_FORM_FACTOR_UNSUPPORTED = -34,
    XR_ERROR_FORM_FACTOR_UNAVAILABLE = -35,
    XR_ERROR_API_LAYER_NOT_PRESENT = -36,
    XR_ERROR_CALL_ORDER_INVALID = -37,
    XR_ERROR_GRAPHICS_DEVICE_INVALID = -38,
    XR_ERROR_POSE_INVALID = -39,
    XR_ERROR_INDEX_OUT_OF_RANGE = -40,
    XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED = -41,
    XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED = -42,
    XR_ERROR_NAME_DUPLICATED = -44,
    XR_ERROR_NAME_INVALID = -45,
    XR_ERROR_ACTIONSET_NOT_ATTACHED = -46,
    XR_ERROR_ACTIONSETS_ALREADY_ATTACHED = -47,
    XR_ERROR_LOCALIZED_NAME_DUPLICATED = -48,
    XR_ERROR_LOCALIZED_NAME_INVALID = -49,
    XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING = -50,
    XR_ERROR_RUNTIME_UNAVAILABLE = -51,
    XR_RESULT_MAX_ENUM = 0x7FFFFFFF
} XrResult;

#define XR_SUCCEEDED(result) ((XrResult)(result) >= 0)
#define XR_FAILED(result)    ((XrResult)(result) < 0)

typedef enum XrStructureType {
    XR_TYPE_UNKNOWN = 0,
    XR_TYPE_API_LAYER_PROPERTIES = 1,
    XR_TYPE_EXTENSION_PROPERTIES = 2,
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_SYSTEM_PROPERTIES = 5,
    XR_TYPE_ACTION_STATE_BOOLEAN = 6,
    XR_TYPE_ACTION_STATE_FLOAT = 7,
    XR_TYPE_ACTION_STATE_VECTOR2F = 8,
    XR_TYPE_ACTION_STATE_POSE = 9,
    XR_TYPE_ACTION_SET_CREATE_INFO = 10,
    XR_TYPE_ACTION_CREATE_INFO = 11,
    XR_TYPE_INSTANCE_PROPERTIES = 12,
    XR_TYPE_FRAME_WAIT_INFO = 13,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 14,
    XR_TYPE_COMPOSITION_LAYER_QUAD = 15,
    XR_TYPE_FRAME_BEGIN_INFO = 16,
    XR_TYPE_FRAME_END_INFO = 17,
    XR_TYPE_VIEW_CONFIGURATION_PROPERTIES = 18,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 19,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 20,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 21,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 22,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 23,
    XR_TYPE_SESSION_CREATE_INFO = 28,
    XR_TYPE_SESSION_BEGIN_INFO = 29,
    XR_TYPE_GRAPHICS_BINDING_D3D11_KHR = 1000027000,
    XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR = 1000027001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR = 1000027002,
    XR_TYPE_GRAPHICS_BINDING_D3D12_KHR = 1000028000,
    XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR = 1000028001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR = 1000028002,
    XR_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrStructureType;

typedef struct XrBaseInStructure {
    XrStructureType             type;
    const struct XrBaseInStructure* next;
} XrBaseInStructure;

typedef struct XrBaseOutStructure {
    XrStructureType             type;
    struct XrBaseOutStructure*  next;
} XrBaseOutStructure;

typedef struct XrVector2f {
    float x;
    float y;
} XrVector2f;

typedef struct XrVector3f {
    float x;
    float y;
    float z;
} XrVector3f;

typedef struct XrVector4f {
    float x;
    float y;
    float z;
    float w;
} XrVector4f;

typedef struct XrQuaternionf {
    float x;
    float y;
    float z;
    float w;
} XrQuaternionf;

typedef struct XrPosef {
    XrQuaternionf orientation;
    XrVector3f    position;
} XrPosef;

typedef struct XrOffset2Di {
    int32_t x;
    int32_t y;
} XrOffset2Di;

typedef struct XrExtent2Di {
    int32_t width;
    int32_t height;
} XrExtent2Di;

typedef struct XrRect2Di {
    XrOffset2Di offset;
    XrExtent2Di extent;
} XrRect2Di;

typedef struct XrFovf {
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
} XrFovf;

typedef struct XrApplicationInfo {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    uint32_t applicationVersion;
    char engineName[XR_MAX_ENGINE_NAME_SIZE];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct XrInstanceCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct XrSessionCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    uint64_t systemId;
} XrSessionCreateInfo;

typedef struct XrSwapchainCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrFlags64 usageFlags;
    int64_t format;
    uint32_t sampleCount;
    uint32_t width;
    uint32_t height;
    uint32_t faceCount;
    uint32_t arraySize;
    uint32_t mipCount;
} XrSwapchainCreateInfo;

typedef struct XrSwapchainImageBaseHeader {
    XrStructureType type;
    void* next;
} XrSwapchainImageBaseHeader;

typedef struct XrSwapchainImageAcquireInfo {
    XrStructureType type;
    const void* next;
} XrSwapchainImageAcquireInfo;

typedef struct XrSwapchainImageWaitInfo {
    XrStructureType type;
    const void* next;
    XrDuration timeout;
} XrSwapchainImageWaitInfo;

typedef struct XrSwapchainImageReleaseInfo {
    XrStructureType type;
    const void* next;
} XrSwapchainImageReleaseInfo;

typedef struct XrSwapchainSubImage {
    XrSwapchain swapchain;
    XrRect2Di imageRect;
    uint32_t imageArrayIndex;
} XrSwapchainSubImage;

typedef struct XrCompositionLayerBaseHeader {
    XrStructureType type;
    const void* next;
    XrFlags64 layerFlags;
    XrSpace space;
} XrCompositionLayerBaseHeader;

typedef struct XrCompositionLayerProjectionView {
    XrStructureType type;
    const void* next;
    XrPosef pose;
    XrFovf fov;
    XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;

typedef struct XrCompositionLayerProjection {
    XrStructureType type;
    const void* next;
    XrFlags64 layerFlags;
    XrSpace space;
    uint32_t viewCount;
    const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;

typedef struct XrFrameEndInfo {
    XrStructureType type;
    const void* next;
    XrTime displayTime;
    uint32_t environmentBlendMode;
    uint32_t layerCount;
    const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;

// Function Pointer Signatures
typedef void (XRAPI_PTR *PFN_xrVoidFunction)(void);
typedef XrResult (XRAPI_PTR *PFN_xrGetInstanceProcAddr)(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
typedef XrResult (XRAPI_PTR *PFN_xrCreateInstance)(const XrInstanceCreateInfo* createInfo, XrInstance* instance);
typedef XrResult (XRAPI_PTR *PFN_xrDestroyInstance)(XrInstance instance);
typedef XrResult (XRAPI_PTR *PFN_xrCreateSession)(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
typedef XrResult (XRAPI_PTR *PFN_xrDestroySession)(XrSession session);
typedef XrResult (XRAPI_PTR *PFN_xrCreateSwapchain)(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
typedef XrResult (XRAPI_PTR *PFN_xrDestroySwapchain)(XrSwapchain swapchain);
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateSwapchainImages)(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images);
typedef XrResult (XRAPI_PTR *PFN_xrAcquireSwapchainImage)(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index);
typedef XrResult (XRAPI_PTR *PFN_xrWaitSwapchainImage)(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo);
typedef XrResult (XRAPI_PTR *PFN_xrReleaseSwapchainImage)(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo);
typedef XrResult (XRAPI_PTR *PFN_xrEndFrame)(XrSession session, const XrFrameEndInfo* frameEndInfo);

#endif // OPENXR_H_
