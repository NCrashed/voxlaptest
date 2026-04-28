#ifndef KEN_VOXLAP5_H
#define KEN_VOXLAP5_H

#include <stdint.h>

#define MAXXDIM 1024
#define MAXYDIM 768
#define PI 3.141592653589793
#define VSID 2048   //Maximum .VXL dimensions in both x & y direction
#define MAXZDIM 256 //Maximum .VXL dimensions in z direction (height)

#include "voxlap_export.h"

#pragma pack(push,1)

typedef struct kv6data kv6data;

typedef struct lpoint3d { int32_t x, y, z; } lpoint3d;
typedef struct point3d { float x, y, z; } point3d;
typedef struct point4d { float x, y, z, z2; } point4d;
typedef struct dpoint3d { double x, y, z; } dpoint3d;

	//Sprite structures:
typedef struct kv6voxtype { int32_t col; unsigned short z; char vis, dir; } kv6voxtype;

typedef struct kv6data
{
	int32_t leng, xsiz, ysiz, zsiz;
	float xpiv, ypiv, zpiv;
	uint32_t numvoxs;
	int32_t namoff;
	kv6data *lowermip;
	kv6voxtype *vox;      //numvoxs*sizeof(kv6voxtype)
	uint32_t *xlen;  //xsiz*sizeof(int32_t)
	unsigned short *ylen; //xsiz*ysiz*sizeof(short)
} kv6data;

typedef struct hingetype
{
	int32_t parent;      //index to parent sprite (-1=none)
	point3d p[2];     //"velcro" point of each object
	point3d v[2];     //axis of rotation for each object
	short vmin, vmax; //min value / max value
	char htype, filler[7];
} hingetype;

typedef struct seqtyp { int32_t tim, frm; } seqtyp;

typedef struct kfatype
{
	int32_t numspr, numhin, numfrm, seqnum;
	int32_t namoff;
	kv6data *basekv6;      //Points to original unconnected KV6 (maybe helpful?)
	struct vx5sprite *spr; //[numspr]
	hingetype *hinge;      //[numhin]
	int32_t *hingesort;       //[numhin]
	short *frmval;         //[numfrm][numhin]
	seqtyp *seq;           //[seqnum]
} kfatype;

	//Notice that I aligned each point3d on a 16-byte boundary. This will be
	//   helpful when I get around to implementing SSE instructions someday...
typedef struct vx5sprite
{
	point3d p; //position in VXL coordinates
	int32_t flags; //flags bit 0:0=use normal shading, 1=disable normal shading
					//flags bit 1:0=points to kv6data, 1=points to kfatype
					//flags bit 2:0=normal, 1=invisible sprite
	union { point3d s, x; }; //kv6data.xsiz direction in VXL coordinates
	union
	{
		kv6data *voxnum; //pointer to KV6 voxel data (bit 1 of flags = 0)
		kfatype *kfaptr; //pointer to KFA animation  (bit 1 of flags = 1)
	};
	union { point3d h, y; }; //kv6data.ysiz direction in VXL coordinates
	int32_t kfatim;        //time (in milliseconds) of KFA animation
	union { point3d f, z; }; //kv6data.zsiz direction in VXL coordinates
	int32_t okfatim;       //make vx5sprite exactly 64 bytes :)
} vx5sprite;

	//Falling voxels shared data: (flst = float list)
#define FLPIECES 256 //Max # of separate falling pieces
typedef struct flstboxtype//(68 bytes)
{
	lpoint3d chk; //a solid point on piece (x,y,pointer) (don't touch!)
	int32_t i0, i1; //indices to start&end of slab list (don't touch!)
	int32_t x0, y0, z0, x1, y1, z1; //bounding box, written by startfalls
	int32_t mass; //mass of piece, written by startfalls (1 unit per voxel)
	point3d centroid; //centroid of piece, written by startfalls

		//userval is set to -1 when a new piece is spawned. Voxlap does not
		//read or write these values after that point. You should use these to
		//play an initial sound and track velocity
	int32_t userval, userval2;
} flstboxtype;

	//Lighting variables: (used by updatelighting)
