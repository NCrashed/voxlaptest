# Stage 4.8 — Make x86_64 Linux operational

This file is a self-contained handoff. After Stage 4.7 the codebase
**compiles** on Linux x86_64 (gcc 15.2 / clang) and links cleanly. It
**doesn't run** there: the oracle aborts at `voxsetframebuffer` time
with `*** buffer overflow detected ***`. This document captures
everything needed to take the runtime green from a fresh session.

## Where things stand

- Windows MSVC x86: builds, oracle runs, hashes frozen in
  `tests/oracle/golden-hashes.txt`. Authoritative gate.
- Linux GCC + Clang x86_64 (CI Ubuntu): builds, oracle runs,
  produces hashes bit-identical to the Windows MSVC golden.
  Same one golden file works for both platforms. (The scalar
  1.0/sqrtf rewrite from Stage 4.7.5/4.7.6 happened to match
  the rsqrtss-derived hashes on this combination of toolchain
  + libm; other Linux distros / compilers may diverge on the
  `sprite_above` / `sprite_coco` rows. If that happens, split
  into per-toolchain tables — but ubuntu-latest is the gating
  Linux for now.)
- macOS Clang x86_64: not in CI. The `macos-13` runner has been
  deprecated by GitHub Actions and jobs queue without ever being
  assigned. Local x86_64 macOS builds are expected to behave like
  Linux.
- macOS Clang arm64 (`macos-latest`): blocked. `<xmmintrin.h>` and
  `<mmintrin.h>` are x86-only headers; voxlap5.c uses 170+ MMX/SSE
  intrinsic calls in opticast / drawboundcubesse / kv6draw. Out of
  scope here — separate NEON-or-sse2neon stage (Stage 5).

CI: see `.github/workflows/ci.yml`. windows-msvc-x86 / linux-gcc /
linux-clang all run the oracle and gate on the same shared golden
hashes. The macos-clang job was removed when the macos-13 runner
became unschedulable; re-add once Stage 5 lands.

## Root cause (one line)

The engine stores host pointers in `int32_t` fields and reconstitutes
them with `*(int32_t *)(...)` casts. Every site silently truncates
the upper 32 bits on x86_64.

## The smoking gun

`oracle` calls `voxsetframebuffer((intptr_t)g_fb, ...)`. Inside:

```c
// voxlap/voxlap5.c:135
static int32_t xres, yres, bytesperline, frameplace, xres4;

// voxlap/voxlap5.c:10876
void voxsetframebuffer (intptr_t p, int32_t b, int32_t x, int32_t y) {
    int32_t i;
    frameplace = p;          // <-- silent truncation on x64
    ...
    kv6frameplace = p - (qsum1[0]*qbplbpp[0] + qsum1[1]*qbplbpp[1]);
    ...
}
```

`frameplace` is later used to address pixels:

```c
// many sites, e.g. voxlap/voxlap5.c:6606
*(int32_t *)(ylookup[sy]+(sx<<2)+frameplace) = col;
```

`(int32_t)` cast of a 32-bit value back to a pointer reinterprets the
low 32 bits as the upper bits of a 64-bit pointer (or zero-extends —
implementation-defined; on x86_64 it sign-extends), giving an
address in the wrong half of the address space. The first `memset`
that uses the truncated base trips glibc's `_FORTIFY_SOURCE` overflow
check and the process aborts.

## The next-most-vicious bug class

`sptr` is a *static array of pointers*:

```c
// voxlap/voxlap5.c:67
char *sptr[(VSID*VSID*4)/3];
```

On x86 each entry is 4 bytes; on x86_64, 8. Five sites do:

```c
// voxlap/voxlap5.c:3241, 3345, 3433, 3522, 3648
memset(&sptr[VSID*VSID], 0, sizeof(sptr) - VSID*VSID*4);
```

`sizeof(sptr)` doubles on x64 but the constant `VSID*VSID*4` does
not, so the size argument balloons to roughly 28 MB (vs the intended
~5.6 MB region) and `memset` walks off the end. GCC catches this with
`-Wstringop-overflow=` at:

- `loadbsp`  (voxlap5.c:3522)
- `loadnul`  (voxlap5.c:3241)
- `loadvxl`  (voxlap5.c:3648)

(plus two more sites without inline-callsite warnings: lines 3345 and
3433.)

## Inventory of pointer-width bugs

Counts from a fresh GCC 15.2 build (`-Wint-to-pointer-cast`,
`-Wpointer-to-int-cast`, `-Wint-to-void-pointer-cast`,
`-Wstringop-overflow=`):

```
voxlap/voxlap5.c   100 warnings  (92 unique source lines)
voxlap/kplib.c      35 warnings
glibc fortified.h    3 warnings  (the memset overflows above)
```

Functions in voxlap5.c that produce warnings (sorted):

