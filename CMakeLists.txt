cmake_minimum_required(VERSION 3.20)
project(zcstream LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZCRUDP_PORTABLE_CODEC "Use portable C11 codec backend instead of Win32/D3D11" OFF)
option(ZCSTREAM_AUTO_FETCH "Automatically download missing sibling repos (zcrudp, zcvid, zpsync) and nvEncodeAPI.h" ON)
option(ZCSTREAM_BUILD_ZPSYNC_PEER "Build zpsync_peer CLI alongside zcstream binaries" ON)

set(ZCRUDP_GIT_REPO "https://github.com/i-Charlys/zcrudp.git" CACHE STRING "Git URL for zcrudp auto-fetch")
set(ZCRUDP_GIT_TAG "main" CACHE STRING "Git branch/tag for zcrudp auto-fetch")
set(ZCVID_GIT_REPO "https://github.com/i-Charlys/zcvid.git" CACHE STRING "Git URL for zcvid auto-fetch")
set(ZCVID_GIT_TAG "main" CACHE STRING "Git branch/tag for zcvid auto-fetch")
set(ZPSYNC_GIT_REPO "https://github.com/i-Charlys/zpsync.git" CACHE STRING "Git URL for zpsync auto-fetch")
set(ZPSYNC_GIT_TAG "main" CACHE STRING "Git branch/tag for zpsync auto-fetch")
set(ZC_NVENC_HEADER_URL
  "https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/master/include/ffnvcodec/nvEncodeAPI.h"
  CACHE STRING "URL used to auto-download nvEncodeAPI.h when not found locally"
)

set(ZCRUDP_DIR "" CACHE PATH "Path to zcrudp repository (optional; auto-detected or auto-fetched)")
set(ZCVID_DIR "" CACHE PATH "Path to zcvid repository (optional; auto-detected or auto-fetched)")
set(ZPSYNC_DIR "" CACHE PATH "Path to zpsync repository (optional; auto-detected or auto-fetched)")

# Architecture-aware SIMD flags (never force -mavx2 on ARM64 / Apple Silicon / RISC-V)
set(ZC_SIMD_FLAGS "")
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(ZC_SIMD_FLAGS "-mavx2")
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