#define MAXLIGHTS 256
typedef struct lightsrctype { point3d p; float r2, sc; } lightsrctype;

	//Used by setspans/meltspans. Ordered this way to allow sorting as longs!
typedef struct vspans { char z1, z0, x, y; } vspans;

typedef struct {
	dpoint3d origin;  // starting point of the ray
	dpoint3d direction; // direction of the ray
} ray3d;

#pragma pack(pop)

#define MAXFRM 1024 //MUST be even number for alignment!

	//Voxlap5 shared global variables:
#ifndef VOXLAP5
extern
#endif
struct vx5
{
	//------------------------ DATA coming from VOXLAP5 ------------------------

		//Clipmove hit point info (use this after calling clipmove):
	double clipmaxcr; //clipmove always calls findmaxcr even with no movement
	dpoint3d cliphit[3];
	int32_t cliphitnum;

		//Bounding box written by last set* VXL writing call
	int32_t minx, miny, minz, maxx, maxy, maxz;

		//Falling voxels shared data:
	int32_t flstnum;
	flstboxtype flstcnt[FLPIECES];

		//Total count of solid voxels in .VXL map (included unexposed voxels)
	int32_t globalmass;

		//Temp workspace for KFA animation (hinge angles)
		//Animsprite writes these values&you may modify them before drawsprite
	short kfaval[MAXFRM];

	//------------------------ DATA provided to VOXLAP5 ------------------------

		//Opticast variables:
	int32_t anginc, sideshademode, mipscandist, maxscandist, vxlmipuse, fogcol;

		//Drawsprite variables:
	int32_t kv6mipfactor, kv6col;
		//Drawsprite x-plane clipping (reset to 0,(high int) after use!)
		//For example min=8,max=12 permits only planes 8,9,10,11 to draw
	int32_t xplanemin, xplanemax;

		//Map modification function data:
	int32_t curcol, currad, curhei;
	float curpow;

		//Procedural texture function data:
	int32_t (*colfunc)(lpoint3d *);
	int32_t cen, amount, *pic, bpl, xsiz, ysiz, xoru, xorv, picmode;
	point3d fpico, fpicu, fpicv, fpicw;
	lpoint3d pico, picu, picv;
	float daf;

		//Lighting variables: (used by updatelighting)
	int32_t lightmode; //0 (default), 1:simple lighting, 2:lightsrc lighting
	lightsrctype lightsrc[MAXLIGHTS]; //(?,?,?),128*128,262144
	int32_t numlights;

	int32_t fallcheck;
} vx5;

	//Initialization functions:
VOXLAP_API extern int32_t initvoxlap ();
VOXLAP_API extern void uninitvoxlap ();

	//File related functions:
