# Stage 4.9 — Recover SSE inner-loop perf on x86_64

This file is a self-contained handoff. After Stage 4.8 the engine
**runs** on Windows MSVC x86 and Linux GCC/Clang x86_64 with bit-
identical oracle hashes; all CI gates green.  But Stage 4.7.5/4.7.6
threw away the 4-pixel-unrolled SSE inner loop in four hot
rasterizers (`hrendzsse`, `hrendzfogsse`, `vrendzsse`,
`vrendzfogsse`), replacing each with a scalar `1.0/sqrtf` C loop.
Per-pixel work in opticast is now ~4× more expensive than the
original.  Stage 4.9 puts the SIMD back, using portable SSE
intrinsics (no inline asm) so the same source compiles under
MSVC + GCC + Clang.

This is a **standalone perf-recovery stage on x86_64.**  macOS
arm64 / NEON is Stage 5 (PORTING-NEON.md, future); the Rust
crate is Stage 6.  Nothing here touches the still-portable
`_mm_*` intrinsics already scattered through opticast,
`drawboundcubesse`, kv6draw, `drawtile`'s alpha blend etc. —
those compile cleanly on x86 and are not in 4.9 scope.

## Where things stand

- Windows MSVC x86: gated CI, `tests/oracle/golden-hashes.txt`.
- Linux GCC + Clang x86_64 (CI Ubuntu): bit-identical to Windows,
  share the same golden table.
- Local Linux dev shells (nix, etc.): may diverge on
  `sprite_above` / `sprite_coco` because their libm `sqrtf`
  rounds slightly differently from the SSE `rsqrtss`
  approximation that MSVC emits.  After 4.9 every x86_64 build
  should converge on the original SSE-derived hashes regardless
  of libm — that's a free win.

## What got lost in 4.7.5 / 4.7.6

Four function bodies, all in `voxlap/voxlap5.c` (line numbers
post-4.8.4):

```
1947  void hrendzsse     (..., int32_t p1, int32_t plc, int32_t incr, int32_t j)
1973  void hrendzfogsse  (..., int32_t p1, int32_t plc, int32_t incr, int32_t j)
2003  void vrendzsse     (..., int32_t p1, int32_t iplc, int32_t iinc)
2028  void vrendzfogsse  (..., int32_t p1, int32_t iplc, int32_t iinc)
```

Each has an `/* Portable scalar replacement … */` comment that
calls out the deferred SIMD work.  The shape:

```c
while (p0 != pe) {
    *(int32_t *)p0 = angstart[plc>>16][j].col;          /* colour */
    *(float *)(p0+i) = (float)angstart[plc>>16][j].dist /* z      */
                     / sqrtf(dirx*dirx + diry*diry);
    dirx += optistrx;
    diry += optistry;
    plc  += incr;
    p0   += 4;
}
```

Per-pixel: one `sqrtf`, one division, one colour store, one
float store, four scalar adds.  The original asm was a
4-pixel-unrolled `rsqrtps` + `mulps` + non-temporal stores
loop.  Reintroducing it is the entire point of 4.9.

**Two more disabled SSE paths exist** but are *not* exercised
by the oracle.  They should be looked at, but only after the
four above are landed — and they might end up deferred to the
Rust+SSE stage (6) rather than completed here.  See "Out of
scope" at the end.

## Scope of Stage 4.9

| Function          | Oracle pose coverage                                            | 4.9 work?       |
|-------------------|-----------------------------------------------------------------|-----------------|
| `hrendzsse`       | every render pose (basic horizontal scan)                       | **Yes**         |
| `hrendzfogsse`    | every pose with `ofogdist >= 0` (oracle sets it)                | **Yes**         |
| `vrendzsse`       | every pose with vertical scan path                              | **Yes**         |
| `vrendzfogsse`    | every fog-on vertical scan                                      | **Yes**         |
| `drawpolyquad`    | not called from oracle.c                                        | Out of scope    |
| `drawspherefill`  | not called from oracle.c                                        | Out of scope    |

Out-of-scope items get a `/* TODO(4.10): SIMD recover */` line
referencing this doc and stay scalar.  No silent leftovers.

## Strategy

### Intrinsics, not inline asm

Every SIMD line uses the portable `<xmmintrin.h>` / `<emmintrin.h>`
intrinsics (`__m128`, `_mm_load_ps`, `_mm_rsqrt_ps`, `_mm_mul_ps`,
`_mm_stream_ps`, …).  These compile on MSVC, GCC, and Clang on
x86_64 with no flag changes — `-msse2` is implied on x86_64
ABI, and MSVC's x86 build already has `/arch:SSE2` baseline.

