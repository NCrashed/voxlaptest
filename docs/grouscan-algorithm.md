# grouscan — voxlap's per-scanline voxel rasterizer

This is a deep-dive into the algorithm at the heart of Ken Silverman's voxlap engine: `grouscanasm` (asm) / `grouscanasm_scalar` (C port). It explains *what the algorithm computes*, *why each design choice was made*, and *which invariants must be preserved* — written for someone who wants to port it (Rust, SSE2, NEON) or simply understand it.

There is essentially no upstream documentation for this routine. What follows was reconstructed by reading `voxasm/v5.asm`, building a scalar C port (`voxlap5.c grouscanasm_scalar`), and debugging the port via per-event trace diffs against the asm. Everything here is verified against the cleaned-up scalar port; cross-references to `voxlap5.c` and `voxasm/v5.asm` use the post-cleanup line numbers.

The companion document `voxasm/GROUSCANASM.md` covers the asm side specifically (register allocation, control-flow map, MMX intrinsic semantics). This document covers the *algorithm* — read this first if you're porting, then GROUSCANASM.md for asm-specific tricks.

---

## 1. What problem does grouscan solve?

`grouscan` renders **one horizontal scanline** of the framebuffer from voxel data. The full frame is rendered scanline-by-scanline by the caller (`opticast`), which builds a list of scan endpoints in world space and calls `gline` for each.

Each `gline` call:

- **Inputs**: scanline endpoints `(x0, y0)` and `(x1, y1)` in screen space (relative to the camera's principal axes), the pixel range to fill (`gscanptr` and `leng`), the voxel column head pointer `gstartv`, and depth-range bounds `gstartz0`, `gstartz1`.
- **Outputs**: a contiguous run of `leng` pixels in the framebuffer, each filled with the colour of whatever voxel is visible along its ray. With Z-buffer enabled, the depth is also written.

Voxlap's distinguishing property is **slab projection**: voxels are stored as variable-height vertical *runs* of solid blocks (slabs) per `(x, y)` ground-grid column, not as individual cubes. Each slab has a single byte-sequence of RGBA voxel colours top-to-bottom, with optional air gaps between slabs. This handles overhangs, caves, and thin shells naturally — and gives `grouscan` cheap visibility queries.

The non-obvious insight: instead of asking "what voxel does this pixel see?" (1 query per pixel × millions of pixels), `grouscan` asks "given a screen scanline projected to a 2D frustum in voxel-grid space, what *contiguous range* of pixels can this voxel column fill?" (1 query per voxel column × ~depth columns per scanline). This pivots the loop from per-pixel to per-voxel-column, fitting the data-flow to where the cache and SIMD wins are.

The algorithm walks 2D — the `(x, y)` ground plane — *not* 3D. The third dimension is collapsed via `gylookup[z]`, a per-camera-pose lookup table: for each voxel-z, `gylookup[z]` encodes "where on the screen does this voxel-z project to, at depth=1". Multiplying by depth gives the actual screen-y. This compression is what turns the per-pixel ray-cast into a per-column 2D scan.

---

## 2. The voxel data model

Voxels live in a `VSID × VSID` ground grid. Each `(x, y)` cell is a *column*. `sptr[y*VSID + x]` is a pointer to the column's **slab list** in the `vbuf` heap.

A slab list (the `vbuf` format, documented at voxlap5.c:73-89) is a sequence of slabs with a 4-byte header per slab:

```
+---------+--------+---------+---------+
| nextptr |   z1   |   z1c   |   z0    |   header (4 bytes, byte fields)
+---------+--------+---------+---------+
|    b    |   g    |    r    | intens  |   colour byte 1 (top voxel of slab)
+---------+--------+---------+---------+
|    b    |   g    |    r    | intens  |   colour byte 2
:                                      :
+---------+--------+---------+---------+
|    b    |   g    |    r    | intens  |   colour byte n (bottom voxel)
+---------+--------+---------+---------+
| nextptr |   z1   |   z1c   |   z0    |   next slab's header
:                                      :
```

- `nextptr` (`v[0]`): byte offset (after `<<2`) to the next slab. `0` means "no more slabs in this column".
- `z1`  (`v[1]`): voxel-z of the **floor** of this slab (top of the floor colour list).
- `z1c` (`v[2]`): voxel-z one past the bottom of this slab's voxel-colour list — used to compute slab size and as a separator.
- `z0`  (`v[3]`): voxel-z of the **ceiling** of this slab (bottom of the ceiling-colour list, i.e. the air gap *above* this slab).

There are **two distinct vertical extents per slab**: the slab's *air ceiling* `z0` (lowest voxel-z above the slab where there's air), the slab's *visible voxel run* `[z0..z1]` (this run's RGBA bytes), and the next slab's *visible top* `next_v[3]`. Air gaps are implicit in the gap between `z1` (floor of this slab) and `next_v[3]` (ceiling of next slab).

The ceiling/floor faces of a slab are visually rendered using the same RGBA bytes (with different side-shading); the *front* and *back* of a slab use the visible voxel-run colours.

`grouscan`'s job, for the column `*ixy_sptr_col`, is to walk this slab list top-to-bottom and figure out which slabs (and which voxel-z ranges within each slab) project onto pixels in the current scanline's pixel range.

