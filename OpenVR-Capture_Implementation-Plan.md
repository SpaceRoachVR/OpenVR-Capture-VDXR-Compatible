# OpenVR Capture — Implementation Plan

> **Superseded (2026-09-28).** This document describes an earlier ~370-line version of the plugin. Its items are implemented or replaced; see `OpenVR-Capture_Correctness-Audit.md` for the current audit and status. Kept for history.

**Source audit:** `OpenVR-Capture_Performance-Audit.md`
**Target file:** `plugins/win-openvr/win-openvr.cpp` (~370 lines, single TU)
**Goal:** Resolve all 6 audit findings with minimal blast radius, in the audit's recommended order, while keeping every existing "already good" behavior intact (redundant-copy skip via `lastFrame`, Release build flags, `ComPtr` RAII).

This plan is phased to match the audit's own risk/value ordering:

- **Phase 1** — Items 1, 2, 3: source lifecycle + throttle correctness. Low risk, biggest user-facing win.
- **Phase 2** — Item 4: Flush → keyed mutex. Medium risk, needs multi-vendor GPU testing.
- **Phase 3** — Items 5, 6: idle backoff + math hardening. Low risk, cleanup.

One cross-cutting note up front: **items 1–3 and 5 all touch `win_openvr_init`, `win_openvr_init1`, `win_openvr_show`, `win_openvr_hide`, and `win_openvr_update`.** They're written up separately below (to mirror the audit numbering) but should land as one coordinated patch to those functions rather than four sequential edits to the same lines.

---

## Phase 1 — Items 1–3: Lifecycle & Throttle Correctness

### Item 1 (🔴 High): Stop full teardown/reinit on hide→show

**Current behavior:** `win_openvr_hide()` calls `win_openvr_deinit()`, which releases the shared D3D11 device/context and calls `vr::VR_Shutdown()`. `win_openvr_show()` calls `win_openvr_init1()`, which rebuilds the device, re-inits OpenVR, and re-fetches the mirror texture. This runs on **every** OBS scene cut into/out of a scene containing the source, not just stream start/stop.

**Target behavior:** The D3D11 device and OpenVR session persist for the lifetime of the source. `hide()`/`show()` only toggle the existing `active` flag (already checked at the top of `win_openvr_render`). Full teardown is reserved for `win_openvr_destroy()` (source removal) and the existing `VREvent_Quit` handler in `win_openvr_tick()` (SteamVR shutting down).

This is not a new pattern — `win_openvr_update()` already does a "soft" reinit on settings changes (reset `initialized`, call `win_openvr_init()` directly) **without** tearing down the device, because of the `if (!context->shared_device.Get())` guard inside `win_openvr_init()`. Item 1 just applies that same soft pattern to hide/show instead of the current hard teardown.

**Change:**

```cpp
static void win_openvr_show(void *data)
{
	win_openvr *context = (win_openvr *)data;
	context->active = true; // resume immediately; tick() reconfirms every frame regardless
	if (!context->initialized) {
		win_openvr_init1(data, true); // forced: see Item 2 — only attempt init if we don't already have one
	}
}

static void win_openvr_hide(void *data)
{
	win_openvr *context = (win_openvr *)data;
	context->active = false; // pause copy/render only — device, OpenVR session, and textures stay alive
}
```

`win_openvr_destroy()` is unchanged (still calls `win_openvr_deinit()` then `bfree()`) — it becomes the sole full-teardown path outside of the `VREvent_Quit` case.

