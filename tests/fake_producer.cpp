// Stands in for an OpenXR game so the OBS side can be tested without a headset.
//
// Drives the real LayerIpcProducer exactly the way xrEndFrame does: attaches to
// OBS's mapping, waits for a visible OBS source, then publishes an animated
// test pattern at a configurable rate through CopyFrame.
//
// Usage: fake_producer [fps=72] [width=1920] [height=1920]
//
// What you should see in OBS (VR Capture source, engine Auto or VDXR/OpenXR):
//   - a grid with a white bar sweeping left->right and a frame counter
//     band at the top whose colour changes every frame
//   - no flicker / black frames in preview, program or projectors, even at
//     fps below OBS's frame rate (try 30)
//   - Zoom / aspect / offsets crop the pattern exactly like SteamVR capture

#include "layer_ipc_producer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace vrcapture;
using Clock = std::chrono::steady_clock;

int main(int argc, char **argv)
{
	const int fps = argc > 1 ? std::max(1, atoi(argv[1])) : 72;
	const UINT width = argc > 2 ? static_cast<UINT>(std::max(16, atoi(argv[2]))) : 1920;
	const UINT height = argc > 3 ? static_cast<UINT>(std::max(16, atoi(argv[3]))) : 1920;

	// Hardware device: OBS opens the shared handle on its own GPU device, and
	// legacy shared handles don't cross adapters (so no WARP here).
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				     device.GetAddressOf(), nullptr, ctx.GetAddressOf()))) {
		std::printf("D3D11CreateDevice failed\n");
		return 1;
	}

	// The "swapchain image": pattern is drawn here each frame, then the
	// producer copies it into the shared texture like a real xrEndFrame.
	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = width;
	sd.Height = height;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_DEFAULT;
	sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Texture2D> image;
	if (FAILED(device->CreateTexture2D(&sd, nullptr, image.GetAddressOf()))) {
		std::printf("CreateTexture2D failed\n");
		return 1;
	}

	std::vector<uint32_t> pixels(size_t(width) * height);
	LayerIpcProducer producer;

	std::printf("fake_producer: %ux%u @ %d fps. Waiting for OBS... (Ctrl+C to quit)\n", width, height, fps);

	const auto period = std::chrono::nanoseconds(1000000000LL / fps);
	auto next = Clock::now();
	uint64_t frame = 0, published = 0, dropped = 0;
	auto lastReport = Clock::now();
	bool wasConnected = false;

	for (;;) {
		next += period;
		std::this_thread::sleep_until(next);

		if (!producer.TryAttach() || !producer.IsObsConnected()) {
			if (wasConnected) {
				std::printf("OBS disconnected / source hidden\n");
				wasConnected = false;
			}
			continue;
		}
		if (!wasConnected) {
			std::printf("OBS connected, publishing\n");
			wasConnected = true;
		}

		if (!producer.HasTexture(width, height, sd.Format)) {
			producer.Initialize(device.Get(), width, height, sd.Format);
		}

		// Grid, a sweeping white bar, and a top band whose colour encodes
		// the frame number (so repeated/dropped frames are visible).
		++frame;
		const UINT bar = static_cast<UINT>((frame * 8) % width);
		const uint32_t band = 0xFF000000u | static_cast<uint32_t>((frame * 2654435761u) & 0x00FFFFFFu);
		for (UINT y = 0; y < height; ++y) {
			uint32_t *row = &pixels[size_t(y) * width];
			for (UINT x = 0; x < width; ++x) {
				uint32_t c;
				if (y < height / 16) {
					c = band;
				} else if (x >= bar && x < bar + width / 64) {
					c = 0xFFFFFFFFu;
				} else if ((x / 64 + y / 64) & 1) {
					c = 0xFF404040u;
				} else {
					c = 0xFF202020u | ((x * 255 / width) << 0) | ((y * 255 / height) << 8);
				}
				row[x] = c;
			}
		}
		ctx->UpdateSubresource(image.Get(), 0, nullptr, pixels.data(), width * 4, 0);

		const D3D11_BOX box = {0, 0, 0, width, height, 1};
		if (producer.CopyFrame(ctx.Get(), image.Get(), 0, box, 0, nullptr, nullptr, nullptr)) {
			++published;
		} else {
			++dropped; // OBS hadn't taken the previous frame yet: expected when fps > OBS fps
		}
		ctx->Flush();

		if (Clock::now() - lastReport > std::chrono::seconds(2)) {
			std::printf("frames published %llu, dropped %llu\n", published, dropped);
			lastReport = Clock::now();
		}
	}
}