---

## 3. The scanline frustum: setting up `gline`

Before each `grouscan` call, `gline` (voxlap5.c:1077+) projects the screen-space scanline to a 2D frustum in the voxel ground-plane:

```c
vd0 = x0*gistr.x + y0*gihei.x + gcorn[0].x;   // world-X of LEFT  ray edge
vd1 = x0*gistr.y + y0*gihei.y + gcorn[0].y;   // world-Y of LEFT  ray edge
vx1 = x1*gistr.x + y1*gihei.x + gcorn[0].x;   // world-X of RIGHT ray edge
vy1 = x1*gistr.y + y1*gihei.y + gcorn[0].y;   // world-Y of RIGHT ray edge
```

`gistr` and `gihei` are the camera's two principal screen-axes mapped to world space; `gcorn[0]` is the world-space scanline origin. After transformation, `(vd0, vd1)` is the LEFT ray's world `(X, Y)` direction, `(vx1, vy1)` is the RIGHT ray's.

The next block (voxlap5.c:1089-1103) projects these onto whichever ground-plane axis dominates (the axis along which the rays travel faster; this picks the axis to march voxel columns along), and computes:

- **`gixy[0]`, `gixy[1]`**: byte-step deltas for advancing one voxel column along X and Y (`±4` or `±4·VSID`).
- **`gpz[0]`, `gpz[1]`**: **distance to the next voxel-grid line** along X and Y from the current ray position. The lane with the smaller `gpz` is the "leading" lane — that's the axis whose voxel boundary comes up next.
- **`gdz[0]`, `gdz[1]`**: per-column-step delta to add to `gpz` after a column advance.

Then the `c[128]` seed of the cfasm linked list is set up:

```c
c->i0 = gscanptr;             // pixel-range left  edge (framebuffer pointer)
c->i1 = &gscanptr[leng];      // pixel-range right edge
c->z0 = gstartz0;             // voxel-z TOP of allowed range (clip)
c->z1 = gstartz1;             // voxel-z BOTTOM
c->cx0 = vd0 * CMPPREC;       // LEFT  ray X-coord (fixed point)
c->cy0 = vz0 * CMPPREC;       // LEFT  ray Y-coord
c->cx1 = vd1 * CMPPREC;       // RIGHT ray X-coord
c->cy1 = vz1 * CMPPREC;       // RIGHT ray Y-coord
gi0 = (vd1 - vd0) * cmprecip[leng];   // X-component, per-pixel step
gi1 = (vz1 - vz0) * cmprecip[leng];   // Y-component, per-pixel step
```

So:
- **`(cx0, cy0)`** is the world-space `(X, Y)` direction of the LEFT-edge ray of the cfasm entry's pixel range.
- **`(cx1, cy1)`** is the world-space direction of the RIGHT-edge ray.
- **`(gi0, gi1)`** is the per-pixel `(X, Y)` step. `cx0 + i*gi0 = X-direction of the i-th ray`. So `(cx, cy)` for any pixel in the range is `cx0 + (i - i0) * gi0`.

These four values together with `i0, i1, z0, z1` are the *frustum state* of one cfasm entry. They are the entry's **invariant**: at any point during the scan, `(cx0, cy0)` and `(cx1, cy1)` are the precomputed world-space directions for the ray-edges of the still-unfilled pixel range `[i0..i1]`.

`grouscan` is invoked from `gline`. From here on, the cfasm linked list is the central data structure.

---

## 4. The cfasm linked list

`cfasm` is an 8 KB byte buffer (`_cfasm db 256*32 dup(0)`), reinterpreted as 256 × 32-byte `cftype` entries:

```c
typedef struct {
    castdat *i0, *i1;       // pixel-range left/right framebuffer pointers
    int32_t  z0, z1;        // voxel-z range (inclusive of z0, exclusive of z1+1)
    int32_t  cx0, cy0;      // LEFT-edge ray (X, Y) world direction
    int32_t  cx1, cy1;      // RIGHT-edge ray (X, Y) world direction
} cftype;     // 32 bytes
```

The seed (the initial single entry that bootstraps the scan) is at `&cf[128]`. `gline` fills it in before calling `grouscan` (and the asm's prologue copies the seed's `z0`/`z1`/`cx0/cy0`/`cx1/cy1` into registers/MMX). The active entries live around `cf[128]`: a split grows the list to `cf[129]`, `cf[130]`, ... up to `cf[191]` (a hard cap of 64 active entries, after which the asm bails out via `retsub`); a column-step pop walks back toward `cf[128]`.

```
       ┌────────┐
cf[64] │  guard │  the 64 slots below cf[128] are reachable via the
       │  area  │  asm's `[esp+offset]` arithmetic but never written
  …    │   …    │  in normal operation — they're a safety margin
       │   …    │  against stray address calculations
cf[127]│        │
       ├────────┤
cf[128]│  SEED  │  ← `gline` writes here before calling grouscan
       │ + ...  │  ← active entries grow upward from here
cf[129]│ active │
  …    │ active │  
cf[191]│ active │  ← split insertion stops here (`if ce >= cf[191] retsub`)
       └────────┘
```

Initial state: only `cf[128]` has valid content. `ce` ("cfasm end") points at `&cf[128]`. `c` (the "current" entry being processed) starts as `cf[128]` too.