**Why this is safe:**
- `win_openvr_render()` already early-returns when `!context->active`, so no `CopySubresourceRegion`/`GetFrameTiming` calls happen while hidden — that's the entire "pause."
- `win_openvr_tick()` calls `vr::VRSystem()->PollNextEvent(...)` unconditionally (not gated on `active`), so the `VREvent_Quit` → `win_openvr_deinit()` safety net for "SteamVR closed while this source happened to be hidden" still fires — OBS calls `video_tick` for all loaded sources every frame regardless of scene visibility, only `video_render` is visibility-gated. **This should be confirmed empirically** in testing (see Test Matrix, item 1) since it's the one assumption load-bearing enough to call out.
- If `show()` is called before any successful init (e.g., first show while SteamVR isn't running yet), `!context->initialized` is true and the existing retry path in `win_openvr_init1`/`tick` takes over exactly as before — no regression there.

**Not doing (explicitly out of scope):** an idle-timeout full teardown for sources hidden a very long time (e.g., to free GPU memory during a long AFK stretch). The audit didn't ask for this, and adding it risks reintroducing the exact stutter this fix removes if the timeout is too aggressive. Flag as a possible future enhancement only if GPU memory pressure from long-idle hidden sources is later observed to be a real problem.

---

### Item 2 (🟠 Medium): Fix the `forced` parameter

**Current behavior:** Both `win_openvr_init` and `win_openvr_init1` declare `bool forced = true` **and never read it**. Because the default is `true`, every call site is implicitly "forced" already (not just the `show()` call the comment singles out), so fixing `forced` naively (make it skip the throttle when true) would make `render()`'s and `tick()`'s per-frame calls bypass throttling too — hammering `VR_Init` at render framerate whenever SteamVR isn't available. That's a regression, and it directly fights Item 5.

**Fix:** Flip the default to `false`, make `forced=true` actually skip the time-based throttle (not the `init_inprog` re-entrancy guard, which must always apply), and make each call site explicit about which behavior it wants:

```cpp
static void win_openvr_init(void *data, bool forced = false)
{
	win_openvr *context = (win_openvr *)data;

	if (context->initialized || context->init_inprog) {
		return; // re-entrancy guard — always enforced, forced or not
	}

	if (!forced) {
		auto now = std::chrono::steady_clock::now();
		if (now - context->last_init_time < retry_delay) {
			return;
		}
	}
	context->last_init_time = std::chrono::steady_clock::now();
	// ... unchanged body below ...
}

static void win_openvr_init1(void *data, bool forced = false)
{
	win_openvr *context = (win_openvr *)data;

	if (context->initialized || context->init_inprog) {
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
```

Call sites, made explicit:

| Call site | Old | New | Rationale |
|---|---|---|---|
| `win_openvr_show` | `win_openvr_init1(data, true)` | `win_openvr_init1(data, true)` | User just made the source visible — should attempt immediately, matching the existing comment's intent. |
| `win_openvr_update` (settings changed while initialized) | `win_openvr_init(data)` | `win_openvr_init(data, true)` | User just changed a setting (righteye/AR/offset) — should apply immediately, not up to 500ms later. |
| `win_openvr_render` | `win_openvr_init1(data)` | `win_openvr_init1(data)` (now defaults `false`) | Ambient per-frame retry while active-but-uninitialized — must stay throttled. |
| `win_openvr_tick` | `win_openvr_init1(data)` | `win_openvr_init1(data)` (now defaults `false`) | Same as above. |

This resolves the audit finding without weakening Item 5's backoff behavior.

---

### Item 3 (🟠 Medium): Move throttle state onto the per-instance struct

**Current behavior:** `init_inprog`, `IsVRSystemInitialized`, `last_init_time`, and `last_init_timeBUFFER` are file-scope statics — shared across every `win_openvr` instance. Given the plugin's own `righteye` toggle implies a common two-instance (left+right eye) setup, one instance mid-init blocks the other's init entirely, and the retry windows are shared rather than independent.

**Bonus finding while migrating this:** `IsVRSystemInitialized` is set to `true` inside `win_openvr_init()` but never read anywhere else in the file (confirmed — this is the only `.cpp` in the plugin). It's dead state. Recommend dropping it rather than migrating it, unless there's a reason to keep it that isn't visible in this translation unit (e.g., planned future use) — worth a quick `grep` across the rest of the fork before deleting, just in case.

**Change — struct additions:**

```cpp
struct win_openvr {
	// ... existing fields unchanged ...

	// Item 3: per-instance throttle/re-entrancy state (was file-scope static)
	bool init_inprog = false;
	std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point last_init_timeBUFFER = std::chrono::steady_clock::now();

	// Item 5: per-instance backoff state for the idle retry loop (see Phase 3)
	std::chrono::milliseconds retry_delayBUFFER_current{500};
};
```

**Change — remove file-scope statics:**

```cpp
// DELETE:
static bool init_inprog = false;
static bool IsVRSystemInitialized = false;
std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point last_init_timeBUFFER = std::chrono::steady_clock::now();
```

**Change — keep as compile-time constants (these are constants, not state, so they stay file-scope):**

```cpp
static constexpr std::chrono::milliseconds retry_delay{8};              // per-call debounce, ~120Hz
static constexpr std::chrono::milliseconds retry_delayBUFFER_base{500};  // base retry cadence, 2Hz (renamed)
static constexpr std::chrono::milliseconds retry_delayBUFFER_max{4000}; // Item 5 backoff cap
```

**Change — `win_openvr_create()`:** `bzalloc` zero-initializes the struct's raw memory but does not run C++ constructors, so — consistent with how the existing code already explicitly re-assigns `nullptr` to the `ComPtr` members even though they have in-class initializers — explicitly initialize the new fields too rather than relying on the in-class defaults:

```cpp
context->init_inprog = false;
context->last_init_time = std::chrono::steady_clock::now();
context->last_init_timeBUFFER = std::chrono::steady_clock::now();
context->retry_delayBUFFER_current = retry_delayBUFFER_base;
```

**Change — every reference to the old globals inside `win_openvr_init`, `win_openvr_init1`, and `win_openvr_deinit`** becomes `context->init_inprog`, `context->last_init_time`, `context->last_init_timeBUFFER` respectively. (Already reflected in the Item 2 code above.) `win_openvr_deinit()`'s `init_inprog = false;` becomes `context->init_inprog = false;`.

---

## Phase 2 — Item 4 (🟠 Medium): Replace unconditional per-frame `Flush()`

**Current behavior:**

```cpp
context->shared_context->CopySubresourceRegion(context->texCrop.Get(), 0, 0, 0, 0, context->tex.Get(), 0, &poksi);
context->shared_context->Flush();
```

runs on every compositor frame-index change (up to headset refresh rate), forcing an immediate command-queue submit instead of letting the driver batch normally.

**Important caveat to flag before implementing:** the shared texture is currently created with `D3D11_RESOURCE_MISC_SHARED` and consumed by OBS via `gs_texture_open_shared()` (legacy NT-handle-less shared resource). A keyed mutex (`D3D11_RESOURCE_MISC_SHARED_KEYMUTEX` + `IDXGIKeyedMutex::AcquireSync`/`ReleaseSync`) only gives a true tear-free handoff guarantee if **both** the producer (this plugin) **and** the consumer (libobs's D3D11 backend, on its own device) acquire/release around their respective write/read. This plugin only controls the producer side — whether libobs's `gs_texture_open_shared` participates in a keyed mutex protocol on the read side is a libobs implementation detail that should be checked against the actual OBS source tree being built against, rather than assumed. **First implementation step: grep the target OBS version's `libobs-d3d11`/`device-d3d11.cpp` for `KeyedMutex`/`AcquireSync` to confirm whether the consumer side cooperates.**

Given that uncertainty, recommend implementing this as the practical, honestly-scoped improvement it actually is — not "eliminate all synchronization," but "replace a blanket CPU-stalling `Flush()` with a scoped, non-blocking handoff on the side we control":

**Change — struct addition:**

```cpp
ComPtr<IDXGIKeyedMutex> keyedMutex; // Item 4
```

**Change — texture creation in `win_openvr_init()`:**

```cpp
desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYMUTEX; // was D3D11_RESOURCE_MISC_SHARED

HRESULT hr = context->shared_device->CreateTexture2D(&desc, nullptr, context->texCrop.GetAddressOf());
if (FAILED(hr)) { /* unchanged failure handling */ }

context->keyedMutex.Reset();
context->texCrop->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(context->keyedMutex.GetAddressOf()));
// If this QueryInterface fails on some driver, context->keyedMutex stays null and the render-side
// fallback below keeps the current Flush() behavior for that instance — no hard dependency introduced.
```

The rest of the shared-handle/`gs_texture_open_shared` path (`IDXGIResource::GetSharedHandle`) is unaffected — `SHARED_KEYMUTEX` textures still support the legacy shared-handle open path.

**Change — `win_openvr_render()`:**

```cpp
if (context->texCrop && context->tex) {
	D3D11_BOX poksi = {context->x, context->y, 0, context->x + context->width, context->y + context->height, 1};

	if (context->keyedMutex) {
		// Non-blocking: if the resource is momentarily contended, skip this frame's copy rather
		// than stalling the render thread. The next changed-frame-index copy will catch up.
		if (context->keyedMutex->AcquireSync(0, 0) == S_OK) {
			context->shared_context->CopySubresourceRegion(context->texCrop.Get(), 0, 0, 0, 0, context->tex.Get(), 0, &poksi);
			context->keyedMutex->ReleaseSync(1); // ReleaseSync submits pending work on this resource
			context->lastFrame = frameTiming.m_nFrameIndex;
		}
		// else: contended this frame — deliberately do not advance lastFrame, so it's retried next tick
	} else {
		// Fallback for drivers where SHARED_KEYMUTEX/QueryInterface isn't available
		context->shared_context->CopySubresourceRegion(context->texCrop.Get(), 0, 0, 0, 0, context->tex.Get(), 0, &poksi);
		context->shared_context->Flush();
		context->lastFrame = frameTiming.m_nFrameIndex;
	}
}
```

**Staged rollout recommendation (given the "Medium effort, needs GPU testing" flag in the audit):** gate the keyed-mutex path behind a simple compile-time or config flag initially (matching the project's existing precedent of gating optional/risky behavior behind a diagnostics flag, as was done for PNG capture in the last performance pass). Run both paths side by side across NVIDIA + AMD (+ Intel iGPU if available) for a full session each before removing the `Flush()` fallback entirely.

---

## Phase 3 — Items 5–6: Cleanup & Hardening

### Item 5 (🟡 Low): Exponential backoff on idle `VR_Init` retry

**Current behavior:** While active-but-uninitialized (e.g., SteamVR not running), `win_openvr_init1` retries at a fixed ~2Hz (`retry_delayBUFFER{500}`) forever.

**Change:** Use the per-instance `retry_delayBUFFER_current` field added in Item 3. Double it on every failed attempt, cap at `retry_delayBUFFER_max` (4s), reset to `retry_delayBUFFER_base` (500ms) on success:

```cpp
static void win_openvr_init(void *data, bool forced = false)
{
	win_openvr *context = (win_openvr *)data;

	if (context->initialized || context->init_inprog) {
		return;
	}
	if (!forced) {
		auto now = std::chrono::steady_clock::now();
		if (now - context->last_init_time < retry_delay) {
			return;
		}
	}
	context->last_init_time = std::chrono::steady_clock::now();
	context->init_inprog = true;

	auto on_failure = [context]() {
		context->retry_delayBUFFER_current = std::min(context->retry_delayBUFFER_current * 2, retry_delayBUFFER_max);
		context->init_inprog = false;
	};

	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Background);
	if (err != vr::VRInitError_None) {
		warn("win_openvr_init: OpenVR initialization failed! %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
		on_failure();
		return;
	}
	// ... replace every other early-return failure path in this function with on_failure(); return; ...

	// At the bottom, on full success:
	context->initialized = true;
	context->lastFrame = 0;
	context->init_inprog = false;
	context->retry_delayBUFFER_current = retry_delayBUFFER_base; // reset backoff
}
```

And `win_openvr_init1` checks `context->retry_delayBUFFER_current` instead of a fixed constant (already reflected in the Item 2 code block above).

**Design choice:** backoff state is driven purely by actual `win_openvr_init()` success/failure, independent of whether a given call was `forced`. A forced call still bypasses the *delay gate* (so it's attempted immediately), but doesn't reset or otherwise special-case the backoff multiplier — that stays tied only to real outcomes, keeping the two concerns (Item 2 vs Item 5) decoupled.

---

### Item 6 (🟡 Low): Harden pan/zoom math against unsigned underflow

**Current behavior:** `context->width`/`height`/`x`/`y` are `unsigned int`. The crop-offset math mixes a local signed `int x, y` with unsigned subtractions (`context->device_width - scaled_width`) before a final `std::max(0, x)` clamp. Structurally, today's `scale_factor` clamp (`< 1.0 ? 1.0 : scale_factor`) keeps `scaled_width <= device_width` in the normal case, and the AR-crop branch only ever shrinks one dimension — so a concrete crash isn't trivially reachable through the properties UI alone. But `x_offset`/`y_offset` (UI range ±10000) and custom aspect ratio (UI range 1–100, i.e. up to 100:1 or 1:100) are extreme enough, and the later bounds-check line only happens to produce a safe result via unsigned-wraparound coincidence rather than intentional signed-safe logic — worth tightening regardless of whether today's UI bounds can currently trigger a visible bug, especially since a hand-edited scene-collection JSON can set these fields outside the slider bounds.

**Change:** do the whole computation in signed 64-bit space with explicit `std::clamp`, and defensively clamp `width`/`height` themselves to never exceed the source texture bounds (protects the `CreateTexture2D`/`CopySubresourceRegion` box from ever describing a region larger than the mirror texture):

```cpp
// After scale_factor + AR-crop logic has produced context->width / context->height (unchanged):

int64_t device_w = context->device_width;
int64_t device_h = context->device_height;
int64_t w = std::min<int64_t>(context->width, device_w);   // defensive clamp
int64_t h = std::min<int64_t>(context->height, device_h);  // defensive clamp
context->width = static_cast<unsigned int>(w);
context->height = static_cast<unsigned int>(h);

int64_t x = 0, y = 0;
int64_t x_offset = context->x_offset;
int64_t y_offset = context->y_offset;

if (!context->righteye) {
	x_offset = -x_offset;
	x = device_w - w; // signed subtraction — never wraps even if w > device_w before the clamp above
}
x += x_offset;
y += y_offset;

x = std::clamp<int64_t>(x, 0, std::max<int64_t>(0, device_w - w));
y = std::clamp<int64_t>(y, 0, std::max<int64_t>(0, device_h - h));

context->x = static_cast<unsigned int>(x);
context->y = static_cast<unsigned int>(y);
```

This removes the old `if (x + width > device_width) x = device_width - width;` pattern entirely (replaced by the `std::clamp` bound), and removes all reliance on unsigned-wraparound-happens-to-be-safe behavior.

---

## Consolidated Struct Diff

```cpp
struct win_openvr {
	obs_source_t *source;

	bool righteye;
	double active_aspect_ratio;
	bool ar_crop;

	uint32_t lastFrame;

	gs_texture_t *texture;
	ComPtr<ID3D11Resource> tex;
	ComPtr<ID3D11ShaderResourceView> mirrorSrv;
	ComPtr<ID3D11Device> shared_device = nullptr;
	ComPtr<ID3D11DeviceContext> shared_context = nullptr;

	ComPtr<IDXGIResource> res;
	ComPtr<ID3D11Texture2D> texCrop;
	ComPtr<IDXGIKeyedMutex> keyedMutex;              // Item 4

	unsigned int device_width;
	unsigned int device_height;

	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;

	double scale_factor;
	int x_offset;
	int y_offset;

	bool initialized;
	bool active;

	// Item 3 — moved from file-scope statics
	bool init_inprog = false;
	std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point last_init_timeBUFFER = std::chrono::steady_clock::now();

	// Item 5 — per-instance backoff
	std::chrono::milliseconds retry_delayBUFFER_current{500};
};
```

`IsVRSystemInitialized` (dead global) is dropped, not migrated — see Item 3 note.

---

## Test Matrix

| # | Test | What to verify |
|---|------|-----------------|
| 1 | **Hide/show stutter** | Add the source to Scene A, cut A→B→A rapidly 20–50×. No visible blank/black flash beyond a frame; add temporary debug logging around `VR_Init`/`VR_Shutdown`/`D3D11CreateDevice` calls and confirm they fire once (at first show), not on every cut. |
| 2 | **`VREvent_Quit` while hidden** | With the source hidden (on a scene not currently active), quit SteamVR. Confirm `win_openvr_tick` still runs and `win_openvr_deinit` still fires (validates the Item 1 assumption that `video_tick` runs regardless of scene visibility). |
| 3 | **Forced settings apply** | While visible, toggle "Right Eye" and Aspect Ratio. Change should be visible near-instantly, not after up to 500ms. |
| 4 | **Multi-instance independence** | Add two source instances (left+right eye) to one scene. Start SteamVR right as both become visible; add temporary logging with the instance's source name to confirm both initialize on independent timers, and one instance mid-init doesn't block the other. |
| 5 | **Flush/keyed-mutex regression** | Multi-vendor: run 10+ minutes on NVIDIA and AMD (Intel iGPU if available). Watch for tearing/corruption in the mirrored eye texture during fast headset motion. Compare OBS's render-lag/dropped-frames stats before/after. If a contention-skip counter is added, log its rate — should be near zero in normal operation. |
| 6 | **Idle backoff** | Don't start SteamVR; add the source. Confirm (via log timestamps) retry interval grows 500→1000→2000→4000ms and holds, and idle CPU usage drops correspondingly versus the old fixed-500ms behavior. |
| 7 | **Pan/zoom extremes** | Set custom aspect ratio to 1:100 and 100:1, x/y offset to ±10000, scale factor at 1.0 and 5.0 (all reachable via the properties UI). No crash, no garbled/oversized crop rect. Optionally hand-edit the scene collection JSON to set `scale_factor` below 1.0 to confirm the existing clamp plus the new hardening both hold. |
| 8 | **Build hygiene** | Full Release build with existing `/O2 /GL /LTCG /OPT:REF /OPT:ICF` flags. Confirm no new signed/unsigned truncation warnings (C4267/C4244) from the Item 6 rewrite, and no warnings about the removed globals/dead `IsVRSystemInitialized`. |

Build steps are unchanged from `README.md`: pull OBS Studio recursively, copy `plugins/win-openvr` into the source tree, pull the OpenVR SDK into `deps/`, add the `add_obs_plugin` line, then `cmake --preset windows-x64 && cmake --build ./build_x64/plugins/win-openvr --config Release`.

---

## Suggested Sequencing

1. **Phase 1 (Items 1–3)** as one coordinated patch — they all touch the same handful of functions and are cheap to test together via the hide/show and multi-instance tests above.
2. **Phase 2 (Item 4)** as its own patch, behind a temporary flag, with the multi-vendor soak test before removing the `Flush()` fallback.
3. **Phase 3 (Items 5–6)** as a final cleanup patch — lowest risk, can land whenever convenient after Phase 1.

Each phase is independently buildable and testable; none of them require the others to compile, so they can also be split into separate PRs/commits if preferred.
