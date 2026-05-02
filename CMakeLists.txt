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
else()
    # Match MSVC's /J: default char to unsigned. The voxlap rle walker
    # at scum2line:4320 (and friends) does `while (v[0]) v += v[0]*4;`
    # where v[0] is the next-slab byte-offset divided by 4. For carved
    # columns tall enough to push that byte past 127, a signed-char
    # read sign-extends to negative and the loop walks backwards
    # through vbuf forever — that's the Linux startup hang we hit
    # post-4.8.4. The original Voxlap project always built with /J
    # (vcxproj AdditionalOptions); GCC/Clang need the flag explicitly.
    add_compile_options(-funsigned-char)
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
    $<$<BOOL:${WIN32}>:WIN32>
    $<$<BOOL:${WIN32}>:_WINDOWS>
    $<$<BOOL:${WIN32}>:_USRDLL>
    # kplib.c has MSVC inline-asm fast paths; NOASM forces the portable C
    # fallback on non-MSVC compilers. Drop once kplib's GCC asm path is
    # ported to x86_64 (today only an i386 path exists).
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:NOASM>
)

# libm: needed on every Unix-like system (sqrt/sin/exp/...). MSVC bundles
# math intrinsics into the C runtime, so this link only matters on
# GCC/Clang. PUBLIC so consumers (oracle) inherit it transitively.
if(NOT WIN32)
    target_link_libraries(voxlap PUBLIC m)
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

# Mirror assets/ into the oracle run dir so the harness can resolve
# 'assets/coco.kvx' and any future external test fixtures relative to
# its own working directory. POST_BUILD on the oracle target keeps the
# copy in sync after every build, including incremental ones.
add_custom_command(TARGET oracle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            ${ORACLE_RUN_DIR}/assets
    COMMENT "Copying assets/ into oracle-run/assets/"
)

# --------------------------------------------------------------------------
# edit_fixture — CD.2.6 byte-fixture harness. Runs ONE setspans carve on
# a fresh loadnul world and saves pre/post .vxl files for the roxlap
# port to validate scum2 + delslab + compilerle byte-equality against
# voxlap C. Invoked as `edit_fixture <pre.vxl> <post.vxl>`. See
# tests/edit_fixture/edit_fixture.c.
# --------------------------------------------------------------------------
add_executable(edit_fixture tests/edit_fixture/edit_fixture.c)
target_link_libraries(edit_fixture PRIVATE voxlap)
