#include "voxlap5.h"
#include <math.h>
#include <stdlib.h>

VOXLAP_API int32_t getVSID() { return VSID; }
VOXLAP_API void setMaxScanDistToMax() {
	/* Clamp to voxlap5.c's documented ceiling of 2047 (see the comment
	 * on vx5.maxscandist initialisation). Upstream Voxlap used VSID=1024
	 * so VSID*sqrt(2) ~1448 fit inside 2047; this fork's VSID=2048 makes
	 * the raw value ~2896, which overflows the raycaster and renders
	 * specific ray columns as black. */
	int32_t v = (int32_t)(VSID * sqrt(2));
	vx5.maxscandist = (v > 2047) ? 2047 : v;
}
VOXLAP_API void setMipUse(int32_t amount) { vx5.vxlmipuse = amount; }

VOXLAP_API void setRectOneColor(lpoint3d* hit1, lpoint3d* hit2, int32_t ARGB)
{
	vx5.curcol = ARGB;
	vx5.colfunc = curcolfunc ;
	setrect(hit1, hit2, 0);
}

VOXLAP_API void setRectWoodColor(lpoint3d* hit1, lpoint3d* hit2, int32_t ARGB)
{
	vx5.curcol = ARGB;
	vx5.colfunc = woodcolfunc ;
	setrect(hit1, hit2, 0);
}

static char curbrightness = 128;

int32_t setbrightnessfunc (lpoint3d *p) {
	return getcube(p->x, p->y, p->z) & 0xffffff | (curbrightness << 24);
}

VOXLAP_API void setRectBrightness(lpoint3d* hit1, lpoint3d* hit2, char brightness)
{
	curbrightness = brightness;
	vx5.colfunc = setbrightnessfunc;
	setrect(hit1, hit2, 0);
}

VOXLAP_API void printString(int32_t x, int32_t y, int32_t fcol, int32_t bcol, const char* str){
	print6x8(x,y,fcol,bcol,"%s",str);
}

VOXLAP_API void setLightingMode(int32_t mode) {
	vx5.lightmode = mode;
}

VOXLAP_API void setMaxScanDist(int32_t v) {
	vx5.maxscandist = v;
}

VOXLAP_API void set_anginc(int32_t v) {
	vx5.anginc = v;
}

VOXLAP_API int32_t get_anginc() {
	return vx5.anginc;
}

VOXLAP_API void set_curcol(int32_t v) {
	vx5.curcol = v;
}

VOXLAP_API void set_curpow(float v) {
	vx5.curpow = v;
}

VOXLAP_API void set_fallcheck(int32_t v) {
	vx5.fallcheck = v;
}

VOXLAP_API void set_fogcol(int32_t v) {
	vx5.fogcol = v;
}

VOXLAP_API void set_kv6col(int32_t v) {
	vx5.kv6col = v;
}

VOXLAP_API void set_colfunc(int32_t (*v)(lpoint3d *)) {
	vx5.colfunc = v;
}

VOXLAP_API void vox_free(void* ptr) {
	free(ptr);
}

VOXLAP_API void set_jitamount(int32_t v) {
	vx5.amount = v;
}

VOXLAP_API int32_t get_curcol() {
	return vx5.curcol;
}

VOXLAP_API int32_t get_jitamount() {
	return vx5.amount;
}

VOXLAP_API int32_t add_light(float px, float py, float pz, float flash_radius, float intens) {
	if (vx5.numlights >= MAXLIGHTS) {
		return MAXLIGHTS-1;
	}
	lightsrctype *light = &vx5.lightsrc[vx5.numlights];
	light->p.x = px; 
	light->p.y = py; 
	light->p.z = pz;
	light->r2 = flash_radius * flash_radius;
	light->sc = intens;
	vx5.numlights += 1;
	return vx5.numlights - 1;
}

VOXLAP_API int32_t get_lights_count() {
	return vx5.numlights;
}

VOXLAP_API void remove_light(int32_t i) {
	if (i >= vx5.numlights || i < 0) {
		return;
	}
	if (vx5.numlights == 1) {
		vx5.numlights = 0;
	}
	else {
		vx5.lightsrc[0] = vx5.lightsrc[vx5.numlights - 1];
		vx5.numlights -= 1;
	}
}
