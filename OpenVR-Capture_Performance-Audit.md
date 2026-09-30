# OpenVR Capture — Performance Audit & Action Plan

**Repo:** `OpenVR-Capture-master` (fork of OpenVR Capture input plugin for OBS)
**Scope reviewed:** `plugins/win-openvr/win-openvr.cpp`, `plugins/win-openvr/CMakeLists.txt`
**Focus:** performance-affecting issues (frame-time impact, init/teardown cost, responsiveness)

---

## Executive Summary

The plugin is small (~370 lines) and generally sound — it already does the right thing by skipping the GPU texture copy when the VR compositor frame index hasn't changed, and the release build flags are correctly tuned. The main performance risks are architectural rather than algorithmic: **the plugin fully tears down and rebuilds its D3D11 device and OpenVR session on every OBS scene hide/show**, which is the single biggest likely source of user-visible stutter, since hide/show happens on every scene switch. A handful of smaller issues (an unused "forced init" flag, global rather than per-instance throttling state, and an unconditional per-frame GPU `Flush()`) compound this.

| # | Issue | Severity | Area |
|---|-------|----------|------|
| 1 | Full OpenVR/D3D11 teardown + reinit on every hide→show | 🔴 High | Scene-switch latency |
| 2 | `forced` init parameter is unused — "forced" show is still throttled | 🟠 Medium | Responsiveness |
| 3 | Init-throttle state is global, not per source instance | 🟠 Medium | Multi-instance correctness/perf |
| 4 | Unconditional `Flush()` every changed VR frame | 🟠 Medium | Frame-time jitter |
| 5 | Indefinite fast retry of `VR_Init` when SteamVR isn't running | 🟡 Low | Background overhead |
| 6 | Unsigned/signed mix in crop math can underflow | 🟡 Low | Correctness (edge case) |

---

## Detailed Findings

### 1. 🔴 Full teardown/reinit on every hide→show

**Where:** `win_openvr_hide()` → `win_openvr_deinit()`, `win_openvr_show()` → `win_openvr_init1()` → `win_openvr_init()`

`win_openvr_deinit()` releases the shared D3D11 device/context and calls `vr::VR_Shutdown()`. The next `show()` calls `D3D11CreateDevice` and `vr::VR_Init()` again from scratch, then re-fetches the mirror texture and recreates the shared crop texture.

**Why it matters:** In OBS, a source's `hide`/`show` callbacks fire every time you switch away from and back to a scene containing it — not just on stream start/stop. `VR_Init`/`VR_Shutdown` involve IPC negotiation with the SteamVR runtime, and `D3D11CreateDevice` is not cheap either. For anyone who keeps this source on a scene they cut to repeatedly during a stream, every cut pays this cost — a likely cause of visible hitches or a blank source for a beat after switching back.

**Recommendation:** Keep the D3D11 device and OpenVR session alive across hide/show. The `active` flag (already checked in `tick`/`render`) can be used to simply pause rendering/copying while hidden. Reserve the full `VR_Shutdown()` / device release for `destroy()` (source removal), not `hide()`.

---

### 2. 🟠 `forced` parameter does nothing

**Where:** `win_openvr_init(void *data, bool forced = true)`, `win_openvr_init1(void *data, bool forced = true)`

The call in `win_openvr_show()`:
```cpp
win_openvr_init1(data, true); // When showing do forced init without delay
```
implies bypassing the retry delay, but `forced` is never referenced in either function body — both still gate on `last_init_timeBUFFER` / `retry_delayBUFFER` (500ms) regardless.

**Why it matters:** A show can leave the source blank for up to 500ms even though the code is written to suggest it shouldn't. This is a responsiveness bug hiding behind a comment that isn't true.

**Recommendation:** Either make `forced` actually skip the throttle check, or remove the parameter and misleading comment so behavior matches what the code reads as.

---

### 3. 🟠 Init-throttle state is global, not per-instance

**Where:** file-scope statics —
```cpp
static bool init_inprog = false;
static bool IsVRSystemInitialized = false;
std::chrono::steady_clock::time_point last_init_time = ...;
std::chrono::steady_clock::time_point last_init_timeBUFFER = ...;
```

