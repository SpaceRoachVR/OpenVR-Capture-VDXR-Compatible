//
// VR Capture (OpenVR & OpenXR / VDXR)
// SpaceRoach Edition
//
// Forked by pigney
// Originally "OpenVR Capture input plugin for OBS" by Keijo "Kegetys" Ruotsalainen
//

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>
#include <wrl/client.h>
#include <cassert>

#include "openxr_ipc_consumer.h"
#include "../../shared/vr_shared_texture.h"

#if __has_include(<openvr.h>)
#include <openvr.h>
#elif __has_include("headers/openvr.h")
#include "headers/openvr.h"
#elif __has_include(<openvr/openvr.h>)
#include <openvr/openvr.h>
#else
#include "openvr.h"
#endif

using Microsoft::WRL::ComPtr;

#if defined(_MSC_VER)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "lib/win64/openvr_api.lib")
#endif

static constexpr std::chrono::milliseconds retry_delay{8};              // per-call debounce, ~120Hz
static constexpr std::chrono::milliseconds retry_delayBUFFER_base{500}; // base retry cadence, 2Hz
static constexpr std::chrono::milliseconds retry_delayBUFFER_max{4000}; // maximum backoff limit

static std::atomic<int> s_openvr_instances{0};
static std::mutex s_openvr_init_mutex;

// Bumped whenever SteamVR signals VREvent_Quit. Every source compares this
// against the generation it initialized under and tears itself down when they
// differ, so ALL sources drop their now-dangling mirror textures -- not just
// whichever one happened to poll the quit event off the queue.
static std::atomic<uint64_t> s_openvr_generation{0};
// s_openvr_generation at the time the current VRSystem() was created.
// Guarded by s_openvr_init_mutex.
static uint64_t s_vr_system_generation = 0;
// Bumped whenever a VR game starts, stops, or changes resolution in SteamVR.
// Sources compare this and re-query their mirror texture from the compositor.
static std::atomic<uint64_t> s_openvr_mirror_generation{0};

