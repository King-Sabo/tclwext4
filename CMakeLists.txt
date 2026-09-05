cmake_minimum_required(VERSION 3.21)
project(tclwext4 C)
enable_language(RC)

if(NOT MSVC)
    message(FATAL_ERROR "tclwext4 targets MSVC (Visual Studio 2022) only")
endif()

set(LWEXT4_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/lwext4)
set(FATFS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/external/fatfs)
set(SQFS_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/external/squashfuse)
set(LZ4_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/external/lz4)
set(ZSTD_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/external/zstd)
set(XZ_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/external/xz-embedded)
if(NOT EXISTS ${LWEXT4_DIR}/src/ext4.c)
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "lwext4 missing and git not found on PATH")
    endif()
    # Stage 1: submodule checkout. Exits 0 and does nothing if .gitmodules
    # lists a path with no gitlink in the index, so it is not conclusive.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
        message(STATUS "lwext4 not present - trying submodule checkout...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    # Stage 2: direct clone.
    if(NOT EXISTS ${LWEXT4_DIR}/src/ext4.c)
        message(STATUS "cloning lwext4...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1
                    https://github.com/gkostka/lwext4.git ${LWEXT4_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

if(NOT EXISTS ${FATFS_DIR}/source/ff.c)
    find_package(Git QUIET)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    if(NOT EXISTS ${FATFS_DIR}/source/ff.c)
        message(STATUS "cloning FatFs...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --depth 1
                    https://github.com/abbrev/fatfs.git ${FATFS_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
if(NOT EXISTS ${FATFS_DIR}/source/ff.c)
    message(FATAL_ERROR "Could not obtain FatFs at ${FATFS_DIR}")
endif()

# Remaining submodules share one bootstrap: submodule update, then a direct
# clone for the no-gitlink and source-download cases.
function(tcl_need_submodule dir probe url name)
    if(EXISTS "${probe}")
        return()
    endif()
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "${name} missing and git not found on PATH")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    if(NOT EXISTS "${probe}")
        message(STATUS "cloning ${name}...")
        execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth 1 "${url}" "${dir}"
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    if(NOT EXISTS "${probe}")
        message(FATAL_ERROR "Could not obtain ${name} at ${dir}")
    endif()
endfunction()

tcl_need_submodule("${SQFS_DIR}" "${SQFS_DIR}/fs.c"
                   "https://github.com/vasi/squashfuse.git" "squashfuse")
tcl_need_submodule("${LZ4_DIR}"  "${LZ4_DIR}/lib/lz4.c"
                   "https://github.com/lz4/lz4.git" "lz4")
tcl_need_submodule("${ZSTD_DIR}" "${ZSTD_DIR}/lib/decompress/zstd_decompress.c"
                   "https://github.com/facebook/zstd.git" "zstd")
tcl_need_submodule("${XZ_DIR}"   "${XZ_DIR}/linux/lib/xz/xz_dec_stream.c"
                   "https://github.com/tukaani-project/xz-embedded.git" "xz-embedded")
if(NOT EXISTS ${LWEXT4_DIR}/src/ext4.c)
    message(FATAL_ERROR "Could not obtain lwext4 at ${LWEXT4_DIR}")
endif()

# ---------------------------------------------------------------------------
# lwext4 -- built from the UNMODIFIED submodule.
#
# We deliberately do NOT add_subdirectory(external/lwext4): its top-level
# CMakeLists uses global add_definitions(), pulls in fs_test, and relies on a
# generated/ext4_config.h. Instead we compile src/*.c into our own target and
# supply that generated header ourselves from config/. Nothing in the submodule
# is touched.
# ---------------------------------------------------------------------------
file(GLOB LWEXT4_SRC ${LWEXT4_DIR}/src/*.c)
add_library(lwext4 STATIC ${LWEXT4_SRC})

target_include_directories(lwext4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${LWEXT4_DIR}/include)

# Configuration comes from config/generated/ext4_config.h, which lwext4's own
# ext4_config.h includes when CONFIG_USE_DEFAULT_CFG is 0 (the default). Keeping
# it in a header rather than in defines means the plugin sources - which reach
# ext4_config.h through ext4.h - are guaranteed to see identical struct layouts.
target_compile_definitions(lwext4 PUBLIC _CRT_SECURE_NO_WARNINGS)
# C4005: ext4_misc.h defines its own offsetof, clashing with ucrt stddef.h.
target_compile_options(lwext4 PRIVATE /wd4244 /wd4267 /wd4996 /wd4005)
# lwext4 needs C11; the .vcxproj sets LanguageStandard_C=stdc11 and CMake must
# match or MSVC uses its older default C mode.
set_property(TARGET lwext4 PROPERTY C_STANDARD 11)
set_property(TARGET lwext4 PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET lwext4 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# FatFs
#
# Unlike lwext4, FatFs has no #ifndef-guarded knobs: ff.h includes "ffconf.h"
# with a quoted include, which resolves against ff.h's own directory before any
# -I path. The only ways to configure it are to edit the submodule or to shadow
# the file. We copy the sources into the build tree and drop our ffconf.h next
# to them, so external/fatfs stays pristine like external/lwext4.
# ---------------------------------------------------------------------------
set(FATFS_BUILD ${CMAKE_CURRENT_BINARY_DIR}/fatfs)
file(MAKE_DIRECTORY ${FATFS_BUILD})
configure_file(${FATFS_DIR}/source/ff.c         ${FATFS_BUILD}/ff.c         COPYONLY)
configure_file(${FATFS_DIR}/source/ff.h         ${FATFS_BUILD}/ff.h         COPYONLY)
configure_file(${FATFS_DIR}/source/ffunicode.c  ${FATFS_BUILD}/ffunicode.c  COPYONLY)
configure_file(${FATFS_DIR}/source/ffsystem.c   ${FATFS_BUILD}/ffsystem.c   COPYONLY)
configure_file(${FATFS_DIR}/source/diskio.h     ${FATFS_BUILD}/diskio.h     COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/ffconf.h
               ${FATFS_BUILD}/ffconf.h COPYONLY)

add_library(fatfs STATIC
    ${FATFS_BUILD}/ff.c
    ${FATFS_BUILD}/ffunicode.c
    ${FATFS_BUILD}/ffsystem.c
)
target_include_directories(fatfs PUBLIC ${FATFS_BUILD})
# UNICODE matters here: ff.h includes windows.h on _WIN32, and without it
# windows.h typedefs TCHAR as char while FatFs uses WCHAR.
target_compile_definitions(fatfs PUBLIC UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(fatfs PRIVATE /wd4244 /wd4267 /wd4996 /wd4005)
set_property(TARGET fatfs PROPERTY C_STANDARD 11)
set_property(TARGET fatfs PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# lz4
#
# squashfuse calls only LZ4_decompress_safe, so this is one translation unit -
# no lz4frame, lz4hc or xxhash.
#
# There is no zlib target: squashfuse's Windows path includes
# win/win_decompress.c.inc unconditionally, which implements the gzip
# decompressor over tinfl (win/tinfl.c). Defining HAVE_ZLIB_H as well gives a
# duplicate definition of sqfs_decompressor_zlib.
# ---------------------------------------------------------------------------
add_library(lz4 STATIC ${LZ4_DIR}/lib/lz4.c)
target_include_directories(lz4 PUBLIC ${LZ4_DIR}/lib)
target_compile_definitions(lz4 PUBLIC _CRT_SECURE_NO_WARNINGS)
target_compile_options(lz4 PRIVATE /wd4244 /wd4267 /wd4996)
set_property(TARGET lz4 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# zstd, decoder only
#
# squashfuse calls ZSTD_decompress and nothing else, so the compressor,
# dictionary builder and CLI are all left out. huf_decompress_amd64.S is
# deliberately absent: that path needs GNU as, and MSVC takes the C fallback.
# ---------------------------------------------------------------------------
add_library(zstddec STATIC
    ${ZSTD_DIR}/lib/common/debug.c
    ${ZSTD_DIR}/lib/common/entropy_common.c
    ${ZSTD_DIR}/lib/common/error_private.c
    ${ZSTD_DIR}/lib/common/fse_decompress.c
    ${ZSTD_DIR}/lib/common/pool.c
    ${ZSTD_DIR}/lib/common/threading.c
    ${ZSTD_DIR}/lib/common/xxhash.c
    ${ZSTD_DIR}/lib/common/zstd_common.c
    ${ZSTD_DIR}/lib/decompress/huf_decompress.c
    ${ZSTD_DIR}/lib/decompress/zstd_ddict.c
    ${ZSTD_DIR}/lib/decompress/zstd_decompress.c
    ${ZSTD_DIR}/lib/decompress/zstd_decompress_block.c
)
target_include_directories(zstddec PUBLIC ${ZSTD_DIR}/lib ${ZSTD_DIR}/lib/common)
target_compile_definitions(zstddec PUBLIC
    ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(zstddec PRIVATE /wd4244 /wd4267 /wd4996 /wd4146)
set_property(TARGET zstddec PROPERTY C_STANDARD 11)
set_property(TARGET zstddec PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# XZ Embedded: a minimal .xz decoder, configured through config/xzemb.
# xz_dec_syms.c is kernel symbol exports and is deliberately absent.
# ---------------------------------------------------------------------------
add_library(xzdec STATIC
    ${XZ_DIR}/linux/lib/xz/xz_dec_stream.c
    ${XZ_DIR}/linux/lib/xz/xz_dec_lzma2.c
    ${XZ_DIR}/linux/lib/xz/xz_dec_bcj.c
    ${XZ_DIR}/linux/lib/xz/xz_crc32.c
    ${XZ_DIR}/linux/lib/xz/xz_crc64.c
)
target_include_directories(xzdec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config/xzemb
    ${XZ_DIR}/linux/include/linux
    ${XZ_DIR}/linux/lib/xz)
target_compile_definitions(xzdec PUBLIC _CRT_SECURE_NO_WARNINGS)
target_compile_options(xzdec PRIVATE /wd4244 /wd4267 /wd4996)
set_property(TARGET xzdec PROPERTY C_STANDARD 11)
set_property(TARGET xzdec PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# squashfuse
#
# Only the non-FUSE core. squashfuse already carries Windows support
# (win/win32.h, sqfs_fd_t = HANDLE), so no shim is needed - but win/ must come
# first on the include path for <win32.h> to resolve.
# ---------------------------------------------------------------------------
add_library(squashfuse STATIC
    ${SQFS_DIR}/cache.c   ${SQFS_DIR}/decompress.c ${SQFS_DIR}/dir.c
    ${SQFS_DIR}/file.c    ${SQFS_DIR}/fs.c         ${SQFS_DIR}/hash.c
    ${SQFS_DIR}/stack.c   ${SQFS_DIR}/swap.c       ${SQFS_DIR}/table.c
    ${SQFS_DIR}/util.c    ${SQFS_DIR}/xattr.c      ${SQFS_DIR}/nonstd-pread.c ${SQFS_DIR}/nonstd-stat.c
    ${SQFS_DIR}/win/tinfl.c
)
# swap.h.inc / swap.c.inc are generated upstream by a sed script; pre-generated
# copies live in config/squashfuse. swap.h includes them with quotes, which
# falls back to the include path after the submodule directory misses.
# config/squashfuse comes FIRST: it shadows win_decompress.c.inc to add the xz
# decompressor. See the comment at the top of that file.
target_include_directories(squashfuse PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config/squashfuse ${SQFS_DIR}/win ${SQFS_DIR})

target_compile_definitions(squashfuse PUBLIC
    HAVE_LZ4_H HAVE_ZSTD_H TCLWEXT4_WITH_XZ _CRT_SECURE_NO_WARNINGS)
target_compile_options(squashfuse PRIVATE /wd4244 /wd4267 /wd4996 /wd4005)
target_link_libraries(squashfuse PUBLIC lz4 zstddec xzdec)
set_property(TARGET squashfuse PROPERTY C_STANDARD 11)
set_property(TARGET squashfuse PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
add_library(tclwext4 SHARED
    src/tclwext4.c
    src/tcl_blockdev.c
    src/tcl_scan.c
    src/tcl_vol.c
    src/tcl_util.c
    src/tcl_content.c
    src/tcl_dialog.c
    src/tcl_fs.c
    src/tcl_fs_fat.c
    src/tcl_fs_sqfs.c
    src/tclwext4.rc
    src/tclwext4.def
)

target_link_libraries(tclwext4 PRIVATE lwext4 fatfs squashfuse)
target_compile_definitions(tclwext4 PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tclwext4 PRIVATE /wd4244 /wd4267 /wd4996 /wd4005)
target_include_directories(tclwext4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The .rc picks OriginalFilename from this; rc.exe cannot be relied on to have
# _WIN64 defined, so state the bitness explicitly.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(tclwext4 PRIVATE TCLWEXT4_WFX64)
endif()
set_property(TARGET tclwext4 PROPERTY C_STANDARD 11)
set_property(TARGET tclwext4 PROPERTY C_STANDARD_REQUIRED ON)
set_property(TARGET tclwext4 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# TC expects .wfx for 32-bit and .wfx64 for 64-bit, same base name.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(tclwext4 PROPERTIES SUFFIX ".wfx64")
else()
    set_target_properties(tclwext4 PROPERTIES SUFFIX ".wfx")
endif()

target_link_libraries(tclwext4 PRIVATE comdlg32 shell32 advapi32)
