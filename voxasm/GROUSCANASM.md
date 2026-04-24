# grouscanasm — Algorithm Specification

**Purpose**: companion document to `v5.asm`'s `_grouscanasm` (lines 172–722), written as the specification for Stage 4.5b's portable-C rewrite. Captures what the 543-line MMX routine *does*, label-by-label, so the C port can be audited against algorithm intent rather than byte-for-byte asm.

Everything here is derived from reading v5.asm — there is no upstream Voxlap doc for this function.

---

## 1. Contract

**Signature** (C): `void grouscanasm(intptr_t vptr)`. One argument on the stack: the voxel-slab linked-list head (`vptr` comes from `gstartv = sptr[ipy*VSID+ipx]` in voxlap5.c). Calling convention: `__cdecl`.

**Preserves**: `EBX`, `ESI`, `EDI`, `EBP`, `ESP` (via `espbak` save/restore). Trashes `EAX`, `ECX`, `EDX`, all MMX regs, CS flags, and the FPU stack (exits via `emms`).

**Reads from (globals declared in v5.asm)**:
| Symbol | Type | Meaning |
|---|---|---|
| `_gi` | qword `{int32 gi0, gi1}` | Per-step delta added to `(cx, cy)` on each pixel advance |
| `_gdz` | `int32 gdz[2]` | Per-step delta added to `_gpz` for the two raycast lanes |
| `_gpz` | `int32 gpz[2]` | Current distance along the two raycast lanes |
| `_gxmax` | int32 | Final distance cap (sky starts beyond this) |
| `_gxmip` | int32 | Distance at which the current mip level ends |
| `_gmipnum` | byte | Total number of mip levels available |
| `_gixy` | `int32 gixy[2]` | Voxel-column step vectors (dx*4, dy*VSID*4) for the two lanes |
| `_gylookup` | `int32[MAXZDIM*4]` | Screen-y for each voxel-z (per-mip, via `gylut`) |
| `_gpixy` | int32 (pointer-sized) | Pointer into `_sptr` for the current column's slab head |
| `_gcsub` | qword `[4]` | Side-shading subtraction vectors per face (wall/wall/ceil/floor) |
| `_skyoff` | int32 | Pointer to loaded sky texture (0 = no sky loaded) |
| `_skyxsiz` | int32 | Sky texture width |
| `_skylat` | int32 | Sky latitude direction vectors |
| `_skycast` | qword | Fallback colour when sky isn't loaded |
| `_sptr` | base pointer | Voxel column head-pointer array |

**Scratch backings (in v5.asm data section)**:
| Symbol | Size | Use |
|---|---|---|
| `_cfasm` | 8192 bytes | The pseudo-stack of `c` (cf-struct) entries, 32 bytes each |
| `gylookoff` | dword | `&_gylookup[mip_offset]` — current mip's screen-y table |
| `ngxmax` | dword | min(`_gxmax`, `_gxmip`, doubled once per mip step) |
| `ce` | dword | `_cfasm` high-water-mark (top of the pseudo-stack) |
| `espbak` | dword | Saved real ESP so we can restore on exit |
| `gmipcnt` | byte | Current mip level (0 = finest) |
| `mmask` | qword | `0xffff0000ffff0000` — `pand` mask for extracting gx fields in mm6 |
| `gyadd` | qword | `(-1 << (LOGPREC-16))` — unused in the code path we care about |
| `w8bmask0/1/2` | qword each | Colour-shading masks, 0x00ff00ff00ff00ff etc. |
| `gxmipk/gymipk/gamipk` | dword[10] each | Per-mip offset and mask tables |
| `gylut` | dword[MAXZDIM*4] | Base `gylookup` pointers indexed by mip level |

**Output**: pixels and (optionally) z-buffer values written to the framebuffer. Specifically, the `c->i0..i1` ranges inside `_cfasm` get filled with colour dwords (and, with `USEZBUFFER`, adjacent z dwords).

---

## 2. The cfasm pseudo-stack

Each `c` entry is 32 bytes:

| Offset | Field | MMX alias |
|---|---|---|
| 0 | `i0` — byte pointer to left edge of this column's draw extent (framebuffer address) | — |
| 4 | `i1` — byte pointer to right edge (exclusive) | — |
| 8 | `z0` — voxel-z at left edge (ceiling z of visible slab) | — |
| 12 | `z1` — voxel-z at right edge (floor z) | — |
| 16 | `cx0, cy0` (two int32) | mm0 |
| 24 | `cx1, cy1` (two int32) | mm1 |

