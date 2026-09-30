# VR Capture for OBS Studio — SpaceRoach Edition (VDXR compatible)

Put your VR game straight into OBS at full resolution: a clean, per-eye view of what's rendered in the headset, without the black borders, lens-distorted edges or low-resolution desktop mirror window.

Works with **SteamVR** games and with **OpenXR games running on Virtual Desktop (VDXR)**, including Quest games launched through Virtual Desktop that never touch SteamVR.

![VR Capture in OBS](https://github.com/user-attachments/assets/98e52da2-f58d-4a63-a975-b07704e4a4e9)

---

## What you get

- **A "VR Capture" source in OBS** that shows one eye of your VR game, at the resolution the game renders.
- **Automatic runtime detection.** In *Auto* mode the source uses the VDXR/OpenXR capture while an OpenXR game is running and SteamVR otherwise. You don't switch anything when you change games.
- **Stream-ready framing.** One click for **16:9**, which keeps the widest view the eye image allows, centered on where you're looking. Zoom and offsets let you fine-tune it.
- **Smooth and colour-accurate.** Frames are copied GPU-to-GPU with no black or torn frames, and colours match the headset. Preview, Program and projectors all show the same image.
- **Left, right or both eyes.** Add two sources for side-by-side or eye switching.
- **Light on your system.** The capture layer stays idle inside your games (a once-a-second check for OBS) and only copies frames while OBS has a visible VR Capture source.

## What works

| You play... | Captured? |
|---|---|
| SteamVR games (any headset connected through SteamVR) | ✅ Yes, choose *SteamVR* or *Auto* |
| Games on **Virtual Desktop with VDXR** (Quest), including Meta/Oculus-plugin games like POPULATION ONE | ✅ Yes, choose *VDXR / OpenXR* or *Auto* |
| Other OpenXR runtimes (e.g. Quest Link's OpenXR) | ✅ Should work. The capture hooks into the game, not the runtime. |
| OpenXR games that render with **Vulkan or OpenGL** | ❌ Not yet (Direct3D 11 and 12 games only) |

Needs Windows 10/11 (64-bit) and OBS Studio 32 (tested with 32.2). Running OBS as administrator is fine.

---

## Install

Download the latest release zip from the [Releases page](../../releases) and unzip it. It has two parts:

### 1. The OBS plugin

1. **Close OBS.**
2. Copy the `win-openvr` folder into `C:\ProgramData\obs-studio\plugins\`
   (create the `plugins` folder if it doesn't exist). You should end up with:
   ```
   C:\ProgramData\obs-studio\plugins\win-openvr\bin\64bit\win-openvr.dll
   C:\ProgramData\obs-studio\plugins\win-openvr\bin\64bit\openvr_api.dll
   C:\ProgramData\obs-studio\plugins\win-openvr\data\locale\en-US.ini
   ```
   This location survives OBS updates. If you installed an older OpenVR capture plugin into `C:\Program Files\obs-studio\obs-plugins\64bit\`, delete its `win-openvr.dll` there so OBS doesn't load two copies.
3. Start OBS.

### 2. The OpenXR / VDXR capture layer (for Virtual Desktop and other OpenXR games)

Skip this if you only use SteamVR.

1. **Close any VR games.**
2. In the `openxr-layer` folder, right-click **`Install-OpenXR-Layer.ps1`** → **Run with PowerShell**, and approve the administrator prompt.
   It installs the layer to `C:\Program Files\SpaceRoachVR\OpenXR Capture Layer` and registers it for all games.
3. Start your game (a game that was already running won't pick it up).

> **Why administrator?** Virtual Desktop only loads capture layers registered for the whole machine when it launches Oculus-plugin games. A per-user install silently never loads in those games.

**Always install the plugin and the layer from the same release.** They're a matched pair; a mismatched pair simply won't connect.

---

## Quick start

1. In OBS, click **+** under *Sources* → **VR Capture**.
2. Leave **Capture Engine** on **Auto (Detect Active Engine)**.
3. Set **Aspect Ratio** to **16:9** for a stream-shaped picture.
4. Start your game. The source switches to it within a second or so.
5. Select the source and press **Ctrl+F** (*Fit to screen*) so it fills your canvas.

## Settings

| Setting | What it does |
|---|---|
| **Capture Engine** | **Auto** picks VDXR/OpenXR when an OpenXR game is running, SteamVR otherwise. Or force **VDXR / OpenXR** or **SteamVR (OpenVR)**. |
| **Right Eye** | Checked = right eye, unchecked = left eye. Two sources, one per eye, work fine together. |
| **Aspect Ratio** | **Native** shows the whole eye image (usually nearly square or taller than wide). **16:9** / **4:3** / **Custom** crop to that shape, keeping the widest possible view. |
| **Ratio Width / Height** | The shape used by *Custom* (e.g. 21 : 9). |
| **Zoom** | 1.0 = widest view. Higher values crop in toward the center of your view. |
| **Horizontal / Vertical Offset** | Nudge the framing in pixels. The horizontal offset is mirrored for the left eye, so a left/right pair moves symmetrically. |

The crop is centered on each eye's actual view direction, not the middle of the image. VR lenses see further to the outside than toward the nose, so this keeps the horizon and your focus point where you'd expect.

---

## Troubleshooting

**The source is black with Virtual Desktop / OpenXR games**
- Make sure OBS is open and the VR Capture source is **visible** in the current scene. The game only sends frames while it is.
- Make sure you ran **`Install-OpenXR-Layer.ps1`** and approved the admin prompt, then **restarted the game**.
- Check the layer's log: `%LOCALAPPDATA%\SpaceRoachVR\openxr-vrcapture-layer.log` (paste that into Explorer's address bar).
  - **No log file, or no entry for your game:** the layer didn't load. Re-run the installer as administrator.
  - **"no D3D11/D3D12 graphics binding":** the game uses Vulkan/OpenGL, which isn't supported yet.
  - **"attached to OBS" but no "first frame":** the log says why frames were skipped.
- The OBS log (*Help → Log Files → View Current Log*) shows `OpenXR game detected` and `receiving OpenXR frames` when everything is connected.

**The source is black with SteamVR games**
- SteamVR must be running, with a game or SteamVR Home.
- OBS must use its default **Direct3D 11** renderer (*Settings → Advanced → Renderer*).
- On PCs with two GPUs, OBS and SteamVR must run on the same GPU.

**The picture is too tall / not the shape I want**
- Set **Aspect Ratio** to **16:9** and press **Ctrl+F** on the source. *Native* is the eye's own shape, which isn't 16:9.

**Colours look washed out or too bright compared to the headset**
- Most games look identical. If one doesn't, please open an issue with the game's name; some games label their colour format in a way that needs special handling.

**Excluding a game from the capture layer**
- Set the environment variable `DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE=1` for that game, or remove the layer completely with `Uninstall-OpenXR-Layer.ps1`.

---

## Credits

- Original *OpenVR Capture* OBS plugin by **Keijo "Kegetys" Ruotsalainen**.
- Maintained as [baffler/OBS-OpenVR-Input-Plugin](https://github.com/baffler/OBS-OpenVR-Input-Plugin), then forked as [Pigney/OpenVR-Capture](https://github.com/Pigney/OpenVR-Capture).
- **SpaceRoach Edition:** OpenXR/VDXR capture layer, Auto engine detection, 16:9 framing, and a correctness overhaul, by [SpaceRoachVR](https://github.com/SpaceRoachVR).

Licensed under the GNU General Public License v2.0. See [LICENSE](LICENSE).

---

## For developers

<details>
<summary>Building, tests and design notes</summary>

### How it works

- **SteamVR:** the plugin opens the SteamVR compositor's per-eye mirror texture on OBS's own D3D11 device and copies each new frame into an OBS texture.
- **OpenXR:** an implicit OpenXR API layer (`layers/openxr-vrcapture-layer`) runs inside the game. When OBS has a visible VR Capture source, it copies the submitted eye views into shared D3D11 textures (D3D12 games through D3D11On12), synchronized with `IDXGIKeyedMutex`. It talks to OBS through a small shared-memory header (`shared/vr_ipc_types.h`) that OBS creates. The OBS side copies each new frame into its own texture, so rendering never waits on or tears against the game.

### Building

Requirements: Visual Studio 2022 (MSVC, x64) and CMake 3.16+. Dependencies are vendored in `deps/`:

| Dependency | Version |
|---|---|
| OBS SDK headers + import lib | libobs 32.2.1 |
| OpenVR SDK headers + import lib | 2.15.6 |
| OpenXR headers | OpenXR-SDK release-1.1.63 (official, unmodified) |

```bash
cmake -B build/plugin -S plugins/win-openvr
cmake --build build/plugin --config Release

cmake -B build/layers -S layers/openxr-vrcapture-layer
cmake --build build/layers --config Release

cmake -B build/tests -S tests
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release
```

`openvr_api.dll` isn't in the repo; take it from the [OpenVR SDK](https://github.com/ValveSoftware/openvr) `bin/win64` at the version above. For local testing, `scripts/Install-OBS-Plugin.ps1` installs a fresh plugin build into OBS (OBS closed), and `scripts/Install-OpenXR-Layer.ps1` picks up the newest layer build. The CI workflow builds everything, runs the tests, and uploads a release-shaped zip.

### Tests and tools

- `ipc_handshake_test`: OBS ↔ layer protocol, per-eye requests from several sources, hide/show, and the keyed-mutex handoff with pixel checks. Needs neither OBS nor a headset (close OBS first).
- `d3d12_capture_test`: D3D12 capture through D3D11On12 on a WARP device.
- `crop_math_test`: aspect ratio, zoom, view-centered crop and offsets.
- `fake_producer [fps] [width] [height] [srgb|unorm]`: pretends to be an OpenXR game and streams a side-by-side test pattern into OBS, for testing without a headset.

`OpenVR-Capture_Correctness-Audit.md` records the correctness audit, the field-test findings and a manual test matrix.

</details>
