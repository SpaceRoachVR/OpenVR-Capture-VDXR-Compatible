#pragma once

#include <stdint.h>
#include <windows.h>
#include <dxgi.h>

#pragma pack(push, 8)

namespace vrcapture {

// Magic identifier for SpaceRoachVR / VR Capture IPC ("VRCP" = 0x56524350)
static constexpr uint32_t VR_IPC_MAGIC = 0x56524350;
// v2: the OBS consumer creates and owns the mapping; the layer only opens it.
// v3: one texture per eye, requested via an eye mask, so several OBS sources
//     (e.g. left + right eye) no longer fight over a single shared texture.
static constexpr uint32_t VR_IPC_VERSION = 3;

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
};

static constexpr uint32_t VR_IPC_EYE_COUNT = 2;

inline uint32_t EyeBit(VREyeSelection eye)
{
    return 1u << static_cast<uint32_t>(eye);
}

// Producer-owned, one per eye. Written only inside the header's seqlock.
struct VRSharedEyeTexture {
    uint64_t shared_handle;             // Legacy DXGI shared handle (castable to HANDLE), 0 = none
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t dxgi_format;               // DXGI_FORMAT
    uint32_t reserved0;

    // Metadata of the frame last copied into this texture
    float fov[4];                       // left, right, up, down (radians)
    float pose_orientation[4];          // x, y, z, w
    float pose_position[3];             // meters
    float reserved1;
};

struct VRSharedFrameHeader {
    uint32_t magic;                     // Must match VR_IPC_MAGIC
    uint32_t version;                   // Protocol version (VR_IPC_VERSION)

    // Seqlock guarding every producer-owned field below: the producer
    // increments it (odd = write in progress, even = stable) around each
    // update; a cross-process reader retries if it observes an odd value or the
    // value changes across the read, so it never pairs a new texture handle
    // with old dimensions, or a new frame_index with old metadata.
    volatile LONG seq;

    // ---- Producer (layer) -> OBS ----
    VRBackendType active_backend;       // OpenXR_VDXR while a capturable session is attached
    uint32_t is_d3d12;                  // 1 if source app was D3D12, 0 if D3D11

    VRSharedEyeTexture eyes[VR_IPC_EYE_COUNT]; // indexed by VREyeSelection

    uint64_t frame_index;               // Incremented per frame in which any eye was published
    int64_t display_time_ns;            // Predicted display time of that frame
    uint64_t last_producer_heartbeat;   // GetTickCount64() timestamp from game process

    // ---- OBS -> producer (layer) ----
    uint32_t requested_eye_mask;        // OR of EyeBit() for every visible OBS source
    uint32_t obs_connected;             // 1 while at least one OBS source is visible
    uint64_t last_consumer_heartbeat;   // GetTickCount64() timestamp from OBS
};

} // namespace vrcapture

#pragma pack(pop)