**Memory layout of `_cfasm`** (the `[esp+2048]` offsets throughout the asm come from this):

```
byte  0 .. 2047    — guard space (never written, safety margin)
byte  2048 .. 4095 — active c entries: c, c+32, c+64, ..., ce (inclusive)
                     ESP points here during the whole function; `esp == c`,
                     `ce` holds the top (deepest) entry's address.
byte  4096 .. 6143 — `c0`, the head entry. Prefilled by the C caller.
                     _grouscanasm's prologue copies c0->z0, z0->z1, cx/cy
                     from `_cfasm[4096..]` into registers/mm0/mm1 before
                     starting the main loop.
byte  6144 .. 8191 — unused (noted as "seems unnecessary" in the asm comment)
```

So "the stack" has two roles in this layout: the *active* stack in 2048..4095 is the one being walked, and the *seed* at 4096 is the C caller's input.

**Stack operations** in the asm:
- Push a new entry: `add esp, 32` (advance inward), then write 32 bytes at `[esp+2048..esp+2079]`. `ce` is updated to the new top.
- Pop/delete an entry: `sub esp, 32`, plus `deleteloop` shifts subsequent entries down to fill the gap.
- `beginsertloop` inserts in the middle (between current `esp` and `ce`) by shifting upward.

---

## 3. Register allocation (from the asm comment block, verified)

| GP reg | Role |
|---|---|
| `eax` | Temp 1 (reused constantly) |
| `ebx` | Temp 2 (often: output pointer into framebuffer during wall/ceiling/floor loops) |
| `ecx` | `z0` — current slab's top voxel-z for the `cx0/cy0` lane |
| `edx` | `z1` — current slab's bottom voxel-z for the `cx1/cy1` lane |
| `esi` | `ixy` — current voxel-column byte pointer into `_sptr` |
| `edi` | `v[]` — current slab pointer inside the voxel column |
| `ebp` | `bakj` — which of the two raycast lanes is leading (0 or 4, used as byte offset) |
| `esp` | `c` — CURRENT pseudo-stack pointer (!!) — not a real stack, just an index |

| MMX reg | Role |
|---|---|
| `mm0` | `[cy0 cx0]` — two int32, left-edge ray coords for the current `c` entry |
| `mm1` | `[cy1 cx1]` — right-edge ray coords |
| `mm2` | Temp (shuffle destination for pmaddwd operands) |
| `mm3` | Temp (`[gx 0 ogx -gy]` vector for dmulrethigh-style tests) |
| `mm4` | `csub` — current side-shading subtraction (from `_gcsub[ebp*8]`) |
| `mm5` | Pixel colour being built (3-cycle pipeline: punpcklbw → pmulhuw → packuswb) |
| `mm6` | `[gx 0 ogx 0]` — two 32-bit depth values, `ogx` = previous, `gx` = current |
| `mm7` | Temp (various) |

The `[gx 0 ogx 0]` layout in `mm6` is clever: the `pshufw mm6, mm6, 0x4e` instruction at `predrawceil`/`predrawflor`/`predeletez` swaps hi↔lo, making `gx ↔ ogx`. That lets the same code dispatch on whichever lane is active without branching.

---

## 4. Control-flow map (entry → exit)

