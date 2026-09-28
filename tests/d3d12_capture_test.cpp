// Exercises the D3D12 capture path without a headset: a D3D12 "swapchain
// image" (on WARP) is wrapped through the layer's D3D12Interop (D3D11On12),
// copied by the real LayerIpcProducer into an eye's shared texture, and read
// back on an unrelated D3D11 device the way OBS opens it.

#include "d3d12_interop.h"
#include "layer_ipc_producer.h"
#include "openxr_ipc_consumer.h"
#include "test_util.h"

#include <dxgi1_4.h>
#include <vector>

using namespace vrcapture;
using namespace vrcapture_test;

namespace {

struct D3D12Env {
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> fence;
	uint64_t fenceValue = 0;
	HANDLE event = nullptr;

	bool Create()
	{
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter> warp;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) ||
		    FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf()))) ||
		    FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()))))
			return false;
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.GetAddressOf()))) ||
		    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))))
			return false;
		event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return event != nullptr;
	}

	void WaitIdle()
	{
		queue->Signal(fence.Get(), ++fenceValue);
		if (fence->GetCompletedValue() < fenceValue) {
			fence->SetEventOnCompletion(fenceValue, event);
			WaitForSingleObject(event, 5000);
		}
	}

	~D3D12Env()
	{
		if (event) CloseHandle(event);
	}
};

// Creates a W x H R8G8B8A8 texture filled with PatternPixel and left in
// RENDER_TARGET state, like a color swapchain image after xrReleaseSwapchainImage.
ComPtr<ID3D12Resource> CreateSwapchainImage(D3D12Env &env, UINT w, UINT h)
{
	D3D12_HEAP_PROPERTIES defaultHeap = {D3D12_HEAP_TYPE_DEFAULT};
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ComPtr<ID3D12Resource> tex;
	if (FAILED(env.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
						       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
						       IID_PPV_ARGS(tex.GetAddressOf()))))
		return nullptr;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT64 uploadSize = 0;
	env.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &uploadSize);

	D3D12_HEAP_PROPERTIES uploadHeap = {D3D12_HEAP_TYPE_UPLOAD};
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = uploadSize;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> upload;
	if (FAILED(env.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
						       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
						       IID_PPV_ARGS(upload.GetAddressOf()))))
		return nullptr;

	uint8_t *mapped = nullptr;
	upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped));
	for (UINT y = 0; y < h; ++y) {
		auto *row = reinterpret_cast<uint32_t *>(mapped + fp.Offset + y * fp.Footprint.RowPitch);
		for (UINT x = 0; x < w; ++x)
			row[x] = PatternPixel(x, y);
	}
	upload->Unmap(0, nullptr);

	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	env.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.GetAddressOf()));
	env.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
				      IID_PPV_ARGS(list.GetAddressOf()));

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = tex.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = upload.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = fp;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = tex.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	list->ResourceBarrier(1, &barrier);
	list->Close();

	ID3D12CommandList *lists[] = {list.Get()};
	env.queue->ExecuteCommandLists(1, lists);
	env.WaitIdle();
	return tex;
}

} // namespace

int main()
{
	D3D12Env env;
	if (!env.Create()) {
		std::printf("could not create a D3D12 WARP device\n");
		return 2;
	}

	std::printf("1. D3D12 swapchain image + D3D11On12 interop\n");
	ComPtr<ID3D12Resource> image = CreateSwapchainImage(env, 128, 64);
	CHECK(image != nullptr);
	D3D12Interop interop;
	CHECK(interop.Initialize(env.device.Get(), env.queue.Get()));
	ComPtr<ID3D11Texture2D> wrapped = interop.Wrap(image.Get());
	CHECK(wrapped != nullptr);

	std::printf("2. OBS side up, layer attached, both eyes requested\n");
	std::shared_ptr<OpenXrIpcConsumer> consumer = OpenXrIpcConsumer::Acquire();
	const uint32_t client = consumer->AddClient();
	const uint32_t client2 = consumer->AddClient();
	consumer->SetInterest(client, true, VREyeSelection::Left);
	consumer->SetInterest(client2, true, VREyeSelection::Right);
	LayerIpcProducer producer;
	CHECK(producer.TryAttach());
	CHECK(producer.IsObsConnected());

	std::printf("3. shared textures created on the D3D11On12 device\n");
	CHECK(producer.Initialize(VREyeSelection::Left, interop.Device(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, true));
	CHECK(producer.Initialize(VREyeSelection::Right, interop.Device(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, true));
	uint64_t leftHandle = 0, rightHandle = 0;
	{
		HeaderView v;
		if (v.hdr) {
			leftHandle = v.hdr->eyes[0].shared_handle;
			rightHandle = v.hdr->eyes[1].shared_handle;
			CHECK(v.hdr->is_d3d12 == 1);
		}
	}
	CHECK(leftHandle != 0 && rightHandle != 0);

	std::printf("4. copy both views out of the D3D12 image (what xrEndFrame does)\n");
	const D3D11_BOX leftBox = {0, 16, 0, 64, 48, 1};
	const D3D11_BOX rightBox = {64, 16, 0, 128, 48, 1};
	interop.Acquire(wrapped.Get());
	const bool copiedL = producer.CopyEye(VREyeSelection::Left, interop.Context(), wrapped.Get(), 0, leftBox,
					      nullptr, nullptr, nullptr);
	const bool copiedR = producer.CopyEye(VREyeSelection::Right, interop.Context(), wrapped.Get(), 0, rightBox,
					      nullptr, nullptr, nullptr);
	interop.Release(wrapped.Get());
	interop.Flush();
	env.WaitIdle();
	CHECK(copiedL && copiedR);
	producer.PublishFrame(42);

	std::printf("5. OBS reads each eye's pixels\n");
	bool keyedMutex = false;
	CHECK(ObsReadsEye(leftHandle, 64, 32, 0, 16, &keyedMutex));
	CHECK(ObsReadsEye(rightHandle, 64, 32, 64, 16));
	std::printf("  info  keyed mutex on D3D11On12 shared texture: %s\n", keyedMutex ? "yes" : "no (unsynchronized fallback)");

	std::printf("6. second frame still flows\n");
	interop.Acquire(wrapped.Get());
	CHECK(producer.CopyEye(VREyeSelection::Left, interop.Context(), wrapped.Get(), 0, leftBox, nullptr, nullptr,
			       nullptr));
	interop.Release(wrapped.Get());
	interop.Flush();
	env.WaitIdle();
	CHECK(ObsReadsEye(leftHandle, 64, 32, 0, 16));

	producer.Shutdown();
	consumer->RemoveClient(client);
	consumer->RemoveClient(client2);
	consumer.reset();
	wrapped.Reset();
	interop.Reset();
	env.WaitIdle();

	return Finish();
}
