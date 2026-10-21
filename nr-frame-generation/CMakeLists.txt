cmake_minimum_required(VERSION 3.24)
project(vdxr_frame_generation LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Frame generation requires Windows x64")
endif()
set(FG_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../bin/fg-deps" CACHE PATH "Pinned dependency source")
set(OFXR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ofxr")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DERIVATIVE "${CMAKE_CURRENT_BINARY_DIR}/ofxr-derivative")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-fg-vendor.py"
    --source "${OFXR}" --output "${DERIVATIVE}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-fg-vendor.py"
    "${OFXR}/src/d3d12/d3d12_frame_synthesizer.cpp" "${OFXR}/manifest.json"
    "${OFXR}/src/d3d12/frame_synthesis.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/src/ffx_endpoint_validation.hlsl")
set(FFX "${FG_DEPS}/fidelityfx/sdk")
set(FXC "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe" CACHE FILEPATH "Pinned shader compiler")
if(NOT EXISTS "${FXC}" OR NOT EXISTS "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib")
    message(FATAL_ERROR "Build pinned dependencies before configuring the module")
endif()
set(SHADER "${DERIVATIVE}/frame_synthesis.hlsl")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
file(MAKE_DIRECTORY "${GENERATED}")
set(FG_HOST_SHADER "${GENERATED}/FrameGenerationCopyCS.h")
add_custom_command(OUTPUT "${FG_HOST_SHADER}"
    COMMAND "${FXC}" /nologo /T cs_5_0 /E main /O3 /WX /Ges
        /Fh "${FG_HOST_SHADER}" /Vn g_FrameGenerationCopyCS
        "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/FrameGenerationCopyCS.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/FrameGenerationCopyCS.hlsl" VERBATIM)
add_custom_target(fg_host_shader DEPENDS "${FG_HOST_SHADER}")
set(SHADER_RECIPES
    "pack_flow_input|cs_5_1|PackFlowInput"
    "fullscreen_vertex|vs_5_1|FullscreenTriangleVS"
    "synthesize_midpoint_pixel|ps_5_1|SynthesizeMidpointPS"
    "game_motion_synthesize_midpoint_pixel|ps_5_1|SynthesizeGameMotionMidpointPS"
    "nvidia_pack_flow_input|cs_5_1|PackNvidiaFlowInput"
    "nvidia_pack_luma_input|cs_5_1|PackNvidiaFlowInput"
    "nvidia_synthesize_midpoint_pixel|ps_5_1|SynthesizeNvidiaMidpointPS"
    "nvidia_bidirectional_synthesize_midpoint_pixel|ps_5_1|SynthesizeNvidiaBidirectionalMidpointPS"
    "nvidia_fast_synthesize_midpoint_pixel|ps_5_1|SynthesizeNvidiaFastMidpointPS"
    "nvidia_fast_bidirectional_synthesize_midpoint_pixel|ps_5_1|SynthesizeNvidiaFastBidirectionalMidpointPS")
foreach(RECIPE IN LISTS SHADER_RECIPES)
    string(REPLACE "|" ";" PARTS "${RECIPE}")
    list(GET PARTS 0 NAME)
    list(GET PARTS 1 PROFILE)
    list(GET PARTS 2 ENTRY)
    set(DEFINES)
    if(NAME STREQUAL "nvidia_pack_luma_input")
        set(DEFINES /D XRFG_NVIDIA_LUMA_INPUT=1)
    endif()
    set(HEADER "${GENERATED}/${NAME}_shader.hpp")
    add_custom_command(OUTPUT "${HEADER}"
        COMMAND "${FXC}" /nologo /T ${PROFILE} /E ${ENTRY} ${DEFINES}
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect /Fh "${HEADER}" /Vn "g_xrfg_${NAME}_shader" "${SHADER}"
        DEPENDS "${SHADER}" VERBATIM)
    list(APPEND SHADER_HEADERS "${HEADER}")
endforeach()
add_library(vdxr_frame_generation SHARED src/fg_module.cpp
    "${OFXR}/src/d3d12/d3d12_history.cpp"
    "${DERIVATIVE}/d3d12_frame_synthesizer.cpp"
    "${OFXR}/src/common/dlss_motion_vectors.cpp"
    "${OFXR}/src/core/pose.cpp"
    "${OFXR}/src/layer/bridge_flight_logger.cpp" ${SHADER_HEADERS})
set_target_properties(vdxr_frame_generation PROPERTIES OUTPUT_NAME "vdxr-frame-generation")
target_compile_features(vdxr_frame_generation PRIVATE cxx_std_20)
target_compile_options(vdxr_frame_generation PRIVATE /W4 /permissive- /EHsc /fp:precise)
target_compile_definitions(vdxr_frame_generation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_include_directories(vdxr_frame_generation PRIVATE include "${OFXR}/include" "${GENERATED}" "${FFX}/include"
    "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
target_link_libraries(vdxr_frame_generation PRIVATE d3d12 dxgi
    "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib" "${FFX}/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
include(CTest)
if(BUILD_TESTING)
    add_executable(nr_fg_bgra_regression ../tests/nr_fg_bgra_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_bgra_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_bgra_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_bgra_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_bgra_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO typeless unorm srgb rgba rgba-srgb motion motion-srgb separate array-mip source-reuse two-live mixed mixed-reverse unsupported-right invalid-right)
        add_test(NAME fg_bgra_${SCENARIO} COMMAND nr_fg_bgra_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_bgra_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_library(fg_v2_host_test_module SHARED ../tests/fg_v2_host_test_module.cpp)
    target_include_directories(fg_v2_host_test_module PRIVATE include)
    target_compile_features(fg_v2_host_test_module PRIVATE cxx_std_17)
    add_library(fg_v1_host_test_module SHARED ../tests/fg_v2_host_test_module.cpp)
    target_include_directories(fg_v1_host_test_module PRIVATE include)
    target_compile_features(fg_v1_host_test_module PRIVATE cxx_std_17)
    target_compile_definitions(fg_v1_host_test_module PRIVATE VDXRFG_TEST_V1_ONLY)
    add_executable(fg_v2_host_regression ../tests/fg_v2_host_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(fg_v2_host_regression PRIVATE include)
    target_compile_features(fg_v2_host_regression PRIVATE cxx_std_17)
    target_compile_definitions(fg_v2_host_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_v2_host_regression PRIVATE d3d11 d3d12 dxgi)
    foreach(MODE RANGE 0 7)
        add_test(NAME fg_v2_host_${MODE} COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" ${MODE})
        set_tests_properties(fg_v2_host_${MODE} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()

    add_test(NAME fg_v2_host_diagnostics COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 5 diagnostics)
    set_tests_properties(fg_v2_host_diagnostics PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    foreach(OPTION ffx ffx-diagnostics)
        add_test(NAME fg_v2_host_${OPTION} COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 8 ${OPTION})
        set_tests_properties(fg_v2_host_${OPTION} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_test(NAME fg_v2_host_unsupported_ffx COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 5 ffx)
    set_tests_properties(fg_v2_host_unsupported_ffx PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    foreach(OPTION v1 v1-ffx)
        add_test(NAME fg_host_${OPTION} COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v1_host_test_module>" 5 ${OPTION})
        set_tests_properties(fg_host_${OPTION} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()

    add_executable(fg_v2_regression ../tests/fg_v2_regression.cpp)
    target_compile_features(fg_v2_regression PRIVATE cxx_std_20)
    target_include_directories(fg_v2_regression PRIVATE include)
    target_include_directories(fg_v2_regression PRIVATE ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_v2_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_v2 COMMAND fg_v2_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_v2 PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")

    add_executable(fg_ffx_regression ../tests/fg_ffx_regression.cpp)
    target_compile_features(fg_ffx_regression PRIVATE cxx_std_20)
    target_include_directories(fg_ffx_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_ffx_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_ffx_full COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_ffx_full PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "PASS:")
    add_test(NAME fg_timing_nvidia COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>" prime-baseline)
    set_tests_properties(fg_timing_nvidia PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "PASS:")

    add_executable(fg_c_abi_regression ../tests/fg_c_abi_regression.c)
    target_include_directories(fg_c_abi_regression PRIVATE include)
    target_compile_features(fg_c_abi_regression PRIVATE c_std_11)
    add_test(NAME fg_c_abi COMMAND fg_c_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_c_abi PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    add_executable(nr_fg_integration_regression ../tests/nr_fg_integration_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_integration_regression PRIVATE include)
    target_compile_features(nr_fg_integration_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_integration_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_integration_regression PRIVATE d3d11 d3d12 dxgi)
    foreach(SCENARIO interop invalid-plan stale-ticket blocked-consumer invalid-context copy-identity copy-alias event-stale source-overwrite)
        add_test(NAME fg_host_${SCENARIO} COMMAND nr_fg_integration_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_host_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_executable(nr_fg_pack_regression ../tests/nr_fg_pack_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_pack_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_pack_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_pack_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_pack_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO packed offset separate gap overlap reverse different-y array-mip invalid-rect invalid-pose wrong-device)
        add_test(NAME fg_pack_${SCENARIO} COMMAND nr_fg_pack_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_pack_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_executable(nr_fg_event_regression ../tests/nr_fg_event_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_event_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_event_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_event_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_event_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO removed event-failure wait-failure stale-wake)
        add_test(NAME fg_event_${SCENARIO} COMMAND nr_fg_event_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_event_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_executable(nr_fg_ready_regression ../tests/nr_fg_ready_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_ready_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_ready_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_ready_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_ready_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO timeout event-failure stale-wake removed invalid next)
        add_test(NAME fg_ready_${SCENARIO} COMMAND nr_fg_ready_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_ready_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_executable(nr_fg_presenter_regression ../tests/nr_fg_presenter_regression.cpp
        ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_presenter_regression PRIVATE ../virtualdesktop-openxr)
    target_compile_features(nr_fg_presenter_regression PRIVATE cxx_std_17)
    add_test(NAME fg_cadence COMMAND nr_fg_presenter_regression)
    set_tests_properties(fg_cadence PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    add_executable(fg_abi_regression ../tests/fg_abi_regression.cpp)
    target_include_directories(fg_abi_regression PRIVATE include)
    target_compile_features(fg_abi_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_abi_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME fg_abi COMMAND fg_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_abi PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    add_executable(fg_gpu_regression ../tests/fg_gpu_regression.cpp)
    target_include_directories(fg_gpu_regression PRIVATE include)
    target_compile_features(fg_gpu_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_gpu_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_gpu_regression PRIVATE d3d12 dxgi)
    add_executable(fg_state_regression ../tests/fg_state_regression.cpp)
    target_include_directories(fg_state_regression PRIVATE include)
    target_compile_features(fg_state_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_state_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_state_regression PRIVATE d3d12 dxgi)
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3)
            foreach(OWNER module host)
                add_test(NAME fg_state_${BACKEND}_${MULTIPLIER}x_${OWNER}
                    COMMAND fg_state_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND} ${MULTIPLIER} ${OWNER})
                set_tests_properties(fg_state_${BACKEND}_${MULTIPLIER}x_${OWNER} PROPERTIES
                    TIMEOUT 180 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "PASS: actual GPU resource states"
                    FAIL_REGULAR_EXPRESSION "FAIL:")
                add_test(NAME fg_state_${BACKEND}_${MULTIPLIER}x_${OWNER}_uniform
                    COMMAND fg_state_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND} ${MULTIPLIER} ${OWNER} uniform)
                set_tests_properties(fg_state_${BACKEND}_${MULTIPLIER}x_${OWNER}_uniform PROPERTIES
                    TIMEOUT 180 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "PASS: actual GPU resource states"
                    FAIL_REGULAR_EXPRESSION "FAIL:")
            endforeach()
        endforeach()
    endforeach()
    foreach(MULTIPLIER 2 3)
        add_test(NAME fg_gpu_${MULTIPLIER}x COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER})
        set_tests_properties(fg_gpu_${MULTIPLIER}x PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    foreach(SCENARIO gamma-alpha cropped-eyes)
        add_test(NAME fg_gpu_${SCENARIO} COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" 3 ${SCENARIO})
        set_tests_properties(fg_gpu_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    endforeach()
    add_executable(fg_initialization_regression ../tests/fg_initialization_regression.cpp)
    target_include_directories(fg_initialization_regression PRIVATE include
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include"
        "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
    target_compile_features(fg_initialization_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_initialization_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_initialization_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_initialization COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_initialization PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
    foreach(SIGNAL 1 2 3 4)
        add_test(NAME fg_signal_${SIGNAL} COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SIGNAL})
        set_tests_properties(fg_signal_${SIGNAL} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
        foreach(MULTIPLIER 2 3)
            add_test(NAME fg_signal_host_${MULTIPLIER}x_${SIGNAL}
                COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SIGNAL} host ${MULTIPLIER})
            set_tests_properties(fg_signal_host_${MULTIPLIER}x_${SIGNAL} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
        endforeach()
    endforeach()
    foreach(HOST fg_v2_host_regression nr_fg_integration_regression nr_fg_pack_regression
            nr_fg_event_regression nr_fg_ready_regression nr_fg_bgra_regression)
        add_dependencies(${HOST} fg_host_shader)
        target_include_directories(${HOST} PRIVATE "${GENERATED}")
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(fg_endpoint_regression ../tests/fg_endpoint_regression.cpp)
    target_include_directories(fg_endpoint_regression PRIVATE include "${GENERATED}")
    target_compile_features(fg_endpoint_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_endpoint_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_endpoint_regression PRIVATE d3d12 dxgi)
    add_dependencies(fg_endpoint_regression vdxr_frame_generation)
    foreach(SCENARIO uniform uniform-srgb nearwhite-gradient nearwhite-gradient-srgb false-match false-match-srgb dark-false-match-srgb translated-detail center-detail isolated-detail border border-no-neighbors scene-change repeated raw-endpoint rotation-motion rotation-static rotation-cap)
        add_test(NAME fg_endpoint_${SCENARIO} COMMAND fg_endpoint_regression ${SCENARIO})
        set_tests_properties(fg_endpoint_${SCENARIO} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "PASS: production endpoint shader draw and readback")
    endforeach()
endif()
