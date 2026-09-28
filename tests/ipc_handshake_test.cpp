// Exercises the OBS <-> OpenXR-layer shared-memory handshake without OBS or a
// headset: the real LayerIpcProducer (on a WARP D3D11 device) and the real
// OpenXrIpcConsumer run in one process against the named mapping.
//
// obs.dll is delay-loaded; none of the code paths used here call into it.

#include "layer_ipc_producer.h"
#include "openxr_ipc_consumer.h"

#include <cstdio>
#include <vector>
#include <cstdlib>

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

// Reads the header the same way the layer/plugin do, for assertions.
static VRSharedFrameHeader *PeekHeader(HANDLE &hMap)
{
	hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, VR_IPC_SHARED_MEMORY_NAME);
	if (!hMap) return nullptr;
	return reinterpret_cast<VRSharedFrameHeader *>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(VRSharedFrameHeader)));
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
		CHECK(!producer.Initialize(device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM));
	}

	std::printf("2. OBS source created -> OBS owns the mapping\n");
	OpenXrIpcConsumer consumer;
	CHECK(consumer.Initialize());
	{
		HANDLE h = nullptr;
		VRSharedFrameHeader *hdr = PeekHeader(h);
		CHECK(hdr != nullptr);
		if (hdr) {
			CHECK(hdr->magic == VR_IPC_MAGIC);
			CHECK(hdr->version == VR_IPC_VERSION);
			CHECK(hdr->active_backend == VRBackendType::Inactive);
			UnmapViewOfFile(hdr);
		}
		if (h) CloseHandle(h);
	}
	CHECK(!consumer.IsProducerActive());

	std::printf("3. layer attaches (first attempt is not throttled)\n");
	LayerIpcProducer producer;
	CHECK(producer.TryAttach());
	CHECK(consumer.IsProducerActive());
	CHECK(!producer.IsObsConnected()); // source not shown yet

	std::printf("4. source shown -> producer sees OBS and publishes a texture\n");
	consumer.SetConnected(true);
	CHECK(producer.IsObsConnected());
	CHECK(producer.Initialize(device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM));
	CHECK(producer.HasTexture(64, 32, DXGI_FORMAT_R8G8B8A8_UNORM));
	uint64_t sharedHandle = 0;
	{
		HANDLE h = nullptr;
		VRSharedFrameHeader *hdr = PeekHeader(h);
		if (hdr) {
			sharedHandle = hdr->shared_handle;
			CHECK(hdr->shared_handle != 0);
			CHECK(hdr->texture_width == 64 && hdr->texture_height == 32);
			CHECK((hdr->seq & 1) == 0);
			UnmapViewOfFile(hdr);
		}
		if (h) CloseHandle(h);
	}

	std::printf("5. CopyFrame + keyed-mutex handoff (OBS side simulated on a 2nd device)\n");
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

	const D3D11_BOX box = {32, 16, 0, 96, 48, 1}; // 64x32 region
	CHECK(producer.CopyFrame(ctx.Get(), src.Get(), 0, box, 0, nullptr, nullptr, nullptr));
	ctx->Flush();
	CHECK(!producer.CopyFrame(ctx.Get(), src.Get(), 0, box, 0, nullptr, nullptr, nullptr)); // OBS still holds key

	const D3D11_BOX wrongSize = {0, 0, 0, 80, 32, 1};

	{
		// OBS: open the shared texture on its own device, take key 1, copy out, give key 0 back.
		ComPtr<ID3D11Device> obsDevice;
		ComPtr<ID3D11DeviceContext> obsCtx;
		D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				  obsDevice.GetAddressOf(), nullptr, obsCtx.GetAddressOf());
		ComPtr<ID3D11Texture2D> shared;
		HRESULT ohr = obsDevice ? obsDevice->OpenSharedResource(reinterpret_cast<HANDLE>(sharedHandle),
									  IID_PPV_ARGS(shared.GetAddressOf()))
					: E_FAIL;
		CHECK(SUCCEEDED(ohr));
		ComPtr<IDXGIKeyedMutex> km;
		if (shared) shared.As(&km);
		CHECK(km != nullptr);
		if (km) {
			CHECK(km->AcquireSync(1, 1000) == S_OK);

			D3D11_TEXTURE2D_DESC stDesc = {};
			shared->GetDesc(&stDesc);
			stDesc.Usage = D3D11_USAGE_STAGING;
			stDesc.BindFlags = 0;
			stDesc.MiscFlags = 0;
			stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> staging;
			obsDevice->CreateTexture2D(&stDesc, nullptr, staging.GetAddressOf());
			obsCtx->CopyResource(staging.Get(), shared.Get());
			km->ReleaseSync(0);

			D3D11_MAPPED_SUBRESOURCE m = {};
			bool contentOk = SUCCEEDED(obsCtx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
			for (UINT y = 0; contentOk && y < 32; ++y) {
				const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
				for (UINT x = 0; x < 64; ++x) {
					if (row[x] != (0xFF000000u | ((y + 16) << 8) | (x + 32))) {
						contentOk = false;
						break;
					}
				}
			}
			if (m.pData) obsCtx->Unmap(staging.Get(), 0);
			CHECK(contentOk); // the crop box landed at (0,0) of the shared texture
		}
	}

	CHECK(producer.CopyFrame(ctx.Get(), src.Get(), 0, box, 0, nullptr, nullptr, nullptr)); // key returned
	CHECK(!producer.CopyFrame(ctx.Get(), src.Get(), 0, wrongSize, 0, nullptr, nullptr, nullptr)); // resize pending
	{
		HANDLE h = nullptr;
		VRSharedFrameHeader *hdr = PeekHeader(h);
		if (hdr) {
			CHECK(hdr->frame_index == 2);
			CHECK((hdr->seq & 1) == 0);
			UnmapViewOfFile(hdr);
		}
		if (h) CloseHandle(h);
	}

	std::printf("6. source hidden -> producer stops\n");
	consumer.SetConnected(false);
	CHECK(!producer.IsObsConnected());

	std::printf("7. game session ends -> OBS sees producer inactive\n");
	producer.Shutdown();
	CHECK(!consumer.IsProducerActive());

	std::printf("8. new session re-attaches immediately after Shutdown\n");
	CHECK(producer.TryAttach());
	CHECK(consumer.IsProducerActive());
	producer.Shutdown();

	std::printf("9. OBS closed -> mapping gone, layer can't attach\n");
	consumer.Shutdown();
	{
		LayerIpcProducer late;
		CHECK(!late.TryAttach());
	}

	std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
