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
    "${OFXR}/src/d3d12/d3d12_frame_synthesizer.cpp" "${OFXR}/manifest.json")
set(FFX "${FG_DEPS}/fidelityfx/sdk")
set(FXC "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe" CACHE FILEPATH "Pinned shader compiler")
if(NOT EXISTS "${FXC}" OR NOT EXISTS "${FFX}/bin/ffx_sdk/ffx_opticalflow_x64.lib")
    message(FATAL_ERROR "Build pinned dependencies before configuring the module")
endif()
set(SHADER "${OFXR}/src/d3d12/frame_synthesis.hlsl")
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
file(MAKE_DIRECTORY "${GENERATED}")
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
    foreach(SCENARIO interop invalid-plan stale-ticket blocked-consumer invalid-context copy-identity copy-alias event-stale)
        add_test(NAME fg_host_${SCENARIO} COMMAND nr_fg_integration_regression "$<TARGET_FILE:vdxr_frame_generation>" ${SCENARIO})
        set_tests_properties(fg_host_${SCENARIO} PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "PASS:")
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
    endforeach()
endif()
