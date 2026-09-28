cmake_minimum_required(VERSION 3.16)
project(win-openvr LANGUAGES CXX)

set(win-openvr_SOURCES
	win-openvr.cpp
	openxr_ipc_consumer.cpp
)

add_library(win-openvr MODULE ${win-openvr_SOURCES})

target_compile_features(win-openvr PRIVATE cxx_std_17)

target_include_directories(
	win-openvr PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../../shared
	${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openvr
	${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openvr/headers
	${CMAKE_CURRENT_SOURCE_DIR}/../../deps/obs/include
)

if(MSVC)
	target_compile_options(win-openvr PRIVATE /W4)
	set_property(TARGET win-openvr APPEND_STRING PROPERTY COMPILE_FLAGS_RELEASE " /O2 /GL")
	set_property(TARGET win-openvr APPEND_STRING PROPERTY LINK_FLAGS_RELEASE " /LTCG /OPT:REF /OPT:ICF")
	target_link_directories(win-openvr PRIVATE 
		${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openvr
		${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openvr/lib/win64
		${CMAKE_CURRENT_SOURCE_DIR}/../../deps/obs
	)
endif()

if(TARGET libobs)
	target_link_libraries(win-openvr PRIVATE libobs d3d11 dxgi openvr_api)
else()
	target_link_libraries(win-openvr PRIVATE obs d3d11 dxgi openvr_api)
endif()

if(COMMAND set_target_properties_obs)
	set_target_properties_obs(win-openvr PROPERTIES FOLDER plugins)
else()
	set_target_properties(win-openvr PROPERTIES
		PREFIX ""
		OUTPUT_NAME "win-openvr"
	)
endif()