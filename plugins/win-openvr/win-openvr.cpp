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
#include <tlhelp32.h>
#include <obs-module.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>
#include <wrl/client.h>

#include "openxr_ipc_consumer.h"
#include "obs_draw_util.h"
#include "crop_math.h"

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

static constexpr std::chrono::milliseconds retry_delay{8};          // per-call debounce, ~120Hz
static constexpr std::chrono::milliseconds retry_backoff_base{500}; // base retry cadence, 2Hz
static constexpr std::chrono::milliseconds retry_backoff_max{4000}; // maximum backoff limit

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
	// Process-wide consumer shared by all sources; this source is one client.
	std::shared_ptr<vrcapture::OpenXrIpcConsumer> openxr_consumer;
	uint32_t openxr_client = 0;

	// Settings
	bool righteye = true;
	double active_aspect_ratio = 16.0 / 9.0;
	int custom_width = 16;
	int custom_height = 9;
	bool ar_crop = false;

	uint32_t lastFrame = 0;

	// OpenVR resources. The mirror texture is opened on OBS's own D3D11
	// device, and each new compositor frame is copied into `texture`, an
	// OBS-owned texture of the same size, with OBS's immediate context -- no
	// second device, no cross-device sharing, no Flush().
	gs_texture_t *texture = nullptr;
	ComPtr<ID3D11Resource> tex = nullptr;
	// Deliberately a raw pointer, NOT a ComPtr: openvr.h states the mirror SRV
	// must be handed back via ReleaseMirrorTextureD3D11 "instead of calling
	// Release on the resource itself". A ComPtr would call Release() on
	// Reset()/destruction and corrupt the compositor's tracking of it.
	ID3D11ShaderResourceView *mirrorSrv = nullptr;
	bool logged_mirror_format = false;

	// Texture dimensions and crop rectangle
	unsigned int device_width = 0;
	unsigned int device_height = 0;
	DXGI_FORMAT mirror_format = DXGI_FORMAT_UNKNOWN;

	unsigned int x = 0;
	unsigned int y = 0;
	unsigned int width = 100;
	unsigned int height = 100;

	double scale_factor = 1.0;

	// Where each engine's eye image is centered on the view direction; the
	// crop is centered there (see crop_math.h).
	vrcapture::OpticalCenter openvr_center;
	vrcapture::OpticalCenter openxr_center;
	int x_offset = 0;
	int y_offset = 0;

	std::atomic<bool> initialized{false};
	std::atomic<bool> active{true};
	// True while this source holds one s_openvr_instances reference.
	std::atomic<bool> vr_initialized{false};

	// s_openvr_generation of the runtime this source's reference belongs to
	uint64_t init_generation = 0;
	uint64_t mirror_generation = 0;

	// Last values written to the OBS log, so only changes are logged.
	int logged_engine = -1;
	bool logged_openxr_active = false;
	bool logged_openxr_frame = false;

	// Per-instance throttle/re-entrancy state
	std::atomic<bool> init_inprog{false};
	// Set by show(): next tick retries immediately instead of waiting out the backoff.
	std::atomic<bool> init_requested{false};
	std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point last_backoff_attempt = std::chrono::steady_clock::now();
	std::chrono::milliseconds retry_backoff_current{retry_backoff_base};
};

// Helper to destroy OBS texture safely inside graphics context
// Sources locked to SteamVR never touch the OpenXR IPC, so they don't create
// the shared mapping or advertise OBS as a consumer to OpenXR games.
static bool uses_openxr_ipc(const win_openvr *context)
{
	return context->openxr_consumer && context->engine_mode != CaptureEngineMode::OpenVR_SteamVR;
}

static vrcapture::VREyeSelection openxr_eye(const win_openvr *context)
{
	return context->righteye ? vrcapture::VREyeSelection::Right : vrcapture::VREyeSelection::Left;
}

