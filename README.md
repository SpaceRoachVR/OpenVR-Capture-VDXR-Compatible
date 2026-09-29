# VR Capture for OBS Studio (SpaceRoach Edition)

VR capture source for OBS Studio with support for both **OpenVR (SteamVR)** and **OpenXR / VDXR (Virtual Desktop OpenXR)**.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen, expanded for modern OpenXR runtimes and the Virtual Desktop ecosystem.

![obs64_4E9advFPF8](https://github.com/user-attachments/assets/98e52da2-f58d-4a63-a975-b07704e4a4e9)

---

### Features

- **Two capture engines**
  - **OpenXR / VDXR:** captures the game's own swapchain through an OpenXR API layer, for games on Virtual Desktop (VDXR), Meta Quest Link and other OpenXR runtimes, without going through SteamVR.
  - **OpenVR / SteamVR:** captures the SteamVR compositor's per-eye mirror texture.
  - **Auto:** uses OpenXR when a capturable OpenXR game is running, SteamVR otherwise.
- **Left or right eye per source.** Several sources (e.g. one per eye) can capture the same game at once.
- **Framing:** Native, 16:9, 4:3 and custom aspect ratios, zoom (1.0x–5.0x), horizontal/vertical offsets. The same settings work for both engines.
- **GPU-only frame path:** frames are copied GPU-to-GPU. OpenXR frames are handed to OBS through a shared texture synchronized with `IDXGIKeyedMutex`; OBS always draws its own copy of the latest frame, so preview, program and projectors never flicker.
- **Colour-correct in OBS's linear pipeline** (`OBS_SOURCE_SRGB`): sRGB content is decoded as sRGB, linear (UNORM/float) OpenXR swapchains as linear.
- **Diagnostics:** the layer writes `%LOCALAPPDATA%\SpaceRoachVR\openxr-vrcapture-layer.log` (whether it loaded into a game, the session's graphics API, OBS connection, first frames, and why frames were skipped); the OBS log shows engine switches and when OpenXR frames arrive.
- **Low idle cost:** OpenXR games only talk to OBS while a VR Capture source is visible, and SteamVR is only probed while it's actually running.

#### What can be captured

| Engine | Supported | Not supported |
|---|---|---|
| OpenXR layer | D3D11 and D3D12 games (D3D12 via D3D11On12) | Vulkan and OpenGL games, MSAA swapchains |
| SteamVR | Any SteamVR game (compositor mirror) | OBS set to a non-D3D11 renderer; SteamVR and OBS on different GPUs |

The OBS plugin and the OpenXR layer speak a versioned protocol. **Always install both from the same build.** A mismatched pair does nothing rather than misbehave.

---

### Installation

A release contains:

```
obs-plugins/64bit/win-openvr.dll
obs-plugins/64bit/openvr_api.dll        <- required; from the OpenVR SDK (bin/win64)
data/obs-plugins/win-openvr/locale/en-US.ini
openxr-layer/openxr-vrcapture-layer.dll
openxr-layer/openxr-vrcapture-layer.json
scripts/Install-OpenXR-Layer.ps1
scripts/Uninstall-OpenXR-Layer.ps1
```

1. Copy `obs-plugins` and `data` into your OBS Studio folder (e.g. `C:\Program Files\obs-studio`).
2. For **OpenXR / VDXR** capture, run `scripts\Install-OpenXR-Layer.ps1` (right-click → Run with PowerShell) and approve the administrator prompt. It copies the layer to `C:\Program Files\SpaceRoachVR\OpenXR Capture Layer` and registers it for all users (HKLM). Machine-wide registration matters: Virtual Desktop's launch path for games built on Meta's OVRPlugin only loads HKLM layers. `-CurrentUser` registers per-user (HKCU) without admin instead, but those games won't see it. `Uninstall-OpenXR-Layer.ps1` removes either kind.
3. To stop the layer loading into one particular app, set the environment variable `DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE=1` for that app.

---

### Building

Requirements: Visual Studio 2022 (MSVC, x64) and CMake ≥ 3.16. Everything else is in `deps/`:

| Dependency | Version | Location |
|---|---|---|
| OBS SDK headers + import lib | libobs 32.2.1 (works with OBS 32.2.x) | `deps/obs` |
| OpenVR SDK headers + import lib | 2.15.6 | `deps/openvr` |
| OpenXR headers | OpenXR-SDK release-1.1.63 (official, unmodified) | `deps/openxr` |

Each part is a standalone CMake project:

```bash
cmake -B build/plugin -S plugins/win-openvr
cmake --build build/plugin --config Release

cmake -B build/layers -S layers/openxr-vrcapture-layer
cmake --build build/layers --config Release

cmake -B build/tests -S tests
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release
```

The layer build writes `openxr-vrcapture-layer.json` next to the DLL, and `Install-OpenXR-Layer.ps1` finds build outputs automatically. `openvr_api.dll` is not in the repo; take it from the [OpenVR SDK](https://github.com/ValveSoftware/openvr) `bin/win64` matching the header version above.

The plugin can also be built inside an OBS source tree: copy `plugins/win-openvr` into `obs-studio/plugins/` and add `add_obs_plugin(win-openvr PLATFORMS WINDOWS)`. It links `libobs` there automatically.

---

### Testing without a headset

- `ipc_handshake_test` covers the OBS ↔ layer protocol: attach/detach, per-eye requests from several sources, hide/show, and the keyed-mutex handoff with pixel checks. It needs neither OBS nor a headset.
- `d3d12_capture_test` covers the D3D12 path: a D3D12 image wrapped through D3D11On12 on WARP, copied by the real producer and read back per eye.
- `fake_producer [fps] [width] [height]` stands in for an OpenXR game. With OBS running and a VR Capture source visible (engine Auto or VDXR / OpenXR), it streams an animated side-by-side test pattern (left eye blue marker, right eye red) through the real IPC path. Run it at an fps below OBS's (e.g. `30`) to check there's no flicker.

See `OpenVR-Capture_Correctness-Audit.md` for the audit these fixes came from and a manual test matrix for headset testing.
