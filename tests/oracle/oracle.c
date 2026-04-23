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
 * Note: voxsetframebuffer's first arg is typed `long`, which on Win32 x86
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
extern void setRectOneColor(lpoint3d *hit1, lpoint3d *hit2, long ARGB);
extern void set_curcol(long v);
extern void set_jitamount(long v);
extern void set_colfunc(long (*v)(lpoint3d *));
extern void set_fogcol(long v);
extern void set_anginc(long v);
extern void setMaxScanDist(long v);

/* Voxlap's 32-bit colour is packed as (brightness<<24) | (R<<16) | (G<<8) | B.
 * The alpha byte is *brightness*, not opacity: setting it to 0x00 renders
 * pixels as black regardless of RGB. 0x80 is the engine's "normal" level,
 * matching the built-in walls placed by loadnul. */
#define BR(rgb) (0x80000000u | (uint32_t)(rgb))

enum { XRES = 640, YRES = 480, BYTESPERLINE = XRES * 4 };

static int32_t g_fb[XRES * YRES];

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
	set_fogcol((long)BR(0x87ceeb));

	setsideshades(0, 0, 0, 0, 0, 0);
	set_colfunc(curcolfunc);
	set_jitamount(0);
	set_anginc(1);

	/* Carve a 448x448x185 playable box at map centre; the carve paints
	 * exposed inside faces of the surrounding solid with curcol. */
	set_curcol((long)BR(0x87ceeb));
	a.x = 800;   a.y = 800;   a.z = 5;
	b.x = 1248;  b.y = 1248;  b.z = 189;
	setrect(&a, &b, -1);

	/* Grey floor slab inside the playable box so looking down reads
	 * as stone. Top surface at z=185 is what the camera sees. */
	set_curcol((long)BR(0x606878));
	a.x = 800;   a.y = 800;   a.z = 185;
	b.x = 1248;  b.y = 1248;  b.z = 189;
	setrect(&a, &b, 0);

	/* Red pillar */
	a.x = 1010; a.y = 1090; a.z = 155;
	b.x = 1020; b.y = 1100; b.z = 184;
	setRectOneColor(&a, &b, (long)BR(0xff3030));

	/* Green cube */
	a.x = 1030; a.y = 1050; a.z = 175;
	b.x = 1040; b.y = 1060; b.z = 184;
	setRectOneColor(&a, &b, (long)BR(0x30c030));

	/* Blue flat tile */
	a.x = 1000; a.y = 1030; a.z = 183;
	b.x = 1050; b.y = 1070; b.z = 184;
	setRectOneColor(&a, &b, (long)BR(0x3060ff));

	/* Yellow sphere */
	set_curcol((long)BR(0xffd050));
	c.x = 1060; c.y = 1040; c.z = 178;
	setsphere(&c, 8, 0);

	updatevxl();
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
};

int main(void) {
	static const struct pose poses[] = {
		{"north",     1024.0, 1024.0, 128.0, 1.5707963267948966, 0.0},
		{"east",      1024.0, 1024.0, 128.0, 0.0,                0.0},
		{"diag_down", 1000.0, 1000.0, 110.0, 0.7853981633974483, 0.4},
		{"high_down", 1024.0, 1024.0,  90.0, 1.5707963267948966, 0.7},
	};
	const size_t N = sizeof(poses) / sizeof(poses[0]);
	size_t i;
	FILE *hf;

	if (initvoxlap() < 0) { fprintf(stderr, "initvoxlap failed\n"); return 1; }

	build_scene();
	voxsetframebuffer((long)(intptr_t)g_fb, BYTESPERLINE, XRES, YRES);

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
		opticast();

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