No new inline asm.  Reasons:
- inline asm is what we *removed* in 4.7; reverting that creates a
  step backward in portability and in MSVC-vs-GCC syntax drift.
- the asm/intrinsic emission for `rsqrtps` + `mulps` + `movntps` is
  identical at -O2 across all three compilers.

### Per-loop shape

Each of the four functions becomes:

```c
/* Process the framebuffer span 4 pixels at a time with rsqrtps;
 * fall back to the scalar tail for any leftover (1..3) pixels. */

const __m128 stride4_x = _mm_set1_ps(optistrx * 4.0f);
const __m128 stride4_y = _mm_set1_ps(optistry * 4.0f);
__m128 dx4 = _mm_setr_ps(dirx, dirx + optistrx,
                          dirx + 2*optistrx, dirx + 3*optistrx);
__m128 dy4 = _mm_setr_ps(diry, diry + optistry,
                          diry + 2*optistry, diry + 3*optistry);

const intptr_t pe4 = pe & ~15;            /* aligned-to-16 quad end */
while (p0 < pe4) {
    /* 4 colours + 4 distances from angstart[(plc>>16)..]: gather */
    const cast4 *a = &angstart[plc>>16][j];   /* … or similar */
    /* colour quad */
    _mm_storeu_si128((__m128i *)p0, _mm_loadu_si128((const __m128i *)a->col4));
    /* z = dist / sqrt(dx*dx + dy*dy) using rsqrt */
    __m128 sqr = _mm_add_ps(_mm_mul_ps(dx4,dx4), _mm_mul_ps(dy4,dy4));
    __m128 inv = _mm_rsqrt_ps(sqr);                /* fast 1/sqrt */
    __m128 z   = _mm_mul_ps(_mm_loadu_ps(a->dist4), inv);
    _mm_storeu_ps((float *)(p0+i), z);
    dx4 = _mm_add_ps(dx4, stride4_x);
    dy4 = _mm_add_ps(dy4, stride4_y);
    plc += incr * 4;
    p0  += 16;
}
/* Scalar tail: 0..3 pixels. */
while (p0 != pe) { … same scalar body 4.7.5/.6 carries today … }
```

Notes:
- `angstart[(plc>>16)..]` is a 1-D table of `castdat` (col + dist
  pair).  The SSE batch needs four `col` ints and four `dist`
  floats packaged into __m128 / __m128i.  In the original asm
  they were gathered with four `lea` + `mov` pairs; in
  intrinsics use `_mm_setr_epi32` / `_mm_setr_ps`.
- For the **fog** variants (`hrendzfogsse` / `vrendzfogsse`) the
  per-channel blend is the asm's `pmulhw` against `foglut[l>>20]`;
  4.7 already ported that to scalar, so 4.9 only has to lift it
  back into 4-lane MMX/SSE2 (`_mm_mulhi_epu16`, `_mm_packus_epi16`,
  …). Watch for the `±255 → ±256` corner cases the scalar code
  documents — the SIMD path needs the same correction.
- For **vertical** variants (`vrendzsse` / `vrendzfogsse`) the
  per-pixel `uurend[sx] += uurend[sx+MAXXDIM];` increment is
  serial as written.  Either compute the SIMD batch first and
  patch up `uurend` after, or hand-unroll 4 increments.
- Use `_mm_stream_ps` for the framebuffer + zbuffer stores if
  you can guarantee 16-byte alignment; otherwise `_mm_storeu_ps`
  is fine and the perf delta is small at typical resolutions.

### Hashes

After 4.9 lands the oracle hashes should NOT shift on the CI
configurations (windows-msvc-x86 / linux-gcc / linux-clang) —
those already match the rsqrt-derived golden.  *Local* dev
shells that currently differ on `sprite_above` /
`sprite_coco` should converge.  If any *CI* hash shifts:
- sanity-check the SSE batch produces the same bit pattern as
  the asm did (rounding mode, `rsqrtps` Newton refinement,
  store ordering on the boundary pixel);
- if a one-off rounding bit flips, refreeze with explicit
  user OK — this is the kind of drift that is allowed
  *only* once the SIMD inner loop is provably equivalent.

## Sub-stages

Each stage ends in green CI on all three platforms.  Move on
only when hashes still match.

**4.9.1 — hrendzsse SIMD inner loop**
Re-introduce 4-pixel batch using `_mm_rsqrt_ps`.  Smallest blast
radius (no fog, no per-pixel uurend update).  Likely the
template the other three stages copy from.

**4.9.2 — hrendzfogsse**
Add the `foglut`-based per-channel fog blend to the batch.
Single-channel test first (R, then G, then B) so the ±255 corner
cases stay debuggable.

