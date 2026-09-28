#pragma once

// Helpers shared by the IPC tests.

#include "../shared/vr_ipc_types.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>

namespace vrcapture_test {

using Microsoft::WRL::ComPtr;

inline int g_failures = 0;

#define CHECK(cond)                                                                       \
	do {                                                                              \
		if (cond) {                                                               \
			std::printf("  ok    %s\n", #cond);                                   \
		} else {                                                                  \
			std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);              \
			++vrcapture_test::g_failures;                                     \
		}                                                                         \
	} while (0)

inline int Finish()
{
	std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}

// Read-only view of the IPC header, for assertions.
struct HeaderView {
	HANDLE map = nullptr;
	const vrcapture::VRSharedFrameHeader *hdr = nullptr;
	HeaderView()
	{
		map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrcapture::VR_IPC_SHARED_MEMORY_NAME);
		if (map)
			hdr = reinterpret_cast<const vrcapture::VRSharedFrameHeader *>(
				MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(vrcapture::VRSharedFrameHeader)));
	}
	~HeaderView()
	{
		if (hdr) UnmapViewOfFile(hdr);
		if (map) CloseHandle(map);
	}
	HeaderView(const HeaderView &) = delete;
	HeaderView &operator=(const HeaderView &) = delete;
};

// Test pattern: pixel (x, y) of the source image encodes its own coordinates.
inline uint32_t PatternPixel(uint32_t x, uint32_t y)
{
	return 0xFF000000u | (y << 8) | x;
}

// Plays OBS's GPU side for one eye: opens the shared texture on a separate
// D3D11 device, takes key 1 if it has a keyed mutex, reads it back, returns
// key 0. Checks that pixel (x, y) equals PatternPixel(x + ox, y + oy).
inline bool ObsReadsEye(uint64_t handle, UINT w, UINT h, UINT ox, UINT oy, bool *hadKeyedMutex = nullptr)
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
	shared.As(&km);
	if (hadKeyedMutex) *hadKeyedMutex = km != nullptr;
	if (km && km->AcquireSync(1, 1000) != S_OK)
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
	if (km) km->ReleaseSync(0);

	D3D11_MAPPED_SUBRESOURCE m = {};
	bool ok = d.Width == w && d.Height == h && SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
	for (UINT y = 0; ok && y < h; ++y) {
		const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
		for (UINT x = 0; x < w; ++x) {
			if (row[x] != PatternPixel(x + ox, y + oy)) {
				ok = false;
				break;
			}
		}
	}
	if (m.pData) ctx->Unmap(staging.Get(), 0);
	return ok;
}

} // namespace vrcapture_test
