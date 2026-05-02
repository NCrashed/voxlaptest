/*
 * CD.2.6 byte-fixture harness.
 *
 * Generates a deterministic pre/post .vxl pair around a single
 * setspans carve so the roxlap port can validate scum2 + delslab +
 * compilerle byte-equality against voxlap C.
 *
 * Invocation:
 *   edit_fixture <pre.vxl> <post.vxl>
 *
 * The harness:
 *   1. Calls loadnul to populate the default 2048×2048 world.
 *   2. Saves the pre-edit state to argv[1].
 *   3. Runs ONE setspans carve over a known 5×5 column patch
 *      centred at (1024, 1024) at z=100..150.
 *   4. Saves the post-edit state to argv[2].
 *
 * The roxlap-side test reads both .vxls, identifies the affected
 * columns, replays the same carve via ScumCtx + delslab, and
 * asserts byte-equality on each touched column.
 *
 * Fixture coords picked so the carved box is comfortably inside
 * the world (no boundary-clamping logic exercised) and small
 * enough that the post .vxl gzips to a few KB.
 */

#include "voxlap5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic colour callback used for the fixture so the roxlap
 * port can mirror it exactly without re-implementing voxlap's
 * stateful jitcolfunc (gkrand). curcolfunc returns vx5.curcol
 * verbatim — set via set_curcol below. Symbol is in voxlap5.c at
 * global scope (used by oracle.c the same way). */
extern int32_t curcolfunc(lpoint3d *);

extern void set_curcol(int32_t v);
extern void set_colfunc(int32_t (*v)(lpoint3d *));

/* Fixture's deterministic colour. Picked to be visually obvious in
 * any debug dump and to have all 4 bytes distinct. Voxlap stores
 * colours as 0xAARRGGBB with the alpha byte being brightness. */
#define FIXTURE_COL ((int32_t)0x80aabbccu)

/* Centre of the 2048×2048 world. The 5×5 patch sits inside loadnul's
 * 180×180 central air-carve (so the columns have a non-trivial 2-slab
 * shape: solid [0..83) + air [83..173) + solid [173..MAXZDIM) ).
 * The carve targets the LOWER solid region (z=200..230) so it
 * actually mutates voxel state — picking a z range inside the
 * existing air carve would no-op. */
#define BASE_X 1024
#define BASE_Y 1024
#define PATCH_W 5  /* columns in x direction */
#define PATCH_H 5  /* columns in y direction */
#define CARVE_Z0 200
#define CARVE_Z1 229  /* inclusive — voxlap stores z1 inclusive in vspans */

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pre.vxl> <post.vxl>\n", argv[0]);
        return 2;
    }

    if (initvoxlap() < 0) {
        fprintf(stderr, "initvoxlap failed\n");
        return 1;
    }

    dpoint3d ipo, ist, ihe, ifo;
    loadnul(&ipo, &ist, &ihe, &ifo);

    /* Override loadnul's stateful jitcolfunc with a deterministic
     * colour callback. compilerle invokes the colfunc for newly-
     * exposed voxels created by the carve (voxels just above/below
     * the deleted span that weren't in the original color list).
     * Using curcolfunc + a fixed curcol means the post-edit bytes
     * are reproducible across runs and trivial to mirror in Rust. */
    set_curcol(FIXTURE_COL);
    set_colfunc(curcolfunc);

    /* Save pre-edit state. */
    if (!savevxl(argv[1], &ipo, &ist, &ihe, &ifo)) {
        fprintf(stderr, "savevxl(%s) failed\n", argv[1]);
        return 1;
    }

    /* Build a vspans list for a 5×5 column patch. vspans.x/.y are
     * char-sized (≤255 voxels) — the world-space origin lives in
     * offs, with the spans holding only the patch-local offsets.
     * List sorted ascending by (y, x) per voxlap's "sortable as
     * longs" contract. */
    vspans spans[PATCH_W * PATCH_H];
    int n = 0;
    for (int dy = 0; dy < PATCH_H; dy++) {
        for (int dx = 0; dx < PATCH_W; dx++) {
            spans[n].x = (char)dx;
            spans[n].y = (char)dy;
            spans[n].z0 = (char)CARVE_Z0;
            spans[n].z1 = (char)CARVE_Z1;
            n++;
        }
    }
    lpoint3d offs = {BASE_X, BASE_Y, 0};

    /* dacol = -1 → carve to air via delslab path. */
    setspans(spans, n, &offs, -1);

    /* Save post-edit state. */
    if (!savevxl(argv[2], &ipo, &ist, &ihe, &ifo)) {
        fprintf(stderr, "savevxl(%s) failed\n", argv[2]);
        return 1;
    }

    fprintf(stderr,
            "edit_fixture: wrote pre=%s post=%s; carved %dx%d patch "
            "at (%d,%d) z=%d..%d (%d spans)\n",
            argv[1], argv[2], PATCH_W, PATCH_H, BASE_X, BASE_Y,
            CARVE_Z0, CARVE_Z1, n);
    return 0;
}
