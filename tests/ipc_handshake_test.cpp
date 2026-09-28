// Exercises the OBS <-> OpenXR-layer shared-memory handshake without OBS or a
// headset: the real LayerIpcProducer (on a WARP D3D11 device) and the real
// OpenXrIpcConsumer run in one process against the named mapping.
//
// obs.dll is delay-loaded; none of the code paths used here call into it.

#include "layer_ipc_producer.h"
#include "openxr_ipc_consumer.h"

#include <cstdio>
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
	CHECK(producer.GetSharedTexture() != nullptr);
	{
		HANDLE h = nullptr;
		VRSharedFrameHeader *hdr = PeekHeader(h);
		if (hdr) {
			CHECK(hdr->shared_handle != 0);
			CHECK(hdr->texture_width == 64 && hdr->texture_height == 32);
			UnmapViewOfFile(hdr);
		}
		if (h) CloseHandle(h);
	}

	std::printf("5. frame copy protocol advances frame_index\n");
	CHECK(producer.BeginFrameCopy(ctx.Get()));
	producer.EndFrameCopy(0, nullptr, nullptr, nullptr);
	{
		HANDLE h = nullptr;
		VRSharedFrameHeader *hdr = PeekHeader(h);
		if (hdr) {
			CHECK(hdr->frame_index == 1);
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
