# VR Capture (SpaceRoach Edition): Correctness Audit & Fix Plan

**Date:** 2026-09-27
**Scope:** every first-party source file: `plugins/win-openvr/*`, `layers/openxr-vrcapture-layer/*`, `shared/*`, `deps/openxr/include/openxr/*` (hand-written), `scripts/*.ps1`, CMake files.
**Method:** static review only. Nothing was compiled or run (the plugin needs an OBS source tree; the layer needs a headset + OpenXR app). Each finding below includes the concrete failure scenario so it can be confirmed in testing.

> The earlier `OpenVR-Capture_Performance-Audit.md` / `Implementation-Plan.md` describe a ~370-line single-file plugin. Most of their items (hide/show teardown, `forced` flag, per-instance throttle, backoff, signed crop math) **are already implemented** in the current code. Those docs are now historical.

---

## Status

| Item | State |
|---|---|
| Phase 0: git baseline, both targets build (MSVC + Ninja, OBS SDK 32.2.1) | ✅ done |
| A1: official OpenXR headers (OpenXR-SDK release-1.1.63) + loader struct validation | ✅ done |
| A2 + D1 + D2: manifest `disable_environment`, manifest emitted next to DLL, installer | ✅ done |
| A3: OBS owns the mapping, layer attaches at ≤1 Hz, IPC v2 version check | ✅ done, covered by `tests/ipc_handshake_test` |
| (new) consumer re-locked its own non-recursive mutex in `UpdateTexture` → `Initialize` | ✅ fixed with A3 |
| B4 / B5: consumer mapping retry throttle, locked header access | ✅ done with A3 |
| SteamVR-only sources no longer touch the OpenXR IPC (part of B3) | ✅ done with A3 |
| A4: producer copy atomic with texture swap; resize-pending frames dropped | ✅ done, covered by `tests/ipc_handshake_test` |
| A5 (partial): correct subresource index, MSAA + out-of-bounds rects skipped | ✅ done (copy still happens in `xrEndFrame`) |
| B1: consumer copies into an OBS-owned texture once per tick, always draws it | ✅ done, verify with `tests/fake_producer` |
| B2: zoom / aspect / offsets applied to the OpenXR draw | ✅ done, verify with `tests/fake_producer` |
| Everything else | open |

---

## TL;DR

The **SteamVR/OpenVR path is mostly sound**. It has one real lifecycle bug (a zombie `VRSystem` after SteamVR quits) and a few smaller issues.

The **OpenXR/VDXR path can't work as written.** Three separate blockers each stop it on their own:

1. The hand-written OpenXR headers get the loader structs and one enum wrong, so the layer reads a garbage pointer during `xrCreateInstance`. That's a likely **crash in every OpenXR app on the machine**, because the layer is registered as implicit.
2. The manifest is missing `disable_environment`, which the OpenXR loader **requires for implicit layers**. The loader rejects it, so in practice (1) probably never fires. The layer just doesn't load.
3. Even with 1 and 2 fixed, there's a **bootstrap deadlock**. The layer only creates the shared memory once OBS is connected, and OBS can only connect once the shared memory exists.

Past those, the OpenXR display path would still **flicker/blank frames** because of the keyed-mutex protocol, and it ignores zoom/crop/aspect.

---

## Findings

Severity: 🔴 Critical (feature broken / crash) · 🟠 High · 🟡 Medium · ⚪ Low

### A. OpenXR layer (`layers/openxr-vrcapture-layer`, `deps/openxr`)

#### A1 🔴 Hand-written OpenXR headers have wrong ABI
`deps/openxr/include/openxr/openxr_loader_negotiation.h`, `openxr.h`

| Item | Stub | Official Khronos header |
|---|---|---|
| `XrApiLayerCreateInfo` | `structType, next, loaderInfo, nextInfo` | `structType, structVersion, structSize, loaderInstance, settings_file_location[512], nextInfo` |
| `XrApiLayerNextInfo` | `XrStructureType structType, next, layerName, nextGIPA, nextCreate` | `XrLoaderInterfaceStructs structType, structVersion, structSize, layerName[256], nextGIPA, nextCreate, next` |
| `XR_TYPE_COMPOSITION_LAYER_PROJECTION` | `14` | `35` |
| other `XR_TYPE_*` (session/swapchain/frame info) | wrong values | (unused by our code today, but traps waiting to happen) |