```
compilerle, dofall, drawline2d, drawline2dclip, drawpicinquad,
drawpoint2d, drawpoint3d, drawpolyquad, drawspherefill, drawtile,
floodsucksprite, hrendzfogsse, hrendzsse, initvoxlap, isnewfloating,
loadsky, meltfall, meltspans, meltsphere, opticast, print4x6,
print6x8, screencapture32bit, setcube, setflash, setkvx, setnormflash,
tmaphulltrisortho, uninitvoxlap, vrendzfogsse, vrendzsse
```

Quick survey command:

```sh
nix develop --command bash -c \
  'rm -rf build-nix && cmake -S . -B build-nix -G Ninja \
     -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
   && cmake --build build-nix' \
  > /tmp/build.log 2>&1
grep -E 'pointer-to-int|int-to-pointer|int-to-void|stringop-overflow' \
  /tmp/build.log | sort -u
```

## Recurring patterns to fix

These cover ~95 % of the warnings. Each pattern has a mechanical fix.

### P1 — `frameplace` family

Globals that hold a raw memory address:

```c
static int32_t frameplace;   // voxlap5.c:135
int32_t kv6frameplace;       // voxlap5.c:7891
```

Plus `kv6frameplace`'s siblings declared at voxlap5.c:7873-7878
(`qsum0`, `qsum1`, `qbplbpp`, `kv6bytesperline`).  Of those, only
`*frameplace` are *addresses*; the others are scalars and stay
`int32_t`.

**Fix**: change the type of `frameplace` and `kv6frameplace` to
`intptr_t`. Audit every site that does pointer arithmetic with them.
Most do `*(int32_t *)(ylookup[y]+(x<<2)+frameplace)` — once the type
is `intptr_t` the addition is pointer-sized and the cast is correct.

`ylookup[]` is `int32_t[]` (line 136). Holds row offsets in bytes
(0 .. yres*bytesperline). `bytesperline * MAXYDIM` fits in int32 for
realistic resolutions, so `ylookup` itself can stay `int32_t`. The
`ylookup[y] + frameplace` expression then becomes
`int32_t + intptr_t` → `intptr_t`. ✓

### P2 — `sptr` and friends (array-of-pointers)

```c
char *sptr[(VSID*VSID*4)/3];      // voxlap5.c:67
char *bacsptr[262144];            // voxlap5.c:146
```

The five `memset(&sptr[VSID*VSID], 0, sizeof(sptr) - VSID*VSID*4)`
sites are wrong on any LP64/LLP64 target.

**Fix**: rewrite the size argument:

```c
memset(&sptr[VSID*VSID], 0,
       sizeof(sptr) - VSID*VSID*sizeof(sptr[0]));
```

Identical mechanical replacement at all five sites
(3241, 3345, 3433, 3522, 3648).

Audit other `sizeof(arr) - count*4` patterns in the file:

```sh
grep -nE 'sizeof\([a-z_]+\)\s*-\s*[A-Z_*0-9]+\*4' voxlap/voxlap5.c
```

### P3 — `(int32_t)pointer` round-trips

139 sites in voxlap5.c, 59 in kplib.c.  Most look like one of:

```c
*(int32_t *)p0 = ...;          // p0 is int32_t holding an address
return((int32_t)kzfs.fil);     // FILE* truncated to int (kplib.c)
free((void *)*pic);            // *pic is int32_t holding malloc'd ptr
```

**Fix strategy**: change the *storage* type from `int32_t` to
`intptr_t`. Then most casts naturally widen and stop truncating.
Likely-affected locals/params:

- Local `p0`, `p1` in `hrendzsse`, `hrendzfogsse`, `vrendzsse`,
  `vrendzfogsse` (the four scalar rasterizers I added in 4.7.5/4.7.6)
  — they hold framebuffer addresses and are typed `int32_t`. Change
  to `intptr_t`.
- Local `p` in `drawline2d`, `drawpoint2d`, `setflash`, etc.
- KZ fileio in kplib.c: `kzfs.fil` is a `FILE *` cast to `int32_t`
  in several `kzopen`-style return paths. Either change the field
  type or stop returning addresses through `int32_t`.

### P4 — `cputype`-style flag dispatch (low-risk)

A handful of `(int32_t)(intptr_t)x` casts where the value is
genuinely small and not used as an address (cputype bits, integer
fields stuffed into pointer slots). Audit case-by-case; usually a
no-op once P1-P3 are clean.

## Strategy — five staged tasks

Suggested decomposition. Each ends in a green CI cycle; each stage's
bit-exactness against `golden-hashes.txt` is asserted by the Windows
oracle (still the gate) and the Linux oracle is added as a second
gate at the end.

**4.8.1 — `frameplace` + `kv6frameplace` to `intptr_t`**
Type change, audit ~50 call sites, no algorithmic change. Probably
gets the engine past `voxsetframebuffer`. Windows hashes must remain
unchanged (the values stored are the same; only the carrier widens).

