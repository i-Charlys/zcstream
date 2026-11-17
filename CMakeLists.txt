cmake_minimum_required(VERSION 3.20)
project(zcrudp_streaming LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ZCRUDP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zcrudp")
if(NOT EXISTS "${ZCRUDP_DIR}/include/rudp_nextgen.h")
  message(FATAL_ERROR "zcrudp not found at ${ZCRUDP_DIR}")
endif()

set(TERN_VID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tern_vid")
if(NOT EXISTS "${TERN_VID_DIR}/include/ternary_zcrudp_bridge.h")
  message(FATAL_ERROR "tern_vid not found at ${TERN_VID_DIR}")
endif()

set(ZCRUDP_SOURCES
  "${ZCRUDP_DIR}/src/codec.c"
  "${ZCRUDP_DIR}/src/reliability.c"
  "${ZCRUDP_DIR}/src/session.c"
  "${ZCRUDP_DIR}/src/session_tx.c"
  "${ZCRUDP_DIR}/src/session_rx.c"
  "${ZCRUDP_DIR}/src/rx.c"
  "${ZCRUDP_DIR}/src/io.c"
  "${ZCRUDP_DIR}/src/crypto.c"
  "${ZCRUDP_DIR}/src/nextgen.c"
)

add_library(zcrudp_lib STATIC ${ZCRUDP_SOURCES})
target_include_directories(zcrudp_lib PUBLIC "${ZCRUDP_DIR}/include")
# No CMAKE_BUILD_TYPE is set by the documented configure line, which left the
# transport (GF(2^8) FEC included) unoptimised while the executables were -O2.
if(MSVC)
  target_compile_options(zcrudp_lib PRIVATE /O2)
else()
  target_compile_options(zcrudp_lib PRIVATE -O2)
endif()
target_compile_definitions(zcrudp_lib PUBLIC
  RUDP_PAGE_SIZE=4096U
  RUDP_PAGE_POOL_PAGES=8192U
)

# Loopback unit test of the fragment/FEC bridge (no GPU needed): ctest --test-dir build
enable_testing()
add_executable(test_video_bridge
  tests/test_video_bridge.c
  src/zcrudp_video_bridge.c
)
target_include_directories(test_video_bridge PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${TERN_VID_DIR}/include"
)
target_link_libraries(test_video_bridge PRIVATE zcrudp_lib ws2_32)
if(NOT MSVC)
  target_compile_options(test_video_bridge PRIVATE -O2 -Wall -Wextra)
endif()
add_test(NAME test_video_bridge COMMAND test_video_bridge)

add_executable(zcrudp_stream_bench
  src/zcrudp_video_bridge.c
  src/gpu_pipeline_win32.c
  src/main.c
)

target_include_directories(zcrudp_stream_bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${TERN_VID_DIR}/include"
)

target_link_libraries(zcrudp_stream_bench PRIVATE
  zcrudp_lib
  d3d11
  dxgi
  dxguid
  d3dcompiler
  mfplat
  mfuuid
  mfreadwrite
  ws2_32
  winmm
  ole32
  user32
  gdi32
)

target_compile_definitions(zcrudp_stream_bench PRIVATE
  TERN_VID_SHADERS_DIR="${TERN_VID_DIR}/shaders"
)

if(MSVC)
  target_compile_options(zcrudp_stream_bench PRIVATE /O2 /W3)
else()
  target_compile_options(zcrudp_stream_bench PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
endif()

# Pyrowave (Vulkan Wavelet GPU Codec) + zcrudp::nextgen Benchmark
set(PYROWAVE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pyrowave-master")
set(PYROWAVE_BUILD_DIR "${PYROWAVE_SRC_DIR}/build")
if(EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave.a")
  add_executable(zcrudp_pyrowave_bench
    src/pyrowave_zcrudp_bench.cpp
    src/zcrudp_video_bridge.c
  )
  target_include_directories(zcrudp_pyrowave_bench PRIVATE
    "${TERN_VID_DIR}/include"
    "${PYROWAVE_SRC_DIR}"
    "${PYROWAVE_SRC_DIR}/Granite/util"
    "${PYROWAVE_SRC_DIR}/Granite/math"
    "${PYROWAVE_SRC_DIR}/Granite/vulkan"
    "${PYROWAVE_SRC_DIR}/Granite/third_party/volk"
    "${PYROWAVE_SRC_DIR}/Granite/third_party/khronos/vulkan-headers/include"
  )
  target_link_libraries(zcrudp_pyrowave_bench PRIVATE
    zcrudp_lib
    "${PYROWAVE_BUILD_DIR}/libpyrowave.a"
    "${PYROWAVE_BUILD_DIR}/Granite/vulkan/libgranite-vulkan.a"
    "${PYROWAVE_BUILD_DIR}/Granite/math/libgranite-math.a"
    "${PYROWAVE_BUILD_DIR}/Granite/util/libgranite-util.a"
    "${PYROWAVE_BUILD_DIR}/Granite/third_party/libgranite-volk.a"
    ws2_32
    winmm
  )
  target_compile_options(zcrudp_pyrowave_bench PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
endif()

# Ternary HLSL Compute + zcrudp::nextgen + Winsock RIO Benchmark
add_executable(zcrudp_ternary_bench
  src/ternary_zcrudp_bench.c
  src/gpu_pipeline_win32.c
  src/zcrudp_video_bridge.c
)

target_include_directories(zcrudp_ternary_bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${TERN_VID_DIR}/include"
)

target_compile_definitions(zcrudp_ternary_bench PRIVATE
  TERN_VID_SHADERS_DIR="${TERN_VID_DIR}/shaders"
)

target_link_libraries(zcrudp_ternary_bench PRIVATE
  zcrudp_lib
  d3d11
  dxgi
  dxguid
  d3dcompiler
  mfplat
  mfuuid
  mfreadwrite
  ws2_32
  winmm
  ole32
  user32
  gdi32
)

if(MSVC)
  target_compile_options(zcrudp_ternary_bench PRIVATE /O2 /W3)
else()
  target_compile_options(zcrudp_ternary_bench PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -mavx2)
endif()

# Ternary codec motion bench (GPU, no network; not part of ctest: needs a
# D3D11 GPU). See the header of tests/ternary_motion_bench.c.
add_executable(ternary_motion_bench tests/ternary_motion_bench.c)
target_include_directories(ternary_motion_bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${TERN_VID_DIR}/include"
)
target_compile_definitions(ternary_motion_bench PRIVATE
  TERN_VID_SHADERS_DIR="${TERN_VID_DIR}/shaders"
)
target_link_libraries(ternary_motion_bench PRIVATE
  zcrudp_lib d3d11 dxgi dxguid d3dcompiler mfplat mfuuid mfreadwrite
  ws2_32 winmm ole32 oleaut32 user32 gdi32
)
if(NOT MSVC)
  target_compile_options(ternary_motion_bench PRIVATE -O2 -mavx2)
endif()

# Live 2-PC / Continuous Screen Streaming (Sender / Receiver)
set(ZPSYNC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zpsync")
if(NOT EXISTS "${ZPSYNC_DIR}/include/zpsync.h")
  message(FATAL_ERROR "zpsync not found at ${ZPSYNC_DIR}")
endif()

add_library(zpsync_lib STATIC
  "${ZPSYNC_DIR}/src/zpsync_net.c"
  "${ZPSYNC_DIR}/src/zpsync_crypto.c"
  "${ZPSYNC_DIR}/src/zpsync_stun.c"
  "${ZPSYNC_DIR}/src/zpsync_dht.c"
  "${ZPSYNC_DIR}/src/zpsync_sentinel.c"
  "${ZPSYNC_DIR}/src/zpsync_punch.c"
  "${ZPSYNC_DIR}/src/zpsync_cruise.c"
  "${ZPSYNC_DIR}/src/zpsync.c"
)
target_include_directories(zpsync_lib
  PUBLIC "${ZPSYNC_DIR}/include"
  PRIVATE "${ZPSYNC_DIR}/src"
)
target_link_libraries(zpsync_lib PUBLIC zcrudp_lib ws2_32 iphlpapi)
target_compile_definitions(zpsync_lib PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(zpsync_lib PRIVATE /O2)
else()
  target_compile_options(zpsync_lib PRIVATE -O2)
endif()

add_executable(zcrudp_stream
  src/stream_live.c
  src/gpu_pipeline_win32.c
  src/zcrudp_video_bridge.c
)

target_include_directories(zcrudp_stream PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${TERN_VID_DIR}/include"
  "${ZPSYNC_DIR}/include"
)

target_compile_definitions(zcrudp_stream PRIVATE
  TERN_VID_SHADERS_DIR="${TERN_VID_DIR}/shaders"
)

target_link_libraries(zcrudp_stream PRIVATE
  zcrudp_lib
  zpsync_lib
  d3d11
  dxgi
  dxguid
  d3dcompiler
  mfplat
  mfuuid
  mfreadwrite
  ws2_32
  winmm
  ole32
  user32
  gdi32
)

if(MSVC)
  target_compile_options(zcrudp_stream PRIVATE /O2 /W3)
else()
  target_compile_options(zcrudp_stream PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -mavx2)
endif()
