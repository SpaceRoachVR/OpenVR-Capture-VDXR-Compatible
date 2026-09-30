#pragma once

#ifndef OPENXR_PLATFORM_H_
#define OPENXR_PLATFORM_H_

#include <openxr/openxr.h>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

#define XR_KHR_D3D11_ENABLE_EXTENSION_NAME "XR_KHR_D3D11_enable"
#define XR_KHR_D3D12_ENABLE_EXTENSION_NAME "XR_KHR_D3D12_enable"

// Direct3D 11 Binding
typedef struct XrGraphicsBindingD3D11KHR {
    XrStructureType type;
    const void* next;
    ID3D11Device* device;
} XrGraphicsBindingD3D11KHR;

// Direct3D 11 Swapchain Image
typedef struct XrSwapchainImageD3D11KHR {
    XrStructureType type;
    void* next;
    ID3D11Texture2D* texture;
} XrSwapchainImageD3D11KHR;

// Direct3D 12 Binding
typedef struct XrGraphicsBindingD3D12KHR {
    XrStructureType type;
    const void* next;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
} XrGraphicsBindingD3D12KHR;

// Direct3D 12 Swapchain Image
typedef struct XrSwapchainImageD3D12KHR {
    XrStructureType type;
    void* next;
    ID3D12Resource* texture;
} XrSwapchainImageD3D12KHR;

#endif // _WIN32

#endif // OPENXR_PLATFORM_H_