#define blog(log_level, message, ...) \
	blog(log_level, "[win_vrcapture] " message, ##__VA_ARGS__)
#define debug(message, ...) \
	blog(LOG_DEBUG, "[%s] " message, (context && context->source) ? obs_source_get_name(context->source) : "win_vrcapture", ##__VA_ARGS__)
#define info(message, ...) \
	blog(LOG_INFO, "[%s] " message, (context && context->source) ? obs_source_get_name(context->source) : "win_vrcapture", ##__VA_ARGS__)
#define warn(message, ...) \
	blog(LOG_WARNING, "[%s] " message, (context && context->source) ? obs_source_get_name(context->source) : "win_vrcapture", ##__VA_ARGS__)

enum class CaptureEngineMode : int {
	Auto = 0,
	OpenXR_VDXR = 1,
	OpenVR_SteamVR = 2
};

struct win_openvr {
	obs_source_t *source = nullptr;
	std::mutex mutex;

	// Engine configuration
	CaptureEngineMode engine_mode = CaptureEngineMode::Auto;
	CaptureEngineMode active_engine = CaptureEngineMode::Auto;

	// OpenXR Consumer Backend
	std::unique_ptr<vrcapture::OpenXrIpcConsumer> openxr_consumer;

	// Settings
	bool righteye = true;
	double active_aspect_ratio = 16.0 / 9.0;
	int custom_width = 16;
	int custom_height = 9;
	bool ar_crop = false;

	uint32_t lastFrame = 0;

	// OpenVR D3D11 Resources
	gs_texture_t *texture = nullptr;
	ComPtr<ID3D11Resource> tex = nullptr;
	// Deliberately a raw pointer, NOT a ComPtr: openvr.h states the mirror SRV
	// must be handed back via ReleaseMirrorTextureD3D11 "instead of calling
	// Release on the resource itself". A ComPtr would call Release() on
	// Reset()/destruction and corrupt the compositor's tracking of it.
	ID3D11ShaderResourceView *mirrorSrv = nullptr;
	ComPtr<ID3D11Device> shared_device = nullptr;
	ComPtr<ID3D11DeviceContext> shared_context = nullptr;

	ComPtr<ID3D11Texture2D> texCrop = nullptr;

	// Texture dimensions and crop rectangle
	unsigned int device_width = 0;
	unsigned int device_height = 0;
	DXGI_FORMAT mirror_format = DXGI_FORMAT_UNKNOWN;

	unsigned int x = 0;
	unsigned int y = 0;
	unsigned int width = 100;
	unsigned int height = 100;

	double scale_factor = 1.0;
	int x_offset = 0;
	int y_offset = 0;

	std::atomic<bool> initialized{false};
	std::atomic<bool> active{true};
	// True while this source holds one s_openvr_instances reference.
	std::atomic<bool> vr_initialized{false};

	// s_openvr_generation of the runtime this source's reference belongs to
	uint64_t init_generation = 0;
	uint64_t mirror_generation = 0;

	// Per-instance throttle/re-entrancy state
	std::atomic<bool> init_inprog{false};
	std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point last_init_timeBUFFER = std::chrono::steady_clock::now();
	std::chrono::milliseconds retry_delayBUFFER_current{retry_delayBUFFER_base};
};

// Helper to destroy OBS texture safely inside graphics context
// Sources locked to SteamVR never touch the OpenXR IPC, so they don't create
// the shared mapping or advertise OBS as a consumer to OpenXR games.
static bool uses_openxr_ipc(const win_openvr *context)
{
	return context->openxr_consumer && context->engine_mode != CaptureEngineMode::OpenVR_SteamVR;
}

static void destroy_obs_texture(gs_texture_t **texture) {
	if (texture && *texture) {
		obs_enter_graphics();
		gs_texture_destroy(*texture);
		obs_leave_graphics();
		*texture = nullptr;
	}
}

// Forward declarations
static void win_openvr_init(void *data, bool forced = false);
static void win_openvr_init1(void *data, bool forced = false);
static void win_openvr_deinit(void *data);

// Hands the mirror SRV back to the compositor. MUST run while the compositor is
// still alive (i.e. before VR_Shutdown) -- releasing it afterwards, or calling
// Release() on it directly, tears down a surface SteamVR still tracks and
// crashes OBS when SteamVR exits. Caller must hold context->mutex.
static void release_mirror_texture_locked(win_openvr *context)
{
	if (context->mirrorSrv) {
		if (vr::VRCompositor()) {
			vr::VRCompositor()->ReleaseMirrorTextureD3D11(context->mirrorSrv);
		}
		// If the compositor is already gone there is nothing safe to call: the
		// surface died with it, so drop the pointer without touching it.
		context->mirrorSrv = nullptr;
	}
	context->tex.Reset();
}

static void recalculate_crop_dimensions_locked(win_openvr *context)
{
	if (context->device_width == 0 || context->device_height == 0) {
		context->width = 100;
		context->height = 100;
		context->x = 0;
		context->y = 0;
		return;
	}

	double scale_factor = context->scale_factor < 1.0 ? 1.0 : context->scale_factor;
	unsigned int scaled_width = static_cast<unsigned int>(static_cast<double>(context->device_width) / scale_factor);
	unsigned int scaled_height = static_cast<unsigned int>(static_cast<double>(context->device_height) / scale_factor);
	scaled_width = std::clamp(scaled_width, 1u, context->device_width);
	scaled_height = std::clamp(scaled_height, 1u, context->device_height);

	context->width = scaled_width;
	context->height = scaled_height;

	if (context->ar_crop && context->active_aspect_ratio > 0.0) {
		double input_aspect_ratio = static_cast<double>(context->width) / static_cast<double>(context->height);
		double target_aspect_ratio = context->active_aspect_ratio;
		if (input_aspect_ratio > target_aspect_ratio) {
			context->width = static_cast<unsigned int>(static_cast<double>(context->height) * target_aspect_ratio);
		} else if (input_aspect_ratio < target_aspect_ratio) {
			context->height = static_cast<unsigned int>(static_cast<double>(context->width) / target_aspect_ratio);
		}
	}

	int64_t device_w = context->device_width;
	int64_t device_h = context->device_height;
	int64_t w = std::clamp<int64_t>(context->width, 1, device_w);
	int64_t h = std::clamp<int64_t>(context->height, 1, device_h);
	context->width = static_cast<unsigned int>(w);
	context->height = static_cast<unsigned int>(h);

	int64_t x = 0, y = 0;
	int64_t x_offset = context->x_offset;
	int64_t y_offset = context->y_offset;
	if (!context->righteye) {
		x_offset = -x_offset;
		x = device_w - w;
	}
	x += x_offset;
	y += y_offset;

	x = std::clamp<int64_t>(x, 0, std::max<int64_t>(0, device_w - w));
	y = std::clamp<int64_t>(y, 0, std::max<int64_t>(0, device_h - h));

	context->x = static_cast<unsigned int>(x);
	context->y = static_cast<unsigned int>(y);
}

static bool recreate_crop_texture_locked(win_openvr *context)
{
	if (!context->shared_device) {
		return false;
	}

	recalculate_crop_dimensions_locked(context);

	context->texCrop.Reset();

	HANDLE handle = nullptr;
	ComPtr<IDXGIKeyedMutex> unusedMutex;
	if (!vrcapture::CreateSharedD3D11Texture(context->shared_device.Get(), context->width, context->height,
						  context->mirror_format, context->texCrop, unusedMutex, handle, false /* useKeyedMutex = false */)) {
		warn("recreate_crop_texture: CreateSharedD3D11Texture failed");
		return false;
	}

	assert((reinterpret_cast<uintptr_t>(handle) >> 32) == 0 && "Win32 HANDLE unexpectedly exceeds 32 bits");
	uint32_t GShandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
	destroy_obs_texture(&context->texture);

	obs_enter_graphics();
	context->texture = gs_texture_open_shared(GShandle);
	obs_leave_graphics();

	if (!context->texture) {
		warn("recreate_crop_texture: gs_texture_open_shared failed");
		context->texCrop.Reset();
		return false;
	}

	return true;
}

static void win_openvr_init(void *data, bool forced)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	if ((!forced && context->initialized.load()) || context->init_inprog.exchange(true)) {
		return;
	}

	if (!forced) {
		auto now = std::chrono::steady_clock::now();
		if (now - context->last_init_time < retry_delay) {
			context->init_inprog.store(false);
			return;
		}
	}
	context->last_init_time = std::chrono::steady_clock::now();

	// Runs with context->mutex held (see lock below). Any partially-acquired
	// mirror texture must go back to the compositor here, otherwise a failed
	// init leaves a dangling SRV that crashes on the next SteamVR shutdown.
	auto on_failure = [context]() {
		release_mirror_texture_locked(context);
		context->retry_delayBUFFER_current = std::min(context->retry_delayBUFFER_current * 2, retry_delayBUFFER_max);
		context->init_inprog.store(false);
	};

	std::lock_guard<std::mutex> lock(context->mutex);

	{
		std::lock_guard<std::mutex> global_lock(s_openvr_init_mutex);
		const uint64_t generation = s_openvr_generation.load();
		if (vr::VRSystem() == nullptr) {
			vr::EVRInitError err = vr::VRInitError_None;
			vr::VR_Init(&err, vr::VRApplication_Background);
			if (err != vr::VRInitError_None) {
				on_failure();
				return;
			}
			s_vr_system_generation = generation;
		} else if (s_vr_system_generation != generation) {
			// VRSystem() is left over from a SteamVR that has since quit: some
			// source still holds a reference and hasn't torn down yet (it will
			// on its next tick). Joining it would leak a reference to a dead
			// runtime and block VR_Shutdown forever, so retry later instead.
			on_failure();
			return;
		}
		if (!context->vr_initialized) {
			context->vr_initialized = true;
			// Tie this reference to the runtime it was taken on, so tick()
			// releases it when SteamVR quits even if the rest of init failed.
			context->init_generation = generation;
			s_openvr_instances++;
		}
	}

	if (!context->shared_device) {
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, context->shared_device.GetAddressOf(), nullptr, context->shared_context.GetAddressOf());
		if (FAILED(hr)) {
			warn("win_openvr_init: SHARED D3D11CreateDevice failed");
			on_failure();
			return;
		}
	}

	context->texCrop.Reset();
	release_mirror_texture_locked(context);

	if (!vr::VRCompositor()) {
		on_failure();
		return;
	}

	vr::EVRCompositorError composError = vr::VRCompositor()->GetMirrorTextureD3D11(context->righteye ? vr::Eye_Right : vr::Eye_Left, context->shared_device.Get(), reinterpret_cast<void**>(&context->mirrorSrv));

	if (composError != vr::VRCompositorError_None || !context->mirrorSrv) {
		context->mirrorSrv = nullptr;
		on_failure();
		return;
	}

	context->mirrorSrv->GetResource(context->tex.GetAddressOf());
	if (!context->tex) {
		warn("win_openvr_init: mirrorSrv->GetResource failed");
		on_failure();
		return;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	ComPtr<ID3D11Texture2D> tex2D = nullptr;
	context->tex.As(&tex2D);
	if (!tex2D) {
		warn("win_openvr_init: tex->QueryInterface ID3D11Texture2D failed");
		on_failure();
		return;
	}

	tex2D->GetDesc(&desc);
	context->device_width = desc.Width;
	context->device_height = desc.Height;
	context->mirror_format = desc.Format;
	tex2D.Reset();

	if (!recreate_crop_texture_locked(context)) {
		on_failure();
		return;
	}

	context->lastFrame = 0;
	context->mirror_generation = s_openvr_mirror_generation.load();
	context->initialized.store(true);
	context->init_inprog.store(false);
	context->retry_delayBUFFER_current = retry_delayBUFFER_base; // Reset backoff on success
}

static void win_openvr_init1(void *data, bool forced)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	if ((!forced && context->initialized.load()) || context->init_inprog.load()) {
		return;
	}

	if (!forced) {
		auto now = std::chrono::steady_clock::now();
		if (now - context->last_init_timeBUFFER < context->retry_delayBUFFER_current) {
			return;
		}
	}
	context->last_init_timeBUFFER = std::chrono::steady_clock::now();

	win_openvr_init(data, forced);
}

static void win_openvr_deinit(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	std::lock_guard<std::mutex> lock(context->mutex);

	destroy_obs_texture(&context->texture);
	if (context->texCrop) context->texCrop.Reset();
	// Must precede VR_Shutdown below -- the compositor still has to be alive to
	// take the mirror texture back.
	release_mirror_texture_locked(context);
	if (context->shared_device) context->shared_device.Reset();
	if (context->shared_context) context->shared_context.Reset();

	if (context->vr_initialized) {
		context->vr_initialized = false;
		std::lock_guard<std::mutex> global_lock(s_openvr_init_mutex);
		if (--s_openvr_instances <= 0) {
			s_openvr_instances = 0;
			vr::VR_Shutdown();
		}
	}

	context->initialized.store(false);
	context->init_inprog.store(false);
	context->last_init_time = std::chrono::steady_clock::now();
	context->last_init_timeBUFFER = std::chrono::steady_clock::now();
}

static const char *win_openvr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "VR Capture";
}

static void win_openvr_update(void *data, obs_data_t *settings)
{
	win_openvr *context = (win_openvr *)data;
	if (!context || !settings) return;

	// Set when the eye changed and the mirror texture has to be re-queried.
	// win_openvr_init() takes context->mutex itself, so it must be called after
	// the guard below has gone out of scope -- std::mutex is not recursive and
	// re-locking it here is undefined behavior (in practice, a hung OBS).
	bool needs_reinit = false;

	{
	std::lock_guard<std::mutex> lock(context->mutex);

	bool old_righteye = context->righteye;
	unsigned int old_width = context->width;
	unsigned int old_height = context->height;

	context->engine_mode = static_cast<CaptureEngineMode>(obs_data_get_int(settings, "engine_mode"));
	context->righteye = obs_data_get_bool(settings, "righteye");
	context->scale_factor = obs_data_get_double(settings, "scale_factor");
	context->x_offset = (int)obs_data_get_int(settings, "x_offset");
	context->y_offset = (int)obs_data_get_int(settings, "y_offset");
	context->active_aspect_ratio = obs_data_get_double(settings, "aspect_ratio");
	context->custom_width = (int)obs_data_get_int(settings, "custom_aspect_width");
	context->custom_height = (int)obs_data_get_int(settings, "custom_aspect_height");

	if (context->active_aspect_ratio == -1.0) {
		context->ar_crop = false;
	} else {
		context->ar_crop = true;
		if (context->active_aspect_ratio == 0.0) {
			if (context->custom_width > 0 && context->custom_height > 0) {
				context->active_aspect_ratio = static_cast<double>(context->custom_width) / context->custom_height;
			} else {
				context->active_aspect_ratio = 16.0 / 9.0;
			}
		}
	}

	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR) {
		// OpenXR draws a sub-rectangle of the consumer's texture directly;
		// no crop texture to rebuild, just the rectangle.
		recalculate_crop_dimensions_locked(context);
	} else if (context->initialized.load() && context->active_engine == CaptureEngineMode::OpenVR_SteamVR) {
		if (old_righteye != context->righteye) {
			// Eye changed: mirror texture must be re-queried for the other eye.
			// Hand the old one back before dropping initialized, so the reinit
			// below starts from a clean slate.
			release_mirror_texture_locked(context);
			context->initialized.store(false);
			needs_reinit = true;
		} else {
			recalculate_crop_dimensions_locked(context);
			// Only recreate texture if crop dimensions changed
			if (old_width != context->width || old_height != context->height || !context->texture) {
				recreate_crop_texture_locked(context);
			}
		}
	}
	} // context->mutex released here

	if (needs_reinit) {
		win_openvr_init(data, true);
	}
}

