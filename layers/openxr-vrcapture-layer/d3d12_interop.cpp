#include "d3d12_interop.h"

namespace vrcapture {

bool D3D12Interop::Initialize(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    Reset();
    if (!device || !queue) {
        return false;
    }

    IUnknown *queues[] = {queue};
    HRESULT hr = D3D11On12CreateDevice(device, 0, nullptr, 0, queues, 1, 0, m_device11.GetAddressOf(),
                                       m_context11.GetAddressOf(), nullptr);
    if (FAILED(hr) || FAILED(m_device11.As(&m_on12))) {
        Reset();
        return false;
    }
    return true;
}

void D3D12Interop::Reset()
{
    if (m_context11) {
        // Submit anything still recorded before the device goes away.
        m_context11->Flush();
    }
    m_on12.Reset();
    m_context11.Reset();
    m_device11.Reset();
}

ComPtr<ID3D11Texture2D> D3D12Interop::Wrap(ID3D12Resource *image) const
{
    ComPtr<ID3D11Texture2D> wrapped;
    if (!m_on12 || !image) {
        return wrapped;
    }

    // Only ever used as a copy source, which needs no bind flags.
    D3D11_RESOURCE_FLAGS flags = {};
    m_on12->CreateWrappedResource(image, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, IID_PPV_ARGS(wrapped.GetAddressOf()));
    return wrapped;
}

void D3D12Interop::Acquire(ID3D11Resource *wrapped) const
{
    if (m_on12 && wrapped) {
        m_on12->AcquireWrappedResources(&wrapped, 1);
    }
}

void D3D12Interop::Release(ID3D11Resource *wrapped) const
{
    if (m_on12 && wrapped) {
        m_on12->ReleaseWrappedResources(&wrapped, 1);
    }
}

void D3D12Interop::Flush() const
{
    if (m_context11) {
        m_context11->Flush();
    }
}

} // namespace vrcapture