```
_grouscanasm:                  [173]  prologue, save regs, init
  cfasm-stack seed load
  mm6 = depth-lane select
  esi = &_gpixy
  if (edi == *esi) jmp drawflor   else jmp drawceil

drawfwall:                     [220]  front wall (rising z range)
  loop0: per-pixel step outer
  loop1: per-pixel step inner, paints right-to-left
    on overshoot → predeletez
  endloop1
  fallthrough ↓

drawcwall:                     [264]  back wall (descending z range)
  if (edi == *esi) jmp predrawflor
  loop2: per-pixel step outer
  loop3: per-pixel step inner, paints left-to-right
    on overshoot → predeletez
  endloop3
  fallthrough ↓

predrawceil / drawceil:        [312]  ceiling (top of slab)
  swap mm6 hi/lo to expose gx
  drawceilloop: paint pixels from i0 rightward
    when filled → deletez
    when exhausted → drawflor
  fallthrough? no — jmp deletez

predrawflor / drawflor:        [347]  floor (bottom of slab)
  swap mm6 hi/lo
  drawflorloop: paint pixels from i1 leftward
    when filled → deletez
    when exhausted → enddrawflor
  fallthrough ↓

enddrawflor:                   [381]  save ebx=esp, fallthrough
afterdelete:                   [383]  pop 32 bytes off cfasm stack
  if (esp < _cfasm[2048])        → step to next voxel column
      mm4 = _gcsub[ebp*8]         (pick which side-shade)
      esi += _gixy[ebp*4]         (advance column)
      edi = *esi                   (new slab head)
      ebp = pick leading lane again
      if (eax > ngxmax)             → remiporend   (try next mip)
      else continue                 → skipixy2
  else (same column, sub-slab): skipixy

skipixy:                       [405]  swap mm6 hi/lo (make gx current again)
skipixy2:                      [407]  sync live (ecx,edx,mm0,mm1) with stack slot
skipixy3:                      [420]
  find highest intersecting slab    → findslabloop / intoslabloop
    if no slab intersects → drawfwall
    if NEXT slab also intersects → split the cfasm entry
      prebegsearchi16 / prebegsearchi / begsearchi:
        find the `col` (i0/i1 split point) to bisect at
      ce += 32 (reserve new top)
      if (ce > _cfasm[4096]) overflow → retsub (bail)
      beginsertloop: shift entries upward to make room
      write new split-entry fields
      esp += 32 (push to new top)
      jmp drawfwall

predeletez:                    [724]  swap mm6 hi/lo
deletez:                       [726]  remove current cfasm entry
  shift remaining entries down via deleteloop
  if stack empty → retsub
  else → afterdelete

remiporend:                    [546]  advance to next mip level
  if (gmipcnt+1 >= _gmipnum) → startsky
  adjust _gdz[0..4], _gpz[0..4] via saturating add
  halve _gixy[4]
  pick new gylookoff from gylut[gmipcnt]
  remask esi via gxmipk/gymipk/gamipk[gmipcnt]
  startremip0: halve (z0,z1,cx,cy) of every cfasm entry
  double ngxmax (capped at _gxmax)
  restore live (ecx,edx) from stack slot
  recompute mm6
  jmp skipixy2

startsky:                      [640]  fill remaining cfasm entries with sky
  if no sky loaded:
    fill with _skycast solid colour      (endprebegloop/endbegloop/endnextloop)
  else:
    for each remaining c:
      skysearch: find sky texel index by walking _skylat
      write pixel (+z) from _skyoff texture
  → retsub

retsub:                        [715]  emms, restore esp from espbak, pop regs, ret
```

---

## 5. Pixel-write patterns (what each draw loop actually produces)

### Wall fill (loop0/loop1 at drawfwall, loop2/loop3 at drawcwall)

Per iteration paints ONE pixel's worth of side-shaded slab colour.

```
eax = slab_offset (derived from z distance to slab edge)
mm5 = slab_voxel_rgba bytes unpacked to words   (punpcklbw mm5, [edi+eax*4])
mm5 = saturate_subtract(mm5, csub)              (psubusb mm5, mm4)
mm5 = shade_by_alpha(mm5, mm5.a replicated)     (pshufw mm2, mm5, 0xff; pmulhuw mm5, mm2; psrlw mm5, 7)
mm5 = pack_u8(mm5)                              (packuswb mm5, mm5)
USEZBUFFER: mm5 = [mm5_low_32 | ogx_32]         (punpckldq mm5, mm6)
write mm5 to framebuffer at ebx (8 bytes w/ zbuf, 4 without)
ebx ± 8 (or ± 4) — direction depends on whether front wall (−) or back wall (+)
advance mm0 or mm1 by _gi to keep [cy cx] in sync
```

Inner-loop exit test uses `pmaddwd mm7, mm3` + `test eax, eax` where `mm3 = [gx 0 ogx -gy]` — classic "inline dmulrethigh" without division: it's checking whether the next pixel still lies in the wall range.

### Ceiling fill (drawceilloop)
Identical structure to walls except:
- `csub = _gcsub[16]` (top-face shading).
- Paints from `c->i0` rightward, advances `mm0` (`[cy0 cx0]`) by `_gi`.
- `mm3` uses `gx` instead of `ogx` (hence the `pshufw mm6, mm6, 0x4e` swap on entry).

### Floor fill (drawflorloop)
Symmetric to ceiling but from `c->i1` leftward, advances `mm1` by `-_gi`.

### Sky fill
Without loaded sky texture: just `movntq mm5=_skycast` repeatedly between `[esp+2048]` and `[esp+4+2048]`.