static void win_openvr_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "engine_mode", static_cast<int>(CaptureEngineMode::Auto));
	obs_data_set_default_bool(settings, "righteye", true);
	obs_data_set_default_double(settings, "aspect_ratio", -1.0);
	obs_data_set_default_int(settings, "custom_aspect_width", 16);
	obs_data_set_default_int(settings, "custom_aspect_height", 9);
	obs_data_set_default_double(settings, "scale_factor", 1.0);
	obs_data_set_default_int(settings, "x_offset", 0);
	obs_data_set_default_int(settings, "y_offset", 0);
}

static uint32_t win_openvr_getwidth(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return 100;
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer) {
		return context->width > 0 ? context->width : context->openxr_consumer->GetWidth();
	}
	return context->width > 0 ? context->width : 100;
}

static uint32_t win_openvr_getheight(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return 100;
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer) {
		return context->height > 0 ? context->height : context->openxr_consumer->GetHeight();
	}
	return context->height > 0 ? context->height : 100;
}

static void win_openvr_show(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;
	context->active.store(true);

	if (uses_openxr_ipc(context)) {
		context->openxr_consumer->SetConnected(true);
	}

	if (context->engine_mode != CaptureEngineMode::OpenXR_VDXR && !context->initialized.load()) {
		win_openvr_init1(data, true); // forced
	}
}