// Tells the shared consumer whether this source currently wants OpenXR frames
// and for which eye. The consumer advertises the union over all sources.
static void publish_openxr_interest(win_openvr *context)
{
	if (context->openxr_consumer) {
		context->openxr_consumer->SetInterest(context->openxr_client,
						      context->active.load() && uses_openxr_ipc(context),
						      openxr_eye(context));
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

	// The horizontal offset is mirrored for the left eye, so one offset value
	// moves both eyes of a left/right source pair symmetrically.
	const int64_t x_offset = context->righteye ? context->x_offset : -static_cast<int64_t>(context->x_offset);
	const vrcapture::OpticalCenter center = context->active_engine == CaptureEngineMode::OpenXR_VDXR
							? context->openxr_center
							: context->openvr_center;
	const vrcapture::CropRect r = vrcapture::ComputeCrop(context->device_width, context->device_height,
							     context->scale_factor,
							     context->ar_crop ? context->active_aspect_ratio : -1.0, center,
							     x_offset, context->y_offset);
	context->x = r.x;
	context->y = r.y;
	context->width = r.width;
	context->height = r.height;
}

// Lock order everywhere is: OBS graphics lock, then context->mutex. render()
// is entered by OBS with the graphics lock held, so any other path that needs
// both must take them in the same order.
struct GraphicsGuard {
	GraphicsGuard() { obs_enter_graphics(); }
	~GraphicsGuard() { obs_leave_graphics(); }
	GraphicsGuard(const GraphicsGuard &) = delete;
	GraphicsGuard &operator=(const GraphicsGuard &) = delete;
};

// VR_Init runs on OBS's graphics thread. While SteamVR is down it would fail
// anyway, but only after loading vrclient and probing for the server; checking
// for the server process first keeps the idle retry loop to a cheap snapshot.
static bool steamvr_server_running()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return true; // can't tell; let VR_Init decide
	}
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
		if (_wcsicmp(entry.szExeFile, L"vrserver.exe") == 0) {
			found = true;
			break;
		}
	}
	CloseHandle(snap);
	return found;
}

// Makes context->texture an OBS-owned texture matching the mirror's size and
// format family. Caller holds the graphics lock and context->mutex.
static bool ensure_mirror_copy_locked(win_openvr *context, const D3D11_TEXTURE2D_DESC &desc)
{
	const gs_color_format format = vrcapture::GsFormatForSrgbContent(desc.Format);
	if (format == GS_UNKNOWN) {
		if (!context->logged_mirror_format) {
			warn("SteamVR mirror texture format %d is not supported", static_cast<int>(desc.Format));
			context->logged_mirror_format = true;
		}
		return false;
	}

	if (context->texture && gs_texture_get_width(context->texture) == desc.Width &&
	    gs_texture_get_height(context->texture) == desc.Height &&
	    gs_texture_get_color_format(context->texture) == format) {
		return true;
	}

	if (context->texture) {
		gs_texture_destroy(context->texture);
	}
	context->texture = gs_texture_create(desc.Width, desc.Height, format, 1, nullptr, 0);
	context->lastFrame = 0; // force a copy into the new texture
	if (!context->texture) {
		warn("gs_texture_create(%ux%u) for the SteamVR mirror copy failed", desc.Width, desc.Height);
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
		// A failed forced refresh (game switched/exited) must not leave the
		// source "initialized": render would keep drawing a frozen frame and
		// tick would retry the forced path, bypassing backoff, every frame.
		// Dropping it hands retries to the normal backoff path in tick().
		context->initialized.store(false);
		context->retry_backoff_current = std::min(context->retry_backoff_current * 2, retry_backoff_max);
		context->init_inprog.store(false);
	};

	GraphicsGuard graphics;
	std::lock_guard<std::mutex> lock(context->mutex);

	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
		static std::atomic<bool> logged{false};
		if (!logged.exchange(true)) {
			warn("SteamVR capture requires OBS's Direct3D 11 renderer");
		}
		on_failure();
		return;
	}

	{
		std::lock_guard<std::mutex> global_lock(s_openvr_init_mutex);
		const uint64_t generation = s_openvr_generation.load();
		if (vr::VRSystem() == nullptr) {
			if (!steamvr_server_running()) {
				on_failure();
				return;
			}
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

	release_mirror_texture_locked(context);

	if (!vr::VRCompositor()) {
		on_failure();
		return;
	}

	// Open the mirror directly on OBS's device, so the per-frame copy and the
	// draw happen on one device with no cross-device synchronization.
	auto *obs_device = static_cast<ID3D11Device *>(gs_get_device_obj());
	vr::EVRCompositorError composError = vr::VRCompositor()->GetMirrorTextureD3D11(
		context->righteye ? vr::Eye_Right : vr::Eye_Left, obs_device, reinterpret_cast<void **>(&context->mirrorSrv));

	if (composError != vr::VRCompositorError_None || !context->mirrorSrv) {
		context->mirrorSrv = nullptr;
		on_failure();
		return;
	}

	context->mirrorSrv->GetResource(context->tex.GetAddressOf());
	ComPtr<ID3D11Texture2D> tex2D;
	if (!context->tex || FAILED(context->tex.As(&tex2D))) {
		warn("win_openvr_init: mirror texture is not a Texture2D");
		on_failure();
		return;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	tex2D->GetDesc(&desc);
	context->device_width = desc.Width;
	context->device_height = desc.Height;
	context->mirror_format = desc.Format;

	// OpenVR's raw projection uses a y-down convention (top < 0 < bottom);
	// convert to up/down tangents.
	float left = 0, right = 0, top = 0, bottom = 0;
	vr::VRSystem()->GetProjectionRaw(context->righteye ? vr::Eye_Right : vr::Eye_Left, &left, &right, &top, &bottom);
	context->openvr_center = vrcapture::OpticalCenterFromTangents(left, right, -top, -bottom);

	if (!ensure_mirror_copy_locked(context, desc)) {
		on_failure();
		return;
	}
	recalculate_crop_dimensions_locked(context);

	context->lastFrame = 0;
	context->mirror_generation = s_openvr_mirror_generation.load();
	context->initialized.store(true);
	context->init_inprog.store(false);
	context->retry_backoff_current = retry_backoff_base; // Reset backoff on success
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
		if (now - context->last_backoff_attempt < context->retry_backoff_current) {
			return;
		}
	}
	context->last_backoff_attempt = std::chrono::steady_clock::now();

	win_openvr_init(data, forced);
}

static void win_openvr_deinit(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	GraphicsGuard graphics;
	std::lock_guard<std::mutex> lock(context->mutex);

	if (context->texture) {
		gs_texture_destroy(context->texture);
		context->texture = nullptr;
	}
	// Must precede VR_Shutdown below -- the compositor still has to be alive to
	// take the mirror texture back.
	release_mirror_texture_locked(context);

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
	context->last_backoff_attempt = std::chrono::steady_clock::now();
}

static const char *win_openvr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OpenVR");
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

	// Both engines draw a sub-rectangle of a full-size texture, so a settings
	// change only moves the rectangle -- no texture has to be rebuilt.
	recalculate_crop_dimensions_locked(context);

	if (context->initialized.load() && context->active_engine == CaptureEngineMode::OpenVR_SteamVR &&
	    old_righteye != context->righteye) {
		// Eye changed: the mirror texture must be re-queried for the other
		// eye (win_openvr_init hands the old one back first).
		needs_reinit = true;
	}
	} // context->mutex released here

	if (context->engine_mode == CaptureEngineMode::OpenXR_VDXR) {
		// Switched to OpenXR-only: release the SteamVR mirror, its copy
		// texture and the runtime reference instead of keeping them alive unused.
		if (context->vr_initialized.load() || context->initialized.load()) {
			win_openvr_deinit(data);
		}
	} else if (needs_reinit) {
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
		return context->width > 0 ? context->width : context->openxr_consumer->GetWidth(openxr_eye(context));
	}
	return context->width > 0 ? context->width : 100;
}

