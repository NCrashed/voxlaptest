/*
 * Voxlap render-hash oracle (Stage 0 of the port).
 *
 * Builds a small deterministic procedural scene, renders it from several
 * fixed camera poses, and for each pose:
 *   - hashes the framebuffer contents (FNV-1a 64-bit) into hashes.txt,
 *   - writes a PNG via screencapture32bit() for human inspection.
 *
 * Determinism scope: WITHIN-TOOLCHAIN ONLY. Hashes are bit-reproducible
 * across runs of the same build, but will NOT match across different
 * compilers or OSes because libm sin/cos and x87-vs-SSE FP rounding differ.
 * Use this harness:
 *   - as a regression detector during Stage 2/3 refactors (MSVC vs MSVC),
 *   - and for visual PNG comparison across platforms from Stage 4 onward.
 *
 * The oracle touches the engine only through exported symbols (declared in
 * voxlap5.h and wrapped in exports.c) — it never dereferences the `vx5`
 * global, which is not exported from voxlap.dll. That keeps this harness
 * free of any engine modifications.
 *
 * Note: voxsetframebuffer's first arg is typed `int32_t`, which on Win32 x86
 * is the same width as a pointer but NOT on x64. Stage 2 widens this API.
 */

#include "voxlap5.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wrappers declared in exports.c (not in voxlap5.h). Re-declare here so the
 * oracle doesn't need a second header. Linkage matches the VOXLAP_API defs. */
extern void setRectOneColor(lpoint3d *hit1, lpoint3d *hit2, int32_t ARGB);
extern void set_curcol(int32_t v);
extern void set_jitamount(int32_t v);
extern void set_colfunc(int32_t (*v)(lpoint3d *));
extern void set_fogcol(int32_t v);
extern void set_anginc(int32_t v);
extern void setMaxScanDist(int32_t v);
extern void setLightingMode(int32_t mode);
extern int32_t add_light(float px, float py, float pz, float radius, float intens);

/* Voxlap's 32-bit colour is packed as (brightness<<24) | (R<<16) | (G<<8) | B.
 * The alpha byte is *brightness*, not opacity: setting it to 0x00 renders
 * pixels as black regardless of RGB. 0x80 is the engine's "normal" level,
 * matching the built-in walls placed by loadnul. */
#define BR(rgb) (0x80000000u | (uint32_t)(rgb))

enum { XRES = 640, YRES = 480, BYTESPERLINE = XRES * 4 };

static int32_t g_fb[XRES * YRES];

/* A procedural kv6 sprite, built once via meltsphere in build_scene and
 * rendered in the "sprite_*" poses below. This covers the drawsprite
 * pipeline — drawboundcube_{sse,3dn}{,init} inside voxlap5.c plus the
 * matching entry points in v5.asm — that Stage 4 will rewrite as SSE2
 * intrinsics. Without this, sprite rendering has zero hash coverage. */
static vx5sprite g_sprite;

/* Sprite extracted from an external KVX file (assets/coco.kvx, slab6
 * format). Exercises the setkvx file-loader path on top of the
 * sprite-render pipeline, ensuring the loader stays correct as the
 * port progresses. */
static vx5sprite g_coco_sprite;

/* Procedural test tile used by the drawtile_* oracle poses. 16×16 ARGB
 * with brightness 0x80 throughout: a 2×2-checkered red/green pattern
 * with a yellow diagonal cross in the centre 4 pixels. Hand-built so
 * the post-blit hashes pin every byte that drawtile touches. */
enum { TILE_SIZE = 16 };
static int32_t g_tile[TILE_SIZE * TILE_SIZE];

static uint64_t fnv1a64(const void *data, size_t n) {
	const uint8_t *p = (const uint8_t *)data;
	uint64_t h = 0xcbf29ce484222325ULL;
	size_t i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
	return h;
}

static void build_test_tile(void) {
	int32_t x, y;
	for (y = 0; y < TILE_SIZE; y++) {
		for (x = 0; x < TILE_SIZE; x++) {
			int32_t cx = x - TILE_SIZE/2, cy = y - TILE_SIZE/2;
			int32_t argb;
			if ((cx == 0) || (cy == 0))            argb = (int32_t)BR(0xffd050); /* yellow cross */
			else if ((((x>>1) ^ (y>>1)) & 1) == 0) argb = (int32_t)BR(0xc03030); /* red */
			else                                   argb = (int32_t)BR(0x30c030); /* green */
			g_tile[y*TILE_SIZE + x] = argb;
		}
	}
}