static void win_openvr_hide(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;
	context->active.store(false); // pause copy/render only

	if (uses_openxr_ipc(context)) {
		context->openxr_consumer->SetConnected(false);
	}
}

static void *win_openvr_create(obs_data_t *settings, obs_source_t *source)
{
	win_openvr *context = new win_openvr();
	context->source = source;
	context->openxr_consumer = std::make_unique<vrcapture::OpenXrIpcConsumer>();

	win_openvr_update(context, settings);
	return context;
}

static void win_openvr_destroy(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	if (context->openxr_consumer) {
		context->openxr_consumer->Shutdown();
		context->openxr_consumer.reset();
	}

	win_openvr_deinit(data);
	delete context;
}

static void win_openvr_render(void *data, gs_effect_t *effect)
{
	win_openvr *context = (win_openvr *)data;
	if (!context || !context->active.load()) {
		return;
	}

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	// Backend 1: OpenXR / VDXR Render Path. Same crop/zoom/offset rectangle
	// as the OpenVR path; tick() keeps it in sync with the frame size.
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer &&
	    context->openxr_consumer->GetTexture()) {
		uint32_t x, y, cx, cy;
		{
			std::lock_guard<std::mutex> lock(context->mutex);
			x = context->x;
			y = context->y;
			cx = context->width;
			cy = context->height;
		}
		if (context->openxr_consumer->Render(effect, x, y, cx, cy)) {
			return;
		}
	}

	// Backend 2: OpenVR / SteamVR Render Path
	if (!context->initialized.load()) {
		if (context->engine_mode != CaptureEngineMode::OpenXR_VDXR) {
			win_openvr_init1(data);
		}
		if (!context->initialized.load()) {
			return;
		}
	}

	{
		std::lock_guard<std::mutex> lock(context->mutex);

		// Re-check under the lock: a concurrent deinit (SteamVR quit, source
		// removal) may have released the mirror texture and shut the runtime
		// down since the initialized check above. Touching the compositor or
		// context->tex after that is a use-after-free.
		vr::IVRCompositor *compositor =
			(context->initialized.load() && context->init_generation == s_openvr_generation.load())
				? vr::VRCompositor()
				: nullptr;

		// Verify mirror texture dimensions have not changed underneath us
		ComPtr<ID3D11Texture2D> tex2D;
		if (context->tex && SUCCEEDED(context->tex.As(&tex2D))) {
			D3D11_TEXTURE2D_DESC srcDesc = {};
			tex2D->GetDesc(&srcDesc);
			if (srcDesc.Width != context->device_width || srcDesc.Height != context->device_height || srcDesc.Format != context->mirror_format) {
				context->device_width = srcDesc.Width;
				context->device_height = srcDesc.Height;
				context->mirror_format = srcDesc.Format;
				recalculate_crop_dimensions_locked(context);
				recreate_crop_texture_locked(context);
			}
		}

		vr::Compositor_FrameTiming frameTiming = {};
		frameTiming.m_nSize = sizeof(vr::Compositor_FrameTiming);
		if (compositor && compositor->GetFrameTiming(&frameTiming, 0)) {
			if (frameTiming.m_nFrameIndex != context->lastFrame) {
				if (context->texCrop && context->tex && context->shared_context) {
					UINT cropX = (context->x < context->device_width) ? context->x : 0;
					UINT cropY = (context->y < context->device_height) ? context->y : 0;
					UINT cropRight = std::min(cropX + context->width, context->device_width);
					UINT cropBottom = std::min(cropY + context->height, context->device_height);

					if (cropRight > cropX && cropBottom > cropY) {
						D3D11_BOX poksi = {
							cropX,
							cropY,
							0,
							cropRight,
							cropBottom,
							1
						};

						context->shared_context->CopySubresourceRegion(context->texCrop.Get(), 0, 0, 0, 0, context->tex.Get(), 0, &poksi);
						context->shared_context->Flush();
						context->lastFrame = frameTiming.m_nFrameIndex;
					}
				}
			}
		}
	}

	if (context->texture) {
		while (gs_effect_loop(effect, "Draw")) {
			obs_source_draw(context->texture, 0, 0, 0, 0, false);
		}
	}
}

