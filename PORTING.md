# Voxlap Cross-Platform Port Plan

Porting this Voxlap fork to build on Linux, macOS, and modern Windows compilers (MSVC 2019+, Clang, GCC).

## Locked decisions

| # | Decision | Consequence |
|---|---|---|
| 1 | **64-bit only** (x86-64 + ARM64 on macOS) | No Win32/x86 target. Lets us assume SSE2 on x86-64. Halves the test matrix. |
| 2 | **Drop MMX and 3DNow!** | No runtime SIMD dispatch. SSE2 on x86-64, NEON (or scalar) on ARM64. Remove `getcputype()` gating. |
| 3 | **SDL2** for windowing/input | Not SDL1 (Tao), not SDL3. Mature, on all three OSes. |
| 4 | **Drop the C# Voxlaptest frontend.** Replace with **Rust bindings + Rust host** | No more Tao Framework, no more .NET 2.0. Rust `voxlap-sys` + safe `voxlap` crate become the primary API for applications; a small Rust example binary replaces `Voxlaptest/`. |
| 5 | **Keep MSVC as a supported compiler** | Windows users can stay on MSVC 2019+ (Clang-cl also works). |

## Current repo layout (starting point)

| Component | Purpose | Port status |
|---|---|---|
| `voxlap/` | Core C engine → voxlap.dll. 45 MSVC `_asm` blocks in `voxlap5.c`. Writes to a caller-supplied framebuffer. No DirectX calls. | Major work: inline asm, 32-bit assumptions |
| `voxasm/v5.asm` | MASM32, 1422 lines. MMX/SSE/3DNow! rasterizer + sprite rasterizers. Includes user's ~300-line no-zbuffer sprite paths. | Must be rewritten |
| `Voxlaptest/` | C# .NET 2.0 frontend, uses Tao.Sdl. Hard-coded path `I:\taoframework-2.1.0\`. | **Delete** |
| `game/` | Ken's demo `game.c` plus a duplicate of the engine. Depends on an upstream `winmain.cpp` that is **not in this repo**. | **Delete or archive** |

The engine's host contract is already minimal: `voxsetframebuffer(xres, yres, pitch, framebuffer*)` + a global `keystatus[256]` polled by the engine. No DirectX lives inside `voxlap/`. That is what makes this tractable.

## What makes the code non-portable today

1. **MSVC inline asm** — 45 `_asm { ... }` blocks in `voxlap5.c` (~200 lines): `cossin`, `mul64`, `shldiv16`, `isshldiv16safe`, CPUID, `bswap`, `QIfist`-era FPU tricks. GCC/Clang reject the syntax. MSVC x64 also rejects `_asm`.
2. **MASM32 `v5.asm`** — full rasterizer. Clang/GCC can't consume MASM; needs conversion to intrinsics or portable C.
3. **MMX/3DNow!** — obsolete (dropped per decision 2).
4. **32-bit-only code** — `long` treated as 4 bytes, `__int64` instead of `int64_t`, x86 registers hardcoded, `.vcxproj` Win32-only.
5. **`__declspec(dllexport)`** on every public API — needs a portable `VOXLAP_API` macro.
6. **Build system** — VS2010 `.sln` with v140 toolset, Windows SDK 8.1, `/QIfist` (deprecated), post-build `install.bat` that isn't in the repo.
7. **Case-sensitive filename risk** on Linux/macOS for `.vxl`, `.kv6`, `.kfa` assets.

## Progress (as of 2026-04-24)

