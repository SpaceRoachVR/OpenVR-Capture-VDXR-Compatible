// Exercises the OBS <-> OpenXR-layer shared-memory protocol without OBS or a
// headset: the real LayerIpcProducer (on a WARP D3D11 device) and the real
// OpenXrIpcConsumer run in one process against the named mapping, and a second
// D3D11 device stands in for OBS's GPU side of the keyed-mutex handoff.
//
// obs.dll is delay-loaded; none of the code paths used here call into it.

#include "layer_ipc_producer.h"
#include "openxr_ipc_consumer.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vrcapture;

static int g_failures = 0;

#define CHECK(cond)                                                             \
	do {                                                                    \
		if (cond) {                                                     \
			std::printf("  ok    %s\n", #cond);                         \
		} else {                                                        \
			std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);    \
			++g_failures;                                           \
		}                                                               \
	} while (0)

// Read-only view of the header, for assertions.
struct HeaderView {
	HANDLE map = nullptr;
	const VRSharedFrameHeader *hdr = nullptr;
	HeaderView()
	{
		map = OpenFileMappingW(FILE_MAP_READ, FALSE, VR_IPC_SHARED_MEMORY_NAME);
		if (map)
			hdr = reinterpret_cast<const VRSharedFrameHeader *>(
				MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(VRSharedFrameHeader)));
	}
	~HeaderView()
	{
		if (hdr) UnmapViewOfFile(hdr);
		if (map) CloseHandle(map);
	}
};

static const uint32_t kLeft = EyeBit(VREyeSelection::Left);
static const uint32_t kRight = EyeBit(VREyeSelection::Right);

// Plays OBS's GPU side for one eye: opens the shared texture on another
// device, takes key 1, reads it back, returns key 0. Checks that pixel (x, y)
// equals the source pattern at (x + ox, y + oy).
static bool ObsReadsEye(uint64_t handle, UINT w, UINT h, UINT ox, UINT oy)
{
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				     dev.GetAddressOf(), nullptr, ctx.GetAddressOf())))
		return false;
	ComPtr<ID3D11Texture2D> shared;
	if (FAILED(dev->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(shared.GetAddressOf()))))
		return false;
	ComPtr<IDXGIKeyedMutex> km;
	if (FAILED(shared.As(&km)) || km->AcquireSync(1, 1000) != S_OK)
		return false;

	D3D11_TEXTURE2D_DESC d = {};
	shared->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.MiscFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	dev->CreateTexture2D(&d, nullptr, staging.GetAddressOf());
	ctx->CopyResource(staging.Get(), shared.Get());
	km->ReleaseSync(0);

	D3D11_MAPPED_SUBRESOURCE m = {};
	bool ok = d.Width == w && d.Height == h && SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
	for (UINT y = 0; ok && y < h; ++y) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
		for (UINT x = 0; x < w; ++x) {
			if (row[x] != (0xFF000000u | ((y + oy) << 8) | (x + ox))) {
				ok = false;
				break;
			}
		}
	}
	if (m.pData) ctx->Unmap(staging.Get(), 0);
	return ok;
}