**Invariants** of the cfasm linked list at any point during the scan:

1. The list is a contiguous slab `cf[128] .. cf[ce_idx]` (`ce` is the topmost active entry, `cf[128]` is always the bottom).
2. Every active entry's pixel range `[i0..i1]` is **non-empty** (`i0 ≤ i1`).
3. The pixel ranges of distinct entries are **disjoint** and together they tile some subset of the original `[gscanptr..gscanptr+leng]`.
4. Every entry's frustum state `(cx0, cy0, cx1, cy1)` is consistent with its own `[i0..i1]`: the rays at `i0` and `i1` are exactly `cx0+i*gi0 + cy0+i*gi1` for `i = (i0 - gscanptr)` and `(i1 - gscanptr)` respectively. (Modulo the cross-call accumulator semantics — see §6.)
5. `c <= ce`. `c` walks down to `cf[128]` and back up; `ce` only grows or shrinks (never crosses).

The list **grows** when the algorithm hits a *split* — a column where two different slabs are simultaneously visible across different sub-ranges of the entry's pixel range. The list **shrinks** when an entry's pixel range gets fully filled (`deletez`).

Why a linked list? Because as the scan walks deeper into the world, what *was* a single pixel range may need to fork into two: a near slab in front of part of the range, a far slab visible through the rest. Tracking those two regions independently — each with its own visible-z range and frustum-state copies — is exactly what a split inserts.

---

## 5. The cross-product test — the algorithm's primitive

This is *the* central operation. It runs millions of times per frame. Every visibility decision (does this voxel-z still project into the pixel-range? is this slab in front of the next one? does the ray edge cross the next voxel-grid line?) reduces to one cross-product sign test.

The asm version (v5.asm uses `pmaddwd`):

```asm
mov  eax, gylookoff
movd mm3, [eax + z*4]            ; mm3.int16[0] = gylookup[z]_lo16,  rest 0
por  mm3, mm6                    ; mm3 |= mm6; mm6 holds depth values
                                 ; mm3.int16[1] = depth>>16
                                 ; mm3.int16[2] = 0  (or other depth)
                                 ; mm3.int16[3] = depth>>16 in other slot
pshufw mm7, mm0, 0DDh            ; mm7 = [cy_hi, cx_hi, cy_hi, cx_hi]
pmaddwd mm7, mm3                 ; mm7.int32[0] = cx_hi*gy_lo + cy_hi*depth_hi
movd eax, mm7
test eax, eax                    ; sign of the test
```

The C port (voxlap5.c:12498-12507):

```c
static inline int32_t grouscan_cross_sign (int32_t cx, int32_t cy,
                                            int32_t depth, int32_t gy_raw)
{
    int32_t gy_s16    = (int32_t)(int16_t)gy_raw;        // sign-ext low 16
    int32_t depth_s16 = (int32_t)(int16_t)(depth >> 16); // sign-ext hi 16
    int32_t cx_s16    = (int32_t)(int16_t)(cx >> 16);
    int32_t cy_s16    = (int32_t)(int16_t)(cy >> 16);
    return cx_s16 * gy_s16 + cy_s16 * depth_s16;
}
```

The expression is `cx · gy + cy · depth` with all four operands **sign-extended from 16 bits**. The 16-bit operands are essential — that's what fits the `pmaddwd` instruction (signed 16×16 → 32 multiply-add) and what `gylookup` is populated to match (see §11).

**Geometric meaning.** At one specific call site, `(cx, cy)` is one of the two ray-edges' world-direction; `depth` is `ogx` or `gx` (the depth value of the current voxel-column boundary, fixed-point); `gy_raw = gylookup[z]` encodes the camera-z-relative depth of voxel-z `z`. Note `gy_raw` *can* be negative — for voxels deeper than the camera, `gposz - z*PREC` is negative and the int16-truncation preserves the sign.

The expression `cx · gy + cy · depth` is, up to sign, the **2D cross product** of two vectors:

```
   (cx, cy)          and          (-gy, depth)
   = ray-edge direction           = "voxel-point" at (column-depth, voxel-z),
                                     measured in the fixed-point space
                                     gylookup is populated for
```