static uint32_t win_openvr_getheight(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return 100;
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer) {
		return context->height > 0 ? context->height : context->openxr_consumer->GetHeight(openxr_eye(context));
	}
	return context->height > 0 ? context->height : 100;
}

static void win_openvr_show(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;
	context->active.store(true);
	publish_openxr_interest(context);

	// Don't initialize here: show() can run on the UI thread. Ask the next
	// tick (graphics thread) to try right away, bypassing the backoff.
	if (context->engine_mode != CaptureEngineMode::OpenXR_VDXR && !context->initialized.load()) {
		context->init_requested.store(true);
	}
}

static void win_openvr_hide(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;
	context->active.store(false); // pause copy/render only
	publish_openxr_interest(context); // other visible sources keep OBS connected
}

static void *win_openvr_create(obs_data_t *settings, obs_source_t *source)
{
	win_openvr *context = new win_openvr();
	context->source = source;
	context->openxr_consumer = vrcapture::OpenXrIpcConsumer::Acquire();
	context->openxr_client = context->openxr_consumer->AddClient();

	win_openvr_update(context, settings);
	return context;
}

static void win_openvr_destroy(void *data)
{
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	if (context->openxr_consumer) {
		// Drops this source's interest; the shared mapping is released when
		// the last source goes away.
		context->openxr_consumer->RemoveClient(context->openxr_client);
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
	    context->openxr_consumer->GetTexture(openxr_eye(context))) {
		uint32_t x, y, cx, cy;
		{
			std::lock_guard<std::mutex> lock(context->mutex);
			x = context->x;
			y = context->y;
			cx = context->width;
			cy = context->height;
		}
		if (context->openxr_consumer->Render(openxr_eye(context), effect, x, y, cx, cy)) {
			return;
		}
	}

	// Backend 2: OpenVR / SteamVR Render Path. Never in forced-OpenXR mode,
	// where a leftover SteamVR mirror would otherwise show through whenever no
	// OpenXR frame is available.
	if (context->engine_mode == CaptureEngineMode::OpenXR_VDXR) {
		return;
	}
	// Initialization is tick()'s job; render only draws.
	if (!context->initialized.load()) {
		return;
	}

	uint32_t x, y, cx, cy;
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

		ComPtr<ID3D11Texture2D> mirror;
		if (!compositor || !context->tex || FAILED(context->tex.As(&mirror))) {
			return;
		}

		// Follow mirror size/format changes (e.g. render resolution changed).
		D3D11_TEXTURE2D_DESC desc = {};
		mirror->GetDesc(&desc);
		if (desc.Width != context->device_width || desc.Height != context->device_height ||
		    desc.Format != context->mirror_format) {
			context->device_width = desc.Width;
			context->device_height = desc.Height;
			context->mirror_format = desc.Format;
			recalculate_crop_dimensions_locked(context);
		}
		if (!ensure_mirror_copy_locked(context, desc)) {
			return;
		}

		vr::Compositor_FrameTiming frameTiming = {};
		frameTiming.m_nSize = sizeof(vr::Compositor_FrameTiming);
		if (compositor->GetFrameTiming(&frameTiming, 0) && frameTiming.m_nFrameIndex != context->lastFrame) {
			// Same device as the mirror, so this is an ordinary GPU copy on
			// OBS's own context (we're on the graphics thread, inside OBS's
			// graphics lock); it's ordered before the draw below.
			auto *dst = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(context->texture));
			ComPtr<ID3D11DeviceContext> obs_context;
			static_cast<ID3D11Device *>(gs_get_device_obj())->GetImmediateContext(obs_context.GetAddressOf());
			if (dst && obs_context) {
				obs_context->CopySubresourceRegion(dst, 0, 0, 0, 0, mirror.Get(), 0, nullptr);
				context->lastFrame = frameTiming.m_nFrameIndex;
			}
		}

		x = context->x;
		y = context->y;
		cx = context->width;
		cy = context->height;
	}

	vrcapture::DrawTextureRegion(effect, context->texture, x, y, cx, cy);
}