**4.9.3 — vrendzsse**
Vertical scan: handle the per-pixel `uurend[sx] += uurend[sx+MAXXDIM]`
update without serializing the SSE batch (gather + scatter, or
unrolled scalar update after the batch store).

**4.9.4 — vrendzfogsse**
Combine 4.9.2's fog blend with 4.9.3's vertical scan.

**4.9.5 — Optional: refreeze hashes**
If any CI hash shifted *because* of a known-good rounding
divergence, refreeze with the user's explicit go-ahead.  Likely
nothing to do; flagged only so it's not a surprise.

## Anti-patterns

- **Don't reintroduce inline asm**.  4.7 paid the price to remove
  it; we want intrinsics-only going forward.  If a path can't be
  expressed in intrinsics, leave it scalar and TODO it.
- **Don't blanket-vectorize**.  Only the four functions above.  The
  rest of voxlap5.c has plenty of `_mm_*` already, doing fine.
- **Don't widen `optistrx`/`optistry` etc**.  They're scalar floats;
  the SSE batch loads four lanes from each via `_mm_set1_ps`.
- **Don't change ABI**.  These functions are called via
  `void (*hrend)(int32_t,…)` / `void (*vrend)(int32_t,…)`
  function pointers (voxlap5.c:1885); keep the signatures as-is.
- **Don't conflate with NEON**.  Stage 5 is the macOS arm64 stage
  and gets its own doc.  Adding `#if defined(__SSE2__)` guards is
  fine; adding NEON paths is not.

## Reference: existing intrinsic uses to mirror

Already-portable intrinsic usage in voxlap5.c (compiles MSVC +
GCC + Clang today):

- `drawboundcubesse` family: ~30 `_mm_*` calls covering the
  rotated-cube projection.
- `kv6draw` / `kv6frame` family: `_mm_set1_*` + `_mm_mul_ps` for
  per-voxel colour modulation.
- `drawtile` alpha blend (line ~6840): `_mm_packus_epi16`,
  `_mm_mullo_epi16` etc. — already SIMD-friendly post-4.7.
- `_mm_rsqrt_ss` *is* used at voxlap5.c:10453 (single-lane), so the
  toolchain support is already in place — 4.9 just lifts it from
  scalar to 4-wide.

The shape to copy is the kv6draw inner loop's `_mm_loadu_ps` →
`_mm_mul_ps` → `_mm_storeu_ps` triplet, with a scalar tail for the
leftover 0..3 pixels.

## Useful one-liners

```sh
# Rebuild + run oracle in nix dev shell
nix develop --command bash -c \
  'cmake --build build-nix && \
   ( cd build-nix/oracle-run && LD_LIBRARY_PATH=../bin ../bin/oracle )'

# Diff against the gating golden, line-ending-tolerant
diff -u --strip-trailing-cr \
    tests/oracle/golden-hashes.txt build-nix/oracle-run/hashes.txt

# Check that no -funsigned-char regression sneaks in
grep -n 'funsigned-char' CMakeLists.txt   # must still be there post-4.9
```

## Out of scope (deferred to later stages)

- **`drawpolyquad` SSE rcpps batch** (voxlap5.c:7429 comment).
  Not exercised by oracle, so SIMD recovery here is pure perf
  with no bit-exactness guard.  Defer to Stage 6 (Rust+SSE) where
  SIMD will be reintroduced uniformly.
- **`drawspherefill` SSE rcpps batch** (voxlap5.c:7368 comment).
  Same reasoning — `#if 0` branch is the only path, no oracle
  coverage.
- **MMX intrinsics in `mmxcoloradd`** etc.  Already portable
  (`_mm_*`) and not lost in 4.7's rewrite.
- **macOS arm64 / NEON port**.  Stage 5.
- **The grouscan mip-transition XOR/halve math TODO** at
  voxlap5.c:~11811 (PORTING-X64.md, end of 4.8.4f).  Not perf;
  bit-shift correctness for a code path the oracle never hits
  (`vxlmipuse == 1` always).  Belongs to a separate mip stage.

## Files that matter

- `voxlap/voxlap5.c` lines ~1947..2070 — the four scalar
  rasterizers.  This is where 4.9 makes its changes.
- `voxlap/voxlap5.h` — no changes expected (signatures via
  function-pointer typedef stay int32_t).
- `tests/oracle/golden-hashes.txt` — must not shift on
  windows-msvc-x86 / linux-gcc / linux-clang.  If any row
  shifts, stop and discuss before refreezing.
- `.github/workflows/ci.yml` — no changes expected.