**4.8.2 — `sptr` size arithmetic + companion arrays**
Five `memset` rewrites, plus any other `sizeof(arr-of-ptr)` sites.
Mechanical. Should clear the immediate `__builtin___memset_chk`
abort.

**4.8.3 — Local `p0` / `p1` rasterizer pointers**
Stage 4.7.5/4.7.6 (hrendzsse, hrendzfogsse, vrendzsse, vrendzfogsse)
introduced `int32_t p0, p1` for framebuffer cursors. Widen to
`intptr_t`. Limited blast radius; hashes should be neutral.

**4.8.4 — kplib.c FILE\* round-trips and remaining `(int32_t)ptr`**
Loose ends. Most invasive of the four because `kzfs.fil` is exposed
through kplib's API surface. Worth checking whether *any* consumer
treats the return value as a token vs as a `FILE *`; if the answer
is "always FILE \*", just change the return type.

**4.8.5 — Linux oracle parity**
Run `oracle` under `nix develop` and capture hashes. Two outcomes:
- (a) Hashes match Windows MSVC — gold standard, freeze a
  per-platform table or trust they always agree.
- (b) Hashes differ — expected, since the scalar rasterizers in
  4.7.5/4.7.6 use 1.0/sqrtf instead of rsqrtss; document the diff,
  freeze a Linux-specific golden file, and have CI compare each
  platform against its own goldens.

After 4.8.5 the Linux GCC and Linux Clang CI jobs add an oracle-run
step like the Windows job already has.

## Useful one-liners

```sh
# Reproduce the Linux build locally
nix develop --command bash -c \
  'rm -rf build-nix && cmake -S . -B build-nix -G Ninja \
     -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
   && cmake --build build-nix'

# Run the oracle (will currently abort)
( cd build-nix/oracle-run && \
  LD_LIBRARY_PATH=../bin ../bin/oracle )

# Check pointer-width warnings
cmake --build build-nix 2>&1 | \
  grep -E 'pointer-to-int|int-to-pointer|int-to-void|stringop-overflow' | \
  sort -u

# Group warnings by function (after a fresh full build into /tmp/build.log)
grep "voxlap5.c.*In function" /tmp/build.log | sort -u
```

## Things to avoid

- **Don't use `-m32`**. Tempting fix; pulls in 32-bit libc/libm/libgcc
  through Nix which complicates the dev shell, doesn't address the
  underlying bug, and means Linux users link against a 32-bit
  binary. We're better off porting to 64-bit-clean.
- **Don't widen everything to `intptr_t` blindly**. `ylookup`,
  `bytesperline`, image dimensions etc. are *scalars*, not addresses.
  Widening them changes file format calculations and hashes.
- **Don't suppress the warnings** with `-Wno-int-to-pointer-cast`.
  They are the inventory.
- **Watch the oracle hash gate**. Every change must keep the Windows
  MSVC hashes green; that's the primary correctness signal until the
  Linux oracle joins it in 4.8.5. If hashes shift unexpectedly,
  refreeze with the user's explicit OK first.

## Reference: types and storage that are correct already

`voxsetframebuffer`'s public signature already uses `intptr_t`:

```c
// voxlap5.h:186
VOXLAP_API extern void voxsetframebuffer (intptr_t, int32_t, int32_t, int32_t);
```

So the *boundary* is right. The bug lives entirely in voxlap's
internal storage.

`oracle.c` passes `(intptr_t)g_fb` correctly:

```c
// tests/oracle/oracle.c:328
voxsetframebuffer((intptr_t)g_fb, BYTESPERLINE, XRES, YRES);
```

intptr_t is already used 111 times in voxlap5.c, 9 times in kplib.c
— so the patch is migration of remaining `int32_t`-typed addresses
into the existing `intptr_t` convention, not invention of a new one.

## Files that matter

- `voxlap/voxlap5.c` — engine, primary surface area.
- `voxlap/kplib.c` — image/zip loader, secondary surface area.
- `voxlap/voxlap5.h` — public API; signatures already use `intptr_t`,
  no changes expected.
- `voxlap/exports.c` — thin DLL exports; no warnings.
- `tests/oracle/oracle.c` — gating harness; no changes expected.
- `CMakeLists.txt` — already adds `target_link_libraries(voxlap
  PUBLIC m)` and the GCC `NOASM` macro. No changes expected for 4.8.

## Out of scope

- macOS arm64 (Apple Silicon): blocked on MMX/SSE → NEON port. Track
  separately as Stage 5.
- Rust crate: per the user, "after we will move to quality of life
  to port to Rust (and SSE)". Stage 6+.
- Performance recovery in the scalar rasterizers: 4.7.5/4.7.6
  replaced 4-pixel-unrolled rsqrtps loops with scalar 1.0/sqrtf. SIMD
  reintroduction lives in the Rust+SSE stage.
