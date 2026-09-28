#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <wrl/client.h>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

// Lets the D3D11 capture path read D3D12 swapchain images: a D3D11On12 device
// layered on the app's own D3D12 device and command queue -- the same
// technique OBS's game-capture hook uses for D3D12 games. Swapchain images are
// wrapped as D3D11 textures, so the shared-texture copy and everything on the
// OBS side are identical for D3D11 and D3D12 apps.
//
// D3D11 work recorded on Context() is submitted to the app's queue by Flush(),
// after everything the app has already submitted, so a copy recorded in
// xrEndFrame is ordered after the frame's rendering.
class D3D12Interop {
public:
    bool Initialize(ID3D12Device *device, ID3D12CommandQueue *queue);
    void Reset();

    bool IsValid() const { return m_device11 != nullptr; }
    ID3D11Device *Device() const { return m_device11.Get(); }
    ID3D11DeviceContext *Context() const { return m_context11.Get(); }

    // Wraps a D3D12 color swapchain image. OpenXR requires the app to hand
    // color images back to the runtime in D3D12_RESOURCE_STATE_RENDER_TARGET,
    // which is the state they're in when xrEndFrame copies them.
    ComPtr<ID3D11Texture2D> Wrap(ID3D12Resource *image) const;

    // Bracket D3D11 commands that touch a wrapped resource.
    void Acquire(ID3D11Resource *wrapped) const;
    void Release(ID3D11Resource *wrapped) const;

    // Submits recorded D3D11 work to the app's D3D12 queue.
    void Flush() const;

private:
    ComPtr<ID3D11Device> m_device11;
    ComPtr<ID3D11DeviceContext> m_context11;
    ComPtr<ID3D11On12Device> m_on12;
};

} // namespace vrcapture
