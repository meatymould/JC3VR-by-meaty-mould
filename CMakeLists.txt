cmake_minimum_required(VERSION 3.24)
project(JC3VR VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(jc3vr_core STATIC
    src/core/logging.cpp
    src/core/config.cpp
    src/core/build_fingerprint.cpp
    src/core/signatures.cpp
    src/core/resource_crash_trace.cpp
    src/openxr/xr_loader.cpp
    src/openxr/xr_instance.cpp
    src/render/swapchain.cpp
    src/game/camera_probe.cpp
    src/game/head_look.cpp
    src/game/vr_gamepad.cpp
    src/render/vr_hands.cpp
    src/render/controller_model.cpp
    src/game/grapple_probe.cpp
    src/game/camera_source_scan.cpp
    src/game/vr_body.cpp
    src/game/vr_body_rig.cpp
    src/game/engine_culling.cpp
    src/game/engine_lighting.cpp
    src/game/vr_weapons.cpp
    src/game/vehicle_camera.cpp
    src/game/render_character_root.cpp
    src/game/dual_wield.cpp
    src/game/vr_c4.cpp
    src/game/native_grab_probe.cpp
    src/game/vr_grapple.cpp
    src/game/weapon_watch.cpp
    src/game/weapon_attachment_probe.cpp
    src/game/hijack_state_probe.cpp
    src/game/physical_hijack.cpp
    src/game/hud_snapshot.cpp
)
target_include_directories(jc3vr_core PUBLIC src)
target_compile_definitions(jc3vr_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(jc3vr_core PUBLIC bcrypt d3d11 dxgi d3dcompiler dxguid windowscodecs ole32)

set(JC3VR_OPENXR_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")
if (EXISTS "${JC3VR_OPENXR_HEADERS}/openxr/openxr.h")
    target_sources(jc3vr_core PRIVATE src/openxr/xr_session.cpp)
    target_include_directories(jc3vr_core PRIVATE "${JC3VR_OPENXR_HEADERS}")
    target_compile_definitions(jc3vr_core PUBLIC JC3VR_HAVE_OPENXR_HEADERS=1)
endif()

set(JC3VR_MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
if (EXISTS "${JC3VR_MINHOOK_DIR}/CMakeLists.txt")
    add_subdirectory("${JC3VR_MINHOOK_DIR}" "${CMAKE_BINARY_DIR}/minhook" EXCLUDE_FROM_ALL)
    target_sources(jc3vr_core PRIVATE src/render/dx11_hook.cpp)
    target_sources(jc3vr_core PRIVATE src/game/camera_capture.cpp)
    target_link_libraries(jc3vr_core PUBLIC minhook)
    target_compile_definitions(jc3vr_core PUBLIC JC3VR_HAVE_MINHOOK=1)
endif()

add_library(jc3vr SHARED src/core/dllmain.cpp)
target_link_libraries(jc3vr PRIVATE jc3vr_core)
target_compile_definitions(jc3vr PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(jc3vr PROPERTIES OUTPUT_NAME "jc3vr")

add_library(dinput8_proxy SHARED src/proxy/dinput8_proxy.cpp src/proxy/dinput8.def)
target_compile_definitions(dinput8_proxy PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dinput8_proxy PRIVATE dinput8)
set_target_properties(dinput8_proxy PROPERTIES OUTPUT_NAME "dinput8" ARCHIVE_OUTPUT_NAME "dinput8_proxy")

add_executable(jc3vr_core_tests tests/core_tests.cpp)
target_link_libraries(jc3vr_core_tests PRIVATE jc3vr_core)
target_compile_definitions(jc3vr_core_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(jc3vr_dll_smoke_tests tests/dll_smoke_tests.cpp)
add_dependencies(jc3vr_dll_smoke_tests jc3vr)
target_compile_definitions(jc3vr_dll_smoke_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(dinput8_proxy_smoke_tests tests/dinput8_proxy_smoke_tests.cpp)
add_dependencies(dinput8_proxy_smoke_tests dinput8_proxy jc3vr)
target_compile_definitions(dinput8_proxy_smoke_tests PRIVATE DIRECTINPUT_VERSION=0x0800 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dinput8_proxy_smoke_tests PRIVATE dinput8)

enable_testing()
add_executable(render_character_root_tests tests/render_character_root_tests.cpp)
target_include_directories(render_character_root_tests PRIVATE src)
target_compile_definitions(render_character_root_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME render_character_root_tests COMMAND render_character_root_tests)
add_test(NAME jc3vr_core_tests COMMAND jc3vr_core_tests)
add_test(NAME jc3vr_dll_smoke_tests COMMAND jc3vr_dll_smoke_tests)
add_test(NAME dinput8_proxy_smoke_tests COMMAND dinput8_proxy_smoke_tests)

add_executable(creator_mirror_tests tests/creator_mirror_tests.cpp)
target_link_libraries(creator_mirror_tests PRIVATE jc3vr_core)
target_compile_definitions(creator_mirror_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME creator_mirror_tests COMMAND creator_mirror_tests)

add_executable(dual_aim_tests tests/dual_aim_tests.cpp)
target_include_directories(dual_aim_tests PRIVATE src)
target_compile_definitions(dual_aim_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME dual_aim_tests COMMAND dual_aim_tests)

# Compile the actual dispatcher in a separate test translation unit. Its native
# consumers are stand-ins; no production test exports or game process needed.
add_executable(dual_dispatch_tests tests/dual_dispatch_tests.cpp)
target_include_directories(dual_dispatch_tests PRIVATE src)
target_compile_definitions(dual_dispatch_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN JC3VR_HAVE_MINHOOK=0)
add_test(NAME dual_dispatch_tests COMMAND dual_dispatch_tests)

add_executable(weapon_attachment_probe_tests tests/weapon_attachment_probe_tests.cpp)
target_include_directories(weapon_attachment_probe_tests PRIVATE src)
target_compile_definitions(weapon_attachment_probe_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN JC3VR_HAVE_MINHOOK=0)
add_test(NAME weapon_attachment_probe_tests COMMAND weapon_attachment_probe_tests)

add_executable(helicopter_pull_tests tests/helicopter_pull_tests.cpp)
target_include_directories(helicopter_pull_tests PRIVATE src)
add_test(NAME helicopter_pull_tests COMMAND helicopter_pull_tests)

add_executable(hijack_state_probe_tests tests/hijack_state_probe_tests.cpp)
target_include_directories(hijack_state_probe_tests PRIVATE src)
target_compile_definitions(hijack_state_probe_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME hijack_state_probe_tests COMMAND hijack_state_probe_tests)

add_executable(physical_hijack_tests tests/physical_hijack_tests.cpp)
target_include_directories(physical_hijack_tests PRIVATE src)
target_compile_definitions(physical_hijack_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME physical_hijack_tests COMMAND physical_hijack_tests)