(Equivalently: it's the determinant of the 2×2 matrix `[[cx, -gy], [cy, depth]]`. The sign of `cx·gy + cy·depth` is the negative of that determinant — same magnitude, just a sign convention.)

The sign of this expression is the **orientation test**: is the voxel-point on one side of the ray-edge or the other? Exactly the test you need to decide:

- whether voxel-z `z` projects above or below the current pixel's screen-y;
- whether the next pixel's ray still hits voxel `z`;
- whether the next slab in this column is occluded by the current one.

Same primitive, different operand bindings at each call site. Roughly a dozen call sites in `grouscanasm`, all `cross_sign(cx0_or_cx1, cy0_or_cy1, ogx_or_gx, gylookup[z])`.

**Watch the comparison sense.** The pre-Stage-4.5b gline path used a different cross-product formula (`dmulrethigh(gy, cx, cy, depth) = (gy*cx - cy*depth) >> 32`) which is the *negative* of `pmaddwd`'s value. So `dmulrethigh < 0` corresponds to `pmaddwd > 0` — the exit conditions of every drawing loop are flipped accordingly (`jle` ↔ `>= 0`, `jg` ↔ `< 0`, etc). When porting from the gline fallback to the grouscanasm form, the sign-flip is the easiest thing to get wrong. The scalar port reproduces the asm's sense literally — see voxlap5.c:12490-12497 (cross_sign comment).

**Why int16, not int32?** Because `pmaddwd` is two int16×int16 → int32 multiplies summed in a single ~3-cycle MMX instruction. An int32 cross product would need full 64-bit precision (one mul each, then subtract, then sign extract — ~4× slower). The trade-off: voxels and rays beyond a certain distance lose the lower bits of precision, but the ranges are clamped (`gxmax`, `gxmip`) such that the int16 representation has enough headroom for the distances we actually care about.

---

## 6. The column step — moving through the voxel grid

The outer scan walks **voxel columns** along the ground plane, stepping from one column to the next-closest in either the X or Y direction (whichever boundary is closer along the ray bundle). This happens at `afterdelete` / column-step (voxlap5.c:13082-13144 in the scalar; v5.asm:383+ in the asm).

State maintained per-step:

- `gpz[2]`: distance to the next X-grid line and Y-grid line, in fixed-point depth units. Each column-step advances along whichever lane has the smaller `gpz`, and adds `gdz[lane]` to `gpz[lane]` (the next boundary along that lane).
- `lane = (gpz[1] < gpz[0]) ? 1 : 0`: which axis we're stepping along this iteration.
- `gx`: depth at the **new** voxel column = `gpz[lane] & 0xFFFF0000`. (The mask is because the int16 cross-product test uses only the high 16 bits of the depth value.)
- `ogx`: depth at the **old** voxel column (the one we just left). Carried forward across iterations as the second slot of `mm6`.
- `wall_lane`: the lane that drove the **previous** column-step (cached because `gcsub[wall_lane]` is the side-shade colour subtraction used by the *previous* slab's drawfwall/drawcwall fills — see §11). Set just before the lane gets overwritten.
- `ixy_sptr_col`: the `sptr` cell pointer for the current column, advanced by `gixy[lane]`.
- `v`: pointer into the new column's slab list, reset to `*ixy_sptr_col` at every column-step.

After the column-step, control hits `skipixy3` and decides what to do for the new column:

- `if (v[0] == 0)` → no more slabs in this column → `goto drawfwall` (fill the rest of the pixel range with the front face of whatever's visible).
- Otherwise → `goto intoslabloop` (walk the slab list to find the topmost intersecting slab — see §7).

`mm6` carries `[ogx_lo, gx_hi]` across iterations. The `pshufw mm6, mm6, 0x4Eh` swap (at `predrawceil`, `predrawflor`, `predeletez`, `skipixy`) flips the two halves so the test code can use the `pmaddwd` operand layout against either depth without branching.

---

## 7. Slab traversal & the split

`intoslabloop` (voxlap5.c:13107-13157) walks the slab list of the current column, looking for the topmost slab whose top voxel `v[2]+1` projects within the current pixel range's frustum. There are two cross-product tests:

```c
intoslabloop:
    test_hi   = cross_sign(cx0, cy0, ogx, gylookup[v[2]+1]);   // test 1
    if (test_hi > 0) goto findslabloop;       // current slab too high — try next
    test_next = cross_sign(cx1, cy1, ogx, gylookup[next_v3]);  // test 2
    if (test_next <= 0) goto drawfwall;       // single-slab case
    // else → split path
```

**Test 1**: tests the LEFT ray-edge against the top of the *current* slab. If the slab's top is "above" the LEFT ray (test > 0), the current slab is entirely above the frustum → skip to the next slab via `findslabloop` (which advances `v += v[0]*4`). Otherwise the current slab intersects the frustum at the LEFT ray-edge.

**Test 2**: tests the RIGHT ray-edge against the top of the *next* slab below. If the next slab's top is at or below the RIGHT ray (test ≤ 0), only the current slab is visible across the entire pixel range → `goto drawfwall` to fill from the current slab. Otherwise *both* the current slab and the next slab are visible across different sub-ranges → split.

**The split** (voxlap5.c:13157-13219):

```c
// Pre-search sync: write current LOCAL state to memory so c->cx0/cy0/cx1/cy1
// reflect the pre-search frustum state (the search is about to mutate locals).
c->z0 = z0; c->z1 = z1;
c->cx0 = cx0; c->cy0 = cy0;
c->cx1 = cx1; c->cy1 = cy1;

// Find the pixel column `col` where the RIGHT ray transitions from
// "next slab visible" to "current slab fully covers". Walk col leftward,
// decreasing the LOCAL cx1/cy1 (right-edge ray) by one per-pixel step
// each iteration, until the cross-product flips.
castdat *col = c->i1;
for (;;) {
    int32_t t = cross_sign(cx1, cy1, ogx, gy_raw);    // gy_raw = gylookup[v[2]+1]
    if (t <= 0) break;
    cx1 -= gi0; cy1 -= gi1;
    col--;
}

// Insert a new entry by shifting cf[c+1..ce] up by one slot.
ce++;
for (cftype *p = ce; p > c; p--) *p = *(p - 1);

// After the shift, c[1] is a clone of c[0]. Two writes are then committed:
//   • c[1].i1 = col       — narrows the new (cfasm-top) entry's range to [orig_i0..col]
//   • c[0].{i0,z0,cx0,cy0} are narrowed for the right-portion [col+1..orig_i1]
// c[0]'s z1, cx1, cy1, i1 stay at the pre-split values (via the pre-search sync above
// for cx1/cy1 — locals had been mutated by the search; the sync had captured the
// correct pre-search values).
c[1].i1 = col;                        // NEW entry's pixel range: [orig_i0 .. col]
c->i0 = col + 1;                      // OLDER entry's pixel range: [col+1 .. orig_i1]
c->z0 = next_v3;                      // OLDER entry's visible top = next slab's ceiling
c->cx0 = cx1 + gi0;                   // OLDER entry's LEFT ray = post-search-end + one step
c->cy0 = cy1 + gi1;

// Advance into the new entry and start drawing.
c++;
z0 = c->z0;                           // = clone of orig_z0
z1 = next_v3;
goto drawfwall;                       // local cx1/cy1 still hold the search-end values
                                       // — exactly what drawfwall wants for its right-edge walk
```

After a split, the linked list has **two** entries. **The new entry (`c`, just incremented from cf[128] to cf[129]) covers the LEFT portion** of the original pixel range and is drawn first. Its drawfwall fills the front face of the *current* slab — the slab whose top intersected the left ray at the original test. **The older entry (now `c-1`, still at cf[128]) covers the RIGHT portion** and will be drawn later: when the new entry's pixel range is filled (`enddrawflor → afterdelete → c--`), control drops back to the older entry, which has `z0 = next_v3` so its drawing begins at the *next* slab. The cross-product transition that the search loop located is the geometric boundary where the LEFT ray "sees" slab A but the RIGHT ray "sees" past slab A into slab B.

The split is the only way the linked list grows. After a split, both halves continue independently; a later split inside one of them grows the list further (rare, but supported up to `cf[191]` — 64 active entries).

---

## 8. The four drawing labels

Once the algorithm has decided "this slab is what fills (some of) my pixel range", it dispatches to one of four drawing labels. Each fills a distinct sub-range of the pixel range using a different colour source and a different cross-product exit test.

```
                       z increases (deeper into world)
                       │
         (sky/clip top)│  ← z = z0 (in the current cfasm entry)
                       │
                       │   ┌─── ceiling colour: voxel ABOVE the slab top
                       │   │       (= the previous slab's last voxel)
        slab top z0  ──┼───┤  ← drawceil fills here
                       │xxx│
                       │xxx│   front-wall colour: this slab's voxel-run RGB
                       │xxx│   bytes, indexed by current voxel-z within slab
                       │xxx│  ← drawfwall fills here (left-side face)
        slab bot z1  ──┼───┤
                       │   │   floor colour: voxel BELOW the slab bottom
                       │   │       (= [v + 4], i.e. first byte of next slab)
                       │   │  ← drawflor fills here
                       │
                       │  ← z = z1
                       │
                       (sky/clip bottom)
```

(Actually `drawcwall` fills the **back** of the slab — the visible interior face on the far side of the column — using the same RGB byte stream as `drawfwall` but advancing right-to-left. But the analogy holds: each label is one of "ceiling void / front wall / back wall / floor void".)

| Label | Colour source | Fill direction | Loop exit when test... |
|---|---|---|---|
| `drawfwall` | `v[off*4]` for voxel-z `z1` of slab (the "front face" voxel) | RIGHT → LEFT (`ebx--`, `cx1 -= gi0`) | ≤ 0 → `endloop1` |
| `drawcwall` | `v[off*4]` for voxel-z `z0` of slab (the "back face" voxel) | LEFT → RIGHT (`ebx++`, `cx0 += gi0`) | > 0 → `endloop3` |
| `drawceil` | `v - 4` (the previous slab's last colour byte) | LEFT → RIGHT (`c->i0++`, `cx0 += gi0`) | > 0 → `drawflor` |
| `drawflor` | `v + 4` (first colour byte of this slab) | RIGHT → LEFT (`c->i1--`, `cx1 -= gi0`) | ≤ 0 → `enddrawflor` |

The fall-through chain when no test forces a jump is `drawfwall → drawcwall → predrawceil → drawceil → predrawflor → drawflor → enddrawflor → afterdelete`. The pre-* labels swap `mm6`'s halves so the cross-product test uses `gx` (current depth) instead of `ogx` (previous depth) — this is the geometric reason for the swap: ceiling/floor voids are evaluated at the *new* column's depth, while front/back walls use the *current* slab's depth (which is where the slab actually sits, between the two column boundaries).

Each draw loop's body, after the cross-product test passes, does:

1. Compute the colour byte address (different formula per label, see table above).
2. Run the **shade pipeline** on the voxel RGBA bytes (see §11).
3. Store the resulting 32-bit colour into the framebuffer at `ebx` (or `c->i0` / `c->i1`).
4. Advance the pixel pointer one slot (`±4` bytes without zbuf, `±8` with — the second 4 bytes hold `ogx` as depth).
5. Advance the cross-product accumulator `(cx, cy)` by `±gi`.
6. Loop back to the test.

If the pixel pointer crosses the entry's other edge (`ebx < c->i0` or `ebx > c->i1`), the entry's pixel range is fully filled → `goto predeletez` (drawfwall/drawcwall) or `goto deletez` (drawceil/drawflor), which pops the entry from the linked list.

---

## 9. `deletez` and `afterdelete` — popping entries

`deletez` (voxlap5.c:13230-13266) pops the topmost cfasm entry when an entry's pixel range is exhausted. If `c` is below `ce` (we're processing an interior entry, not the top), the entries above must be shifted down to close the gap:

```c
deletez:
    if (ce <= &cf[128]) goto retsub;        // empty stack — done
    cftype *old_ce = ce;
    ce--;
    if (c < old_ce) {
        // Shift cf[c+1..old_ce] down to cf[c..old_ce-1].
        // After this, cf[c]'s memory holds what used to be at cf[c+1].
        for (cftype *p = c; p < old_ce; p++) *p = *(p + 1);
        c_presync = old_ce;          // see below
        goto afterdelete_kept_presync;
    }
    goto afterdelete;
```

`afterdelete` (voxlap5.c:13063-13144) handles the post-fill "what next" decision:

```c
afterdelete:
    c_presync = c;
afterdelete_kept_presync:
    c--;
    if (c >= &cf[128]) goto skipixy_with_presync;     // still in cfasm region
                                                       // → continue with the entry
                                                       //   below the one we popped

    // Otherwise: column step.
    wall_lane = lane;
    ixy_sptr_col = ... + gixy[lane];
    v = *ixy_sptr_col;
    lane = (gpz[1] < gpz[0]) ? 1 : 0;
    gx = gpz[lane] & 0xFFFF0000;
    if ((uint32_t)gpz[lane] > (uint32_t)ngxmax) goto remiporend;   // out of range — try next mip
    gpz[lane] += gdz[lane];
    c = ce;                                            // top of stack (post-pop)
    if (c_presync == c) goto skipixy3;                 // skip sync — locals are correct
    goto skipixy2_sync_from_presync;                   // sync from c's memory

skipixy2_sync_from_presync:
    c_presync->{z0,z1,cx0,cy0,cx1,cy1} = {z0,z1,cx0,cy0,cx1,cy1};   // save to old slot
    z0 = c->z0; z1 = c->z1;                                           // load from new slot
    cx0 = c->cx0; cy0 = c->cy0;
    cx1 = c->cx1; cy1 = c->cy1;
    /* fall through to skipixy3 */
```

The **`c_presync = old_ce` trick** in `deletez` is the subtle bit (and the source of one rendering bug — Stage 4.5b.8c). Setting `c_presync = old_ce` (the just-freed slot, one past the current top) means the post-column-step `c_presync == c` test fails — both sides aren't equal because `old_ce > new_ce` — so the sync runs and locals get reloaded from `cf[c]`'s memory.

Why does that matter? Because `deletez`'s shift overwrote `cf[c]`'s memory with what used to live at `cf[c+1]` (the freed slot's old data). The *registers* still hold the pre-deletez (now-stale) frustum state. Without the sync, the next column's draw would use those stale `cx/cy` values — and the visible artifact is exactly the "everything behind multi-slab terrain is garbaged" symptom. The asm gets this right via `ebx = old_ce` inside `deletez` (v5.asm `deletez:`); the C port had to mirror that semantics explicitly via `c_presync = old_ce`.

**Lesson for porters**: any port must preserve this invariant — *after a deletez that shifted, locals MUST be reloaded from the post-shift `cf[c]` memory*. Don't let your sync-skip optimization fire on this path.

---

## 10. The mip transition — distance-based LOD

When a column-step's new `gpz[lane]` exceeds `ngxmax`, the algorithm transitions to a coarser mip level (voxlap5.c:13230-13290, `remiporend`). At each mip step:

- `gmipcnt` (current mip level) increments.
- `gdz[0]` and `gdz[1]` double (with saturation at `0x7fffffff`) — column steps now cover twice the distance.
- `gixy[1]` halves — Y-step is coarser.
- `ixy_sptr_col` is re-pointed to the coarser mip's `sptr`-equivalent (via `grouscan_gxmipk[]` / `grouscan_gymipk[]` masks and `grouscan_gamipk_offsets[]`).
- `gylookoff` (the active `gylookup` slice) advances to the next mip's table (via `grouscan_gylut_offsets[]`).
- **All active cfasm entries** have their `z0`/`z1` halved (they're now indexing into the coarser z-grid).
- `ngxmax` doubles, capped at `gxmax`.

After remiporend, the algorithm re-enters `skipixy2` and resumes the scan with the coarser data. There are up to 10 mip levels (`grouscan_gxmipk[]` has 10 entries). When all are exhausted, `goto startsky` fills the remaining pixel ranges with sky.

Why mips? Because at extreme distance, individual voxels project to less than one pixel — sampling them all is wasteful and shimmery. Mip-N stores 2^N-voxel-cubes pre-merged. The transition keeps the geometric meaning of each cfasm entry intact (the rays don't change; only the voxel grid resolution does).

---

## 11. Colour shading — the mm5 byte carry

The voxel colour pipeline (per pixel):

```asm
punpcklbw mm5, [edi + offset]    ; interleave bytes of mm5_low4 and voxel RGBA
psubusb   mm5, mm4               ; saturated unsigned subtract per byte (side-shade)
pshufw    mm2, mm5, 0FFh         ; replicate the high byte (intensity) to all
pmulhuw   mm5, mm2               ; per-word: mm5 = (mm5 * mm2_hi16) >> 16
psrlw     mm5, 7                 ; shift right 7 — divides by 128
packuswb  mm5, mm5               ; saturate-pack 4 words back to 4 bytes (twice)
```

C port (voxlap5.c `grouscan_shade`, ~line 12500+).

The pipeline does:

1. **Unpack** voxel RGBA bytes interleaved with the previous pixel's `mm5` byte tail.
2. **Saturated subtract** a per-face "shade" colour: `gcsub[wall_lane]` for front/back walls (depends on which lane drove the column step), `gcsub[2]` for ceiling, `gcsub[3]` for floor. Side-shading: faces that point toward the camera-light direction get less subtracted; faces away get more. This is what makes voxel cubes look 3D.
3. **Modulate** by the voxel's intensity byte (the 4th RGBA byte) via `pmulhuw` — multiply each colour channel by `(intensity*256+something)>>16`. Effectively `colour *= intensity / 256`.
4. **Saturate-pack** to 8-bit RGBA.

The `mm5` byte carry is a subtle perf trick: `punpcklbw mm5, [vox]` interleaves the LOW 4 bytes of `mm5` with the 4 voxel bytes. Those low 4 bytes of `mm5` are the *previous* pixel's packed-back result (since `packuswb mm5, mm5` duplicates the result into both halves). Effect: each pixel's pipeline has one byte of "carry" from the previous one, exploited as a tiny temporal blur during the unpack. In the scalar port, `mm5_tail` is a `uint32_t` updated by `grouscan_shade`'s out-parameter to mirror this.

For ports targeting modern hardware: this is direct SSE2 (`_mm_unpacklo_epi8`, `_mm_subs_epu8`, `_mm_mulhi_epu16`, `_mm_srli_epi16`, `_mm_packus_epi16`) — same instructions, 128-bit registers. The carry trick is preserved by passing the `mm5_tail` as a state variable across calls.

---

## 12. The asm ESP-as-pointer trick

Internal to `grouscanasm`, ESP is **not the OS stack pointer**. The function's prologue saves real ESP to `espbak` and sets ESP to `&_cfasm[2048]`. From then on, ESP serves as `c` (the current entry pointer): when ESP holds the address of byte `2048 + 32k` of `cfasm`, the current entry is `cf[128 + k]`, accessed via the displacement-2048 addressing mode:

- `[esp + 2048]`     = byte 0 of current entry (`c->i0`)
- `[esp + 4 + 2048]` = byte 4 (`c->i1`)
- `[esp + 8 + 2048]` = byte 8 (`c->z0`). Etc.
- `add esp, 32` ↔ `c++` (advance up: cf[128]→cf[129])
- `sub esp, 32` ↔ `c--` (retreat down: cf[128]→cf[127])

The `+ 2048` displacement turns out to be exactly what's needed so that ESP lives in `cfasm[2048..4096]` (a region addressable but never written) while the actual entries live in `cfasm[4096..]` (the cfasm slot region accessed via `[esp+2048]`).

**Why the trick?** Free up one register (no separate `c` pointer needed), and `[esp + offset]` addressing is one byte shorter than `[ebx + offset]` for some offsets — saving a few percent of code size. Cosmetic, not performance-defining. **Don't replicate this in any port.** Use a normal `cftype *c` pointer; the perf cost is negligible (~1-2% on modern CPUs) and the clarity gain is enormous.

The trick has one nasty consequence: any code path inside `grouscanasm` that calls a C function (or uses the OS stack at all) must first save the cfasm-ESP, switch ESP back to `espbak`, do the call, then restore. The H8b trace harness used exactly this pattern. Avoid it in pure-portable ports by never calling out from the inner loop.

---

## 13. Notes for porters

### Algorithm correctness
- Match the int16 cross-product semantics literally. `dmulrethigh`-style (full int32) gives a different sign on far columns. The flip from `dmulrethigh < 0` to `pmaddwd ≤ 0` is non-trivial — voxlap5.c:12490-12497 documents the exit-sense per loop.
- Populate `gylookup` in the int16 format: `gylookup[i+u] = (((gposz>>j) - i*PREC) >> (16-j)) & 0xFFFF`. The C-format `(i*PREC - gposz)` only matches `dmulrethigh`-style; with `pmaddwd`-style cross product it produces wrong results.
- The `c_presync = old_ce` invariant in `deletez` (voxlap5.c grouscanasm_scalar, around the deletez block) MUST be preserved. Deletez+shift makes locals stale; the post-column-step sync MUST reload from cf[c]'s memory on this path. Asm gets this via `ebx = old_ce`; portable ports need an explicit equivalent.
- `wall_lane` must be captured **before** the column-step recomputes `lane`. The mm4-cached side-shade is keyed on the *previous* lane.
- `drawcwall` sets `z1 = v[1]` unconditionally on entry — even on the `drawfwall → jge drawcwall` early-exit path. Skipping this leaves z1 stale and mis-projects the floor.
- The split path's pre-search sync writes ALL six fields (z0, z1, cx0, cy0, cx1, cy1) to `c->memory` before the search loop modifies locals. Don't reduce this to a partial sync — H7 falsified that simplification.

### Performance
- The cross-product test runs once per inner-loop iteration. SSE2's `_mm_madd_epi16` is the direct equivalent of `pmaddwd` and runs in 4–5 cycles on modern x86-64.
- The shade pipeline (`punpcklbw → psubusb → pmulhuw → psrlw → packuswb`) maps 1:1 to SSE2 intrinsics. Modern CPUs have wider SSE2 ports than the original P6's MMX ports, so the SSE2 port should *match or slightly beat* the MMX asm.
- `movntq` (non-temporal store) bypasses the cache. Use `_mm_stream_si64` (or `_mm_stream_si128` for batched stores). Without it, per-pixel writes evict the framebuffer cache lines that the next scanline would re-touch — measurable on some CPUs.
- The cfasm linked list is small (max 64 entries × 32 bytes = 2 KB) and stays in L1. Don't worry about its layout.
- The slab data (`vbuf`) is the main memory-bandwidth consumer. Per-column, the algorithm reads ~1 cache-line per slab traversed. Scenes with deep, complex columns are bandwidth-bound; scenes with shallow columns are compute-bound on the cross-product test.
- The 16-step "big jump" loop (`prebegsearchi16` in v5.asm) accelerates the split-search by stepping 16 pixels at a time before refining. Worth porting if scenes have many wide splits.

### Rust port
- Use `core::arch::x86_64::*` for SSE2 intrinsics; gate on `cfg(target_feature = "sse2")` (always-on for x86_64 by default).
- The cfasm pointer is best modeled as `usize` index into a `[Cftype; 256]` array. No need for raw pointers.
- The voxel column traversal needs `*const u8` arithmetic (slab list is a byte stream with byte-offset `nextptr`). Wrap in an iterator: `unsafe fn next_slab(v: *const u8) -> Option<*const u8>`. Contained `unsafe` is fine.
- Globals (`gpz`, `gdz`, `gylookup`, `sptr`, `vbuf`) become a `RasterContext` struct passed by `&mut`. Avoid `static mut`.
- For bit-exact match against the asm: not realistic (x87 vs SSE float rounding in `gline` setup will drift). Aim for visual parity verified by the oracle hash suite, with new goldens for any scene where rounding diverges.

### NEON / WebAssembly SIMD
- `pmaddwd` → NEON `vmlal_s16` (long multiply-accumulate) or WASM `i32x4.dot_i16x8_s`. Same semantics, different intrinsic.
- The shade pipeline maps to NEON's saturated-byte ops (`vqsub_u8`, `vmull_u8`, `vshrn_n_u16`, `vqmovn_u16`).
- `movntq` has no direct NEON equivalent; use a regular store. Negligible perf delta on ARM, which has different cache behavior anyway.

---

## 14. Glossary

| Term | Meaning |
|---|---|
| **scanline** | One horizontal row of the output framebuffer |
| **cfasm** | The linked list of pixel-range × frustum-state entries that grouscan walks |
| **column** (voxel column) | One `(x, y)` cell of the ground-plane voxel grid; each owns a slab list |
| **slab** | A vertical run of solid voxels with a contiguous RGBA colour stream |
| **slab list** (vbuf) | The linked list of slabs for one voxel column |
| **frustum state** | The four `(cx, cy)` accumulators that encode the LEFT and RIGHT ray-edges of a cfasm entry |
| **lane** | Either the X-axis or Y-axis the algorithm steps along; determined by which voxel-grid line is closer ahead |
| **wall_lane** | The lane that drove the *previous* column-step (used for side-shading the *current* slab's wall fills) |
| **gylookup** | Per-mip lookup table from voxel-z to a 16-bit fixed-point depth proxy |
| **mip** | LOD level; mip-N has voxels merged in 2^N × 2^N × 2^N cubes |
| **split** | The point where one cfasm entry forks into two because two slabs in the same column are simultaneously visible |
| **deletez** | The point where an entry's pixel range is fully filled and the entry is popped from the linked list |
| **`pmaddwd`** | MMX/SSE2 instruction: signed 16×16 → 32 multiply-add, two pairs summed in parallel |
| **side-shade** | Per-face colour subtraction (`gcsub`) that makes voxel cubes look 3D by darkening edge-on faces |
| **mm5 byte carry** | The previous pixel's packed result staying in the low 4 bytes of `mm5` across calls — exploited by `punpcklbw` for one-pixel temporal blur |

---

## 15. References

- `voxlap/voxlap5.c grouscanasm_scalar` (the cleaned-up scalar port; treat as the algorithmic reference)
- `voxasm/v5.asm` `_grouscanasm` (the asm; treat as the perf reference)
- `voxasm/GROUSCANASM.md` (asm-side companion: register allocation, control-flow map, MMX semantics)
- `voxlap/voxlap5.c gline` ~line 1077 (frustum setup, `gline → grouscan` dispatch)
- `voxlap/voxlap5.c` lines 73-89 (vbuf format ASCII diagram)
- Stage 4.5b.8b/c trace + fix history (the H8b hooks and the deletez sync bug — the bug-as-teacher case study for §9)