static void win_openvr_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	context->active.store(obs_source_showing(context->source));

	// Dual-Engine Resolution and Auto-Detection
	bool openxr_active = uses_openxr_ipc(context) && context->openxr_consumer->IsProducerActive();

	if (context->engine_mode == CaptureEngineMode::OpenXR_VDXR) {
		context->active_engine = CaptureEngineMode::OpenXR_VDXR;
	} else if (context->engine_mode == CaptureEngineMode::OpenVR_SteamVR) {
		context->active_engine = CaptureEngineMode::OpenVR_SteamVR;
	} else {
		// Auto Mode: Prioritize OpenXR/VDXR if running, otherwise OpenVR
		if (openxr_active) {
			context->active_engine = CaptureEngineMode::OpenXR_VDXR;
		} else {
			context->active_engine = CaptureEngineMode::OpenVR_SteamVR;
		}
	}

	// Update OpenXR if active
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer && context->active.load()) {
		vrcapture::VREyeSelection eye = context->righteye ? vrcapture::VREyeSelection::Right : vrcapture::VREyeSelection::Left;
		if (context->openxr_consumer->UpdateTexture(eye)) {
			const uint32_t w = context->openxr_consumer->GetWidth();
			const uint32_t h = context->openxr_consumer->GetHeight();
			std::lock_guard<std::mutex> lock(context->mutex);
			if (context->device_width != w || context->device_height != h) {
				context->device_width = w;
				context->device_height = h;
				recalculate_crop_dimensions_locked(context);
			}
		}
	}

	// Poll SteamVR events if OpenVR is initialized
	if (vr::VRSystem() != nullptr) {
		vr::VREvent_t e;
		while (vr::VRSystem()->PollNextEvent(&e, sizeof(vr::VREvent_t))) {
			switch (e.eventType) {
			case vr::VREvent_Quit:
				if (e.data.process.pid == 0 || e.data.process.pid == GetCurrentProcessId()) {
					// SteamVR itself is shutting down. Acknowledge so vrserver does not kill us after 5s.
					vr::VRSystem()->AcknowledgeQuit_Exiting();
					s_openvr_generation++;
				} else {
					// Another process (such as a VR game) closed. Refresh the mirror texture.
					s_openvr_mirror_generation++;
				}
				break;
			case vr::VREvent_ProcessQuit:
			case vr::VREvent_SceneApplicationChanged:
			case vr::VREvent_SceneAppPipeDisconnected:
			case vr::VREvent_InvalidateSwapTextureSets:
				// VR scene changed, game exited, or swapchain invalidated. Refresh the mirror texture.
				s_openvr_mirror_generation++;
				break;
			default:
				break;
			}
		}
	}

	// Tear down if SteamVR quit under us (either we saw the event above, or
	// another source did). Must happen before any further use of the mirror
	// texture -- it belongs to a compositor that no longer exists.
	// Keyed on vr_initialized, not initialized: a source whose VR_Init
	// succeeded but whose mirror setup failed still holds a runtime reference,
	// and if it never released it VR_Shutdown would never run and the plugin
	// could not reconnect to a restarted SteamVR without restarting OBS.
	if (context->vr_initialized.load() && context->init_generation != s_openvr_generation.load()) {
		win_openvr_deinit(data);
	} else if (context->initialized.load() && context->mirror_generation != s_openvr_mirror_generation.load()) {
		// Game closed or switched: refresh the mirror texture cleanly without tearing down VRSystem
		win_openvr_init1(data, true);
	}

	// Initialize OpenVR if needed
	if (context->active_engine == CaptureEngineMode::OpenVR_SteamVR) {
		if (!context->initialized.load() && context->active.load()) {
			win_openvr_init1(data);
		}
	}
}