| Stage | Status |
|---|---|
| 0 — Baseline & CI | ✅ Complete. Oracle renders 7 fixed poses (4 terrain + 3 sprite) and diffs against frozen goldens on every CI run. |
| 1 — CMake + `VOXLAP_API` + VS artifact deletion | ✅ Complete. CMake is the sole build system; `.sln`/`.vcxproj`/`install.bat` are deleted. |
| 2 — 64-bit-clean the C | ✅ Complete, hash-neutral on MSVC x86. `long`→`int32_t`, `__int64`→`int64_t`, pointer casts flipped to `intptr_t`, structs audited (no blob-serialisation → 64-bit pointer widening is safe), `MAX_PATH` internalised, `setMaxScanDistToMax` VSID=2048 bug clamped. Variable declarations holding pointer values (`frameplace`, `gpixy`, etc.) are still `int32_t` — deferred because they feed the Stage-4-doomed inline asm. |
| 3 — Replace MSVC inline asm | ◑ Enumerated categories done (see table below). The remaining inline asm in voxlap5.c is MMX/SSE rasterization and 3DNow point4d helpers — these share registers and lookup tables with `v5.asm` and are rewritten as one piece in Stage 4. |
| 4 — Replace `v5.asm` + MMX inline asm | ⏭ Not started. Natural scope: the ~17 `_asm emms` flushes, ~60 inline `_asm { }` blocks in voxlap5.c, the entire MASM `v5.asm`, and the supporting MSVC-gated helpers (`expandbit256`, `mmxcoloradd`, `mmxcolorsub`). |
| 5 — Rust bindings | ⏭ Not started. |
| 6 — Polish | ⏭ Not started. |

**Oracle coverage.** 7 frozen hashes on MSVC x86, captured in `tests/oracle/golden-hashes.txt`:
- 4 terrain poses exercising opticast's raycaster, the fog path, and the voxel colorfunc dispatch.
- 3 sprite poses exercising `drawsprite` → `drawboundcube_sse` (the SSE sprite rasterizer v5.asm exposes). Sprite is built procedurally via `meltsphere` from a hidden multi-coloured block inside the surrounding solid mass, so no file I/O and no Linux-CI pre-flight.

**Non-Windows CI status.** Linux and macOS build continues to be red, but progressed from "header cascade" through "implicit declarations" and now halts at the Stage 4 boundary: bare `_asm emms` / `_asm { }` statements and MMX-only static helpers. The kplib.c `_asm` blocks (Paeth686, rgbhlineasm, pal8hlineasm, mulshr24/32, bitrev, testflag) all sit behind `#ifdef _MSC_VER && !NOASM` with portable-C `#else` fallbacks — non-MSVC compilers take the portable path there, so kplib.c is not a port blocker.

## Staged port

Each stage is independently shippable and testable. The DLL's C ABI stays stable across all stages so the Rust bindings built in Stage 5 keep working as the internals change.

### Stage 0 — Baseline & CI (½ week)

- Create a `port` branch; `master` stays on the current MSVC build.
- GitHub Actions matrix: Windows MSVC (x64), Linux GCC, Linux Clang, macOS Clang (x86_64 + arm64). Most will be red initially — that's the point.
- Produce a **reference correctness oracle**: a set of screenshots (or headless render hashes) from the current MSVC build. Every later stage diffs against this oracle.

### Stage 1 — CMake replacement of the .sln (1 week)

- Write `CMakeLists.txt` producing `voxlap` (shared library) on all three OSes.
- Keep the MSVC + MASM path working first; do not change any source yet. Validate nothing regressed vs. Stage 0 oracle.
- Replace raw `__declspec(dllexport)` with a `VOXLAP_API` macro via CMake's `generate_export_header` (handles `__declspec` on Windows, `__attribute__((visibility("default")))` elsewhere).
- Drop dependency on `install.bat`.

### Stage 2 — 64-bit-clean the C (1–2 weeks, MSVC only)

Refactor-only stage. 32-bit MSVC still builds. Screenshots must match the oracle.

