#include "voxlap5.h"
#include <math.h>

DLLEXPORT long getVSID() { return VSID; }
DLLEXPORT void setMaxScanDistToMax() { vx5.maxscandist = VSID*sqrt(2); }
DLLEXPORT void setMipUse(long amount) { vx5.vxlmipuse = amount; }

DLLEXPORT void setRectOneColor(lpoint3d* hit1, lpoint3d* hit2, long ARGB)
{
	vx5.curcol = ARGB;
	vx5.colfunc = curcolfunc ;
	setrect(hit1, hit2, 0);
}

DLLEXPORT void setRectWoodColor(lpoint3d* hit1, lpoint3d* hit2, long ARGB)
{
	vx5.curcol = ARGB;
	vx5.colfunc = woodcolfunc ;
	setrect(hit1, hit2, 0);
}

static char curbrightness = 128;

long setbrightnessfunc (lpoint3d *p) {
	return getcube(p->x, p->y, p->z) & 0xffffff | (curbrightness << 24);
}

DLLEXPORT void setRectBrightness(lpoint3d* hit1, lpoint3d* hit2, char brightness)
{
	curbrightness = brightness;
	vx5.colfunc = setbrightnessfunc;
	setrect(hit1, hit2, 0);
}

DLLEXPORT void printString(long x, long y, long fcol, long bcol, const char* str){
	print6x8(x,y,fcol,bcol,"%s",str);
}

DLLEXPORT void setLightingMode(long mode) {
	vx5.lightmode = mode;
}

DLLEXPORT void setMaxScanDist(long v) {
	vx5.maxscandist = v;
}

DLLEXPORT void set_anginc(long v) {
	vx5.anginc = v;
}

DLLEXPORT long get_anginc() {
	return vx5.anginc;
}

DLLEXPORT void set_curcol(long v) {
	vx5.curcol = v;
}

DLLEXPORT void set_curpow(float v) {
	vx5.curpow = v;
}

DLLEXPORT void set_fallcheck(long v) {
	vx5.fallcheck = v;
}

DLLEXPORT void set_fogcol(long v) {
	vx5.fogcol = v;
}

DLLEXPORT void set_kv6col(long v) {
	vx5.kv6col = v;
}

DLLEXPORT void set_colfunc(long (*v)(lpoint3d *)) {
	vx5.colfunc = v;
}

DLLEXPORT void vox_free(void* ptr) {
	free(ptr);
}

DLLEXPORT void set_jitamount(long v) {
	vx5.amount = v;
}

DLLEXPORT long get_curcol() {
	return vx5.curcol;
}

DLLEXPORT long get_jitamount() {
	return vx5.amount;
}