**Failure:** `Hook_xrCreateApiLayerInstance` ([layer_main.cpp:40-51](layers/openxr-vrcapture-layer/layer_main.cpp:40)) reads `layerInfo->nextInfo` at byte offset 24 instead of 536. It then dereferences a garbage pointer, and the host app crashes inside `xrCreateInstance`. If that somehow survived, the `xrEndFrame` projection-layer check ([openxr_interceptor.cpp:201](layers/openxr-vrcapture-layer/openxr_interceptor.cpp:201)) compares against `14`, never matches, and nothing ever gets captured.
**Fix:** delete the stubs and vendor the real headers from [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) (`include/openxr/openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`, `openxr_reflection.h`, `openxr_loader_negotiation.h`). Define `XR_USE_GRAPHICS_API_D3D11`/`D3D12` + `XR_USE_PLATFORM_WIN32` before including `openxr_platform.h`. In `Hook_xrCreateApiLayerInstance`, validate `layerInfo->structType == XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO` and `nextInfo->structType == XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO`.

#### A2 🔴 Implicit-layer manifest missing `disable_environment`
[layer_manifest.json.in](layers/openxr-vrcapture-layer/layer_manifest.json.in), and the manifest generated inside [Install-OpenXR-Layer.ps1:56-71](scripts/Install-OpenXR-Layer.ps1:56)

The OpenXR loader rejects implicit layer manifests that have no `disable_environment` key, so the layer is silently never loaded.
**Fix:** add `"disable_environment": "DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE"` (optionally `"enable_environment"` too) to both manifests. Set `"api_version"` to `"1.0"`, which is already done.

#### A3 🔴 Bootstrap deadlock: shared memory is never created
[openxr_interceptor.cpp:197](layers/openxr-vrcapture-layer/openxr_interceptor.cpp:197), [layer_ipc_producer.cpp:205-219](layers/openxr-vrcapture-layer/layer_ipc_producer.cpp:205), [openxr_ipc_consumer.cpp:47](plugins/win-openvr/openxr_ipc_consumer.cpp:47)

- `xrEndFrame` does work only `if (... m_ipc.IsObsConnected())`.
- `IsObsConnected()` returns `false` when `m_sharedHeader` is null.
- `m_sharedHeader` is set only by `Initialize()`, which is reached only via `RequestAsyncInitialize()`, which is called only **inside** that `if`.
- OBS's consumer only calls `OpenFileMappingW` (open, never create).

**Failure:** nobody ever creates `Local\SpaceRoachVR_Capture_IPC`. Auto mode never detects OpenXR, and forced OpenXR mode shows nothing.
**Fix (recommended):** invert ownership. **OBS creates the mapping** (`CreateFileMappingW`) when an OpenXR-capable source is created, and keeps it for the source's lifetime. The layer does a cheap, **throttled** `OpenFileMappingW` (e.g. once per second) from `xrEndFrame` until it succeeds. That preserves the "don't burden apps that aren't being captured" goal. Keep producer-owned fields (handle/dims/heartbeat) and consumer-owned fields (connected/eye/heartbeat) clearly separated.

#### A4 🟠 Producer texture swap race → use-after-free / unacquired keyed mutex
[openxr_interceptor.cpp:250-297](layers/openxr-vrcapture-layer/openxr_interceptor.cpp:250), [layer_ipc_producer.cpp:146-178](layers/openxr-vrcapture-layer/layer_ipc_producer.cpp:146)

`BeginFrameCopy()` acquires `m_keyedMutex` and releases the lock, and then `GetSharedTexture()` returns a raw pointer. If the init worker runs `CreateSharedTexture()` → `DestroySharedTexture()` in between, the app thread either:
- copies into a freed texture (`Reset()` dropped the last ref), or
- copies into the *new* texture without holding its keyed mutex, then `ReleaseSync(1)`s a mutex it never acquired (DXGI error, and possibly device removal).