function(zc_resolve_repo OUT_VAR REPO_NAME CHECK_REL GIT_URL GIT_TAG)
  set(_candidates "")
  if(NOT "${${OUT_VAR}}" STREQUAL "")
    list(APPEND _candidates "${${OUT_VAR}}")
  endif()
  if(DEFINED ENV{${OUT_VAR}} AND NOT "$ENV{${OUT_VAR}}" STREQUAL "")
    list(APPEND _candidates "$ENV{${OUT_VAR}}")
  endif()
  list(APPEND _candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../${REPO_NAME}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../${REPO_NAME}-main"
    "${CMAKE_CURRENT_SOURCE_DIR}/../${REPO_NAME}-src"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/${REPO_NAME}"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/${REPO_NAME}-src"
  )

  set(_found "")
  foreach(_cand IN LISTS _candidates)
    if(EXISTS "${_cand}/${CHECK_REL}")
      set(_found "${_cand}")
      break()
    endif()
  endforeach()

  if("${_found}" STREQUAL "" AND ZCSTREAM_AUTO_FETCH)
    set(_dest "${CMAKE_CURRENT_BINARY_DIR}/_deps/${REPO_NAME}-src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps")
    find_package(Git QUIET)
    set(_ok FALSE)
    if(GIT_FOUND)
      message(STATUS "zcstream: ${REPO_NAME} not found locally, cloning ${GIT_URL} (${GIT_TAG})...")
      file(REMOVE_RECURSE "${_dest}")
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --branch "${GIT_TAG}" "${GIT_URL}" "${_dest}"
        RESULT_VARIABLE _git_rc
      )
      if(_git_rc EQUAL 0 AND EXISTS "${_dest}/${CHECK_REL}")
        set(_ok TRUE)
      else()
        file(REMOVE_RECURSE "${_dest}")
      endif()
    endif()
    if(NOT _ok)
      string(REGEX REPLACE "\\.git$" "" _repo_base "${GIT_URL}")
      set(_archive_url "${_repo_base}/archive/refs/heads/${GIT_TAG}.zip")
      set(_archive_file "${CMAKE_CURRENT_BINARY_DIR}/_deps/${REPO_NAME}.zip")
      set(_extract_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/_extract_${REPO_NAME}")
      message(STATUS "zcstream: downloading ${_archive_url}...")
      file(DOWNLOAD "${_archive_url}" "${_archive_file}" STATUS _dl_status TIMEOUT 60)
      list(GET _dl_status 0 _dl_code)
      if(_dl_code EQUAL 0)
        file(REMOVE_RECURSE "${_extract_dir}" "${_dest}")
        file(MAKE_DIRECTORY "${_extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_archive_file}" DESTINATION "${_extract_dir}")
        file(GLOB _extracted_dirs LIST_DIRECTORIES true "${_extract_dir}/*")
        foreach(_d IN LISTS _extracted_dirs)
          if(EXISTS "${_d}/${CHECK_REL}")
            file(RENAME "${_d}" "${_dest}")
            set(_ok TRUE)
            break()
          endif()
        endforeach()
        file(REMOVE_RECURSE "${_extract_dir}" "${_archive_file}")
      endif()
    endif()
    if(_ok)
      set(_found "${_dest}")
    endif()
  endif()

  if("${_found}" STREQUAL "")
    message(FATAL_ERROR
      "${REPO_NAME} not found (missing ${CHECK_REL}). "
      "Place it at ../${REPO_NAME}, pass -D${OUT_VAR}=<path>, or enable -DZCSTREAM_AUTO_FETCH=ON with internet access."
    )
  endif()
  set(${OUT_VAR} "${_found}" PARENT_SCOPE)
  message(STATUS "zcstream: using ${REPO_NAME} at ${_found}")
endfunction()

zc_resolve_repo(ZCRUDP_DIR zcrudp "include/rudp_nextgen.h" "${ZCRUDP_GIT_REPO}" "${ZCRUDP_GIT_TAG}")
zc_resolve_repo(ZCVID_DIR zcvid "include/zcvid_zcrudp_bridge.h" "${ZCVID_GIT_REPO}" "${ZCVID_GIT_TAG}")
zc_resolve_repo(ZPSYNC_DIR zpsync "include/zpsync.h" "${ZPSYNC_GIT_REPO}" "${ZPSYNC_GIT_TAG}")

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
  "${ZCVID_DIR}/include"
)
if(WIN32)
  target_link_libraries(test_video_bridge PRIVATE zcrudp_lib ws2_32)
else()
  target_link_libraries(test_video_bridge PRIVATE zcrudp_lib m Threads::Threads)
endif()
if(MSVC)
  target_compile_options(test_video_bridge PRIVATE /O2 /UNDEBUG)
else()
  target_compile_options(test_video_bridge PRIVATE -O2 -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME test_video_bridge COMMAND test_video_bridge)

