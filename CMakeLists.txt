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
target_compile_definitions(zcrudp_lib PUBLIC
  RUDP_PAGE_SIZE=4096U
  RUDP_PAGE_POOL_PAGES=1024U
)

add_executable(zcrudp_stream_bench
  src/zcrudp_video_bridge.c
  src/gpu_pipeline_win32.c
  src/main.c
)

target_include_directories(zcrudp_stream_bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(zcrudp_stream_bench PRIVATE
  zcrudp_lib
  d3d11
  dxgi
  dxguid
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