static bool ar_modd(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	if (!props || !settings) return false;

	double aspect_ratio = obs_data_get_double(settings, "aspect_ratio");
	bool custom_active = (aspect_ratio == 0.0);

	obs_property_t *custom_width = obs_properties_get(props, "custom_aspect_width");
	obs_property_t *custom_height = obs_properties_get(props, "custom_aspect_height");

	if (custom_width) obs_property_set_visible(custom_width, custom_active);
	if (custom_height) obs_property_set_visible(custom_height, custom_active);

	return true;
}

static obs_properties_t *win_openvr_properties(void *data)
{
	win_openvr *context = (win_openvr *)data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	// Engine Selector
	p = obs_properties_add_list(props, "engine_mode", obs_module_text("CaptureEngine"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("EngineAuto"), static_cast<int>(CaptureEngineMode::Auto));
	obs_property_list_add_int(p, obs_module_text("EngineOpenXR"), static_cast<int>(CaptureEngineMode::OpenXR_VDXR));
	obs_property_list_add_int(p, obs_module_text("EngineOpenVR"), static_cast<int>(CaptureEngineMode::OpenVR_SteamVR));

	p = obs_properties_add_bool(props, "righteye", obs_module_text("RightEye"));

	// Preset aspect ratios
	p = obs_properties_add_list(props, "aspect_ratio", obs_module_text("AspectRatio"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(p, obs_module_text("Native"), -1.0);
	obs_property_list_add_float(p, "16:9", 16.0 / 9.0);
	obs_property_list_add_float(p, "4:3", 4.0 / 3.0);
	obs_property_list_add_float(p, obs_module_text("Custom"), 0.0);

	obs_property_set_modified_callback(p, ar_modd);

	p = obs_properties_add_int(props, "custom_aspect_width", obs_module_text("RatioWidth"), 1, 100, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, "custom_aspect_height", obs_module_text("RatioHeight"), 1, 100, 1);
	obs_property_set_visible(p, false);

	// Pan and zoom
	p = obs_properties_add_float_slider(props, "scale_factor", obs_module_text("Zoom"), 1.0, 5.0, 0.01);
	p = obs_properties_add_int(props, "x_offset", obs_module_text("HorizontalOffset"), -10000, 10000, 1);
	p = obs_properties_add_int(props, "y_offset", obs_module_text("VerticalOffset"), -10000, 10000, 1);

	if (context && context->source) {
		obs_data_t *settings = obs_source_get_settings(context->source);
		if (settings) {
			ar_modd(props, nullptr, settings);
			obs_data_release(settings);
		}
	}

	return props;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-openvr", "en-US")

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = "openvr_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = win_openvr_get_name;
	info.create = win_openvr_create;
	info.destroy = win_openvr_destroy;
	info.update = win_openvr_update;
	info.get_defaults = win_openvr_defaults;
	info.show = win_openvr_show;
	info.hide = win_openvr_hide;
	info.get_width = win_openvr_getwidth;
	info.get_height = win_openvr_getheight;
	info.video_render = win_openvr_render;
	info.video_tick = win_openvr_tick;
	info.get_properties = win_openvr_properties;
	obs_register_source(&info);
	return true;
}
