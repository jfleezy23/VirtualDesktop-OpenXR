cmake_minimum_required(VERSION 3.24)
project(vdxr_frame_generation LANGUAGES C CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Frame generation requires Windows x64")
endif()
set(FG_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../bin/fg-deps" CACHE PATH "Pinned dependency source")
set(OFXR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ofxr")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DERIVATIVE "${CMAKE_CURRENT_SOURCE_DIR}/prepared-ofxr" CACHE PATH "Qualified synthesis sources")
option(VDXR_FG_PATCH_MOTION_EXPERIMENT "Build experimental NVIDIA patch correction" OFF)
option(VDXR_FG_BLEND_PROMOTION_EXPERIMENT "Build experimental NVIDIA blend promotion" OFF)
option(VDXR_FG_BOUNDED_MOTION_EXPERIMENT "Build experimental bounded NVIDIA motion correction" OFF)
if(VDXR_FG_PATCH_MOTION_EXPERIMENT AND VDXR_FG_BLEND_PROMOTION_EXPERIMENT)
    message(FATAL_ERROR "Patch motion and blend promotion experiments are mutually exclusive")
endif()
if(VDXR_FG_BOUNDED_MOTION_EXPERIMENT AND (VDXR_FG_PATCH_MOTION_EXPERIMENT OR VDXR_FG_BLEND_PROMOTION_EXPERIMENT))
    message(FATAL_ERROR "Bounded motion, patch motion and blend promotion experiments are mutually exclusive")
endif()
set(PATCH_ARGUMENTS)
option(VDXR_FG_DEPTH_ENDPOINT_PREFERENCE "Build optional genuine-depth NVIDIA endpoint guidance" OFF)
option(VDXR_FG_DEPTH_RECONSTRUCTION "Build optional real-depth camera reconstruction" OFF)
option(VDXR_FG_DEPTH_RECONSTRUCTION_CELL_SUPPORT "Use same-layer B-depth flow-cell support in reconstruction" ON)
option(VDXR_FG_A_OWNED_DEPTH_HISTORY "Use bounded A-owned depth camera gather in reconstruction" OFF)
option(VDXR_FG_RECONSTRUCTION_REUSE "Reuse exact reconstruction results without changing images" OFF)
option(VDXR_FG_DEPTH_AVAILABILITY "Exact GPU unusable-depth color-only fallback" OFF)
if(VDXR_FG_DEPTH_AVAILABILITY AND NOT VDXR_FG_DEPTH_RECONSTRUCTION)
    message(FATAL_ERROR "Depth availability requires reconstruction")
endif()
if(VDXR_FG_DEPTH_RECONSTRUCTION AND NOT VDXR_FG_DEPTH_ENDPOINT_PREFERENCE)
    message(FATAL_ERROR "Reconstruction candidate retains the existing depth-assist path")
endif()
if(VDXR_FG_DEPTH_ENDPOINT_PREFERENCE AND
   (VDXR_FG_PATCH_MOTION_EXPERIMENT OR VDXR_FG_BLEND_PROMOTION_EXPERIMENT OR VDXR_FG_BOUNDED_MOTION_EXPERIMENT))
    message(FATAL_ERROR "Depth endpoint guidance requires the retained baseline flow path")
endif()
if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
    list(APPEND PATCH_ARGUMENTS --patch-motion)
endif()
if(VDXR_FG_BLEND_PROMOTION_EXPERIMENT)
    list(APPEND PATCH_ARGUMENTS --blend-promotion)
endif()
if(VDXR_FG_BOUNDED_MOTION_EXPERIMENT)
    list(APPEND PATCH_ARGUMENTS --bounded-motion)
endif()
# The candidate uses the exact prepared retained derivative. Re-running the
# generator here would replace its accepted lineage hooks before compilation.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-fg-vendor.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_early_readiness.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_patch_motion.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_blend_promotion.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_bounded_safe.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_bounded_correction.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nvidia_patch_motion.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fg_depth_guidance.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/depth_endpoint_preference.hlsli"
    "${OFXR}/src/d3d12/d3d12_frame_synthesizer.cpp" "${OFXR}/manifest.json"
    "${OFXR}/include/xrfg/d3d12_frame_synthesizer.hpp"
    "${OFXR}/src/d3d12/d3d12_history.cpp" "${OFXR}/include/xrfg/d3d12_history.hpp"
    "${OFXR}/src/d3d12/frame_synthesis.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/src/ffx_endpoint_validation.hlsl")
set(FFX "${FG_DEPS}/fidelityfx/sdk")
set(FXC "$ENV{WindowsSdkVerBinPath}/x64/fxc.exe" CACHE FILEPATH "Pinned shader compiler")
option(VDXR_FG_NVIDIA_GRID4 "Specialize validated NVIDIA composition grid coordinates" OFF)
set(VDXR_FG_SHADER_COMPILER "FXC" CACHE STRING "Synthesis shader compiler: FXC or experimental DXC")
set_property(CACHE VDXR_FG_SHADER_COMPILER PROPERTY STRINGS FXC DXC)
set(DXC "" CACHE FILEPATH "Qualified app-local DXC executable")
option(VDXR_FG_DXC_IEEE_STRICT "Experimental DXC IEEE-strict arm" OFF)
if(NOT VDXR_FG_SHADER_COMPILER STREQUAL "FXC" AND NOT VDXR_FG_SHADER_COMPILER STREQUAL "DXC")
    message(FATAL_ERROR "Unknown synthesis shader compiler")
endif()
if(VDXR_FG_SHADER_COMPILER STREQUAL "DXC" AND NOT EXISTS "${DXC}")
    message(FATAL_ERROR "A qualified DXC executable is required for the experimental build")
endif()
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
set(FG_DEPTH_HOST_SHADER "${GENERATED}/FrameGenerationDepthCS.h")
add_custom_command(OUTPUT "${FG_DEPTH_HOST_SHADER}"
    COMMAND "${FXC}" /nologo /T cs_5_0 /E main /O3 /WX /Ges
        /Fh "${FG_DEPTH_HOST_SHADER}" /Vn g_FrameGenerationDepthCS
        "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/FrameGenerationDepthCS.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/FrameGenerationDepthCS.hlsl" VERBATIM)
add_custom_target(fg_depth_host_shader DEPENDS "${FG_DEPTH_HOST_SHADER}")
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
if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
    list(APPEND SHADER_RECIPES "nvidia_patch_proposals|cs_5_1|BuildNvidiaPatchProposalsCS")
endif()
if(VDXR_FG_DEPTH_ENDPOINT_PREFERENCE)
    list(APPEND SHADER_RECIPES
        "nvidia_depth_synthesize_pixel|ps_5_1|SynthesizeNvidiaMidpointPS"
        "nvidia_depth_shared_synthesize_pixel|ps_5_1|SynthesizeNvidiaShared3xPS")
endif()
if(VDXR_FG_DEPTH_RECONSTRUCTION)
    list(APPEND SHADER_RECIPES
        "nvidia_reconstruction_pack_flow_input|cs_5_1|PackNvidiaFlowInput"
        "nvidia_reconstruction_pack_luma_input|cs_5_1|PackNvidiaFlowInput"
        "nvidia_reconstruction_synthesize_pixel|ps_5_1|SynthesizeNvidiaMidpointPS"
        "nvidia_reconstruction_shared_synthesize_pixel|ps_5_1|SynthesizeNvidiaShared3xPS")
endif()
foreach(RECIPE IN LISTS SHADER_RECIPES)
    string(REPLACE "|" ";" PARTS "${RECIPE}")
    list(GET PARTS 0 NAME)
    list(GET PARTS 1 PROFILE)
    list(GET PARTS 2 ENTRY)
    set(DEFINES)
    if(NAME STREQUAL "nvidia_pack_luma_input" OR NAME STREQUAL "nvidia_reconstruction_pack_luma_input")
        set(DEFINES /D XRFG_NVIDIA_LUMA_INPUT=1)
    endif()
    if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
        list(APPEND DEFINES /D XRFG_PATCH_MOTION=1)
    endif()
    if(NAME MATCHES "^nvidia_depth_")
        list(APPEND DEFINES /D XRFG_DEPTH_GUIDANCE=1)
    endif()
    if(NAME MATCHES "^nvidia_reconstruction_")
        list(APPEND DEFINES /D XRFG_DEPTH_GUIDANCE=1 /D XRFG_DEPTH_RECONSTRUCTION=1)
        if(VDXR_FG_DEPTH_RECONSTRUCTION_CELL_SUPPORT)
            list(APPEND DEFINES /D XRFG_DEPTH_RECONSTRUCTION_CELL_SUPPORT=1)
        endif()
        if(VDXR_FG_A_OWNED_DEPTH_HISTORY)
            list(APPEND DEFINES /D XRFG_A_OWNED_DEPTH_HISTORY=1)
        endif()
        if(VDXR_FG_DEPTH_AVAILABILITY)
            list(APPEND DEFINES /D XRFG_DEPTH_AVAILABILITY=1)
        endif()
        if(VDXR_FG_RECONSTRUCTION_REUSE)
            list(APPEND DEFINES /D XRFG_RECONSTRUCTION_REUSE=1)
        endif()
    endif()
    set(HEADER "${GENERATED}/${NAME}_shader.hpp")
    if(VDXR_FG_NVIDIA_GRID4 AND NAME MATCHES "^nvidia_.*synthesize")
        list(APPEND DEFINES /D XRFG_NVIDIA_GRID4=1)
    endif()
    if(VDXR_FG_SHADER_COMPILER STREQUAL "DXC")
        string(REPLACE "_5_1" "_6_0" DXC_PROFILE "${PROFILE}")
        set(DXC_PRECISION)
        if(VDXR_FG_DXC_IEEE_STRICT)
            list(APPEND DXC_PRECISION -Gis)
        endif()
        add_custom_command(OUTPUT "${HEADER}"
            COMMAND "${DXC}" -T ${DXC_PROFILE} -E ${ENTRY} ${DEFINES}
                -HV 2016 -O3 -Ges -WX ${DXC_PRECISION} -Qstrip_debug -Qstrip_reflect
                -Fo "${GENERATED}/${NAME}.dxil" -Fh "${HEADER}" -Vn "g_xrfg_${NAME}_shader" "${SHADER}"
            DEPENDS "${DXC}" "${SHADER}" "${DERIVATIVE}/depth_reconstruction.hlsli" "${DERIVATIVE}/depth_reconstruction_a_owned.hlsli" "${DERIVATIVE}/depth_endpoint_preference.hlsli" VERBATIM)
    else()
    add_custom_command(OUTPUT "${HEADER}"
        COMMAND "${FXC}" /nologo /T ${PROFILE} /E ${ENTRY} ${DEFINES}
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect /Fh "${HEADER}" /Vn "g_xrfg_${NAME}_shader" "${SHADER}"
        DEPENDS "${SHADER}" "${DERIVATIVE}/depth_reconstruction.hlsli" "${DERIVATIVE}/depth_reconstruction_a_owned.hlsli" "${DERIVATIVE}/depth_endpoint_preference.hlsli" VERBATIM)
    endif()
    list(APPEND SHADER_HEADERS "${HEADER}")
endforeach()
add_custom_target(fg_synthesis_shaders DEPENDS ${SHADER_HEADERS})
add_library(fg_depth_trace STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/fg_depth_trace.cpp)
target_compile_features(fg_depth_trace PRIVATE cxx_std_17)
target_compile_definitions(fg_depth_trace PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(fg_depth_trace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fg_depth_trace PUBLIC advapi32)
add_library(vdxr_frame_generation SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/fg_module.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_private.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_nr_receiver.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_nr_private.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_nr_worker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_nr_fg.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/PairCaptureNrOwner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pair_capture_nr_validate.c"
    "${DERIVATIVE}/d3d12_history.cpp"
    "${DERIVATIVE}/d3d12_frame_synthesizer.cpp"
    "${OFXR}/src/common/dlss_motion_vectors.cpp"
    "${OFXR}/src/core/pose.cpp"
    "${OFXR}/src/layer/bridge_flight_logger.cpp")
add_dependencies(vdxr_frame_generation fg_synthesis_shaders)
if(VDXR_FG_DEPTH_AVAILABILITY)
    set(AVAILABILITY_HEADERS)
    foreach(STAGE Tiles Reduce)
        set(HEADER "${GENERATED}/depth_availability_${STAGE}.hpp")
        add_custom_command(OUTPUT "${HEADER}"
            COMMAND "${FXC}" /nologo /T cs_5_0 /E "${STAGE}" /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
                /Fh "${HEADER}" /Vn "g_depth_availability_${STAGE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/depth_availability.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/depth_availability.hlsl" VERBATIM)
        list(APPEND AVAILABILITY_HEADERS "${HEADER}")
    endforeach()
    target_sources(vdxr_frame_generation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/depth_availability.cpp" ${AVAILABILITY_HEADERS})
    target_compile_definitions(vdxr_frame_generation PRIVATE VDXR_FG_DEPTH_AVAILABILITY=1)
endif()
set_target_properties(vdxr_frame_generation PROPERTIES OUTPUT_NAME "vdxr-frame-generation")
target_compile_features(vdxr_frame_generation PRIVATE cxx_std_20)
target_compile_features(vdxr_frame_generation PRIVATE c_std_17)
target_include_directories(vdxr_frame_generation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr"
    "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ofxr/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
target_compile_options(vdxr_frame_generation PRIVATE /W4 /permissive- /EHsc /fp:precise)
target_compile_definitions(vdxr_frame_generation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
if(VDXR_FG_DEPTH_ENDPOINT_PREFERENCE)
    target_compile_definitions(vdxr_frame_generation PRIVATE VDXR_FG_DEPTH_ENDPOINT_PREFERENCE=1)
endif()
if(VDXR_FG_DEPTH_RECONSTRUCTION)
    target_compile_definitions(vdxr_frame_generation PRIVATE VDXR_FG_DEPTH_RECONSTRUCTION=1)
endif()
target_include_directories(vdxr_frame_generation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${DERIVATIVE}" "${OFXR}/include" "${GENERATED}" "${FFX}/include"
    "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
target_link_libraries(vdxr_frame_generation PRIVATE fg_depth_trace d3d12 dxgi bcrypt
    "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib" "${FFX}/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
include(CTest)
option(VDXR_FG_DIAGNOSTIC_TESTS "Register strict stationary-flow diagnostics with known rendering failures" OFF)
if(BUILD_TESTING)
    add_library(fg_depth_probe_plan_regression OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_probe_plan_regression.cpp)
    target_compile_features(fg_depth_probe_plan_regression PRIVATE cxx_std_17)
    target_compile_options(fg_depth_probe_plan_regression PRIVATE /W4 /WX /permissive-)
    target_include_directories(fg_depth_probe_plan_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_executable(fg_depth_envelope_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_envelope_regression.cpp)
    target_compile_features(fg_depth_envelope_regression PRIVATE cxx_std_17)
    target_include_directories(fg_depth_envelope_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME fg_depth_envelope COMMAND fg_depth_envelope_regression)
    add_executable(fg_depth_api_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_api_regression.cpp)
    target_compile_features(fg_depth_api_regression PRIVATE cxx_std_20)
    target_include_directories(fg_depth_api_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_depth_api_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_depth_api COMMAND fg_depth_api_regression "$<TARGET_FILE:vdxr_frame_generation>")
    add_executable(fg_host_depth_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_host_depth_regression.cpp)
    target_compile_features(fg_host_depth_regression PRIVATE cxx_std_17)
    target_include_directories(fg_host_depth_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr)
    add_test(NAME fg_host_depth_metadata COMMAND fg_host_depth_regression)
    add_library(fg_depth_capture_test_module SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_capture_test_module.cpp)
    target_compile_features(fg_depth_capture_test_module PRIVATE cxx_std_17)
    target_include_directories(fg_depth_capture_test_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(fg_depth_capture_test_module PRIVATE d3d12)
    add_executable(nr_fg_guide_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_guide_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_compile_features(nr_fg_guide_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_guide_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(nr_fg_guide_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${GENERATED}")
    target_link_libraries(nr_fg_guide_regression PRIVATE d3d11 d3d12 dxgi)
    add_dependencies(nr_fg_guide_regression fg_host_shader fg_depth_host_shader fg_depth_capture_test_module)
    if(VDXR_FG_DEPTH_ENDPOINT_PREFERENCE)
        add_executable(fg_depth_shader_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_shader_regression.cpp)
        add_dependencies(fg_depth_shader_regression fg_synthesis_shaders)
        target_compile_features(fg_depth_shader_regression PRIVATE cxx_std_20)
        target_compile_definitions(fg_depth_shader_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(fg_depth_shader_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${GENERATED}")
        target_link_libraries(fg_depth_shader_regression PRIVATE d3d12 dxgi)
    endif()
    add_executable(fg_depth_layer_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_depth_layer_regression.cpp)
    target_include_directories(fg_depth_layer_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr ${CMAKE_CURRENT_SOURCE_DIR}/../external/LibOVR/Include)
    target_compile_features(fg_depth_layer_regression PRIVATE cxx_std_17)
    add_test(NAME fg_depth_layers COMMAND fg_depth_layer_regression)
    add_executable(fg_resource_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_resource_regression.cpp)
    add_dependencies(fg_resource_regression fg_synthesis_shaders)
    target_compile_features(fg_resource_regression PRIVATE cxx_std_20)
    target_compile_options(fg_resource_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_resource_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(fg_resource_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${GENERATED}"
        ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_resource_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(API 2 3 4)
            foreach(MULTIPLIER 2 3 4)
                if(API EQUAL 2 AND MULTIPLIER EQUAL 4)
                    continue()
                endif()
                add_test(NAME fg_resources_${BACKEND}_v${API}_${MULTIPLIER}x
                    COMMAND fg_resource_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND} ${MULTIPLIER} ${API} lean)
                set_tests_properties(fg_resources_${BACKEND}_v${API}_${MULTIPLIER}x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()
    foreach(MULTIPLIER 2 3)
        add_test(NAME fg_resources_nvidia_v1_${MULTIPLIER}x
            COMMAND fg_resource_regression "$<TARGET_FILE:vdxr_frame_generation>" nvidia ${MULTIPLIER} 1 lean)
        set_tests_properties(fg_resources_nvidia_v1_${MULTIPLIER}x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    foreach(MODE current-failure mrt-failure)
        add_test(NAME fg_resources_${MODE}
            COMMAND fg_resource_regression "$<TARGET_FILE:vdxr_frame_generation>" nvidia 3 4 ${MODE})
        set_tests_properties(fg_resources_${MODE} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    add_executable(nr_fg_early_ready_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_early_ready_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_early_ready_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
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
    add_executable(fg_early_ready_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_early_ready_regression.cpp)
    target_include_directories(fg_early_ready_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
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
    add_executable(nr_fg_bgra_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_bgra_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_bgra_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_bgra_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_bgra_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_bgra_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO typeless unorm srgb rgba rgba-srgb motion motion-srgb separate array-mip source-reuse two-live mixed mixed-reverse unsupported-right invalid-right reuse-completed reuse-pending reuse-removed reuse-stale-submit reuse-reset reuse-poisoned-capture)
        add_test(NAME fg_bgra_${SCENARIO} COMMAND nr_fg_bgra_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_bgra_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_library(fg_v2_host_test_module SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_v2_host_test_module.cpp)
    target_include_directories(fg_v2_host_test_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_v2_host_test_module PRIVATE cxx_std_17)
    add_library(fg_v1_host_test_module SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_v2_host_test_module.cpp)
    target_include_directories(fg_v1_host_test_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_v1_host_test_module PRIVATE cxx_std_17)
    target_compile_definitions(fg_v1_host_test_module PRIVATE VDXRFG_TEST_V1_ONLY)
    add_executable(fg_v2_host_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_v2_host_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(fg_v2_host_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
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

    add_executable(nr_fg_ready_wait_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_ready_wait_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_ready_wait_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
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

    add_executable(fg_v2_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_v2_regression.cpp)
    target_compile_features(fg_v2_regression PRIVATE cxx_std_20)
    target_include_directories(fg_v2_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(fg_v2_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_v2_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_v2 COMMAND fg_v2_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_v2 PROPERTIES TIMEOUT 120)
    add_test(NAME fg_direct_synthetic_output COMMAND fg_v2_regression "$<TARGET_FILE:vdxr_frame_generation>" direct-output)
    set_tests_properties(fg_direct_synthetic_output PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    set(FG_MRT_LEGACY_SHADERS "${GENERATED}" CACHE PATH "Frozen legacy shader headers for MRT differential fixtures")
    add_executable(fg_mrt_operation_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_mrt_operation_regression.cpp)
    add_dependencies(fg_mrt_operation_regression fg_synthesis_shaders)
    target_compile_features(fg_mrt_operation_regression PRIVATE cxx_std_20)
    target_compile_options(fg_mrt_operation_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_mrt_operation_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VDXR_FG_MRT_SHADER_AVAILABLE=1)
    target_include_directories(fg_mrt_operation_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${FG_MRT_LEGACY_SHADERS}" "${GENERATED}"
        ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_mrt_operation_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_mrt_native_3x COMMAND fg_mrt_operation_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_mrt_native_3x PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_test(NAME fg_mrt_native_pso_fallback COMMAND fg_mrt_operation_regression "$<TARGET_FILE:vdxr_frame_generation>" mrt-pso-failure)
    set_tests_properties(fg_mrt_native_pso_fallback PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(fg_mrt_shader_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_mrt_shader_regression.cpp)
    add_dependencies(fg_mrt_shader_regression fg_synthesis_shaders)
    set_source_files_properties(../tests/fg_mrt_operation_regression.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_mrt_shader_regression.cpp
        PROPERTIES OBJECT_DEPENDS "${GENERATED}/nvidia_shared_3x_synthesize_pixel_shader.hpp")
    target_compile_features(fg_mrt_shader_regression PRIVATE cxx_std_20)
    target_compile_options(fg_mrt_shader_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_mrt_shader_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VDXR_FG_MRT_SHADER_AVAILABLE=1)
    if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
        target_compile_definitions(fg_mrt_shader_regression PRIVATE VDXR_FG_PATCH_MOTION_EXPERIMENT=1)
    endif()
    target_include_directories(fg_mrt_shader_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${FG_MRT_LEGACY_SHADERS}" "${GENERATED}")
    target_link_libraries(fg_mrt_shader_regression PRIVATE d3d12 dxgi)
    add_test(NAME fg_mrt_shader_exact COMMAND fg_mrt_shader_regression)
    set_tests_properties(fg_mrt_shader_exact PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(fg_atlas_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_atlas_regression.cpp)
    target_include_directories(fg_atlas_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_atlas_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_atlas_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_atlas_regression PRIVATE d3d12 dxgi)
    add_executable(fg_atlas_backend_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_atlas_backend_regression.cpp
        "${DERIVATIVE}/d3d12_history.cpp" "${DERIVATIVE}/d3d12_frame_synthesizer.cpp"
        "${OFXR}/src/common/dlss_motion_vectors.cpp" "${OFXR}/src/core/pose.cpp"
        "${OFXR}/src/layer/bridge_flight_logger.cpp")
    add_dependencies(fg_atlas_backend_regression fg_synthesis_shaders)
    target_compile_features(fg_atlas_backend_regression PRIVATE cxx_std_20)
    target_compile_options(fg_atlas_backend_regression PRIVATE /W4 /permissive- /EHsc /fp:precise)
    target_compile_definitions(fg_atlas_backend_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_include_directories(fg_atlas_backend_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${DERIVATIVE}" "${OFXR}/include"
        "${GENERATED}" "${FFX}/include"
        "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
    target_link_libraries(fg_atlas_backend_regression PRIVATE fg_depth_trace d3d12 dxgi
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

    add_executable(fg_history_consumed_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_history_consumed_regression.cpp)
    target_compile_features(fg_history_consumed_regression PRIVATE cxx_std_20)
    target_include_directories(fg_history_consumed_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_history_consumed_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            foreach(OWNER host module)
                add_test(NAME fg_history_consumed_${BACKEND}_${MULTIPLIER}x_${OWNER}
                    COMMAND fg_history_consumed_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND} ${MULTIPLIER} ${OWNER})
                set_tests_properties(fg_history_consumed_${BACKEND}_${MULTIPLIER}x_${OWNER}
                    PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()

    add_executable(fg_ffx_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_ffx_regression.cpp)
    target_compile_features(fg_ffx_regression PRIVATE cxx_std_20)
    target_include_directories(fg_ffx_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_ffx_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_ffx_full COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_ffx_full PROPERTIES TIMEOUT 120)
    add_test(NAME fg_timing_nvidia COMMAND fg_ffx_regression "$<TARGET_FILE:vdxr_frame_generation>" prime-baseline)
    set_tests_properties(fg_timing_nvidia PROPERTIES TIMEOUT 120)

    add_executable(fg_c_abi_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_c_abi_regression.c)
    target_include_directories(fg_c_abi_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_c_abi_regression PRIVATE c_std_11)
    add_test(NAME fg_c_abi COMMAND fg_c_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_c_abi PROPERTIES TIMEOUT 60)
    add_executable(nr_fg_integration_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_integration_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_integration_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(nr_fg_integration_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_integration_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_integration_regression PRIVATE d3d11 d3d12 dxgi)
    foreach(SCENARIO interop invalid-plan stale-ticket blocked-consumer invalid-context copy-identity copy-alias event-stale source-overwrite)
        add_test(NAME fg_host_${SCENARIO} COMMAND nr_fg_integration_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_host_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(nr_fg_pack_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_pack_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_pack_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_pack_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_pack_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_pack_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO packed offset separate gap overlap reverse different-y array-mip invalid-rect invalid-pose wrong-device)
        add_test(NAME fg_pack_${SCENARIO} COMMAND nr_fg_pack_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_pack_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(nr_fg_event_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_event_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_event_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_event_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_event_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_event_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO removed event-failure wait-failure stale-wake)
        add_test(NAME fg_event_${SCENARIO} COMMAND nr_fg_event_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_event_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(nr_fg_ready_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_ready_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_ready_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_compile_features(nr_fg_ready_regression PRIVATE cxx_std_17)
    target_compile_definitions(nr_fg_ready_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nr_fg_ready_regression PRIVATE d3d11 d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(SCENARIO timeout event-failure stale-wake removed invalid next)
        add_test(NAME fg_ready_${SCENARIO} COMMAND nr_fg_ready_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_ready_${SCENARIO} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(nr_fg_presenter_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_presenter_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_presenter_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr)
    target_compile_features(nr_fg_presenter_regression PRIVATE cxx_std_17)
    add_test(NAME fg_cadence COMMAND nr_fg_presenter_regression)
    set_tests_properties(fg_cadence PROPERTIES TIMEOUT 60)
    add_executable(nr_fg_4x_phase_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/nr_fg_4x_phase_regression.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr/frame_generation_pacing.cpp)
    target_include_directories(nr_fg_4x_phase_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../virtualdesktop-openxr)
    target_compile_features(nr_fg_4x_phase_regression PRIVATE cxx_std_17)
    add_test(NAME fg_4x_phase COMMAND nr_fg_4x_phase_regression)
    set_tests_properties(fg_4x_phase PROPERTIES TIMEOUT 60)
    add_executable(fg_abi_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_abi_regression.cpp)
    target_include_directories(fg_abi_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_abi_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_abi_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME fg_abi COMMAND fg_abi_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_abi PROPERTIES TIMEOUT 60)
    add_executable(fg_submit_validation_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_submit_validation_regression.cpp)
    target_include_directories(fg_submit_validation_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_submit_validation_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_submit_validation_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_submit_validation_regression PRIVATE d3d12 dxgi)
    foreach(SCENARIO foreign-token foreign-busy fractions)
        add_test(NAME fg_submit_validation_${SCENARIO}
            COMMAND fg_submit_validation_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_submit_validation_${SCENARIO} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
    add_executable(fg_gpu_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_gpu_regression.cpp)
    target_include_directories(fg_gpu_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(fg_gpu_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_gpu_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_gpu_regression PRIVATE d3d12 dxgi)
    add_executable(fg_state_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_state_regression.cpp)
    target_include_directories(fg_state_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
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
    add_executable(fg_initialization_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_initialization_regression.cpp)
    target_include_directories(fg_initialization_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include"
        "${OFXR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7/NvOFInterface")
    target_compile_features(fg_initialization_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_initialization_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fg_initialization_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    add_test(NAME fg_initialization COMMAND fg_initialization_regression "$<TARGET_FILE:vdxr_frame_generation>")
    set_tests_properties(fg_initialization PROPERTIES TIMEOUT 60)
    foreach(API 2 3 4)
        add_test(NAME fg_initialization_allocation_v${API} COMMAND fg_initialization_regression
            "$<TARGET_FILE:vdxr_frame_generation>" allocation ${API})
        set_tests_properties(fg_initialization_allocation_v${API} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME fg_initialization_destroy_v${API} COMMAND fg_initialization_regression
            "$<TARGET_FILE:vdxr_frame_generation>" destroy ${API})
        set_tests_properties(fg_initialization_destroy_v${API} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()
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
        add_dependencies(${HOST} fg_host_shader fg_depth_host_shader)
        target_include_directories(${HOST} PRIVATE "${GENERATED}")
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(fg_endpoint_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_endpoint_regression.cpp)
    target_include_directories(fg_endpoint_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${GENERATED}")
    target_compile_features(fg_endpoint_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_endpoint_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
        target_compile_definitions(fg_endpoint_regression PRIVATE VDXR_FG_PATCH_MOTION_EXPERIMENT=1)
    endif()
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

if(BUILD_TESTING)
    add_executable(fg_poll_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_poll_regression.cpp)
    target_compile_features(fg_poll_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_poll_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(fg_poll_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_poll_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        add_test(NAME fg_poll_${BACKEND}
            COMMAND fg_poll_regression "$<TARGET_FILE:vdxr_frame_generation>" ${BACKEND})
        set_tests_properties(fg_poll_${BACKEND} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(fg_work_regression ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fg_work_regression.cpp)
    add_dependencies(fg_work_regression fg_synthesis_shaders)
    target_compile_features(fg_work_regression PRIVATE cxx_std_20)
    target_compile_definitions(fg_work_regression PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(VDXR_FG_PATCH_MOTION_EXPERIMENT)
        target_compile_definitions(fg_work_regression PRIVATE VDXR_FG_PATCH_MOTION_EXPERIMENT=1)
    endif()
    target_include_directories(fg_work_regression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/module-checkpoint/include" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${GENERATED}" ${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/include)
    target_link_libraries(fg_work_regression PRIVATE d3d12 dxgi
        "${CMAKE_CURRENT_SOURCE_DIR}/../packages/Detours.4.0.1/lib/native/libs/x64/detours.lib")
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            foreach(TIMING off on)
                add_test(NAME fg_work_${BACKEND}_${MULTIPLIER}x_${TIMING}
                    COMMAND fg_work_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} ${BACKEND} ${TIMING})
                set_tests_properties(fg_work_${BACKEND}_${MULTIPLIER}x_${TIMING} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()
    add_test(NAME fg_work_mrt_fallback COMMAND fg_work_regression "$<TARGET_FILE:vdxr_frame_generation>" 3 nvidia off mrt-failure)
    set_tests_properties(fg_work_mrt_fallback PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING)
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            foreach(TIMING off on)
                add_test(NAME fg_work_gbv_${BACKEND}_${MULTIPLIER}x_${TIMING}
                    COMMAND fg_work_regression "$<TARGET_FILE:vdxr_frame_generation>" ${MULTIPLIER} ${BACKEND} ${TIMING})
                set_tests_properties(fg_work_gbv_${BACKEND}_${MULTIPLIER}x_${TIMING} PROPERTIES
                    TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "VDXR_FG_WORK_GBV=1")
            endforeach()
        endforeach()
    endforeach()
endif()

if(BUILD_TESTING)
    foreach(BACKEND nvidia fidelityfx-full)
        foreach(MULTIPLIER 2 3 4)
            foreach(MODE deferred host-deferred)
                add_test(NAME fg_work_core_${BACKEND}_${MULTIPLIER}x_${MODE}
                    COMMAND fg_atlas_backend_regression ${BACKEND} ${MULTIPLIER} full "" ${MODE})
                set_tests_properties(fg_work_core_${BACKEND}_${MULTIPLIER}x_${MODE} PROPERTIES
                    TIMEOUT 180 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
    endforeach()
endif()

# Private source/object/PE/PDB binding; shader recipes and optimization unchanged.
target_compile_options(vdxr_frame_generation PRIVATE /Zi /ZH:SHA_256)
target_link_options(vdxr_frame_generation PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