static void build_scene(void) {
	dpoint3d ipo, ist, ihe, ifo;
	lpoint3d a, b, c;

	loadnul(&ipo, &ist, &ihe, &ifo);

	/* maxscandist must be <= 2047 (voxlap5.c:13062). The exported
	 * setMaxScanDistToMax() helper is broken on this fork — it
	 * computes VSID*sqrt(2) ≈ 2896 for VSID=2048 and overflows that
	 * ceiling, rendering specific ray columns as black. See
	 * memory/project_setmaxscandist_bug.md. Use a safe manual value
	 * large enough to cover the diagonal of our 448-voxel playable
	 * box (~633) with headroom. */
	setMaxScanDist(1024);
	/* Fog colour is a global, not a per-voxel render colour, so it
	 * must NOT carry the BR brightness bit — that bit makes the int32
	 * negative, and `voxsetframebuffer`'s `if (vx5.fogcol >= 0)`
	 * branch silently skips the fog setup, leaving `ofogdist = -1`
	 * and routing `opticast` through the non-fog rasterizers. Pass
	 * the raw RGB so the fog code path is actually exercised. */
	set_fogcol(0x87ceeb);

	setsideshades(0, 0, 0, 0, 0, 0);
	set_colfunc(curcolfunc);
	set_jitamount(0);
	set_anginc(1);

	/* Carve a 448x448x185 playable box at map centre; the carve paints
	 * exposed inside faces of the surrounding solid with curcol. */
	set_curcol((int32_t)BR(0x87ceeb));
	a.x = 800;   a.y = 800;   a.z = 5;
	b.x = 1248;  b.y = 1248;  b.z = 189;
	setrect(&a, &b, -1);

	/* Grey floor slab inside the playable box so looking down reads
	 * as stone. Top surface at z=185 is what the camera sees. */
	set_curcol((int32_t)BR(0x606878));
	a.x = 800;   a.y = 800;   a.z = 185;
	b.x = 1248;  b.y = 1248;  b.z = 189;
	setrect(&a, &b, 0);

	/* Red pillar */
	a.x = 1010; a.y = 1090; a.z = 155;
	b.x = 1020; b.y = 1100; b.z = 184;
	setRectOneColor(&a, &b, (int32_t)BR(0xff3030));

	/* Green cube */
	a.x = 1030; a.y = 1050; a.z = 175;
	b.x = 1040; b.y = 1060; b.z = 184;
	setRectOneColor(&a, &b, (int32_t)BR(0x30c030));

	/* Blue flat tile */
	a.x = 1000; a.y = 1030; a.z = 183;
	b.x = 1050; b.y = 1070; b.z = 184;
	setRectOneColor(&a, &b, (int32_t)BR(0x3060ff));

	/* Yellow sphere */
	set_curcol((int32_t)BR(0xffd050));
	c.x = 1060; c.y = 1040; c.z = 178;
	setsphere(&c, 8, 0);

	/* -- Hidden voxel source for the meltsphere sprite --
	 * setrect with op=0 only fills *empty* voxels, so we first have
	 * to carve a cavity in the surrounding solid mass at (600, 600,
	 * 100). The cavity and everything inside it sit ~190 voxels
	 * outside the 800..1248 playable box, so no camera reaches it
	 * through the solid walls — the first four pose hashes stay put.
	 *
	 * Inside the cavity we paint a ~12x12x15 multi-tone block: red/
	 * green/blue z-stripes for layered colour plus a magenta marker
	 * on one face so the sprite has visible orientation cues in the
	 * PNGs. */
	set_curcol((int32_t)BR(0x87ceeb)); /* carve color — gets overwritten */
	a.x = 590; a.y = 590; a.z = 90;
	b.x = 610; b.y = 610; b.z = 110;
	setrect(&a, &b, -1);

	set_curcol((int32_t)BR(0xff4030)); /* red top stripe */
	a.x = 594; a.y = 594; a.z = 93;
	b.x = 606; b.y = 606; b.z = 98;
	setrect(&a, &b, 0);

	set_curcol((int32_t)BR(0x30c040)); /* green middle stripe */
	a.x = 594; a.y = 594; a.z = 98;
	b.x = 606; b.y = 606; b.z = 103;
	setrect(&a, &b, 0);

	set_curcol((int32_t)BR(0x3060ff)); /* blue bottom stripe */
	a.x = 594; a.y = 594; a.z = 103;
	b.x = 606; b.y = 606; b.z = 108;
	setrect(&a, &b, 0);

	set_curcol((int32_t)BR(0xff40c0)); /* magenta marker on -x face */
	a.x = 593; a.y = 598; a.z = 99;
	b.x = 594; b.y = 602; b.z = 102;
	setrect(&a, &b, 0);

	updatevxl();

	/* Extract the painted block as a sprite kv6. After this call
	 * g_sprite.voxnum points at a fresh kv6data the engine owns; we
	 * just need to place it in world space. */
	c.x = 600; c.y = 600; c.z = 100;
	{
		int32_t nvox = meltsphere(&g_sprite, &c, 8);
		fprintf(stderr, "meltsphere: %d voxels extracted\n", nvox);
	}

	g_sprite.flags = 0; /* normal shading, voxnum is kv6data* */
	g_sprite.p.x = 1050.f; g_sprite.p.y = 1050.f; g_sprite.p.z = 175.f;
	g_sprite.s.x = 1.f; g_sprite.s.y = 0.f; g_sprite.s.z = 0.f;
	g_sprite.h.x = 0.f; g_sprite.h.y = 1.f; g_sprite.h.z = 0.f;
	g_sprite.f.x = 0.f; g_sprite.f.y = 0.f; g_sprite.f.z = 1.f;
	g_sprite.kfatim = 0;
	g_sprite.okfatim = 0;

	/* -- External KVX sprite (assets/coco.kvx) --
	 * Same isolation pattern as the meltsphere block above: carve a
	 * cavity in the surrounding solid mass at (660, 660, 110), stamp
	 * the kvx file's voxels with setkvx (which palette-maps + plants
	 * voxels into the world voxel grid), then extract via meltsphere
	 * to a kv6 sprite we can drawsprite anywhere in the scene. The
	 * cavity sits well outside the 800..1248 playable box, so no
	 * existing camera pose's frustum reaches it — adding this leaves
	 * the first 7 hashes untouched.
	 *
	 * setkvx takes a path resolved relative to the oracle's CWD; the
	 * CMakeLists.txt POST_BUILD step copies assets/ alongside oracle's
	 * run directory so 'assets/coco.kvx' finds the file. */
	set_curcol((int32_t)BR(0x87ceeb)); /* carve color — overwritten by setkvx */
	a.x = 640; a.y = 640; a.z = 95;
	b.x = 680; b.y = 680; b.z = 125;
	setrect(&a, &b, -1);

	setkvx("assets/coco.kvx", 660, 660, 110, 0, 0);

	c.x = 660; c.y = 660; c.z = 110;
	{
		int32_t nvox = meltsphere(&g_coco_sprite, &c, 12);
		fprintf(stderr, "meltsphere coco: %d voxels extracted\n", nvox);
	}

	g_coco_sprite.flags = 0;
	g_coco_sprite.p.x = 1110.f; g_coco_sprite.p.y = 1080.f; g_coco_sprite.p.z = 175.f;
	/* Rotate the kv6 model 120° about world-Z so its local (xsiz, ysiz)
	 * axes are no longer aligned with world (X, Y). Exercises the
	 * non-axis-aligned drawsprite path (rotated bound-cube projection
	 * + rotated voxel-walk in drawboundcube_*) which axis-aligned
	 * sprites would skip. */
	{
		const float ang = 2.f * 3.14159265358979323846f / 3.f;
		const float ca = (float)cos(ang), sa = (float)sin(ang);
		g_coco_sprite.s.x =  ca;  g_coco_sprite.s.y = sa; g_coco_sprite.s.z = 0.f;
		g_coco_sprite.h.x = -sa;  g_coco_sprite.h.y = ca; g_coco_sprite.h.z = 0.f;
		g_coco_sprite.f.x = 0.f;  g_coco_sprite.f.y = 0.f; g_coco_sprite.f.z = 1.f;
	}
	g_coco_sprite.kfatim = 0;
	g_coco_sprite.okfatim = 0;

	genmipvxl(0, 0, VSID, VSID);
}

