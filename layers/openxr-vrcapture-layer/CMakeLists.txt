cmake_minimum_required(VERSION 3.16)
project(openxr-vrcapture-layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAYER_SOURCES
    layer_main.cpp
    openxr_interceptor.cpp
    layer_ipc_producer.cpp
)

add_library(openxr-vrcapture-layer SHARED ${LAYER_SOURCES})

target_include_directories(openxr-vrcapture-layer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openxr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

# Required by the official openxr_platform.h to expose the D3D11 binding types.
target_compile_definitions(openxr-vrcapture-layer PRIVATE
    XR_USE_PLATFORM_WIN32
    XR_USE_GRAPHICS_API_D3D11
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(openxr-vrcapture-layer PRIVATE
    d3d11
    dxgi
)

if(MSVC)
    target_compile_options(openxr-vrcapture-layer PRIVATE /W4 /O2 /GL)
    set_property(TARGET openxr-vrcapture-layer APPEND_STRING PROPERTY LINK_FLAGS_RELEASE " /LTCG /OPT:REF /OPT:ICF")
endif()

# Emit the manifest next to the DLL for every configuration, so its relative
# library_path ("./openxr-vrcapture-layer.dll") resolves. With multi-config
# generators the DLL lands in e.g. build/Release/, not the binary dir root.
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:openxr-vrcapture-layer>/openxr-vrcapture-layer.json"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/layer_manifest.json.in"
)

install(TARGETS openxr-vrcapture-layer RUNTIME DESTINATION .)
install(FILES "$<TARGET_FILE_DIR:openxr-vrcapture-layer>/openxr-vrcapture-layer.json" DESTINATION .)