static void win_openvr_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	win_openvr *context = (win_openvr *)data;
	if (!context) return;

	context->active.store(obs_source_showing(context->source));
	publish_openxr_interest(context);

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

	if (uses_openxr_ipc(context) && openxr_active != context->logged_openxr_active) {
		info("OpenXR game %s", openxr_active ? "detected (capture layer connected)" : "no longer detected");
		context->logged_openxr_active = openxr_active;
	}
	if (static_cast<int>(context->active_engine) != context->logged_engine) {
		static const char *const names[] = {"Auto", "OpenXR/VDXR", "SteamVR"};
		const int engine = static_cast<int>(context->active_engine);
		info("capture engine: %s (mode setting: %s)", names[engine], names[static_cast<int>(context->engine_mode)]);
		context->logged_engine = engine;
		context->logged_openxr_frame = false;
	}

	// Update OpenXR if active
	if (context->active_engine == CaptureEngineMode::OpenXR_VDXR && context->openxr_consumer && context->active.load()) {
		const vrcapture::VREyeSelection eye = openxr_eye(context);
		const bool haveFrame = context->openxr_consumer->UpdateTexture(eye);
		if (haveFrame && !context->logged_openxr_frame) {
			info("receiving OpenXR frames (%s eye, %ux%u)", context->righteye ? "right" : "left",
			     context->openxr_consumer->GetWidth(eye), context->openxr_consumer->GetHeight(eye));
			context->logged_openxr_frame = true;
		}
		if (haveFrame) {
			const uint32_t w = context->openxr_consumer->GetWidth(eye);
			const uint32_t h = context->openxr_consumer->GetHeight(eye);
			const vrcapture::OpticalCenter center = context->openxr_consumer->GetOpticalCenter(eye);
			std::lock_guard<std::mutex> lock(context->mutex);
			if (context->device_width != w || context->device_height != h ||
			    std::abs(center.x - context->openxr_center.x) > 1e-4 ||
			    std::abs(center.y - context->openxr_center.y) > 1e-4) {
				context->device_width = w;
				context->device_height = h;
				context->openxr_center = center;
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
			win_openvr_init1(data, context->init_requested.exchange(false));
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
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
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
