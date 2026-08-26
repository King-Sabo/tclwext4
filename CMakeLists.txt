cmake_minimum_required(VERSION 3.21)
project(tclwext4 C)
enable_language(RC)

if(NOT MSVC)
    message(FATAL_ERROR "tclwext4 targets MSVC (Visual Studio 2022) only")
endif()

set(LWEXT4_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/lwext4)
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
    src/tclwext4.rc
    src/tclwext4.def
)

target_link_libraries(tclwext4 PRIVATE lwext4)
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