**Why it matters:** These aren't members of `win_openvr`, so they're shared across *all* instances of the source. The plugin's own `righteye` toggle strongly implies a common setup of two source instances (left eye + right eye) side by side. If one instance is mid-init, `init_inprog` blocks the other instance's init entirely, and the 8ms/500ms retry windows are shared rather than independent — so the second instance can be delayed or starved depending on timing.

**Recommendation:** Move `init_inprog`, `last_init_time`, `last_init_timeBUFFER`, and `IsVRSystemInitialized` onto the `win_openvr` struct so each source instance throttles independently.

---

### 4. 🟠 Unconditional `Flush()` every changed frame

**Where:** `win_openvr_render()`
```cpp
context->shared_context->CopySubresourceRegion(...);
context->shared_context->Flush();
```

**Why it matters:** This runs on every VR compositor frame index change — up to the headset's refresh rate (90–144Hz+). `Flush()` forces the driver to submit the command queue immediately instead of batching normally, which can introduce CPU/GPU sync stalls and frame-time jitter, particularly since this is a separate D3D11 device/context from OBS's own render device.

**Recommendation:** Consider a D3D11 keyed mutex on the shared texture for cross-device synchronization instead of relying on an eager `Flush()` every frame. This gives correct visibility guarantees without forcing a submit on every single copy.

---

### 5. 🟡 Indefinite fast retry when SteamVR isn't running

**Where:** `win_openvr_init1()` gated by `retry_delayBUFFER{500}` (~2Hz)

**Why it matters:** While the source is visible but uninitialized (e.g., SteamVR isn't open), `VR_Init` is retried every ~500ms forever. Not incorrect, but it's background overhead for anyone using the source without a headset connected.

**Recommendation:** Not urgent — but an exponential backoff (capped at, say, a few seconds) would reduce idle overhead without meaningfully hurting reconnect responsiveness.

---

### 6. 🟡 Unsigned/signed mix in crop math

**Where:** `win_openvr_init()` pan/zoom block
```cpp
unsigned int scaled_width = ...;
...
x = context->device_width - scaled_width; // both unsigned int
```
then later assigned into a signed `x`, clamped by `std::max(0, x)`.

**Why it matters:** If `scaled_width` (post-zoom) exceeds `device_width`, the unsigned subtraction underflows to a very large value before being narrowed into a signed int — behavior here is implementation-defined and the later clamp doesn't reliably fix a bad intermediate. This is a correctness edge case (extreme zoom/offset combos) rather than a steady-state perf issue, but it's adjacent to the perf-critical crop path so worth tightening.

**Recommendation:** Do the arithmetic in signed types with explicit bounds checks before assigning into `x`/`y`.

---

## What's Already Good

- **Redundant-copy avoidance:** `frameTiming.m_nFrameIndex != context->lastFrame` correctly skips the GPU copy when the compositor hasn't produced a new frame — a real, already-implemented optimization.
- **Release build flags** in `CMakeLists.txt` (`/O2 /GL`, `/LTCG /OPT:REF /OPT:ICF`) are exactly what you'd want for a hot-path native plugin.
- **RAII via `ComPtr`** avoids manual D3D11 resource leaks.

---

## Prioritized Action Plan

| Priority | Action | Est. Effort |
|----------|--------|--------------|
| 1 | Stop fully tearing down OpenVR/D3D11 on `hide()`; gate rendering via `active` instead, defer teardown to `destroy()` | Medium (touches init/deinit lifecycle) |
| 2 | Fix or remove the unused `forced` parameter so show-time behavior matches intent | Small |
| 3 | Move throttle statics (`init_inprog`, timers) onto the per-instance `win_openvr` struct | Small |
| 4 | Replace per-frame `Flush()` with keyed-mutex-based cross-device sync | Medium (needs testing across GPU vendors) |
| 5 | Add backoff to the idle `VR_Init` retry loop | Small |
| 6 | Harden the pan/zoom math against unsigned underflow | Small |

**Suggested order of implementation:** items 1–3 first (biggest user-facing win, lowest risk), then 4 (needs more careful GPU-side testing), then 5–6 as cleanup.
