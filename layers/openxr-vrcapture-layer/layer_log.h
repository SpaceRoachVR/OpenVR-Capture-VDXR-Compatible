#pragma once

// Diagnostic log for the capture layer. The layer runs inside every OpenXR
// game, where nothing else is visible, so it records what happened at each
// step (loaded, session/graphics API, swapchains, OBS attach, first frames) to
//     %LOCALAPPDATA%\SpaceRoachVR\openxr-vrcapture-layer.log
// Only state changes and first occurrences are logged -- never per frame.
// Disabled until the layer is actually negotiated by an OpenXR loader, so unit
// tests and tools that link the producer stay silent.

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vrcapture {

inline std::atomic<bool> &LayerLogEnabled()
{
    static std::atomic<bool> enabled{false};
    return enabled;
}

inline void LayerLog(const char *fmt, ...)
{
    if (!LayerLogEnabled().load()) {
        return;
    }

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    static wchar_t s_path[MAX_PATH] = {};
    static char s_exe[MAX_PATH] = {};
    if (!s_path[0]) {
        wchar_t dir[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            return;
        }
        wcscat_s(dir, L"\\SpaceRoachVR");
        CreateDirectoryW(dir, nullptr);
        swprintf_s(s_path, L"%s\\openxr-vrcapture-layer.log", dir);

        char full[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, full, MAX_PATH);
        const char *base = strrchr(full, '\\');
        strcpy_s(s_exe, base ? base + 1 : full);

        // Keep the file bounded: start over once it passes 1 MB.
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (GetFileAttributesExW(s_path, GetFileExInfoStandard, &info) && info.nFileSizeLow > 1024 * 1024) {
            DeleteFileW(s_path);
        }
    }

    FILE *f = nullptr;
    if (_wfopen_s(&f, s_path, L"a") != 0 || !f) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s:%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds, s_exe, GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

} // namespace vrcapture

// Logs only the first time this line is reached (per process).
#define LAYER_LOG_ONCE(...)                                         \
    do {                                                            \
        static std::atomic<bool> s_logged_{false};                  \
        if (!s_logged_.exchange(true)) {                            \
            ::vrcapture::LayerLog(__VA_ARGS__);                     \
        }                                                           \
    } while (0)