VOXLAP_API extern int32_t loadsxl (const char *, char **, char **, char **);
VOXLAP_API extern char *parspr (vx5sprite *, char **);
VOXLAP_API extern void loadnul (dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern int32_t loaddta (const char *, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern int32_t loadpng (const char *, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern void loadbsp (const char *, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern int32_t loadvxl (const char *, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern int32_t savevxl (const char *, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern int32_t loadsky (const char *);

	//Screen related functions:
VOXLAP_API extern void voxsetframebuffer (intptr_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void setsideshades (char, char, char, char, char, char);
VOXLAP_API extern void setcamera (dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *, float, float, float);
VOXLAP_API extern void opticast ();

VOXLAP_API extern void drawpoint2d (int32_t, int32_t, int32_t);
VOXLAP_API extern void drawpoint3d (float, float, float, int32_t);
VOXLAP_API extern void drawline2d (float, float, float, float, int32_t);
VOXLAP_API extern void drawline3d (float, float, float, float, float, float, int32_t);
VOXLAP_API extern int32_t project2d (float, float, float, float *, float *, float *);
VOXLAP_API extern ray3d unproject2d(float, float);
VOXLAP_API extern void drawspherefill (float, float, float, float, int32_t);
VOXLAP_API extern void drawpicinquad (intptr_t, int32_t, int32_t, int32_t, intptr_t, int32_t, int32_t, int32_t, float, float, float, float, float, float, float, float);
VOXLAP_API extern void drawpolyquad (intptr_t, int32_t, int32_t, int32_t, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float);
VOXLAP_API extern void print4x6 (int32_t, int32_t, int32_t, int32_t, const char *, ...);
VOXLAP_API extern void print6x8 (int32_t, int32_t, int32_t, int32_t, const char *, ...);
VOXLAP_API extern void drawtile (intptr_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern int32_t screencapture32bit (const char *);
VOXLAP_API extern int32_t surroundcapture32bit (dpoint3d *, const char *, int32_t);

	//Sprite related functions:
VOXLAP_API extern kv6data *getkv6 (const char *);
VOXLAP_API extern kfatype *getkfa (const char *);
VOXLAP_API extern void freekv6 (kv6data *kv6);
VOXLAP_API extern void savekv6 (const char *, kv6data *);
VOXLAP_API extern void getspr (vx5sprite *, const char *);
VOXLAP_API extern kv6data *genmipkv6 (kv6data *);
VOXLAP_API extern char *getkfilname (int32_t);
VOXLAP_API extern void animsprite (vx5sprite *, int32_t);
VOXLAP_API extern void drawsprite (vx5sprite *);
VOXLAP_API extern int32_t meltsphere (vx5sprite *, lpoint3d *, int32_t);
VOXLAP_API extern int32_t meltspans (vx5sprite *, vspans *, int32_t, lpoint3d *);

	//Physics helper functions:
VOXLAP_API extern void orthonormalize (point3d *, point3d *, point3d *);
VOXLAP_API extern void dorthonormalize (dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern void orthorotate (float, float, float, point3d *, point3d *, point3d *);
VOXLAP_API extern void dorthorotate (double, double, double, dpoint3d *, dpoint3d *, dpoint3d *);
VOXLAP_API extern void axisrotate (point3d *, point3d *, float);
VOXLAP_API extern void slerp (point3d *, point3d *, point3d *, point3d *, point3d *, point3d *, point3d *, point3d *, point3d *, float);
VOXLAP_API extern int32_t cansee (point3d *, point3d *, lpoint3d *);
VOXLAP_API extern void hitscan (dpoint3d *, dpoint3d *, lpoint3d *, int32_t **, int32_t *);
VOXLAP_API extern void sprhitscan (dpoint3d *, dpoint3d *, vx5sprite *, lpoint3d *, kv6voxtype **, float *vsc);
VOXLAP_API extern double findmaxcr (double, double, double, double);
VOXLAP_API extern void clipmove (dpoint3d *, dpoint3d *, double);
VOXLAP_API extern int32_t triscan (point3d *, point3d *, point3d *, point3d *, lpoint3d *);
VOXLAP_API extern void estnorm (int32_t, int32_t, int32_t, point3d *);

	//VXL reading functions (fast!):
VOXLAP_API extern int32_t isvoxelsolid (int32_t, int32_t, int32_t);
VOXLAP_API extern int32_t anyvoxelsolid (int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern int32_t anyvoxelempty (int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern int32_t getfloorz (int32_t, int32_t, int32_t);
VOXLAP_API extern int32_t getcube (int32_t, int32_t, int32_t);

	//VXL writing functions (optimized & bug-free):
VOXLAP_API extern void setcube (int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void setsphere (lpoint3d *, int32_t, int32_t);
VOXLAP_API extern void setellipsoid (lpoint3d *, lpoint3d *, int32_t, int32_t, int32_t);
VOXLAP_API extern void setcylinder (lpoint3d *, lpoint3d *, int32_t, int32_t, int32_t);
VOXLAP_API extern void setrect (lpoint3d *, lpoint3d *, int32_t);
VOXLAP_API extern void settri (point3d *, point3d *, point3d *, int32_t);
VOXLAP_API extern void setsector (point3d *, int32_t *, int32_t, float, int32_t, int32_t);
VOXLAP_API extern void setspans (vspans *, int32_t, lpoint3d *, int32_t);
VOXLAP_API extern void setheightmap (const unsigned char *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void setkv6 (vx5sprite *, int32_t);

	//VXL writing functions (slow or buggy):
VOXLAP_API extern void sethull3d (point3d *, int32_t, int32_t, int32_t);
VOXLAP_API extern void setlathe (point3d *, int32_t, int32_t, int32_t);
VOXLAP_API extern void setblobs (point3d *, int32_t, int32_t, int32_t);
VOXLAP_API extern void setfloodfill3d (int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void sethollowfill ();
VOXLAP_API extern void setkvx (const char *, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void setflash (float, float, float, int32_t, int32_t, int32_t);
VOXLAP_API extern void setnormflash (float, float, float, int32_t, int32_t);

	//VXL MISC functions:
VOXLAP_API extern void updatebbox (int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void updatevxl ();
VOXLAP_API extern void genmipvxl (int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void updatelighting (int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);

	//Falling voxels functions:
VOXLAP_API extern void checkfloatinbox (int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void startfalls ();
VOXLAP_API extern void dofall (int32_t);
VOXLAP_API extern int32_t meltfall (vx5sprite *, int32_t, int32_t);
VOXLAP_API extern void finishfalls ();

	//Procedural texture functions:
VOXLAP_API extern int32_t curcolfunc (lpoint3d *);
VOXLAP_API extern int32_t floorcolfunc (lpoint3d *);
VOXLAP_API extern int32_t jitcolfunc (lpoint3d *);
VOXLAP_API extern int32_t manycolfunc (lpoint3d *);
VOXLAP_API extern int32_t sphcolfunc (lpoint3d *);
VOXLAP_API extern int32_t woodcolfunc (lpoint3d *);
VOXLAP_API extern int32_t pngcolfunc (lpoint3d *);
VOXLAP_API extern int32_t kv6colfunc (lpoint3d *);

	//Editing backup/restore functions
VOXLAP_API extern void voxbackup (int32_t, int32_t, int32_t, int32_t, int32_t);
VOXLAP_API extern void voxdontrestore ();
VOXLAP_API extern void voxrestore ();
VOXLAP_API extern void voxredraw ();

	//High-level (easy) picture loading function:
VOXLAP_API extern void kpzload (const char *, intptr_t *, int32_t *, int32_t *, int32_t *);
	//Low-level PNG/JPG functions:
VOXLAP_API extern void kpgetdim (const char *, int32_t, int32_t *, int32_t *);
VOXLAP_API extern int32_t kprender (const char *, int32_t, intptr_t, int32_t, int32_t, int32_t, int32_t, int32_t);

	//ZIP functions:
VOXLAP_API extern int32_t kzaddstack (const char *);
VOXLAP_API extern void kzuninit ();
VOXLAP_API extern intptr_t kzopen (const char *); //returns FILE* cast to intptr_t (or 0)
VOXLAP_API extern int32_t kzread (void *, int32_t);
VOXLAP_API extern int32_t kzfilelength ();
VOXLAP_API extern int32_t kzseek (int32_t, int32_t);
VOXLAP_API extern int32_t kztell ();
VOXLAP_API extern int32_t kzgetc ();
VOXLAP_API extern int32_t kzeof ();
VOXLAP_API extern void kzclose ();

VOXLAP_API extern void kzfindfilestart (const char *); //pass wildcard string
VOXLAP_API extern int32_t kzfindfile (char *); //you alloc buf, returns 1:found,0:~found


#endif
