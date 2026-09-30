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

target_link_libraries(openxr-vrcapture-layer PRIVATE
    d3d11
    dxgi
)

if(MSVC)
    target_compile_options(openxr-vrcapture-layer PRIVATE /W4 /O2 /GL)
    set_property(TARGET openxr-vrcapture-layer APPEND_STRING PROPERTY LINK_FLAGS_RELEASE " /LTCG /OPT:REF /OPT:ICF")
endif()

# Configure the OpenXR layer manifest
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/layer_manifest.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/openxr-vrcapture-layer.json
    @ONLY
)