With loaded sky: `skysearch` walks `_skylat` (latitude direction vectors) indexing into `edi=_skyxsiz`, pmaddwd-tests against `mm1=[cy1 cx1]`, writes the texel at `[_skyoff+edi*4]`.

---

## 6. The `pmaddwd mm7, mm3; test eax` idiom

Used at ~a dozen sites. Takes two `int16[4]` vectors, multiplies lanes pairwise, sums adjacent pairs into two int32s, stores result in `mm7`. Then `movd eax, mm7` takes the LOW int32 and tests its sign.

Given `mm3 = [gx 0 ogx -gy]` (as 4 int16s: `[-gy, ogx, 0, gx]` in memory order) and `mm7 = pshufw(mm?, 0xdd) = [cy cx cy cx]`, the low int32 of `pmaddwd` is:

```
(-gy * cx) + (ogx * cy) = cy*ogx - cx*gy
```

The sign of that expression is exactly `sgn(dmulrethigh(gy, cx, cy, ogx))` — i.e., whether the screen-y for a given voxel-z is above or below the ray's current screen-y. That's the core "is this slab visible right now" test.

So every `pmaddwd mm7, mm3; test eax` in `_grouscanasm` is *semantically* `if (sign_of(cy*ogx - cx*gy) ≷ 0)` — a cross-product sign test.

---

## 7. Precision notes

- `psubusb` / `packuswb` / `paddusb`: **saturated** unsigned byte arithmetic. Scalar C needs explicit `if (x > 255) x = 255; if (x < 0) x = 0;` per byte. (Or SSE2's `_mm_adds_epu8`/`_mm_subs_epu8` equivalents.)
- `pmulhuw`: **high 16 of unsigned 16x16=32** multiply. `uint16_t result = (uint32_t)(a * b) >> 16;`.
- `pmaddwd`: **signed 16x16=32 multiply, adjacent pairs summed**. For `a = [a0 a1 a2 a3]` and `b = [b0 b1 b2 b3]` (int16), `result = [a0*b0 + a1*b1, a2*b2 + a3*b3]` (int32).
- `movntq`: **non-temporal** 8-byte store — bypasses the cache. Semantically identical to a plain store for correctness; the only effect is cache pollution / prefetch pressure. Scalar C can use a plain store.
- `pshufw imm8`: byte-lane shuffle. `result[i] = src[imm8[2*i+1:2*i]]`. 0xDD = `[3,1,3,1]` (replicate hi lane pairs), 0xFF = `[3,3,3,3]`, 0xEE = `[2,3,2,3]`, 0x4E = `[1,0,3,2]` (swap 64-bit halves — the hi↔lo-in-mm6 trick).

---

## 8. Scalar-C port strategy for 4.5b

**Phase 1 — data-model replacement.** Replace the `esp`-as-pointer-into-`_cfasm` trick with an explicit array:
```c
typedef struct cfentry {
    int32_t *i0, *i1;
    int32_t z0, z1;
    int32_t cx0, cy0;
    int32_t cx1, cy1;
} cfentry;
static cfentry cfasm_c[32];   // enough slots
static int32_t cfasm_top;     // index of top (== ce/32 in the asm)
static int32_t cfasm_cur;     // index of current (== esp/32 in the asm)
```
`esp += 32` → `cfasm_cur++`. `ce += 32` → `cfasm_top++`. The shift loops in `deletez` and `beginsertloop` become `memmove`.

**Phase 2 — lane tracking.** Replace `mm0`/`mm1` with plain int32 pairs. The `pmaddwd mm7, mm3` cross-product tests become a single-line expression `if (cy * ogx - cx * gy > 0)`.

**Phase 3 — pixel-write inner loops.** Scalar byte arithmetic with explicit saturation. Start correctness-first; add SSE2 intrinsics later if perf matters.

**Phase 4 — mip transition and sky fill.** Straightforward translation; no tricky MMX here.

**Phase 5 — gate behind `#ifdef VOXLAP_SCALAR_GROUSCAN`** so the asm stays default while we verify equivalence.

---

## 9. Expected hash impact

Near-certain re-freeze of all four terrain goldens when the scalar C becomes default:
- `psrlw mm5, 7` (divide by 128) uses truncation; scalar C with `>> 7` matches, but `pmulhuw` of saturated bytes may round a half-LSB differently on the boundary.
- Different pixel-order within a scanline (the asm uses `movntq` which is semantically the same but the register pipeline may interleave stores with depth tests differently — scalar C executes in program order).

Plan to refreeze once scalar matches visually AND is deterministic across runs.