static void set_camera_yaw_pitch(double px, double py, double pz,
                                 double yaw, double pitch) {
	dpoint3d pos, istr, ihei, ifor;
	double cy = cos(yaw), sy = sin(yaw);
	double cp = cos(pitch), sp = sin(pitch);

	pos.x  = px;       pos.y  = py;       pos.z  = pz;
	ifor.x = cy * cp;  ifor.y = sy * cp;  ifor.z = sp;
	istr.x = -sy;      istr.y = cy;       istr.z = 0.0;
	ihei.x = -cy * sp; ihei.y = -sy * sp; ihei.z = cp;

	dorthonormalize(&istr, &ihei, &ifor);
	setcamera(&pos, &istr, &ihei, &ifor,
	          (float)(XRES * 0.5), (float)(YRES * 0.5), (float)(XRES * 0.5));
}

struct pose {
	const char *name;
	double px, py, pz;
	double yaw, pitch;
	vx5sprite *sprite; /* NULL = no sprite, else drawsprite this kv6data */
	int32_t lit;       /* 1 = bake lightmode-2 lighting into voxel intensities
	                    *     before this pose's render. Bake is one-shot
	                    *     (subsequent poses keep the lit voxel state). */
	int32_t tile;      /* drawtile coverage:
	                    *   0 = no tile overlay
	                    *   1 = 1× zoom, alpha-disabled (texture-stretch path
	                    *       with black==white triggering ignore-alpha)
	                    *   2 = 0.5× zoom (the 32768/32768 fast 2×2-average
	                    *       downsample path)
	                    *   3 = 1.5× zoom + alpha-blend (texture-stretch +
	                    *       per-channel modulate + per-pixel blend) */
};