- `long` → `int32_t` (via `<stdint.h>`) where 32-bit is required.
- `long` → `intptr_t` where the value is cast to/from a pointer.
- `__int64` → `int64_t`.
- Audit `#pragma pack(push,1)` structs (`vx5sprite`, `kv6data`, `kv6voxtype`) for pointer-sized fields.
- Replace the local `MAX_PATH` fallback with an engine-internal define that doesn't pull in `windows.h`.
- **Fix the `setMaxScanDistToMax()` VSID=2048 bug.** The helper in `voxlap/exports.c` computes `vx5.maxscandist = VSID*sqrt(2)`, which on this fork's 2048-wide map is ~2896 and overflows the raycaster's documented 2047 ceiling (`voxlap5.c:13062`). Upstream was safe with VSID=1024 (~1448). Symptom is specific ray columns rendering as black at fixed angles. Minimal fix: clamp to `min(VSID*sqrt(2), 2047)` inside the helper. Proper fix belongs in Stage 4 — investigate whether the 2047 cap is algorithmic or just a stale assumption, and lift it when rewriting `grouscanasm` if the former.

### Stage 3 — Replace MSVC inline asm with portable C/intrinsics (2 weeks)

Walk the `_asm` blocks in `voxlap5.c`. Most are trivial in modern C:

| Current asm | Replacement | Status |
|---|---|---|
| `cossin` / `dcossin` (x87 `fsincos`) | `sinf`/`cosf` | ✅ Stage 3.3 |
| `mul64` / `shldiv16` / `isshldiv16safe` / `mulshr16` / `umulshr32` / `scale` / `dmulrethigh` / `copybuf` / `clearbuf` | Plain C with `int64_t` | ✅ Stage 3.1 |
| `bswap` | `_byteswap_ulong` on MSVC. `__builtin_bswap32` on GCC/Clang still TODO. | ◑ Stage 3.4 (MSVC only) |
| CPUID | `__cpuid` on MSVC. `<cpuid.h>` on GCC/Clang still TODO. | ◑ Stage 3.4 (MSVC only) |
| `ftol` / `dtol` / `dbound` (FPU-stack tricks) | `lrintf`/`lrint` + ternary | ✅ Stage 3.2 |
| `bitrev` / `testflag` (kplib.c) | Portable C loop / hardcoded `return 1` | ✅ Stage 3.5 |
| `v5_asm_dep_unlock` (VirtualProtect dance) | **Delete** — deferred to Stage 4, bundled with `v5.asm` rewrite |

Verification (done): Stage 0 oracle goldens unchanged on MSVC x86 through every Stage 3 commit.

Outside Stage 3's intent but uncovered during it: the remaining inline asm in voxlap5.c is MMX/SSE rasterization (~60 blocks, ~17 `_asm emms`) plus the 3DNow point4d helpers at `voxlap5.c:9741+`. These are intertwined with `v5.asm`'s entry points and are rewritten in Stage 4.

### Stage 4 — Replace `v5.asm` with portable SSE2 intrinsics + scalar fallback (3–4 weeks)

`v5.asm` has these public entry points:

