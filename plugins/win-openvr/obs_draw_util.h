#pragma once

// Texture format mapping and sRGB-correct drawing shared by the SteamVR and
// OpenXR render paths. The source registers with OBS_SOURCE_SRGB, so OBS may
// render it into a linear-light target; these helpers pick the right shader
// view per texture so both sRGB-encoded and linear content come out correct.

#include <obs-module.h>
#include <dxgi.h>
#include <algorithm>

namespace vrcapture {

// Formats OBS creates with a typeless resource and an sRGB shader view. Their
// content is treated as sRGB-encoded (decoded when sampled). Everything else
// (*_UNORM, 10-bit, float) is treated as linear.
inline bool GsFormatHasSrgbView(gs_color_format format)
{
	return format == GS_RGBA || format == GS_BGRA || format == GS_BGRX;
}

// gs format for an OBS-owned copy of a texture whose 8-bit content is
// sRGB-encoded regardless of its DXGI view format -- e.g. the SteamVR mirror,
// which is what a user sees in the desktop mirror window. Returns GS_UNKNOWN
// for formats the copy path doesn't support.
inline gs_color_format GsFormatForSrgbContent(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return GS_RGBA;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return GS_BGRA;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return GS_BGRX;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return GS_RGBA16F;
	default:
		return GS_UNKNOWN;
	}
}

// gs format for an OBS-owned copy of an OpenXR swapchain image, honouring
// what the swapchain format says about its content: *_SRGB (and typeless)
// images hold sRGB-encoded values and get a format with an sRGB view; plain
// UNORM and float images hold linear values (per the OpenXR spec) and get a
// linear format. Every result has a typeless/compatible D3D11 resource format,
// so the image can be CopyResource'd into it. Returns GS_UNKNOWN if unsupported.
inline gs_color_format GsFormatForSwapchainContent(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return GS_RGBA_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return GS_BGRA_UNORM;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return GS_BGRX_UNORM;
	default:
		return GsFormatForSrgbContent(format);
	}
}

// Draws the (x, y, cx, cy) sub-rectangle of `tex` at the origin with the
// current effect's "Draw" technique. A 0 extent means "to the edge"; the
// rectangle is clamped to the texture.
inline void DrawTextureRegion(gs_effect_t *effect, gs_texture_t *tex, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
	const uint32_t w = gs_texture_get_width(tex);
	const uint32_t h = gs_texture_get_height(tex);
	if (!effect || w == 0 || h == 0) {
		return;
	}
	x = (std::min)(x, w - 1);
	y = (std::min)(y, h - 1);
	cx = (cx == 0 || x + cx > w) ? w - x : cx;
	cy = (cy == 0 || y + cy > h) ? h - y : cy;

	// Same pattern OBS uses for its own sources: when the target is linear,
	// enable sRGB encoding on output and decode sRGB content on input.
	const bool linear_srgb = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (linear_srgb && GsFormatHasSrgbView(gs_texture_get_color_format(tex))) {
		gs_effect_set_texture_srgb(image, tex);
	} else {
		gs_effect_set_texture(image, tex);
	}
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite_subregion(tex, 0, x, y, cx, cy);
	}

	gs_enable_framebuffer_srgb(previous);
}

} // namespace vrcapture
