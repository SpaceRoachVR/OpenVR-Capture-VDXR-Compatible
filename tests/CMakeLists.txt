cmake_minimum_required(VERSION 3.16)
project(vrcapture-tests LANGUAGES CXX)

# Standalone test build:
#   cmake -B build/tests -S tests && cmake --build build/tests && ctest --test-dir build/tests

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(ipc_handshake_test
	ipc_handshake_test.cpp
	${ROOT}/layers/openxr-vrcapture-layer/layer_ipc_producer.cpp
	${ROOT}/plugins/win-openvr/openxr_ipc_consumer.cpp
)
target_compile_features(ipc_handshake_test PRIVATE cxx_std_17)
target_compile_definitions(ipc_handshake_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ipc_handshake_test PRIVATE
	${ROOT}/layers/openxr-vrcapture-layer
	${ROOT}/plugins/win-openvr
	${ROOT}/shared
	${ROOT}/deps/obs/include
)
target_link_directories(ipc_handshake_test PRIVATE ${ROOT}/deps/obs)
# obs.dll is only needed by texture paths this test never calls; delay-loading
# lets it run on machines (and CI) without OBS installed.
target_link_libraries(ipc_handshake_test PRIVATE obs d3d11 dxgi delayimp)
target_link_options(ipc_handshake_test PRIVATE /DELAYLOAD:obs.dll)

enable_testing()
add_test(NAME ipc_handshake COMMAND ipc_handshake_test)

# Manual tool, not a ctest: impersonates an OpenXR game for testing OBS
# without a headset. See the header comment in fake_producer.cpp.
add_executable(fake_producer
	fake_producer.cpp
	${ROOT}/layers/openxr-vrcapture-layer/layer_ipc_producer.cpp
)
target_compile_features(fake_producer PRIVATE cxx_std_17)
target_compile_definitions(fake_producer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(fake_producer PRIVATE
	${ROOT}/layers/openxr-vrcapture-layer
	${ROOT}/shared
)
target_link_libraries(fake_producer PRIVATE d3d11 dxgi)
