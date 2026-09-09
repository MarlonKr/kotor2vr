cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(kotorvr_host64 LANGUAGES CXX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # The top-level build supplies the full common target. A standalone host
  # still needs the concrete shared-memory channel, not headers alone.
  add_library(kotor2vr-common STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/movie_frame.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/shared_memory_channel.cpp")
  target_compile_features(kotor2vr-common PUBLIC cxx_std_20)
  target_include_directories(kotor2vr-common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../common")
endif()

if(NOT WIN32)
  message(FATAL_ERROR "kotorvr-host64 currently targets 64-bit Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "kotorvr-host64 must be built for x64")
endif()

option(KOTORVR_ENABLE_OPENXR "Build OpenXR with the pinned static loader (OFF builds a stub)" ON)
option(KOTORVR_WARNINGS_AS_ERRORS "Treat host warnings as errors" OFF)

set(KOTORVR_HAS_OPENXR 0)
set(KOTORVR_OPENXR_TARGET "")

if(KOTORVR_ENABLE_OPENXR)
  # Release and developer builds deliberately use the pinned source checkout.
  # Accepting an arbitrary package here can silently select an import library
  # for openxr_loader[d].dll and reintroduce a machine-local deployment
  # dependency. A dependency-free diagnostic stub remains available via
  # -DKOTORVR_ENABLE_OPENXR=OFF.
  set(KOTORVR_VENDORED_OPENXR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/OpenXR-SDK")
  if(NOT EXISTS "${KOTORVR_VENDORED_OPENXR}/CMakeLists.txt")
    message(FATAL_ERROR
      "KOTORVR_ENABLE_OPENXR=ON requires the pinned third_party/OpenXR-SDK checkout. "
      "Restore the vendored source snapshot or configure the diagnostic stub with "
      "-DKOTORVR_ENABLE_OPENXR=OFF.")
  endif()

  if(TARGET openxr_loader OR TARGET OpenXR::openxr_loader)
    message(FATAL_ERROR
      "An OpenXR loader target already exists before the pinned SDK is added; "
      "refusing an ambiguous or potentially dynamic loader dependency.")
  endif()

  set(BUILD_LOADER ON CACHE BOOL "" FORCE)
  set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNAMIC_LOADER OFF CACHE BOOL "Build the pinned OpenXR loader statically" FORCE)
  add_subdirectory("${KOTORVR_VENDORED_OPENXR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/openxr-sdk"
                   EXCLUDE_FROM_ALL)

  if(NOT TARGET openxr_loader)
    message(FATAL_ERROR "Pinned OpenXR SDK did not define the openxr_loader target")
  endif()

  get_target_property(KOTORVR_OPENXR_TARGET_TYPE openxr_loader TYPE)
  if(NOT KOTORVR_OPENXR_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
      "Pinned OpenXR loader must be STATIC_LIBRARY, got '${KOTORVR_OPENXR_TARGET_TYPE}'. "
      "Dynamic loader targets are not supported.")
  endif()

  # Override the SDK static-loader default to match portable /MT binaries.
  set_property(TARGET openxr_loader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(KOTORVR_HAS_OPENXR 1)
  set(KOTORVR_OPENXR_TARGET openxr_loader)
endif()

# V1 preserves the optional backend ABI with an explicit unavailable stub.
add_library(kotorvr-direct-neural STATIC src/direct_neural_eye_stub.cpp)
target_compile_features(kotorvr-direct-neural PRIVATE cxx_std_20)
target_include_directories(kotorvr-direct-neural PUBLIC include)
add_executable(kotorvr-host64
  src/main.cpp
  src/neural_worker_client_stub.cpp
  src/neural_guides.cpp
  src/neural_stereo_pipeline.cpp
  src/neural_reprojection.cpp
  src/composition_depth.cpp
  src/d3d12_bgra_upload.cpp
  src/d3d12_context.cpp
  src/dlss_backend.cpp
  src/frame_ring.cpp
  src/game_image_snapshot.cpp
  src/gpu_stream_consumer.cpp
  src/host_ipc_publisher.cpp
  src/logger.cpp
  src/openxr_runtime.cpp
  src/session_state_machine.cpp
  src/visible_smoke.cpp)

set_target_properties(kotorvr-host64 PROPERTIES OUTPUT_NAME "kotor2vr-host64")

target_compile_features(kotorvr-host64 PRIVATE cxx_std_20)
target_compile_definitions(kotorvr-host64 PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  KOTORVR_HAS_OPENXR=${KOTORVR_HAS_OPENXR})
target_include_directories(kotorvr-host64 PRIVATE include)
target_link_libraries(kotorvr-host64 PRIVATE kotor2vr-common kotorvr-direct-neural d3d12 dxgi d3dcompiler)

if(KOTORVR_HAS_OPENXR)
  target_link_libraries(kotorvr-host64 PRIVATE ${KOTORVR_OPENXR_TARGET})
  message(STATUS "kotorvr-host64: OpenXR runtime path enabled with pinned static loader")
else()
  message(STATUS "kotorvr-host64: OpenXR disabled; building diagnostic stub")
endif()

if(MSVC)
  target_compile_options(kotorvr-host64 PRIVATE /W4 /permissive- /EHsc /utf-8)
  if(KOTORVR_WARNINGS_AS_ERRORS)
    target_compile_options(kotorvr-host64 PRIVATE /WX)
  endif()
else()
  target_compile_options(kotorvr-host64 PRIVATE -Wall -Wextra -Wpedantic)
  if(KOTORVR_WARNINGS_AS_ERRORS)
    target_compile_options(kotorvr-host64 PRIVATE -Werror)
  endif()
endif()

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT kotorvr-host64)

if(BUILD_TESTING)
  add_test(NAME host-contract COMMAND kotorvr-host64 --self-test)

  add_executable(kotor2vr-visible-smoke-tests
    src/visible_smoke.cpp
    ../../tests/visible_smoke_tests.cpp)
  target_compile_features(kotor2vr-visible-smoke-tests PRIVATE cxx_std_20)
  target_include_directories(kotor2vr-visible-smoke-tests PRIVATE include)
  target_link_libraries(kotor2vr-visible-smoke-tests PRIVATE kotor2vr-common)
  if(MSVC)
    target_compile_options(kotor2vr-visible-smoke-tests PRIVATE
      /W4 /permissive- /EHsc /utf-8)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-visible-smoke-tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(kotor2vr-visible-smoke-tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-visible-smoke-tests PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME host-visible-smoke-layout
    COMMAND kotor2vr-visible-smoke-tests)

  add_executable(kotor2vr-game-image-smoke-tests
    src/d3d12_bgra_upload.cpp
    src/game_image_snapshot.cpp
    ../../tests/game_image_smoke_tests.cpp)
  target_compile_features(kotor2vr-game-image-smoke-tests PRIVATE cxx_std_20)
  target_compile_definitions(kotor2vr-game-image-smoke-tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(kotor2vr-game-image-smoke-tests PRIVATE include)
  target_link_libraries(kotor2vr-game-image-smoke-tests PRIVATE
    kotor2vr-common d3d12 dxgi)
  if(MSVC)
    target_compile_options(kotor2vr-game-image-smoke-tests PRIVATE
      /W4 /permissive- /EHsc /utf-8)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-game-image-smoke-tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(kotor2vr-game-image-smoke-tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-game-image-smoke-tests PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME host-game-image-smoke
    COMMAND kotor2vr-game-image-smoke-tests)

  add_executable(kotor2vr-gpu-stream-consumer-tests
    src/gpu_stream_consumer.cpp
    ../../tests/gpu_stream_consumer_tests.cpp)
  target_compile_features(kotor2vr-gpu-stream-consumer-tests PRIVATE cxx_std_20)
  target_compile_definitions(kotor2vr-gpu-stream-consumer-tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(kotor2vr-gpu-stream-consumer-tests PRIVATE include)
  target_link_libraries(kotor2vr-gpu-stream-consumer-tests PRIVATE
    kotor2vr-common d3d12 dxgi)
  if(MSVC)
    target_compile_options(kotor2vr-gpu-stream-consumer-tests PRIVATE
      /W4 /permissive- /EHsc /utf-8)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-gpu-stream-consumer-tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(kotor2vr-gpu-stream-consumer-tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-gpu-stream-consumer-tests PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME host-gpu-stream-consumer
    COMMAND kotor2vr-gpu-stream-consumer-tests)

  add_executable(kotor2vr-host-ipc-tests
    src/host_ipc_publisher.cpp
    ../../tests/host_ipc_publisher_tests.cpp)
  target_compile_features(kotor2vr-host-ipc-tests PRIVATE cxx_std_20)
  target_compile_definitions(kotor2vr-host-ipc-tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(kotor2vr-host-ipc-tests PRIVATE include)
  target_link_libraries(kotor2vr-host-ipc-tests PRIVATE kotor2vr-common)
  if(MSVC)
    target_compile_options(kotor2vr-host-ipc-tests PRIVATE
      /W4 /permissive- /EHsc /utf-8)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-host-ipc-tests PRIVATE /WX)
    endif()
  else()
    target_compile_options(kotor2vr-host-ipc-tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(KOTORVR_WARNINGS_AS_ERRORS)
      target_compile_options(kotor2vr-host-ipc-tests PRIVATE -Werror)
    endif()
  endif()
  add_test(NAME host-ipc-publisher COMMAND kotor2vr-host-ipc-tests)
endif()



