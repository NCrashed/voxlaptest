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

static uint64_t fnv1a64(const void *data, size_t n) {
	const uint8_t *p = (const uint8_t *)data;
	uint64_t h = 0xcbf29ce484222325ULL;
	size_t i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
	return h;
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
	set_fogcol((int32_t)BR(0x87ceeb));

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
	int32_t draw_sprite; /* 1 -> call drawsprite(&g_sprite) after opticast */
};

int main(void) {
	static const struct pose poses[] = {
		{"north",        1024.0, 1024.0, 128.0, 1.5707963267948966, 0.0, 0},
		{"east",         1024.0, 1024.0, 128.0, 0.0,                0.0, 0},
		{"diag_down",    1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4, 0},
		{"high_down",    1024.0, 1024.0,  90.0, 1.5707963267948966, 0.7, 0},
		/* Sprite poses: camera aimed at g_sprite at (1050, 1050, 175).
		 * front: eye-level, looking at sprite along +x.
		 * above: slightly in front, pitched steeply toward the sprite.
		 * iso:   diagonal approach, mild pitch. */
		{"sprite_front", 1020.0, 1050.0, 175.0, 0.0,                0.0, 1},
		{"sprite_above", 1050.0, 1050.0, 150.0, 0.0,                1.3, 1},
		{"sprite_iso",   1020.0, 1020.0, 160.0, 0.7853981633974483, 0.4, 1},
	};
	const size_t N = sizeof(poses) / sizeof(poses[0]);
	size_t i;
	FILE *hf;

	if (initvoxlap() < 0) { fprintf(stderr, "initvoxlap failed\n"); return 1; }

	build_scene();
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

		set_camera_yaw_pitch(poses[i].px, poses[i].py, poses[i].pz,
		                     poses[i].yaw, poses[i].pitch);

#ifdef VOXLAP_GROUSCAN_TRACE
		/* Stage 4.5b.7b: per-pixel trace harness. Only open for the
		 * high_down scene (cleanest 43-pixel sphere-edge bug locus); a
		 * full-scene trace blows past the 5M-event cap and produces
		 * 100+ MB logs that overwhelm CI artifact upload. The scalar
		 * and C-fallback builds each open `trace_high_down.log`; the
		 * post-processing line-diff at tests/oracle/trace_diff.py
		 * compares them. Other scenes get no trace.
		 *
		 * Without VOXLAP_GROUSCAN_TRACE in the lib, voxlap_trace_open
		 * is a no-op, so the unconditional call here is safe across
		 * trace-on / trace-off builds. */
		{
			char trace_path[64];
			if (strcmp(poses[i].name, "high_down") == 0) {
				snprintf(trace_path, sizeof(trace_path),
				         "trace_%s.log", poses[i].name);
				voxlap_trace_open(trace_path, (int32_t)i);
			}
		}
#endif

		opticast();
		if (poses[i].draw_sprite) drawsprite(&g_sprite);

#ifdef VOXLAP_GROUSCAN_TRACE
		voxlap_trace_close();
#endif

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
