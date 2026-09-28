# VR Capture for OBS Studio (SpaceRoach Edition)

High-performance, zero-copy VR capture plugin for OBS Studio with native support for both **OpenVR (SteamVR)** and **OpenXR / VDXR (Virtual Desktop OpenXR)**.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen, expanded for modern OpenXR runtimes and the Virtual Desktop ecosystem.

![obs64_4E9advFPF8](https://github.com/user-attachments/assets/98e52da2-f58d-4a63-a975-b07704e4a4e9)

---

### Features & Improvements

- **Dual-Engine Support:**
  - **VDXR & Native OpenXR:** Captures OpenXR games running over Virtual Desktop (VDXR), Meta Quest Link OpenXR, and standard OpenXR runtimes directly from the game's swapchain without SteamVR overhead.
  - **OpenVR / SteamVR:** High-performance compositor mirror capture with automatic device persistence.
  - **Auto-Detection:** Automatically switches between active VR runtimes.
- **Real-Time Aspect Ratio & Cropping:**
  - Native, 16:9, 4:3, and Custom aspect ratio cropping.
  - Smooth Zoom (1.0x – 5.0x) and Horizontal/Vertical offsets.
- **Zero-Copy DXGI Texture Sharing:**
  - Synchronized via `IDXGIKeyedMutex` for tear-free, non-blocking frame delivery.
- **Threaded Re-entrancy & Exponential Backoff:**
  - Eliminates OBS UI hitching when games start, stop, or scenes change.

---

### Installation

1. Download the latest release `.zip`.
2. Extract the `obs-plugins` and `data` folders to the root of your OBS Studio installation (e.g. `C:\Program Files\obs-studio`).
3. For **VDXR / OpenXR** capture, run `scripts/Install-OpenXR-Layer.ps1` (Right-click -> Run with PowerShell) to register the OpenXR capture layer.

---

### Compiling

#### 1. Building the OBS Plugin (`win-openvr` / `win-vrcapture`)
1. Pull OBS Studio source code recursively:
   ```bash
   git clone https://github.com/obsproject/obs-studio.git --recursive
   ```
2. Copy `plugins/win-openvr` into `obs-studio/plugins/win-openvr`.
3. Pull OpenVR SDK into `obs-studio/deps/openvr`:
   ```bash
   git clone https://github.com/ValveSoftware/openvr.git ./deps/openvr
   ```
4. Add `add_obs_plugin(win-openvr PLATFORMS WINDOWS)` to `obs-studio/plugins/CMakeLists.txt`.
5. Compile with:
   ```bash
   cmake --preset windows-x64
   cmake --build ./build_x64/plugins/win-openvr --config Release
   ```

#### 2. Building the OpenXR API Layer (`openxr-vrcapture-layer`)
1. In the repository root:
   ```bash
   cmake -B build/layers -S layers/openxr-vrcapture-layer
   cmake --build build/layers --config Release
   ```
2. Register the compiled layer with `scripts/Install-OpenXR-Layer.ps1`.

#### 3. Tests & headset-free testing
```bash
cmake -B build/tests -S tests
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release
```
- `ipc_handshake_test` exercises the OBS <-> layer shared-memory protocol and keyed-mutex frame handoff (no OBS or headset needed).
- `fake_producer [fps] [width] [height]` impersonates an OpenXR game: with OBS running and a VR Capture source visible (engine Auto or VDXR/OpenXR), it streams an animated test pattern through the real IPC path.
