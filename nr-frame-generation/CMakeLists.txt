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
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_early_readiness.py"
    "${OFXR}/src/d3d12/d3d12_frame_synthesizer.cpp" "${OFXR}/manifest.json"
    "${OFXR}/include/xrfg/d3d12_frame_synthesizer.hpp"
    "${OFXR}/src/d3d12/d3d12_history.cpp" "${OFXR}/include/xrfg/d3d12_history.hpp"
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
    "nvidia_shared_3x_synthesize_pixel|ps_5_1|SynthesizeNvidiaShared3xPS"
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
    "${DERIVATIVE}/d3d12_history.cpp"
    "${DERIVATIVE}/d3d12_frame_synthesizer.cpp"
    "${OFXR}/src/common/dlss_motion_vectors.cpp"
    "${OFXR}/src/core/pose.cpp"
    "${OFXR}/src/layer/bridge_flight_logger.cpp" ${SHADER_HEADERS})
set_target_properties(vdxr_frame_generation PROPERTIES OUTPUT_NAME "vdxr-frame-generation")
target_compile_features(vdxr_frame_generation PRIVATE cxx_std_20)
target_compile_options(vdxr_frame_generation PRIVATE /W4 /permissive- /EHsc /fp:precise)
target_compile_definitions(vdxr_frame_generation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_include_directories(vdxr_frame_generation PRIVATE include "${DERIVATIVE}" "${OFXR}/include" "${GENERATED}" "${FFX}/include"
    "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
target_link_libraries(vdxr_frame_generation PRIVATE d3d12 dxgi
    "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib" "${FFX}/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
include(CTest)
option(VDXR_FG_DIAGNOSTIC_TESTS "Register strict stationary-flow diagnostics with known rendering failures" OFF)
if(BUILD_TESTING)
    add_executable(nr_fg_early_ready_regression ../tests/nr_fg_early_ready_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_early_ready_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_early_ready_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_early_ready_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_early_ready_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            add_test(NAME fg_early_host_${BACKEND}_${MULTIPLIER}x
                COMMAND nr_fg_early_ready_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} ${BACKEND})
            set_tests_properties(fg_early_host_${BACKEND}_${MULTIPLIER}x PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
                FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
    add_executable(fg_early_ready_regression ../tests/fg_early_ready_regression.cpp)
    target_include_directories(fg_early_ready_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(fg_early_ready_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_early_ready_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_early_ready_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            add_test(NAME fg_early_ready_${BACKEND}_${MULTIPLIER}x
                COMMAND fg_early_ready_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} ${BACKEND})
            set_tests_properties(fg_early_ready_${BACKEND}_${MULTIPLIER}x PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
                FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
        add_test(NAME fg_early_ready_${BACKEND}_tail_failure
            COMMAND fg_early_ready_regression "$<TARGET_FILE:vdxr_frame_generation>" 4 ${BACKEND} fail-tail)
        set_tests_properties(fg_early_ready_${BACKEND}_tail_failure PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_sampler_regression.cmake")
    add_executable(nr_fg_bgra_regression ../tests/nr_fg_bgra_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_bgra_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_bgra_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_bgra_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_bgra_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO typeless unorm srgb rgba rgba-srgb motion motion-srgb separate array-mip source-reuse two-live mixed mixed-reverse unsupported-right invalid-right reuse-completed reuse-pending reuse-removed reuse-stale-submit reuse-reset reuse-poisoned-capture)
        add_test(NAME fg_bgra_${SCENARIO} COMMAND nr_fg_bgra_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_bgra_${SCENARIO} PROPERTIES TIMEOUT 60)
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
        set_tests_properties(fg_v2_host_${MODE} PROPERTIES TIMEOUT 60)
    endforeach()

    add_test(NAME fg_v2_host_diagnostics COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 5 diagnostics)
    set_tests_properties(fg_v2_host_diagnostics PROPERTIES TIMEOUT 60)
    foreach(OPTION ffx ffx-diagnostics)
        add_test(NAME fg_v2_host_${OPTION} COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 8 ${OPTION})
        set_tests_properties(fg_v2_host_${OPTION} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME fg_v2_host_unsupported_ffx COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v2_host_test_module>" 5 ffx)
    set_tests_properties(fg_v2_host_unsupported_ffx PROPERTIES TIMEOUT 60)
    foreach(OPTION v1 v1-ffx)
        add_test(NAME fg_host_${OPTION} COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v1_host_test_module>" 5 ${OPTION})
        set_tests_properties(fg_host_${OPTION} PROPERTIES TIMEOUT 60)
    endforeach()

    foreach(VERSION 1 2)
        add_test(NAME fg_v${VERSION}_host_rejects_4x
            COMMAND fg_v2_host_regression "$<TARGET_FILE:fg_v${VERSION}_host_test_module>" 5 v${VERSION}-4x)
        set_tests_properties(fg_v${VERSION}_host_rejects_4x PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()

    add_executable(nr_fg_ready_wait_regression ../tests/nr_fg_ready_wait_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_ready_wait_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_ready_wait_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_ready_wait_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_ready_wait_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(SCENARIO all wait-failure)
            add_test(NAME fg_ready_wait_${BACKEND}_${SCENARIO}
                COMMAND nr_fg_ready_wait_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
            set_tests_properties(fg_ready_wait_${BACKEND}_${SCENARIO} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
                ENVIRONMENT "VDXR_FG_TEST_BACKEND=${BACKEND}")
        endforeach()
    endforeach()

    add_executable(fg_v2_regression ../tests/fg_v2_regression.cpp)
    target_compile_features(fg_v2_regression PRIVATE cxx_std_20)
    target_include_directories(fg_v2_regression PRIVATE include)
    target_include_directories(fg_v2_regression PRIVATE ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_v2_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_v2 COMMAND fg_v2_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_v2 PROPERTIES TIMEOUT 120)
    add_test(NAME fg_direct_synthetic_output COMMAND fg_v2_regression "$<TARGET_FILE:vdxr_frame_generation>" direct-output)
    set_tests_properties(fg_direct_synthetic_output PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    set(FG_MRT_LEGACY_SHADERS "${GENERATED}" CACHE PATH "Frozen legacy shader headers for MRT differential fixtures")
    add_executable(fg_mrt_operation_regression ../tests/fg_mrt_operation_regression.cpp ${SHADER_HEADERS})
    target_compile_features(fg_mrt_operation_regression PRIVATE cxx_std_20)
    target_compile_options(fg_mrt_operation_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_mrt_operation_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VDXR_FG_MRT_SHADER_AVAILABLE=1)
    target_include_directories(fg_mrt_operation_regression PRIVATE include "${FG_MRT_LEGACY_SHADERS}" "${GENERATED}"
        ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_mrt_operation_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_mrt_native_3x COMMAND fg_mrt_operation_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_mrt_native_3x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_test(NAME fg_mrt_native_pso_fallback COMMAND fg_mrt_operation_regression "$<TARGET_FILE:vdxr_frame_generation>" mrt-pso-failure)
    set_tests_properties(fg_mrt_native_pso_fallback PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(fg_mrt_shader_regression ../tests/fg_mrt_shader_regression.cpp ${SHADER_HEADERS})
    set_source_files_properties(../tests/fg_mrt_operation_regression.cpp ../tests/fg_mrt_shader_regression.cpp
        PROPERTIES OBJECT_DEPENDS "${GENERATED}/nvidia_shared_3x_synthesize_pixel_shader.hpp")
    target_compile_features(fg_mrt_shader_regression PRIVATE cxx_std_20)
    target_compile_options(fg_mrt_shader_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_mrt_shader_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VDXR_FG_MRT_SHADER_AVAILABLE=1)
    target_include_directories(fg_mrt_shader_regression PRIVATE include "${FG_MRT_LEGACY_SHADERS}" "${GENERATED}")
    target_link_libraries(fg_mrt_shader_regression PRIVATE d3d12 dxgi)
    add_test(NAME fg_mrt_shader_exact COMMAND fg_mrt_shader_regression)
    set_tests_properties(fg_mrt_shader_exact PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(fg_atlas_regression ../tests/fg_atlas_regression.cpp)
    target_include_directories(fg_atlas_regression PRIVATE include)
    target_compile_features(fg_atlas_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_atlas_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_atlas_regression PRIVATE d3d12 dxgi)
    add_executable(fg_atlas_backend_regression ../tests/fg_atlas_backend_regression.cpp
        "${DERIVATIVE}/d3d12_history.cpp" "${DERIVATIVE}/d3d12_frame_synthesizer.cpp"
        "${OFXR}/src/common/dlss_motion_vectors.cpp" "${OFXR}/src/core/pose.cpp"
        "${OFXR}/src/layer/bridge_flight_logger.cpp" ${SHADER_HEADERS})
    target_compile_features(fg_atlas_backend_regression PRIVATE cxx_std_20)
    target_compile_options(fg_atlas_backend_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_atlas_backend_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_include_directories(fg_atlas_backend_regression PRIVATE include "${DERIVATIVE}" "${OFXR}/include"
        "${GENERATED}" "${FFX}/include"
        "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
    target_link_libraries(fg_atlas_backend_regression PRIVATE d3d12 dxgi
        "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib" "${FFX}/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
    foreach(CONTROL shared-3x shared-3x-slow shared-3x-fast shared-3x-bidir)
        add_test(NAME fg_mrt_backend_${CONTROL}
            COMMAND fg_atlas_backend_regression nvidia 3 srgb-crop-marker ${CONTROL})
        set_tests_properties(fg_mrt_backend_${CONTROL} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    foreach(SCENARIO full reversed-crop-marker)
        add_test(NAME fg_mrt_backend_${SCENARIO}
            COMMAND fg_atlas_backend_regression nvidia 3 ${SCENARIO} shared-3x)
        set_tests_properties(fg_mrt_backend_${SCENARIO} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME fg_mrt_backend_nvidia_2x COMMAND fg_atlas_backend_regression nvidia 2 srgb-crop-marker shared-3x)
    set_tests_properties(fg_mrt_backend_nvidia_2x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    foreach(MULTIPLIER 2 3)
        add_test(NAME fg_mrt_backend_ffx_${MULTIPLIER}x
            COMMAND fg_atlas_backend_regression fidelityfx-full ${MULTIPLIER} srgb-crop-marker shared-3x)
        set_tests_properties(fg_mrt_backend_ffx_${MULTIPLIER}x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            foreach(SCENARIO full srgb-crop-marker reversed-crop-marker)
                add_test(NAME fg_atlas_backend_${BACKEND}_${MULTIPLIER}x_${SCENARIO}
                    COMMAND fg_atlas_backend_regression ${BACKEND} ${MULTIPLIER} ${SCENARIO})
                set_tests_properties(fg_atlas_backend_${BACKEND}_${MULTIPLIER}x_${SCENARIO}
                    PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()

    add_executable(fg_history_consumed_regression ../tests/fg_history_consumed_regression.cpp)
    target_compile_features(fg_history_consumed_regression PRIVATE cxx_std_20)
    target_include_directories(fg_history_consumed_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_history_consumed_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3)
            foreach(OWNER host module)
                add_test(NAME fg_history_consumed_${BACKEND}_${MULTIPLIER}x_${OWNER}
                    COMMAND fg_history_consumed_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND} ${MULTIPLIER} ${OWNER})
                set_tests_properties(fg_history_consumed_${BACKEND}_${MULTIPLIER}x_${OWNER}
                    PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()

    add_executable(fg_ffx_regression ../tests/fg_ffx_regression.cpp)
    target_compile_features(fg_ffx_regression PRIVATE cxx_std_20)
    target_include_directories(fg_ffx_regression PRIVATE include ../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_ffx_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_ffx_full COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_ffx_full PROPERTIES TIMEOUT 120)
    add_test(NAME fg_timing_nvidia COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>" prime-baseline)
    set_tests_properties(fg_timing_nvidia PROPERTIES TIMEOUT 120)

    add_executable(fg_c_abi_regression ../tests/fg_c_abi_regression.c)
    target_include_directories(fg_c_abi_regression PRIVATE include)
    target_compile_features(fg_c_abi_regression PRIVATE c_std_11)
    add_test(NAME fg_c_abi COMMAND fg_c_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_c_abi PROPERTIES TIMEOUT 60)
    add_executable(nr_fg_integration_regression ../tests/nr_fg_integration_regression.cpp
        ../virtualdesktop-openxr/frame_generation.cpp ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_integration_regression PRIVATE include)
    target_compile_features(nr_fg_integration_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_integration_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_integration_regression PRIVATE d3d11 d3d12 dxgi)
    foreach(SCENARIO interop invalid-plan stale-ticket blocked-consumer invalid-context copy-identity copy-alias event-stale source-overwrite)
        add_test(NAME fg_host_${SCENARIO} COMMAND nr_fg_integration_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_host_${SCENARIO} PROPERTIES TIMEOUT 60)
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
        set_tests_properties(fg_pack_${SCENARIO} PROPERTIES TIMEOUT 60)
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
        set_tests_properties(fg_event_${SCENARIO} PROPERTIES TIMEOUT 60)
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
        set_tests_properties(fg_ready_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(nr_fg_presenter_regression ../tests/nr_fg_presenter_regression.cpp
        ../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_presenter_regression PRIVATE ../virtualdesktop-openxr)
    target_compile_features(nr_fg_presenter_regression PRIVATE cxx_std_17)
    add_test(NAME fg_cadence COMMAND nr_fg_presenter_regression)
    set_tests_properties(fg_cadence PROPERTIES TIMEOUT 60)
    add_executable(fg_abi_regression ../tests/fg_abi_regression.cpp)
    target_include_directories(fg_abi_regression PRIVATE include)
    target_compile_features(fg_abi_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_abi_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME fg_abi COMMAND fg_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_abi PROPERTIES TIMEOUT 60)
    add_executable(fg_submit_validation_regression ../tests/fg_submit_validation_regression.cpp)
    target_include_directories(fg_submit_validation_regression PRIVATE include)
    target_compile_features(fg_submit_validation_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_submit_validation_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_submit_validation_regression PRIVATE d3d12 dxgi)
    foreach(SCENARIO foreign-token foreign-busy fractions)
        add_test(NAME fg_submit_validation_${SCENARIO}
            COMMAND fg_submit_validation_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_submit_validation_${SCENARIO} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
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
        set_tests_properties(fg_gpu_${MULTIPLIER}x PROPERTIES TIMEOUT 60)
    endforeach()
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            add_test(NAME fg_early_pixels_${BACKEND}_${MULTIPLIER}x
                COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} early-motion)
            set_tests_properties(fg_early_pixels_${BACKEND}_${MULTIPLIER}x PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
                ENVIRONMENT "VDXR_FG_TEST_BACKEND=${BACKEND}" FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
        foreach(SCENARIO motion gamma-alpha cropped-eyes)
            add_test(NAME fg_gpu_${BACKEND}_4x_${SCENARIO}
                COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" 4 ${SCENARIO})
            set_tests_properties(fg_gpu_${BACKEND}_4x_${SCENARIO} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                ENVIRONMENT "VDXR_FG_TEST_BACKEND=${BACKEND}" FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
    foreach(SCENARIO gamma-alpha cropped-eyes)
        add_test(NAME fg_gpu_${SCENARIO} COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" 3 ${SCENARIO})
        set_tests_properties(fg_gpu_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    if(VDXR_FG_DIAGNOSTIC_TESTS)
        foreach(BACKEND nvidia fidelityfx-full)
            foreach(MULTIPLIER 2 3)
                add_test(NAME fg_gpu_stationary_${BACKEND}_${MULTIPLIER}x
                    COMMAND fg_gpu_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} stationary)
                set_tests_properties(fg_gpu_stationary_${BACKEND}_${MULTIPLIER}x PROPERTIES
                    TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "VDXR_FG_TEST_BACKEND=${BACKEND}"
                    FAIL_REGULAR_EXPRESSION "FAIL:" LABELS "diagnostic;stationary-flow")
            endforeach()
        endforeach()
    endif()
    add_executable(fg_initialization_regression ../tests/fg_initialization_regression.cpp)
    target_include_directories(fg_initialization_regression PRIVATE include
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include"
        "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
    target_compile_features(fg_initialization_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_initialization_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_initialization_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_initialization COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_initialization PROPERTIES TIMEOUT 60)
    foreach(SIGNAL 1 2 3 4)
        add_test(NAME fg_signal_${SIGNAL} COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SIGNAL})
        set_tests_properties(fg_signal_${SIGNAL} PROPERTIES TIMEOUT 60)
        foreach(MULTIPLIER 2 3)
            add_test(NAME fg_signal_host_${MULTIPLIER}x_${SIGNAL}
                COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SIGNAL} host ${MULTIPLIER})
            set_tests_properties(fg_signal_host_${MULTIPLIER}x_${SIGNAL} PROPERTIES TIMEOUT 60)
        endforeach()
    endforeach()
    foreach(HOST fg_v2_host_regression nr_fg_integration_regression nr_fg_pack_regression
            nr_fg_event_regression nr_fg_ready_regression nr_fg_ready_wait_regression nr_fg_bgra_regression
            nr_fg_early_ready_regression)
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
    foreach(SCENARIO nvidia-current-warp nvidia-current-warp-srgb
            nvidia-edge-fold nvidia-edge-fold-srgb nvidia-edge-motion nvidia-edge-motion-srgb
            nvidia-nearby-motion nvidia-nearby-motion-srgb nvidia-relative-motion nvidia-relative-motion-srgb
            nvidia-dark-edge-fold-srgb nvidia-dark-edge-motion-srgb
            nvidia-vector-confidence nvidia-vector-confidence-srgb
            nvidia-vector-unequal nvidia-vector-unequal-srgb
            nvidia-vector-controls nvidia-vector-controls-srgb
            nvidia-vector-boundaries nvidia-vector-boundaries-srgb
            nvidia-vector-uniform nvidia-vector-uniform-srgb)
        add_test(NAME fg_endpoint_${SCENARIO} COMMAND fg_endpoint_regression ${SCENARIO})
        set_tests_properties(fg_endpoint_${SCENARIO} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "PASS: production endpoint shader")
    endforeach()
    foreach(SCENARIO uniform uniform-srgb nearwhite-gradient nearwhite-gradient-srgb false-match false-match-srgb dark-false-match-srgb translated-detail translated-detail-aperiodic half-pixel-left half-pixel-right half-pixel-top half-pixel-bottom valid-edge-centers previous-camera-left previous-camera-right previous-camera-top previous-camera-bottom nvidia-half-input-bottom nvidia-half-input-cropped-view nvidia-full-input-padding nvidia-three-quarter-input-padding nvidia-small-half-input-padding ffx-half-input-padding ffx-full-input-padding ffx-three-quarter-input-padding ffx-small-half-input-padding center-detail isolated-detail border border-no-neighbors scene-change repeated raw-endpoint rotation-motion rotation-static rotation-cap stationary-alias stationary-alias-srgb stationary-alias-reverse stationary-alias-rotation stationary-alias-asymmetric ghost-thin-motion ghost-thin-motion-srgb ghost-thin-motion-reverse ghost-alpha-motion ghost-large-motion ghost-large-motion-reverse)
        if(SCENARIO MATCHES "^stationary-alias" AND NOT VDXR_FG_DIAGNOSTIC_TESTS)
            continue()
        endif()
        add_test(NAME fg_endpoint_${SCENARIO} COMMAND fg_endpoint_regression ${SCENARIO})
        set_tests_properties(fg_endpoint_${SCENARIO} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            PASS_REGULAR_EXPRESSION "PASS: production (endpoint|NVIDIA pack|FFX pack) shader")
        if(SCENARIO MATCHES "^stationary-alias")
            set_tests_properties(fg_endpoint_${SCENARIO} PROPERTIES LABELS "diagnostic;stationary-flow")
        endif()
    endforeach()
endif()
