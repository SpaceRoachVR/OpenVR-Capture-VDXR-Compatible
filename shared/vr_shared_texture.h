#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace vrcapture {

using Microsoft::WRL::ComPtr;

// Creates a D3D11 texture flagged for cross-process sharing, falling back to
// D3D11_RESOURCE_MISC_SHARED (no keyed mutex) if the driver rejects the
// keyed-mutex variant for this format. Used identically by the OpenXR capture
// layer's producer and the OBS plugin's OpenVR mirror path, which previously
// duplicated this creation/query/handle-extraction sequence.
inline bool CreateSharedD3D11Texture(
    ID3D11Device *device,
    uint32_t width, uint32_t height, DXGI_FORMAT format,
    ComPtr<ID3D11Texture2D> &outTexture,
    ComPtr<IDXGIKeyedMutex> &outKeyedMutex,
    HANDLE &outSharedHandle,
    bool useKeyedMutex = true)
{
    outTexture.Reset();
    outKeyedMutex.Reset();
    outSharedHandle = nullptr;

    if (!device || width == 0 || height == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (format != DXGI_FORMAT_UNKNOWN) ? format : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = useKeyedMutex ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTexture.GetAddressOf());
    if (FAILED(hr) && useKeyedMutex) {
        // Fallback without KEYEDMUTEX if the driver doesn't support it for this format
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = device->CreateTexture2D(&desc, nullptr, outTexture.GetAddressOf());
    }

    if (FAILED(hr) || !outTexture) {
        return false;
    }

    if (useKeyedMutex) {
        // Best-effort: absent on the D3D11_RESOURCE_MISC_SHARED fallback path.
        outTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(outKeyedMutex.GetAddressOf()));
    }

    ComPtr<IDXGIResource> dxgiResource;
    hr = outTexture.As(&dxgiResource);
    if (FAILED(hr) || !dxgiResource) {
        outTexture.Reset();
        outKeyedMutex.Reset();
        return false;
    }

    hr = dxgiResource->GetSharedHandle(&outSharedHandle);
    if (FAILED(hr) || !outSharedHandle) {
        outTexture.Reset();
        outKeyedMutex.Reset();
        outSharedHandle = nullptr;
        return false;
    }

    return true;
}

} // namespace vrcapture
