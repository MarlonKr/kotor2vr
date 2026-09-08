cmake_minimum_required(VERSION 3.24)

project(kotor2vr VERSION 0.1.0 LANGUAGES CXX)

# Portable native binaries do not require an installed Visual C++ runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/EHsc)
endif()

include(CTest)

option(KOTOR2VR_WARNINGS_AS_ERRORS "Treat KOTOR2VR warnings as errors" OFF)

add_library(kotor2vr-common STATIC
  src/common/build_descriptor.cpp
  src/common/hook_validation.cpp
  src/common/stereo_stream.cpp
  src/common/vr_input.cpp
  src/common/shared_memory_channel.cpp)

target_compile_features(kotor2vr-common PUBLIC cxx_std_20)
target_include_directories(kotor2vr-common PUBLIC src/common)

if(MSVC)
  target_compile_options(kotor2vr-common PRIVATE /W4 /permissive- /EHsc /utf-8)
  if(KOTOR2VR_WARNINGS_AS_ERRORS)
    target_compile_options(kotor2vr-common PRIVATE /WX)
  endif()
else()
  target_compile_options(kotor2vr-common PRIVATE -Wall -Wextra -Wpedantic)
  if(KOTOR2VR_WARNINGS_AS_ERRORS)
    target_compile_options(kotor2vr-common PRIVATE -Werror)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(kotor2vr-controller-input-filter-tests tests/controller_input_filter_tests.cpp)
  target_compile_features(kotor2vr-controller-input-filter-tests PRIVATE cxx_std_20)
  target_include_directories(kotor2vr-controller-input-filter-tests PRIVATE src/game32)
  add_test(NAME controller-input-filter COMMAND kotor2vr-controller-input-filter-tests)
  add_executable(kotor2vr-native-stereo-tests tests/native_stereo_tests.cpp)
  target_link_libraries(kotor2vr-native-stereo-tests PRIVATE kotor2vr-common)
  add_test(NAME native-stereo-contract COMMAND kotor2vr-native-stereo-tests)
  add_executable(kotor2vr-common-tests tests/common_tests.cpp)
  target_link_libraries(kotor2vr-common-tests PRIVATE kotor2vr-common)
  add_test(NAME common-core COMMAND kotor2vr-common-tests)

  add_executable(kotor2vr-shared-memory-tests
    tests/shared_memory_channel_tests.cpp)
  target_link_libraries(kotor2vr-shared-memory-tests PRIVATE kotor2vr-common)
  add_test(NAME common-shared-memory COMMAND kotor2vr-shared-memory-tests)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(BUILD_TESTING)
    add_executable(kotor2vr-composition-depth-tests tests/composition_depth_cpu_tests.cpp src/host64/src/composition_depth.cpp)
    target_compile_features(kotor2vr-composition-depth-tests PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-composition-depth-tests PRIVATE src/host64/include)
    target_link_libraries(kotor2vr-composition-depth-tests PRIVATE d3d12 d3dcompiler)
    add_test(NAME composition-depth-contract COMMAND kotor2vr-composition-depth-tests)
  endif()
  add_subdirectory(src/host64)
  set_target_properties(kotorvr-host64 PROPERTIES
    OUTPUT_NAME "kotor2vr-host64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/kotor2vr-camera.ini"
       INPUT "${CMAKE_SOURCE_DIR}/kotor2vr-camera.ini")
  add_library(kotor2vr-game32 SHARED
    src/game32/dll_main.cpp
    src/game32/controller_input.cpp
    src/game32/probe.cpp
    src/game32/game_image_capture.cpp
    src/game32/gl_ext_d3d12_bridge.cpp
    src/game32/gpu_stream_producer.cpp
    src/game32/nv_dx_interop_bridge.cpp
    src/game32/render_trace.cpp
    src/game32/native_stereo.cpp
    src/game32/depth_pack.cpp
    src/game32/scene_replay.cpp
    src/game32/vr_bridge.cpp
    src/game32/game32.def)
  target_compile_features(kotor2vr-game32 PRIVATE cxx_std_20)
  target_compile_definitions(kotor2vr-game32 PRIVATE K2VR_GAME32_BUILD=1)
  target_include_directories(kotor2vr-game32 PRIVATE src/common src/game32)
  target_link_libraries(kotor2vr-game32 PRIVATE
    kotor2vr-common bcrypt d3d11 d3d12 dxgi opengl32)
  set_target_properties(kotor2vr-game32 PROPERTIES
    OUTPUT_NAME "kotor2vr-game32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  if(MSVC)
    target_compile_options(kotor2vr-game32 PRIVATE /W4 /permissive- /EHsc /utf-8)
    if(KOTOR2VR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-game32 PRIVATE /WX)
    endif()
  endif()

  if(BUILD_TESTING)
    # Explicit hardware smoke test; a hidden real OpenGL context is required.
    add_executable(kotor2vr-gl-context-rebind-live-smoke
      tests/gl_context_rebind_live_smoke.cpp
      src/game32/gl_ext_d3d12_bridge.cpp src/game32/nv_dx_interop_bridge.cpp)
    target_compile_features(kotor2vr-gl-context-rebind-live-smoke PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-gl-context-rebind-live-smoke PRIVATE src/game32 src/common)
    target_link_libraries(kotor2vr-gl-context-rebind-live-smoke PRIVATE
      kotor2vr-common d3d11 d3d12 dxgi opengl32 gdi32 user32)
    set_target_properties(kotor2vr-gl-context-rebind-live-smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_executable(kotor2vr-depth-pack-live-smoke tests/depth_pack_live_smoke.cpp src/game32/depth_pack.cpp)
    target_compile_features(kotor2vr-depth-pack-live-smoke PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-depth-pack-live-smoke PRIVATE src/game32)
    target_link_libraries(kotor2vr-depth-pack-live-smoke PRIVATE opengl32 gdi32 user32)
    set_target_properties(kotor2vr-depth-pack-live-smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_executable(kotor2vr-scene-replay-live-smoke
      tests/scene_replay_live_smoke.cpp src/game32/scene_replay.cpp)
    target_compile_features(kotor2vr-scene-replay-live-smoke PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-scene-replay-live-smoke PRIVATE src/game32)
    target_link_libraries(kotor2vr-scene-replay-live-smoke PRIVATE opengl32 gdi32 user32)
    set_target_properties(kotor2vr-scene-replay-live-smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    add_executable(kotor2vr-game32-bootstrap-tests
      tests/game32_bootstrap_tests.cpp)
    target_compile_features(kotor2vr-game32-bootstrap-tests PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-game32-bootstrap-tests PRIVATE src/game32)
    target_link_libraries(kotor2vr-game32-bootstrap-tests PRIVATE kotor2vr-common)
    add_dependencies(kotor2vr-game32-bootstrap-tests kotor2vr-game32)
    set_target_properties(kotor2vr-game32-bootstrap-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-game32-bootstrap-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-game32-bootstrap-tests PRIVATE /WX)
      endif()
    endif()
    add_test(NAME game32-explicit-bootstrap
      COMMAND kotor2vr-game32-bootstrap-tests)

    add_executable(kotor2vr-render-trace-logic-tests
      tests/render_trace_logic_tests.cpp)
    target_compile_features(kotor2vr-render-trace-logic-tests PRIVATE cxx_std_20)
    target_include_directories(kotor2vr-render-trace-logic-tests PRIVATE
      src/game32)
    set_target_properties(kotor2vr-render-trace-logic-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-render-trace-logic-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-render-trace-logic-tests PRIVATE /WX)
      endif()
    endif()
    add_test(NAME game32-render-trace-cell-decisions
      COMMAND kotor2vr-render-trace-logic-tests)

    add_executable(kotor2vr-game-image-capture-logic-tests
      tests/game_image_capture_logic_tests.cpp)
    target_compile_features(kotor2vr-game-image-capture-logic-tests PRIVATE
      cxx_std_20)
    target_include_directories(kotor2vr-game-image-capture-logic-tests PRIVATE
      src/common src/game32)
    set_target_properties(kotor2vr-game-image-capture-logic-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-game-image-capture-logic-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-game-image-capture-logic-tests PRIVATE /WX)
      endif()
    endif()
    add_test(NAME game32-game-image-capture-decisions
      COMMAND kotor2vr-game-image-capture-logic-tests)

    add_executable(kotor2vr-nv-dx-interop-logic-tests
      tests/nv_dx_interop_bridge_logic_tests.cpp)
    target_compile_features(kotor2vr-nv-dx-interop-logic-tests PRIVATE
      cxx_std_20)
    target_include_directories(kotor2vr-nv-dx-interop-logic-tests PRIVATE
      src/game32)
    set_target_properties(kotor2vr-nv-dx-interop-logic-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-nv-dx-interop-logic-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-nv-dx-interop-logic-tests PRIVATE /WX)
      endif()
    endif()
    add_test(NAME game32-nv-dx-interop-decisions
      COMMAND kotor2vr-nv-dx-interop-logic-tests)

    add_executable(kotor2vr-gl-ext-d3d12-logic-tests
      tests/gl_ext_d3d12_bridge_logic_tests.cpp)
    target_compile_features(kotor2vr-gl-ext-d3d12-logic-tests PRIVATE
      cxx_std_20)
    target_include_directories(kotor2vr-gl-ext-d3d12-logic-tests PRIVATE
      src/game32)
    set_target_properties(kotor2vr-gl-ext-d3d12-logic-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-gl-ext-d3d12-logic-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-gl-ext-d3d12-logic-tests PRIVATE /WX)
      endif()
    endif()
    add_test(NAME game32-gl-ext-d3d12-decisions
      COMMAND kotor2vr-gl-ext-d3d12-logic-tests)

    # Deliberately not registered with CTest: this is a bounded, manually run
    # hardware diagnostic that needs a real NVIDIA OpenGL context.
    add_executable(kotor2vr-nv-dx-interop-live-smoke
      tests/nv_dx_interop_live_smoke.cpp
      src/game32/nv_dx_interop_bridge.cpp)
    target_compile_features(kotor2vr-nv-dx-interop-live-smoke PRIVATE
      cxx_std_20)
    target_include_directories(kotor2vr-nv-dx-interop-live-smoke PRIVATE
      src/game32)
    target_link_libraries(kotor2vr-nv-dx-interop-live-smoke PRIVATE
      d3d11 dxgi opengl32 gdi32 user32)
    set_target_properties(kotor2vr-nv-dx-interop-live-smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-nv-dx-interop-live-smoke PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-nv-dx-interop-live-smoke PRIVATE /WX)
      endif()
    endif()

    add_executable(kotor2vr-gpu-stream-producer-logic-tests
      tests/gpu_stream_producer_logic_tests.cpp)
    target_compile_features(kotor2vr-gpu-stream-producer-logic-tests PRIVATE
      cxx_std_20)
    target_include_directories(kotor2vr-gpu-stream-producer-logic-tests PRIVATE
      src/common src/game32)
    set_target_properties(kotor2vr-gpu-stream-producer-logic-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
      target_compile_options(kotor2vr-gpu-stream-producer-logic-tests PRIVATE
        /W4 /permissive- /EHsc /utf-8)
      if(KOTOR2VR_WARNINGS_AS_ERRORS)
        target_compile_options(kotor2vr-gpu-stream-producer-logic-tests PRIVATE
          /WX)
      endif()
    endif()
    add_test(NAME game32-gpu-stream-producer-decisions
      COMMAND kotor2vr-gpu-stream-producer-logic-tests)
  endif()
endif()

foreach(target kotor2vr-common-tests kotor2vr-shared-memory-tests)
  if(TARGET ${target})
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  endif()
endforeach()