- `grouscanasm` (606 lines, MMX) — main voxel scanline rasterizer. **Highest risk item.**
- `drawboundcube{sse,3dn}{init,}` — 4 sprite rasterizers (with/without init helper)
- `drawboundcubenoz{sse,3dn}{init,}` — 4 no-zbuffer sprite rasterizers (user's custom 2023 work)
- `opti4asm` — constants table
- `v5_asm_dep_unlock` — **delete** (Stage 3)

Approach:

1. **Write scalar C reference first.** Correctness on all ten entry points. This alone unlocks Linux/macOS/ARM64.
2. **Add SSE2 intrinsics** (`<emmintrin.h>`) for x86-64. SSE2 is baseline; no runtime dispatch.
3. **Optionally NEON** for ARM64 macOS later. Scalar may be fast enough on Apple Silicon for this era of engine.
4. Keep the scalar path compiled behind `VOXLAP_SCALAR=1` as a permanent correctness oracle and a landing pad for future architectures.
5. Gut `getcputype()` — compile target guarantees SSE2 on x86-64.

Risk: the MMX rasterizer uses subtle fixed-point tricks and register pairing. Budget for off-by-one color/depth bugs. Use screenshot diffs aggressively.

**Stage 4 sub-plan** (2026-04-24). Each step lands as its own commit; the oracle's 7 frozen hashes are the regression net.

| Step | Scope | Hash impact | Linux impact |
|---|---|---|---|
| 4.1 | `mmxcoloradd` / `mmxcolorsub` / `expandbit256` → portable C | Bit-exact (saturated byte add/sub, bit-RLE loop) | Helpers come out of `#ifdef _MSC_VER`; clears ~5 implicit-declaration errors |
| 4.2 | Delete all `*_3dn` variants (per decision 2 — drop 3DNow). Gut `getcputype`'s dispatch use while keeping the function for diagnostics | Hash-neutral on modern CPUs (CI runner never selects `_3dn`) | Halves the inline-asm block count; removes 3DNow from `v5.asm` |
| 4.3 | SSE point4d helpers (line ~9741+) → `<emmintrin.h>` SSE2 intrinsics | Bit-exact (same SSE2 instructions emitted) | Block compiles under `-msse2` on any x86-64 |
| 4.4 | `drawboundcube_sse` sprite rasterizer → SSE2 intrinsics + scalar fallback | Target bit-exact; budget for re-freeze of the 3 sprite_* goldens | Sprite path renders on Linux/macOS |
| 4.5 | `grouscanasm` (the 606-line MMX scanline) → scalar C + SSE2. See [voxasm/GROUSCANASM.md](voxasm/GROUSCANASM.md) for the algorithm spec (written in 4.5a). | Re-freeze likely, probably all 7 poses | Terrain path renders on Linux/macOS |
| 4.6 | Delete `voxasm/v5.asm`, remove `v5_asm_dep_unlock`, gut `getcputype`, drop `enable_language(ASM_MASM)` in CMake | Hash-neutral if 4.5 lands correctly | MASM no longer required — CMake can target non-MSVC toolchains end-to-end |

### Stage 5 — Rust bindings + Rust host (2–3 weeks)

Replace the C# Voxlaptest frontend with a Rust-based alternative.

- **`voxlap-sys` crate** — `bindgen`-generated raw FFI from `voxlap5.h`. One-to-one mapping. `build.rs` links the shared library. No safety layer.
- **`voxlap` crate** — safe Rust wrapper. Rust-idiomatic types for `dpoint3d`, `vx5sprite`, `kv6data`, etc. RAII for engine init/shutdown, map load/unload, lighting handles.
- **`voxlap-sdl` example binary** — SDL2 window via the `sdl2` crate. Create an `SDL_Texture` in streaming mode, lock for pixels each frame, hand the pointer to `voxlap` via `voxsetframebuffer`, unlock, present. Pump SDL keyboard events into the `keystatus[256]` array the engine polls.
- Cargo workspace at the repo root. Cargo drives CMake (via the `cmake` crate) so `cargo build` produces both the native lib and the Rust binary.
- Delete `Voxlaptest/` and `game/`.

### Stage 6 — Polish & packaging (1 week)

- Case-insensitive asset lookup helper for `.vxl`/`.kv6`/`.kfa` (try as-given, then lowercase, then uppercase).
- CMake install rules + CMake config package so C/C++ consumers can `find_package(Voxlap)`.
- Publish the Rust crates (or at least tag them) for downstream use.
- Document Ken Silverman's original Voxlap license terms — royalty-free for non-commercial use, commercial use requires a license from Ken. Relevant for anyone redistributing.

## Effort estimate

- **Minimum viable port** (Linux + macOS scalar-only, Rust SDL2 host, modern MSVC on Windows): **6–7 weeks**
- **Full port** (SSE2 intrinsics + ARM64 NEON, CI matrix, Rust crates, polish): **10–13 weeks**
- Biggest risk: Stage 4 (`grouscanasm`). The 606-line MMX loop is where hidden bugs will hide.

## Out of scope

- Networking, multiplayer, or any engine feature work.
- Porting the original demo game logic (`game/game.c`) — it's dormant and depends on the missing `winmain.cpp`.
- A Vulkan/Metal backend — the engine is a software rasterizer; the GPU only needs to blit one texture per frame, which SDL2 already does via its default renderer.
