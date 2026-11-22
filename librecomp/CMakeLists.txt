cmake_minimum_required(VERSION 3.20)

project(librecomp)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

# Define the library
add_library(librecomp STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cont.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config_option.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/eep.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/euc-jp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extensions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/files.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flash.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/heap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kuseg.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/math_routines.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mods.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_events.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_hooks.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mod_config_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/overlays.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pak.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/patcher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/print.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rsp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_stubs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultra_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/o1heap/o1heap/o1heap.c"
)

target_include_directories(librecomp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/../ultramodern/include"
    "${PROJECT_SOURCE_DIR}/../thirdparty"
    "${PROJECT_SOURCE_DIR}/../thirdparty/concurrentqueue"
    "${PROJECT_SOURCE_DIR}/../thirdparty/o1heap"
)
target_include_directories(librecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/librecomp"
)

target_compile_options(librecomp PRIVATE
#    -Wall
#    -Wextra
    -Wno-unused-parameter
)

if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/../thirdparty/miniz ${CMAKE_CURRENT_BINARY_DIR}/miniz)
add_subdirectory(${PROJECT_SOURCE_DIR}/../N64Recomp ${CMAKE_CURRENT_BINARY_DIR}/N64Recomp EXCLUDE_FROM_ALL)

# WebAssembly can't generate code at runtime, so Emscripten builds have no live recompiler (mods that need it don't load).
if (EMSCRIPTEN)
    target_link_libraries(librecomp PRIVATE ultramodern N64Recomp)
else()
    target_link_libraries(librecomp PRIVATE ultramodern N64Recomp LiveRecomp)
endif()
target_link_libraries(librecomp PUBLIC miniz)

# Load ROM (select_rom) takes the ROM inside a .zip, .7z, .gz, .xz or .tar (src/rom_archive.cpp), with the LZMA SDK's
# 7z and xz decoders (thirdparty/lzma-sdk).
option(RECOMP_ROM_ARCHIVES "Load ROM takes a ROM inside a .zip, .7z, .gz, .xz or .tar (always on with Emscripten)" OFF)
if (RECOMP_ROM_ARCHIVES OR EMSCRIPTEN)
    set(RECOMP_LZMA_SDK_DIR "${PROJECT_SOURCE_DIR}/../thirdparty/lzma-sdk")
    add_library(recomp_lzma_sdk STATIC
        "${RECOMP_LZMA_SDK_DIR}/7zArcIn.c"
        "${RECOMP_LZMA_SDK_DIR}/7zBuf.c"
        "${RECOMP_LZMA_SDK_DIR}/7zCrc.c"
        "${RECOMP_LZMA_SDK_DIR}/7zCrcOpt.c"
        "${RECOMP_LZMA_SDK_DIR}/7zDec.c"
        "${RECOMP_LZMA_SDK_DIR}/7zStream.c"
        "${RECOMP_LZMA_SDK_DIR}/Alloc.c"
        "${RECOMP_LZMA_SDK_DIR}/Bcj2.c"
        "${RECOMP_LZMA_SDK_DIR}/Bra.c"
        "${RECOMP_LZMA_SDK_DIR}/Bra86.c"
        "${RECOMP_LZMA_SDK_DIR}/BraIA64.c"
        "${RECOMP_LZMA_SDK_DIR}/CpuArch.c"
        "${RECOMP_LZMA_SDK_DIR}/Delta.c"
        "${RECOMP_LZMA_SDK_DIR}/Lzma2Dec.c"
        "${RECOMP_LZMA_SDK_DIR}/LzmaDec.c"
        "${RECOMP_LZMA_SDK_DIR}/Ppmd7.c"
        "${RECOMP_LZMA_SDK_DIR}/Ppmd7Dec.c"
        "${RECOMP_LZMA_SDK_DIR}/Sha256.c"
        "${RECOMP_LZMA_SDK_DIR}/Sha256Opt.c"
        "${RECOMP_LZMA_SDK_DIR}/Xz.c"
        "${RECOMP_LZMA_SDK_DIR}/XzCrc64.c"
        "${RECOMP_LZMA_SDK_DIR}/XzCrc64Opt.c"
        "${RECOMP_LZMA_SDK_DIR}/XzDec.c"
    )
    target_compile_definitions(recomp_lzma_sdk PUBLIC Z7_ST Z7_PPMD_SUPPORT)
    target_include_directories(recomp_lzma_sdk PUBLIC "${RECOMP_LZMA_SDK_DIR}")
    target_sources(librecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/rom_archive.cpp")
    target_compile_definitions(librecomp PRIVATE RECOMP_ROM_ARCHIVES=1)
    target_link_libraries(librecomp PRIVATE recomp_lzma_sdk)
endif()
