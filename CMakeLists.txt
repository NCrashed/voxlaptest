cmake_minimum_required(VERSION 3.21)

project(voxlaptest
    VERSION 0.1.0
    DESCRIPTION "Voxlap engine fork + Stage 0 regression oracle"
    LANGUAGES C
)

# Single-config or multi-config: put every runtime artefact in build/bin so
# voxlap.dll and oracle.exe land in the same directory. On Windows that
# means oracle.exe can find voxlap.dll at runtime without extra copying.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# v5.asm is MSVC MASM. CMake has built-in support for it via ASM_MASM. The
# assembler isn't available on GCC/Clang toolchains; Linux/macOS configure
# still succeeds but will fail to link voxlap (unresolved grouscanasm /
# drawboundcube* references) until Stage 3/4 replaces v5.asm.
if(MSVC)
    enable_language(ASM_MASM)
endif()

# All targets share the static multi-threaded CRT so there's only one copy
# of the runtime in the process, matching the vcxproj's
# RuntimeLibrary=MultiThreaded. CMake applies this only on MSVC; GCC/Clang
# ignore the variable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Shared compile flags for every C target in the project.
if(MSVC)
    # /J: char defaults to unsigned. Voxlap assumes this throughout (the
    #     vcxproj had /J in AdditionalOptions); flipping the default now
    #     would silently change comparisons in voxlap5.c and kplib.c.
    # /fp:precise: deterministic FP ordering across optimisation levels.
    add_compile_options(/J /fp:precise)

    # voxlap5.c and kplib.c use plenty of K&R-era stdio (fopen, strcpy,
    # sprintf) that modern SDKs flag with C4996. Silence them for now;
    # the proper fix — replacing with *_s variants or portable
    # equivalents — belongs in Stage 2/3.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# --------------------------------------------------------------------------
# voxasm — MASM-assembled static library, linked into voxlap.dll on Windows.
# --------------------------------------------------------------------------
if(MSVC)
    add_library(voxasm STATIC voxasm/v5.asm)
endif()

# --------------------------------------------------------------------------
# voxlap — the engine, built as a shared library. Public header is
# voxlap/voxlap5.h; consumers get it via target_include_directories PUBLIC.
# --------------------------------------------------------------------------
add_library(voxlap SHARED
    voxlap/voxlap5.c
    voxlap/exports.c
    voxlap/kplib.c
)

target_include_directories(voxlap PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/voxlap>
)

target_compile_definitions(voxlap PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
)

# Stage 4.5b dev toggle: build voxlap using the scalar C port of
# grouscanasm (voxlap/voxlap5.c) instead of the MMX asm entry point
# in voxasm/v5.asm. OFF by default — the scalar port is incomplete
# and rendering with it set ON will be visibly wrong until 4.5b
# lands its final chunks. Enable with -DVOXLAP_SCALAR_GROUSCAN=ON to
# diff scalar output against the asm goldens as the port progresses.
option(VOXLAP_SCALAR_GROUSCAN "Use the scalar C port of grouscanasm" OFF)
if(VOXLAP_SCALAR_GROUSCAN)
    target_compile_definitions(voxlap PRIVATE VOXLAP_SCALAR_GROUSCAN)
endif()

# Stage 4.5b.6g shakedown diagnostic: when ON, grouscanasm_scalar emits
# per-event stderr traces gated on the yellow-sphere's top-z slab
# signature (v[1]==178 && v[0]!=0) to help pin down the dark-column
# artifact above the voxel sphere. Implies VOXLAP_SCALAR_GROUSCAN. No
# effect in normal builds.
option(VOXLAP_SCALAR_GROUSCAN_DEBUG "Emit scalar grouscan diagnostic traces" OFF)
if(VOXLAP_SCALAR_GROUSCAN_DEBUG)
    target_compile_definitions(voxlap PRIVATE VOXLAP_SCALAR_GROUSCAN VOXLAP_SCALAR_GROUSCAN_DEBUG)
endif()

# Generate voxlap_export.h with a portable VOXLAP_API macro. Resolves to
# __declspec(dllexport) when building voxlap (CMake auto-defines
# voxlap_EXPORTS for SHARED targets), __declspec(dllimport) in consumers,
# and __attribute__((visibility("default"))) on GCC/Clang. Replaces the
# old unconditional `#define DLLEXPORT __declspec(dllexport)` that broke
# every non-MSVC compile.
include(GenerateExportHeader)
generate_export_header(voxlap
    BASE_NAME voxlap
    EXPORT_MACRO_NAME VOXLAP_API
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/voxlap_export.h
)
target_include_directories(voxlap PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

if(MSVC)
    target_link_libraries(voxlap PRIVATE voxasm)
    # /SAFESEH:NO — v5.asm isn't annotated for Safe Exception Handlers,
    # matching the vcxproj's ImageHasSafeExceptionHandlers=false.
    target_link_options(voxlap PRIVATE /SAFESEH:NO)
endif()

# --------------------------------------------------------------------------
# oracle — Stage 0 regression-hash harness. Builds the procedural scene,
# renders four fixed camera poses, writes tests/oracle-run/hashes.txt and
# per-pose PNGs. See tests/oracle/oracle.c for detail.
# --------------------------------------------------------------------------
add_executable(oracle tests/oracle/oracle.c)
target_link_libraries(oracle PRIVATE voxlap)

# Oracle writes hashes.txt and *.png to its CWD. Give it a dedicated dir
# so the CI upload step has a stable path to point at.
set(ORACLE_RUN_DIR ${CMAKE_BINARY_DIR}/oracle-run)
file(MAKE_DIRECTORY ${ORACLE_RUN_DIR})