int main(void) {
	static const struct pose poses[] = {
		{"north",          1024.0, 1024.0, 128.0, 1.5707963267948966, 0.0, NULL,            0, 0},
		{"east",           1024.0, 1024.0, 128.0, 0.0,                0.0, NULL,            0, 0},
		{"diag_down",      1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, NULL,            0, 0},
		{"high_down",      1024.0, 1024.0,  90.0, 1.5707963267948966, 0.7, NULL,            0, 0},
		/* Sprite poses: camera aimed at g_sprite at (1050, 1050, 175).
		 * front: eye-level, looking at sprite along +x.
		 * above: slightly in front, pitched steeply toward the sprite.
		 * iso:   diagonal approach, mild pitch. */
		{"sprite_front",   1020.0, 1050.0, 175.0, 0.0,                0.0, &g_sprite,       0, 0},
		{"sprite_above",   1050.0, 1050.0, 150.0, 0.0,                1.3, &g_sprite,       0, 0},
		{"sprite_iso",     1020.0, 1020.0, 160.0, 0.7853981633974483, 0.4, &g_sprite,       0, 0},
		/* External KVX sprite (g_coco_sprite at (1110, 1080, 175),
		 * rotated 120° about Z), viewed isometrically from SW. The
		 * iso angle + non-axis-aligned model orientation together
		 * exercise the rotated drawsprite path (drawboundcube_*). */
		{"sprite_coco",    1080.0, 1050.0, 160.0, 0.7853981633974483, 0.4, &g_coco_sprite,  0, 0},
		/* Variant of diag_down with lightmode-2 baking enabled, so
		 * voxel intensities reflect a single point light at
		 * (1100, 1100, 70) — top faces brighter than walls/floor.
		 * MUST come after every unlit pose (the bake mutates the
		 * world voxel intensities and persists across renders). */
		{"diag_down_lit",  1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, NULL,            1, 0},
		/* drawtile coverage. All 3 use diag_down's camera so the
		 * underlying scene render is identical across them — the
		 * hash difference comes purely from the post-opticast
		 * drawtile call. */
		{"tile_1x",        1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, NULL,            0, 1},
		{"tile_half",      1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, NULL,            0, 2},
		{"tile_blend",     1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, NULL,            0, 3},
	};
	const size_t N = sizeof(poses) / sizeof(poses[0]);
	size_t i;
	FILE *hf;
	int32_t lighting_baked = 0;

	if (initvoxlap() < 0) { fprintf(stderr, "initvoxlap failed\n"); return 1; }

	build_scene();
	build_test_tile();

	/* Optional one-shot dump of the procedurally-built world to a
	 * .vxl file, used by the roxlap port's R2.3 parser tests as a
	 * shared fixture (same world both engines render). Gated on env
	 * var so the default CI run is unaffected. Camera vectors are
	 * placeholders — savevxl just records them in the header for
	 * loadvxl to expose as the file's "starting pose"; world data
	 * lives in sptr[] which is what the roxlap parser validates. */
	{
		const char *vxl_save_path = getenv("ROXLAP_SAVE_VXL");
		if (vxl_save_path) {
			dpoint3d save_ipo = { 1024.0, 1024.0, 128.0 };
			dpoint3d save_ist = {    1.0,    0.0,   0.0 };
			dpoint3d save_ihe = {    0.0,    0.0,   1.0 };
			dpoint3d save_ifo = {    0.0,    1.0,   0.0 };
			if (savevxl(vxl_save_path, &save_ipo, &save_ist, &save_ihe, &save_ifo)) {
				fprintf(stderr, "saved oracle world to %s\n", vxl_save_path);
			} else {
				fprintf(stderr, "savevxl(%s) failed\n", vxl_save_path);
			}
		}
	}

	/* Optional one-shot dump of the meltsphere-extracted sprites to
	 * .kv6 files. Used by the roxlap port's R6.0e meltsphere byte-
	 * equality test: roxlap calls its own meltsphere on the same
	 * loaded oracle.vxl + same hit/radius and byte-compares the
	 * resulting kv6 against this dump.
	 *
	 * The format mirrors voxlap's loadkv6 (`Kvxl` magic + header +
	 * voxels + xlen + ylen, all little-endian on x86). kv6voxtype is
	 * a packed 8-byte struct so the voxel array can be fwrite'd
	 * verbatim. Set the env var to a directory; we write
	 * `<dir>/sprite_meltsphere.kv6` and `<dir>/sprite_coco_melt.kv6`. */
	{
		const char *spr_dir = getenv("ROXLAP_DUMP_SPRITES");
		if (spr_dir) {
			static const struct {
				const char *name;
				const vx5sprite *spr;
			} dumps[] = {
				{ "sprite_meltsphere", &g_sprite },
				{ "sprite_coco_melt",  &g_coco_sprite },
			};
			size_t k;
			for (k = 0; k < sizeof(dumps) / sizeof(dumps[0]); k++) {
				char path[512];
				FILE *fp;
				const kv6data *kv;
				size_t n_vox, n_xlen, n_ylen;
				snprintf(path, sizeof(path), "%s/%s.kv6", spr_dir, dumps[k].name);
				fp = fopen(path, "wb");
				if (!fp) {
					fprintf(stderr, "fopen(%s) failed\n", path);
					continue;
				}
				kv = dumps[k].spr->voxnum;
				if (!kv) {
					fprintf(stderr, "%s: voxnum NULL\n", dumps[k].name);
					fclose(fp);
					continue;
				}
				n_vox = (size_t)kv->numvoxs;
				n_xlen = (size_t)kv->xsiz;
				n_ylen = (size_t)kv->xsiz * (size_t)kv->ysiz;
				fwrite("Kvxl", 1, 4, fp);
				fwrite(&kv->xsiz, 4, 1, fp);
				fwrite(&kv->ysiz, 4, 1, fp);
				fwrite(&kv->zsiz, 4, 1, fp);
				fwrite(&kv->xpiv, 4, 1, fp);
				fwrite(&kv->ypiv, 4, 1, fp);
				fwrite(&kv->zpiv, 4, 1, fp);
				fwrite(&kv->numvoxs, 4, 1, fp);
				fwrite(kv->vox, sizeof(kv6voxtype), n_vox, fp);
				fwrite(kv->xlen, 4, n_xlen, fp);
				fwrite(kv->ylen, 2, n_ylen, fp);
				fclose(fp);
				fprintf(stderr, "saved meltsphere sprite to %s "
				                "(%zu voxels, %ux%ux%u)\n",
				        path, n_vox, kv->xsiz, kv->ysiz, kv->zsiz);
			}
		}
	}

	voxsetframebuffer((intptr_t)g_fb, BYTESPERLINE, XRES, YRES);

	hf = fopen("hashes.txt", "w");
	if (!hf) { fprintf(stderr, "cannot open hashes.txt\n"); return 2; }

	for (i = 0; i < N; i++) {
		uint64_t h;
		char png[64];
		size_t k;

		/* Pre-fill with the same sky-blue that fogcol uses, so any pixel
		 * opticast happens to leave untouched still reads as sky. */
		for (k = 0; k < XRES * YRES; k++) g_fb[k] = (int32_t)BR(0x87ceeb);

		/* Lit pose: one-shot bake of lightmode-2 lighting into the
		 * voxel-intensity bytes of every voxel inside the playable box.
		 * vx5.lightmode = 2 picks the path in updatelighting that uses
		 * estnorm() per voxel + Lambertian fall-off against each light
		 * source (voxlap5.c:11479+), so faces get differential
		 * brightness based on their estimated surface normal. */
		if (poses[i].lit && !lighting_baked) {
			setLightingMode(2);
			add_light(1100.f, 1100.f, 70.f, 600.f, 1.0f);
			updatelighting(800, 800, 0, 1248, 1248, 200);
			lighting_baked = 1;
		}

		set_camera_yaw_pitch(poses[i].px, poses[i].py, poses[i].pz,
		                     poses[i].yaw, poses[i].pitch);

		opticast();
		if (poses[i].sprite) drawsprite(poses[i].sprite);

		/* drawtile coverage: overlay the procedural test tile. The
		 * three flag values exercise all three drawtile code paths
		 * (32768-zoom 2×2 averaging, generic texture stretch, and the
		 * alpha-blend modulate-then-blend path). Tile is anchored at
		 * its centre via tcx=tcy=8<<16 (= centre of the 16×16 tile),
		 * placed at screen (320, 240) (= centre of the 640×480 fb). */
		switch (poses[i].tile) {
		case 1:
			/* 1× zoom, alpha disabled (black==white triggers ignore-alpha). */
			drawtile((intptr_t)g_tile, TILE_SIZE * 4,
			         TILE_SIZE, TILE_SIZE, 8 << 16, 8 << 16,
			         320 << 16, 240 << 16,
			         1 << 16, 1 << 16,
			         (int32_t)BR(0x000000), (int32_t)BR(0x000000));
			break;
		case 2:
			/* 0.5× zoom (xz=yz=32768) — fast 2×2 averaging path. */
			drawtile((intptr_t)g_tile, TILE_SIZE * 4,
			         TILE_SIZE, TILE_SIZE, 8 << 16, 8 << 16,
			         320 << 16, 240 << 16,
			         32768, 32768,
			         (int32_t)BR(0x000000), (int32_t)BR(0x000000));
			break;
		case 3:
			/* 1.5× zoom + alpha modulate-and-blend path. black/white
			 * differ in alpha (0x40 vs 0xc0), so the alpha branch fires;
			 * the 0x40-vs-0xc0 channel spread tints the tile cyan-ward. */
			drawtile((intptr_t)g_tile, TILE_SIZE * 4,
			         TILE_SIZE, TILE_SIZE, 8 << 16, 8 << 16,
			         320 << 16, 240 << 16,
			         (3 << 16) / 2, (3 << 16) / 2,
			         (int32_t)0x40103060, (int32_t)0xc0e0a0c0);
			break;
		default:
			break;
		}

		h = fnv1a64(g_fb, sizeof(g_fb));
		fprintf(hf,     "%s  %016llx\n", poses[i].name, (unsigned long long)h);
		fprintf(stdout, "%s  %016llx\n", poses[i].name, (unsigned long long)h);

		snprintf(png, sizeof(png), "%s.png", poses[i].name);
		screencapture32bit(png);
	}

	fclose(hf);
	uninitvoxlap();
	return 0;
}