The first frames after a resize also copy a `cropWidth×cropHeight` box into the *old* smaller texture.
**Fix:** do the whole Begin → copy → End as one producer method under `m_mutex` (e.g. `bool CopyFrame(ctx, srcTex, subresource, box, metadata)`) that holds its own `ComPtr` to the texture and mutex. Skip the copy when the current texture's dims don't match the box. Read `m_width`/`m_height` under the lock (they're currently read unlocked from the app thread).

#### A5 🟡 Copying after `xrReleaseSwapchainImage` / wrong subresource
[openxr_interceptor.cpp:172-190, 266-274](layers/openxr-vrcapture-layer/openxr_interceptor.cpp:172)

- The copy happens in `xrEndFrame`, after the app has released the image back to the runtime. That works on most runtimes, but it's outside the spec's ownership rules. Copying inside the `xrReleaseSwapchainImage` hook (before chaining) is the safer point. It needs the eye→swapchain mapping from the previous frame's `xrEndFrame`, or you capture the image on release and choose the eye at `xrEndFrame`.
- `arrayIndex` is passed as the subresource index. That's only correct when `mipCount == 1`. Use `D3D11CalcSubresource(0, arrayIndex, mipCount)` and store `mipCount` in `SwapchainInfo`.
- MSAA swapchains (`sampleCount > 1`) make `CopySubresourceRegion` a no-op. They'd need `ResolveSubresource` or should be skipped with a log.

#### A6 🟡 Only D3D11 apps are captured
`m_isD3D12` is set and never used, and Vulkan/OpenGL apps are ignored. Many OpenXR titles (Unreal/Unity D3D12, Vulkan) will show nothing. The README says "Captures OpenXR games" with no qualifier.
**Fix (short term):** say "D3D11 OpenXR apps" in the README and log once when a non-D3D11 session is seen. **Longer term:** add D3D12 via `ID3D12Device::CreateSharedHandle` + fence, or `D3D11On12`.

#### A7 ⚪ Singleton state assumes one instance/session
`OpenXRInterceptor` holds a single `m_instance`, device, and set of cached `m_pfn*`. If an app destroys its instance and creates a new one, the cached function pointers came from the old instance. **Fix:** hook `xrDestroyInstance` and clear the cached PFNs + `m_instance` there.

#### A8 ⚪ Dead IPC surface
`VR_IPC_MUTEX_NAME`, the frame event (`CreateEventW`, never waited on), `frame_index`/pose/FOV consumption (`GetFrameIndex()` is unused), and `is_d3d12`. Either use them (e.g. skip redundant draws when `frame_index` hasn't moved) or remove them to shrink the protocol. Bump `VR_IPC_VERSION` whenever the header changes, and have the consumer **check `version`** (it currently checks only `magic`).

### B. OBS plugin: OpenXR consumer path (`openxr_ipc_consumer.cpp`, `win-openvr.cpp`)

#### B1 🟠 Keyed-mutex protocol blanks frames and makes them flicker
[openxr_ipc_consumer.cpp:214-233](plugins/win-openvr/openxr_ipc_consumer.cpp:214)

`Render()` draws only if `AcquireSync(1, 0)` succeeds, and then releases with key 0. After one successful draw, **every further render call fails until the producer delivers a new frame**:
- OBS at 60 fps with a game at 45 fps (ASW) or with frame-pacing jitter → frames where the source draws nothing (flicker).
- OBS renders a source several times per frame (preview + program + projectors + Studio Mode). Only the first render succeeds, so the preview or the output goes blank.

**Fix:** under the keyed mutex, `CopyResource` the shared texture into a **consumer-owned** `gs_texture` (created on OBS's device with the same format/dims), then `ReleaseSync(0)`. Always draw the private copy. You get a stable last-good frame and the producer is unblocked immediately. Do the acquire+copy **once per OBS frame in `video_tick`** (or on first render per frame), not on every render call.

#### B2 🟠 Zoom / crop / aspect / offsets ignored for OpenXR; source size mismatched
[win-openvr.cpp:498-516, 577-584](plugins/win-openvr/win-openvr.cpp:498)

`get_width/height` return the cropped dimensions, but `Render()` draws the full texture at native size with `obs_source_draw(tex, 0, 0, 0, 0, false)`. With any crop set, OBS shows the top-left part of the eye (wrong region, and it ignores offsets and the left-eye inner-edge anchoring).
**Fix:** draw with `gs_draw_sprite_subregion(tex, 0, x, y, width, height)` using the same `recalculate_crop_dimensions_locked` output as the OpenVR path. After B1, you can also just copy only the crop box into the private texture.

#### B3 🟠 Multiple sources fight over the shared header
[win-openvr.cpp:518-542, 683-691](plugins/win-openvr/win-openvr.cpp:518), [openxr_ipc_consumer.cpp:140-167](plugins/win-openvr/openxr_ipc_consumer.cpp:140)

- A left-eye source and a right-eye source both write `requested_eye` every tick. The producer's single texture alternates eyes frame to frame.
- Hiding **any** source writes `obs_connected = 0` even if another source is still visible. `show()` sets `obs_connected = 1` even for sources locked to SteamVR mode, which makes the game copy frames nobody reads.

**Fix:** have the producer publish **both eyes** (two textures, or one side-by-side texture) so eye choice is purely consumer-side. Make "connected" a refcount across sources, or better, derive it from the consumer heartbeat alone and have one shared consumer object per process (a static, refcounted `OpenXrIpcConsumer`). Only touch OpenXR IPC when `engine_mode != OpenVR_SteamVR`.

#### B4 🟡 `OpenFileMappingW` every tick per source when no game is running
`IsProducerActive()` → `Initialize()` runs on every `video_tick` (60+ Hz × N sources) until a mapping exists. **Fix:** throttle it to about 1 Hz. This goes away entirely if OBS owns the mapping (A3).

#### B5 ⚪ Unlocked access to `m_sharedHeader`
`IsProducerActive()` and `SetConnected()` check/initialize `m_sharedHeader` outside `m_mutex`, while `show`/`hide` can run off the graphics thread. **Fix:** take the lock in both. Also check `version` and `active_backend` before trusting `shared_handle`.

### C. OBS plugin: OpenVR/SteamVR path (`win-openvr.cpp`)

#### C1 🟠 Zombie `VRSystem` after SteamVR quits → never reconnects until OBS restart
[win-openvr.cpp:293-305, 402-409, 724-729](plugins/win-openvr/win-openvr.cpp:293)

`vr_initialized` (the per-source refcount on `VR_Init`) is set as soon as `VR_Init` succeeds, even if the rest of init fails. The quit-teardown in `tick()` only runs `if (context->initialized && ...)`.

**Failure:** SteamVR is up and a source's `VR_Init` succeeds, but mirror acquisition fails (e.g. compositor not ready yet). That source has `vr_initialized = true` and `initialized = false`. SteamVR quits, and every fully-initialized source deinits, but this one never does. `s_openvr_instances` never reaches 0, so `VR_Shutdown()` is never called. When SteamVR comes back, `VRSystem()` is non-null (stale), `VR_Init` is skipped, and `GetMirrorTextureD3D11` fails forever.
**Fix:** in `tick()`, tear down when `context->vr_initialized && context->init_generation != s_openvr_generation`. Also record `init_generation` at the moment `VR_Init` is counted, not only on full success.

#### C2 🟡 Failed forced re-init leaves `initialized == true` → frozen frame + per-frame retry storm
[win-openvr.cpp:283-287, 726-729](plugins/win-openvr/win-openvr.cpp:283)

A mirror refresh (`mirror_generation` changed: game exited or switched) calls `init1(forced)`. If that fails, `on_failure` releases the mirror and resets `texCrop` **but leaves `initialized = true` and `mirror_generation` stale**. So:
- `render` keeps drawing the last frame (frozen image), and
- `tick` calls `init1(data, true)` **every tick**. `forced` bypasses the backoff, so `GetMirrorTextureD3D11` gets hammered at 60 Hz on the graphics thread.

**Fix:** `on_failure` sets `initialized = false`. The normal backoff path in `tick` then takes over. Optionally clear the displayed texture so the source shows black and not a stale frame.

#### C3 🟡 VR/D3D init runs on OBS's graphics thread
`win_openvr_init` (`VR_Init` IPC, `D3D11CreateDevice`, `GetMirrorTextureD3D11`) is called from `video_render` and `video_tick`. Each retry (every 0.5–4 s while SteamVR is down, or on every game switch) can stall OBS's render loop and drop frames.
**Fix:** move init to a per-plugin worker thread that publishes a ready "mirror bundle", and have render just pick it up. At minimum, remove the call from `video_render` (tick already retries).

#### C4 🟡 Cross-device sharing without synchronization; extra D3D device
The crop copy runs on a **private** D3D11 device, then `Flush()`. OBS samples the texture on **its** device via a legacy shared handle with no keyed mutex. `Flush` doesn't guarantee the GPU finished before OBS reads, so torn or partial frames are possible, especially across vendors. The private device is also created on the **default adapter**, while OBS and the HMD may be on different GPUs (laptops, multi-GPU), where `gs_texture_open_shared` fails.
**Fix (best):** drop the private device. Pass OBS's own device (`gs_get_device_obj()` inside `obs_enter_graphics`) to `GetMirrorTextureD3D11` and copy/draw with `gs_draw_sprite_subregion` directly. That removes the shared crop texture, `Flush`, the second device, and the adapter mismatch in one go. Log a warning if `VRSystem()->GetDXGIOutputInfo()` names a different adapter than OBS's.

#### C5 ⚪ Forced-OpenXR mode can still render a stale OpenVR mirror
If the source was initialized on OpenVR (Auto) and the user switches to "VDXR / OpenXR", `render` falls through to the OpenVR path whenever there's no OpenXR texture. **Fix:** in `update()`, when `engine_mode == OpenXR_VDXR`, deinit the OpenVR side (or gate the OpenVR render path on `active_engine`).

#### C6 ⚪ sRGB / format handling
The mirror/crop texture format is passed through as-is. It may be `_TYPELESS` or `_SRGB`, and OBS may not create a correct SRV for typeless formats. Modern OBS expects sources to declare `OBS_SOURCE_SRGB` and use `gs_effect_set_texture_srgb`. **Fix:** map typeless → the `_UNORM_SRGB` variant when creating the shared texture, and add `OBS_SOURCE_SRGB` handling. Verify on a light/dark test scene against the headset.

#### C7 ⚪ Lock ordering is fragile (not currently a live deadlock)
`update()` and `deinit()` take `context->mutex` and then `obs_enter_graphics`, while `render()` holds graphics and then takes `context->mutex`. Today this is safe only because OBS defers `update` for video sources to the tick (outside the graphics lock) and never renders a source that's being destroyed. Add a comment stating that invariant, or restructure `update()` to set a "dirty" flag that tick applies.

### D. Build, install, packaging

| # | Sev | Issue | Fix |
|---|---|---|---|
| D1 | 🟠 | `configure_file` writes `openxr-vrcapture-layer.json` to the build dir with `library_path: "./openxr-vrcapture-layer.dll"`, but multi-config MSVC puts the DLL in `build/.../Release/`. The installer finds that JSON first, so the manifest points at a missing DLL. | Use `file(GENERATE OUTPUT $<TARGET_FILE_DIR:...>/openxr-vrcapture-layer.json ...)` so the manifest lands beside the DLL. Add an `install()` rule. |
| D2 | 🟡 | The installer never removes older registrations at other paths, so you get duplicate layer entries after moving the folder. | Remove any existing `*openxr-vrcapture-layer.json` values before adding (reuse the uninstaller's loop). Verify the DLL exists before registering. |
| D3 | 🟡 | The README's build steps disagree with CMake. The README says to copy into the obs-studio tree, but `CMakeLists.txt` links against the in-repo `deps/obs/obs.lib` + headers of an unknown OBS version. Release packaging must also ship `openvr_api.dll` next to `win-openvr.dll`, and the README doesn't mention it. | Pin/document the OBS version the vendored `obs.lib`/headers came from, document one supported build path, and list `openvr_api.dll` in the release layout. |
| D4 | ⚪ | Redundant `#pragma comment(lib, "lib/win64/openvr_api.lib")` (path-relative and fragile) alongside the CMake link. | Link `openvr_api` in CMake and drop the pragmas. |
| D5 | ⚪ | Locale: `Custom` key missing from `en-US.ini`; `OpenVR` key unused; `get_name` is hardcoded. | Add `Custom=`, use `obs_module_text("OpenVR")` in `get_name`. |
| D6 | ⚪ | Unused `debug`/`info` macros, unused `retry_delayBUFFER` naming, unused `<cassert>` in release. | Cleanup. |

### E. Project hygiene

- **Not a git repository.** There's no history to diff or revert against. Run `git init` and make a baseline commit before any fixes.
- **No CI / no automated build.** Neither target is known to compile right now. A GitHub Actions `windows-latest` job that builds both the layer and the plugin would have caught A1 (after switching to the official headers).
- **Stale docs:** Performance-Audit.md / Implementation-Plan.md. Mark them superseded, or fold their "done" status into this file.

---

## Fix Plan

Ordered by dependency and value. Each phase is independently shippable.

### Phase 0: Baseline (≈½ day)
1. `git init`, add a `.gitignore` for `build*/`, and make a baseline commit.
2. Get both targets compiling locally (layer standalone, plugin against a pinned OBS SDK). Record the exact OBS version.
3. Optional: a CI workflow that builds both on `windows-latest`.

### Phase 1: Make OpenXR capture work at all (≈2–3 days)
Order matters. Each step unblocks the next.
1. **A1** Vendor official OpenXR-SDK headers and fix `Hook_xrCreateApiLayerInstance` validation.
2. **A2 + D1** Fix the manifest (`disable_environment`, correct `library_path` via `file(GENERATE)`). Fix the installer paths (D2).
3. **A3** Invert shared-memory ownership (OBS creates; layer polls `OpenFileMappingW` at 1 Hz). Add a `version` check.
4. **A4** Make the producer copy atomic under one lock with owned refs, and skip on dimension mismatch.
5. **B1** Consumer copies into a private texture under the keyed mutex once per frame, and always draws the private copy.
6. **B2** Apply crop/zoom/offsets to the OpenXR draw path.

*Verify:* the `XR_LOADER_DEBUG=all` log shows the layer loaded. A D3D11 OpenXR sample (e.g. `hello_xr -g D3D11`) shows in OBS in Auto mode. Start/stop the app 5× with no crash. Preview + program + a projector all show a stable image with no flicker at 45/72/90 Hz. Zoom/aspect/offsets behave the same as the SteamVR path.

### Phase 2: Robustness of both paths (≈2 days)
1. **C1** Zombie `VRSystem`: tear down on generation change based on `vr_initialized`.
2. **C2** `on_failure` clears `initialized`.
3. **B3** Producer publishes both eyes, plus a shared refcounted consumer.
4. **A5** Correct subresource index, skip MSAA, and move the copy into the release hook if testing shows tearing/garbage on any runtime.
5. **C5** Gate the OpenVR render on `active_engine`.
6. **B4/B5** Throttle and lock the consumer header access.

*Verify:* quit/restart SteamVR 5× with the source visible *and* hidden, and it reconnects every time. Switch games in SteamVR and the source follows. Two sources (L+R eye) show distinct, stable eyes on OpenXR. Toggle engine modes while running.

### Phase 3: Performance & architecture (≈2–3 days)
1. **C4** Use OBS's D3D device for the OpenVR mirror (removes the private device, the shared crop texture, and `Flush`). This is the largest simplification in the codebase.
2. **C3** Move VR init off the graphics thread (worker + handoff).
3. **C6** sRGB/typeless correctness.

*Verify:* OBS "Stats" shows no render-lag spikes while SteamVR is down or games are switching. Colors match the headset mirror (sRGB). Test on at least NVIDIA + AMD, and on a hybrid-GPU laptop if available.

### Phase 4: Scope & polish (as time allows)
- **A6** D3D12 capture (or document D3D11-only clearly now).
- **A7** Hook `xrDestroyInstance`.
- **A8 / D4–D6** Protocol and code cleanup, locale, README build/packaging refresh, and retire the stale audit docs.

---

## Suggested test matrix (manual, until there's a harness)

| Scenario | OpenVR | OpenXR (D3D11) |
|---|---|---|
| OBS starts before runtime / game | ✅ reconnects | ✅ detects within ~1 s |
| Runtime quits & restarts while source visible | C1 | A3/B1 |
| Same, source hidden | C1 | B3 |
| Game switch / resolution change | C2 | A4 |
| Scene cut in/out ×20 rapidly | no hitch | no hitch |
| Preview + Program + projector simultaneously | stable | B1 |
| Left + right eye sources together | ✅ | B3 |
| Zoom 5×, extreme offsets, custom aspect | ✅ | B2 |
| Non-D3D11 OpenXR app (D3D12/Vulkan) | n/a | no crash, logs "unsupported" |
| Any other OpenXR app with OBS closed | n/a | zero overhead, no crash (A1/A3) |