int main()
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				       device.GetAddressOf(), nullptr, ctx.GetAddressOf());
	if (FAILED(hr)) {
		std::printf("could not create WARP device (0x%08lx)\n", hr);
		return 2;
	}

	std::printf("1. game running, OBS not running\n");
	{
		LayerIpcProducer producer;
		CHECK(!producer.TryAttach());
		CHECK(!producer.IsObsConnected());
		CHECK(!producer.Initialize(VREyeSelection::Right, device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM));
	}

	std::printf("2. OBS sources created: one shared consumer, no mapping until a source wants OpenXR\n");
	std::shared_ptr<OpenXrIpcConsumer> consumer = OpenXrIpcConsumer::Acquire();
	CHECK(OpenXrIpcConsumer::Acquire() == consumer);
	const uint32_t leftSrc = consumer->AddClient();
	const uint32_t rightSrc = consumer->AddClient();
	consumer->SetInterest(leftSrc, false, VREyeSelection::Left);
	consumer->SetInterest(rightSrc, false, VREyeSelection::Right);
	CHECK(HeaderView().hdr == nullptr);
	CHECK(consumer->Initialize()); // what the Auto-mode producer probe does
	{
		HeaderView v;
		CHECK(v.hdr != nullptr);
		if (v.hdr) {
			CHECK(v.hdr->magic == VR_IPC_MAGIC);
			CHECK(v.hdr->version == VR_IPC_VERSION);
			CHECK(v.hdr->active_backend == VRBackendType::Inactive);
			CHECK(v.hdr->obs_connected == 0 && v.hdr->requested_eye_mask == 0);
		}
	}
	CHECK(!consumer->IsProducerActive());

	std::printf("3. layer attaches (first attempt is not throttled)\n");
	LayerIpcProducer producer;
	CHECK(producer.TryAttach());
	CHECK(consumer->IsProducerActive());
	CHECK(!producer.IsObsConnected()); // no source shown yet
	CHECK(producer.GetRequestedEyeMask() == 0);

	std::printf("4. left + right sources shown: producer is asked for both eyes\n");
	consumer->SetInterest(rightSrc, true, VREyeSelection::Right);
	CHECK(producer.IsObsConnected());
	CHECK(producer.GetRequestedEyeMask() == kRight);
	consumer->SetInterest(leftSrc, true, VREyeSelection::Left);
	CHECK(producer.GetRequestedEyeMask() == (kLeft | kRight));

	std::printf("5. per-eye textures, copies and keyed-mutex handoff\n");
	// "Swapchain image": 128x64, pixel (x,y) encodes its own coordinates.
	const UINT srcW = 128, srcH = 64;
	std::vector<uint32_t> pixels(srcW * srcH);
	for (UINT y = 0; y < srcH; ++y)
		for (UINT x = 0; x < srcW; ++x)
			pixels[y * srcW + x] = 0xFF000000u | (y << 8) | x;
	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = srcW;
	sd.Height = srcH;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_DEFAULT;
	sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init = {pixels.data(), srcW * 4, 0};
	ComPtr<ID3D11Texture2D> src;
	CHECK(SUCCEEDED(device->CreateTexture2D(&sd, &init, src.GetAddressOf())));

	// Side-by-side "stereo" image: left view is the left half, right view the right half.
	const D3D11_BOX leftBox = {0, 16, 0, 64, 48, 1};
	const D3D11_BOX rightBox = {64, 16, 0, 128, 48, 1};

	CHECK(!producer.CopyEye(VREyeSelection::Left, ctx.Get(), src.Get(), 0, leftBox, nullptr, nullptr, nullptr)); // no texture yet
	CHECK(producer.Initialize(VREyeSelection::Left, device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM));
	CHECK(producer.Initialize(VREyeSelection::Right, device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM));
	CHECK(producer.HasTexture(VREyeSelection::Left, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM));

	uint64_t leftHandle = 0, rightHandle = 0;
	{
		HeaderView v;
		if (v.hdr) {
			leftHandle = v.hdr->eyes[0].shared_handle;
			rightHandle = v.hdr->eyes[1].shared_handle;
			CHECK(leftHandle != 0 && rightHandle != 0 && leftHandle != rightHandle);
			CHECK(v.hdr->eyes[1].texture_width == 64 && v.hdr->eyes[1].texture_height == 32);
			CHECK((v.hdr->seq & 1) == 0);
		}
	}

	const float fovL[4] = {-1, 0.5f, 1, -1}, fovR[4] = {-0.5f, 1, 1, -1};
	CHECK(producer.CopyEye(VREyeSelection::Left, ctx.Get(), src.Get(), 0, leftBox, fovL, nullptr, nullptr));
	CHECK(producer.CopyEye(VREyeSelection::Right, ctx.Get(), src.Get(), 0, rightBox, fovR, nullptr, nullptr));
	producer.PublishFrame(1234);
	ctx->Flush();
	CHECK(!producer.CopyEye(VREyeSelection::Right, ctx.Get(), src.Get(), 0, rightBox, fovR, nullptr, nullptr)); // OBS holds key

	CHECK(ObsReadsEye(leftHandle, 64, 32, 0, 16));   // left eye got the left view
	CHECK(ObsReadsEye(rightHandle, 64, 32, 64, 16)); // right eye got the right view

	CHECK(producer.CopyEye(VREyeSelection::Right, ctx.Get(), src.Get(), 0, rightBox, fovR, nullptr, nullptr)); // key returned
	const D3D11_BOX wrongSize = {0, 0, 0, 80, 32, 1};
	CHECK(!producer.CopyEye(VREyeSelection::Left, ctx.Get(), src.Get(), 0, wrongSize, fovL, nullptr, nullptr)); // resize pending
	producer.PublishFrame(5678);
	{
		HeaderView v;
		if (v.hdr) {
			CHECK(v.hdr->frame_index == 2);
			CHECK(v.hdr->display_time_ns == 5678);
			CHECK(v.hdr->eyes[0].fov[1] == 0.5f && v.hdr->eyes[1].fov[1] == 1.0f);
			CHECK((v.hdr->seq & 1) == 0);
		}
	}

	std::printf("6. hiding one source keeps the other connected\n");
	consumer->SetInterest(rightSrc, false, VREyeSelection::Right);
	CHECK(producer.IsObsConnected());
	CHECK(producer.GetRequestedEyeMask() == kLeft);

	std::printf("7. switching a source's eye updates the mask\n");
	consumer->SetInterest(leftSrc, true, VREyeSelection::Right);
	CHECK(producer.GetRequestedEyeMask() == kRight);

	std::printf("8. last visible source hidden / removed -> producer stops\n");
	consumer->SetInterest(leftSrc, false, VREyeSelection::Right);
	CHECK(!producer.IsObsConnected());
	CHECK(producer.GetRequestedEyeMask() == 0);
	consumer->SetInterest(leftSrc, true, VREyeSelection::Left);
	CHECK(producer.IsObsConnected());
	consumer->RemoveClient(leftSrc);
	CHECK(!producer.IsObsConnected());

	std::printf("9. game session ends -> OBS sees producer inactive, handles cleared\n");
	producer.Shutdown();
	CHECK(!consumer->IsProducerActive());
	{
		HeaderView v;
		if (v.hdr) CHECK(v.hdr->eyes[0].shared_handle == 0 && v.hdr->eyes[1].shared_handle == 0);
	}

	std::printf("10. new session re-attaches immediately after Shutdown\n");
	CHECK(producer.TryAttach());
	CHECK(consumer->IsProducerActive());
	producer.Shutdown();

	std::printf("11. last OBS source destroyed -> mapping gone, layer can't attach\n");
	consumer->RemoveClient(rightSrc);
	consumer.reset();
	{
		LayerIpcProducer late;
		CHECK(!late.TryAttach());
	}

	std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