# ZPSync static library (portable: Windows, Linux, macOS)
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
target_link_libraries(zpsync_lib PUBLIC zcrudp_lib)
if(WIN32)
  target_link_libraries(zpsync_lib PUBLIC ws2_32 iphlpapi)
  target_compile_definitions(zpsync_lib PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
else()
  target_link_libraries(zpsync_lib PUBLIC Threads::Threads)
  target_compile_definitions(zpsync_lib PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(MSVC)
  target_compile_options(zpsync_lib PRIVATE /O2)
else()
  target_compile_options(zpsync_lib PRIVATE -O2)
endif()

set(ZCSTREAM_INSTALL_TARGETS "")

if(ZCSTREAM_BUILD_ZPSYNC_PEER AND EXISTS "${ZPSYNC_DIR}/examples/zpsync_peer.c")
  add_executable(zpsync_peer "${ZPSYNC_DIR}/examples/zpsync_peer.c")
  target_link_libraries(zpsync_peer PRIVATE zpsync_lib)
  if(MSVC)
    target_compile_options(zpsync_peer PRIVATE /O2 /W3)
  else()
    target_compile_options(zpsync_peer PRIVATE -O2 -Wall -Wextra)
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zpsync_peer)
endif()

# Pyrowave (Vulkan Wavelet GPU Codec) + zcrudp::nextgen Benchmark (cross-platform Vulkan)
set(PYROWAVE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pyrowave-master")
set(PYROWAVE_BUILD_DIR "${PYROWAVE_SRC_DIR}/build")
if(EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave.a" AND NOT ZCRUDP_PORTABLE_CODEC AND NOT CMAKE_CROSSCOMPILING)
  add_executable(zcrudp_pyrowave_bench
    src/pyrowave_zcrudp_bench.cpp
    src/zcrudp_video_bridge.c
  )
  target_include_directories(zcrudp_pyrowave_bench PRIVATE
    "${ZCVID_DIR}/include"
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
  )
  if(WIN32)
    target_link_libraries(zcrudp_pyrowave_bench PRIVATE ws2_32 winmm)
  else()
    target_link_libraries(zcrudp_pyrowave_bench PRIVATE m Threads::Threads)
  endif()
  target_compile_options(zcrudp_pyrowave_bench PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_pyrowave_bench)
endif()

# Portable Zcvid pipeline unit test (no GPU needed): ctest --test-dir build
add_executable(test_portable_pipeline
  tests/test_portable_pipeline.c
  src/gpu_pipeline_portable.c
  src/zc_periph.c
)
target_include_directories(test_portable_pipeline PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${ZCVID_DIR}/include"
)
if(WIN32)
  target_link_libraries(test_portable_pipeline PRIVATE zcrudp_lib ole32 user32)
else()
  target_link_libraries(test_portable_pipeline PRIVATE zcrudp_lib m Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  target_compile_options(test_portable_pipeline PRIVATE /O2 /W3 /UNDEBUG /experimental:c11atomics)
else()
  target_compile_options(test_portable_pipeline PRIVATE -O2 -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME test_portable_pipeline COMMAND test_portable_pipeline)

# Peripheral I/O unit test (wire packets, scancodes, ADPCM, SPSC ring, enumeration)
add_executable(test_periph
  tests/test_periph.c
  src/zc_periph.c
)
target_include_directories(test_periph PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WIN32)
  target_link_libraries(test_periph PRIVATE ole32 user32)
else()
  target_link_libraries(test_periph PRIVATE m Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  target_compile_options(test_periph PRIVATE /O2 /W3 /UNDEBUG /experimental:c11atomics)
else()
  target_compile_options(test_periph PRIVATE -O2 -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME test_periph COMMAND test_periph)

# Windows D3D11 / DXGI / MediaFoundation GPU pipeline targets
if(WIN32 AND NOT ZCRUDP_PORTABLE_CODEC)
  # Locate or auto-download MIT-licensed nvEncodeAPI.h (optional; falls back cleanly to MF H.264 + Zcvid)
  set(ZC_NVENC_INCLUDE_DIR "")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvEncodeAPI.h")
    set(ZC_NVENC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/nvenc/nvEncodeAPI.h")
    set(ZC_NVENC_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/nvenc")
  elseif(ZCSTREAM_AUTO_FETCH)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps/nvenc")
    set(_nvenc_dl "${CMAKE_CURRENT_BINARY_DIR}/_deps/nvenc/nvEncodeAPI.h")
    message(STATUS "zcstream: nvEncodeAPI.h not found locally, downloading from FFmpeg/nv-codec-headers...")
    file(DOWNLOAD "${ZC_NVENC_HEADER_URL}" "${_nvenc_dl}" STATUS _nv_status TIMEOUT 30)
    list(GET _nv_status 0 _nv_code)
    if(_nv_code EQUAL 0 AND EXISTS "${_nvenc_dl}")
      file(SIZE "${_nvenc_dl}" _nv_sz)
      if(_nv_sz GREATER 10000)
        set(ZC_NVENC_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/nvenc")
      else()
        file(REMOVE "${_nvenc_dl}")
      endif()
    else()
      file(REMOVE "${_nvenc_dl}")
    endif()
  endif()

  if(NOT "${ZC_NVENC_INCLUDE_DIR}" STREQUAL "")
    message(STATUS "zcstream: Direct NVENC enabled (${ZC_NVENC_INCLUDE_DIR}/nvEncodeAPI.h)")
  else()
    message(STATUS "zcstream: nvEncodeAPI.h unavailable — Direct NVENC disabled (MediaFoundation H.264 & Zcvid HLSL remain enabled)")
  endif()

  function(zc_attach_win32_gpu_assets target)
    if(NOT "${ZC_NVENC_INCLUDE_DIR}" STREQUAL "")
      target_include_directories(${target} PRIVATE "${ZC_NVENC_INCLUDE_DIR}")
      target_compile_definitions(${target} PRIVATE ZC_HAVE_NVENC=1)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/shaders"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ZCVID_DIR}/shaders/zcvid_encode.hlsl"
        "${ZCVID_DIR}/shaders/zcvid_decode.hlsl"
        "$<TARGET_FILE_DIR:${target}>/shaders"
    )
  endfunction()

  add_executable(zcrudp_stream_bench
    src/zcrudp_video_bridge.c
    src/gpu_pipeline_win32.c
    src/main.c
  )

  target_include_directories(zcrudp_stream_bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${ZCVID_DIR}/include"
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
  zc_attach_win32_gpu_assets(zcrudp_stream_bench)

  if(MSVC)
    target_compile_options(zcrudp_stream_bench PRIVATE /O2 /W3)
  else()
    target_compile_options(zcrudp_stream_bench PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_stream_bench)

  # Zcvid HLSL Compute + zcrudp::nextgen + Winsock RIO Benchmark
  add_executable(zcrudp_zcvid_bench
    src/zcvid_zcrudp_bench.c
    src/gpu_pipeline_win32.c
    src/zcrudp_video_bridge.c
  )

  target_include_directories(zcrudp_zcvid_bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${ZCVID_DIR}/include"
  )

  target_link_libraries(zcrudp_zcvid_bench PRIVATE
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
  zc_attach_win32_gpu_assets(zcrudp_zcvid_bench)

  if(MSVC)
    target_compile_options(zcrudp_zcvid_bench PRIVATE /O2 /W3)
  else()
    target_compile_options(zcrudp_zcvid_bench PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter ${ZC_SIMD_FLAGS})
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_zcvid_bench)

  # Zcvid codec motion bench (GPU, no network; not part of ctest: needs a
  # D3D11 GPU). See the header of tests/zcvid_motion_bench.c.
  add_executable(zcvid_motion_bench tests/zcvid_motion_bench.c)
  target_include_directories(zcvid_motion_bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${ZCVID_DIR}/include"
  )
  target_link_libraries(zcvid_motion_bench PRIVATE
    zcrudp_lib d3d11 dxgi dxguid d3dcompiler mfplat mfuuid mfreadwrite
    ws2_32 winmm ole32 oleaut32 user32 gdi32
  )
  zc_attach_win32_gpu_assets(zcvid_motion_bench)
  if(NOT MSVC)
    target_compile_options(zcvid_motion_bench PRIVATE -O3 ${ZC_SIMD_FLAGS})
  endif()
  if(EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a" AND EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave-shared-1.dll")
    target_sources(zcvid_motion_bench PRIVATE src/pyrowave_gpu_bridge.cpp)
    target_compile_definitions(zcvid_motion_bench PRIVATE ZC_HAVE_PYROWAVE=1)
    target_include_directories(zcvid_motion_bench PRIVATE
      "${PYROWAVE_SRC_DIR}"
      "${PYROWAVE_SRC_DIR}/Granite/third_party/khronos/vulkan-headers/include"
    )
    target_link_libraries(zcvid_motion_bench PRIVATE "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a")
  endif()

  # Live 2-PC / Continuous Screen Streaming (Sender / Receiver)
  add_executable(zcrudp_stream
    src/stream_live.c
    src/gpu_pipeline_win32.c
    src/zcrudp_video_bridge.c
    src/zc_periph.c
  )

  target_include_directories(zcrudp_stream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${ZCVID_DIR}/include"
    "${ZPSYNC_DIR}/include"
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
  zc_attach_win32_gpu_assets(zcrudp_stream)

  if(EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a" AND EXISTS "${PYROWAVE_BUILD_DIR}/libpyrowave-shared-1.dll")
    target_sources(zcrudp_stream PRIVATE src/pyrowave_gpu_bridge.cpp)
    target_compile_definitions(zcrudp_stream PRIVATE ZC_HAVE_PYROWAVE=1)
    target_include_directories(zcrudp_stream PRIVATE
      "${PYROWAVE_SRC_DIR}"
      "${PYROWAVE_SRC_DIR}/Granite/third_party/khronos/vulkan-headers/include"
    )
    target_link_libraries(zcrudp_stream PRIVATE "${PYROWAVE_BUILD_DIR}/libpyrowave-shared.dll.a")
    add_custom_command(TARGET zcrudp_stream POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PYROWAVE_BUILD_DIR}/libpyrowave-shared-1.dll"
        "$<TARGET_FILE_DIR:zcrudp_stream>/libpyrowave-shared-1.dll"
    )
    install(FILES "${PYROWAVE_BUILD_DIR}/libpyrowave-shared-1.dll" DESTINATION .)
  endif()

  if(MSVC)
    target_compile_options(zcrudp_stream PRIVATE /O2 /W3)
  else()
    target_compile_options(zcrudp_stream PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter ${ZC_SIMD_FLAGS})
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_stream)

  install(FILES
    "${ZCVID_DIR}/shaders/zcvid_encode.hlsl"
    "${ZCVID_DIR}/shaders/zcvid_decode.hlsl"
    DESTINATION shaders
  )
else()
  message(STATUS "zcstream: building portable C11 codec backend (zcrudp_zcvid_bench, zcrudp_stream, test_portable_pipeline)")

  add_executable(zcrudp_zcvid_bench
    src/zcvid_zcrudp_bench.c
    src/gpu_pipeline_portable.c
    src/zcrudp_video_bridge.c
    src/zc_periph.c
  )
  target_include_directories(zcrudp_zcvid_bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ZCVID_DIR}/include"
  )
  if(WIN32)
    target_link_libraries(zcrudp_zcvid_bench PRIVATE zcrudp_lib ws2_32 winmm ole32 user32)
  else()
    target_link_libraries(zcrudp_zcvid_bench PRIVATE zcrudp_lib m Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  if(MSVC)
    target_compile_options(zcrudp_zcvid_bench PRIVATE /O2 /W3 /experimental:c11atomics)
  else()
    target_compile_options(zcrudp_zcvid_bench PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter ${ZC_SIMD_FLAGS})
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_zcvid_bench)

  add_executable(zcrudp_stream
    src/stream_live.c
    src/gpu_pipeline_portable.c
    src/zcrudp_video_bridge.c
    src/zc_periph.c
  )
  target_include_directories(zcrudp_stream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ZCVID_DIR}/include"
    "${ZPSYNC_DIR}/include"
  )
  if(WIN32)
    target_link_libraries(zcrudp_stream PRIVATE zcrudp_lib zpsync_lib ws2_32 winmm ole32 user32)
  else()
    target_link_libraries(zcrudp_stream PRIVATE zcrudp_lib zpsync_lib m Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  if(MSVC)
    target_compile_options(zcrudp_stream PRIVATE /O2 /W3 /experimental:c11atomics)
  else()
    target_compile_options(zcrudp_stream PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter ${ZC_SIMD_FLAGS})
  endif()
  list(APPEND ZCSTREAM_INSTALL_TARGETS zcrudp_stream)
endif()

install(TARGETS ${ZCSTREAM_INSTALL_TARGETS} RUNTIME DESTINATION .)
