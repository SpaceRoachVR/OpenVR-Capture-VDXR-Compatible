#pragma once

#include <stdint.h>
#include <windows.h>
#include <dxgi.h>

#pragma pack(push, 8)

namespace vrcapture {

// Magic identifier for SpaceRoachVR / VR Capture IPC ("VRCP" = 0x56524350)
static constexpr uint32_t VR_IPC_MAGIC = 0x56524350;
// v2: the OBS consumer creates and owns the mapping; the layer only opens it.
static constexpr uint32_t VR_IPC_VERSION = 2;

// Upper bound on texture_width/texture_height accepted from the shared header.
// The header is written by another process, so dimensions must be sanity-checked
// before being used to size/allocate anything on the consumer side.
static constexpr uint32_t VR_IPC_MAX_TEXTURE_DIM = 16384;

// Shared memory name. Ownership: the OBS plugin creates the mapping (and fills
// in magic/version) when an OpenXR-capable source exists. The OpenXR layer only
// ever opens it, polling at a low rate, so OpenXR apps pay nothing while OBS
// isn't capturing. magic is written last by the creator; a reader that sees a
// valid magic + matching version can trust the rest of the layout.
static const wchar_t *const VR_IPC_SHARED_MEMORY_NAME = L"Local\\SpaceRoachVR_Capture_IPC";

enum class VRBackendType : uint32_t {
    Inactive = 0,
    OpenXR_VDXR = 1,
    OpenVR_SteamVR = 2
};

enum class VREyeSelection : uint32_t {
    Left = 0,
    Right = 1,
    StereoBoth = 2
};

struct VRSharedFrameHeader {
    uint32_t magic;                     // Must match VR_IPC_MAGIC
    uint32_t version;                   // Protocol version (VR_IPC_VERSION)

    // Seqlock guarding the "Frame tracking & timing" and "6-DoF Head Pose" blocks
    // below: producer increments this (odd = write in progress, even = stable)
    // around each update; a cross-process reader retries if it observes an odd
    // value or the value changes across the read, so it never sees a torn mix of
    // an old pose paired with a new frame_index (or vice versa).
    volatile LONG seq;

    // Backend state
    VRBackendType active_backend;       // 1 = OpenXR/VDXR, 2 = OpenVR
    uint32_t is_d3d12;                  // 1 if source app was D3D12, 0 if D3D11

    // Texture information for OBS consumption
    uint64_t shared_handle;             // Windows DXGI Shared Handle (castable to HANDLE)
    uint32_t texture_width;             // Native resolution width
    uint32_t texture_height;            // Native resolution height
    uint32_t dxgi_format;               // DXGI_FORMAT (e.g. DXGI_FORMAT_R8G8B8A8_UNORM)

    // Frame tracking & timing
    uint64_t frame_index;               // Incremented per submitted frame
    int64_t display_time_ns;            // Predicted display time in nanoseconds
    uint64_t last_producer_heartbeat;   // GetTickCount64() timestamp from game process

    // VR Field of View (angles in radians)
    float fov_left;                     // Left tangent
    float fov_right;                    // Right tangent
    float fov_up;                       // Up tangent
    float fov_down;                     // Down tangent

    // 6-DoF Head Pose (Quaternion + Position in meters)
    float pose_orientation[4];          // x, y, z, w
    float pose_position[3];             // x, y, z

    // Control parameters from OBS -> Layer
    uint32_t requested_eye;             // VREyeSelection (0 = Left, 1 = Right, 2 = Both)
    uint32_t obs_connected;             // 1 if OBS source is active and reading, 0 otherwise
    uint64_t last_consumer_heartbeat;   // GetTickCount64() timestamp from OBS
};

} // namespace vrcapture

#pragma pack(pop)
