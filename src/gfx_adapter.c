/* F3DZEX2 display-list interpreter: walks Link's flex skeleton exactly like
 * SkelAnime_DrawFlexLod (real Player_OverrideLimbDrawGameplayDefault included,
 * so hands/sheath/shield models follow equipment), executes each limb's
 * display list and emits world-space triangles into OoTLinkGeometryBuffers.
 *
 * Display lists live in two address domains, switched per stream:
 *   - 0x06xxxxxx segment tokens -> the active link object blob (big-endian)
 *   - native pointers           -> generated wrapper arrays (host Gfx)
 *   - 0x0Dxxxxxx (G_MTX only)   -> the flex matrix buffer
 * Textures are decoded from the object blob into an RGBA32 cache (TMEM-lite):
 * physical dims come from the SETTILE mask period, seg 8/9 face SETTIMGs are
 * rewritten per frame to the selected eye/mouth offsets in seg 6, and tris
 * also carry material color (env = tunic) + normals for lighting.
 */
#include "liboot.h"
#include "liboot_assets.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ultra64.h"
#include "animation.h"
#include "gfx.h"
#include "player.h"
#include "play_state.h"
#include "save.h"
#include "sys_matrix.h"
/* liboot v0.4: EnElf layout for Navi wing rendering (vendored header) */
#include "decomp/src/overlays/actors/ovl_En_Elf/z_en_elf.h"

extern s32 Player_OverrideLimbDrawGameplayDefault( PlayState *play, s32 limbIndex, Gfx **dList,
                                                   Vec3f *pos, Vec3s *rot, void *thisx );

#define FLEX_MAX_MTX  24
#define VTX_CACHE     32
#define MAX_DEPTH     16
#define MAX_CMDS      100000
#define MAX_TEXTURE_DIMENSION 1024u
#define MAX_TEXTURE_PIXELS (1024u * 1024u)
#define LINK_RENDER_LIMBS 21
#define NAVI_RENDER_LIMBS 14

typedef struct {
    float pos[3];
    float normal[3];
    float color[4];
    float st[2];                  /* raw texel coords (s10.5 / 32) */
    u8 lit;                       /* G_LIGHTING state when this vertex loaded */
} XVtx;

/* ---- runtime texture cache (decoded from the caller's ROM) ------------- */
/* Raised for full multi-room dungeon loads: every room's distinct seg-3
   textures are cached at once (see roomGen below). */
#define MAX_TEXTURES 1024
typedef struct {
    u32 dataAddr, palAddr;
    u8 fmt, siz;
    u16 width, height;            /* physical dims (mask period), not tilesize */
    u8 wrapS, wrapT;
    u8 palette;                   /* CI4 palette bank nibble */
    u8 age;                       /* linkAge when decoded: seg6 tokens collide across ages */
    u32 roomGen;                  /* per-room key: seg-3 addresses alias across rooms in a
                                     full-dungeon load, so room textures are disambiguated
                                     by room (0 = scene-shared seg-2 or non-room texture) */
    u32 revision;
    u8 *rgba;
} TexEntry;

/* liboot vNEXT: current room discriminator for the texture cache. Set per room
   while interpreting a full multi-room scene; reset to 0 (scene-shared) on each
   scene load. Only seg-3 (per-room) textures use it. */
static u32 sRoomGen;
void liboot_gfx_set_room( int roomIdx ) { sRoomGen = (u32)( roomIdx + 1 ); }

typedef struct {
    u8 fmt, siz, wrapS, wrapT, palette;
    u8 maskS, maskT, shiftS, shiftT;
    u16 width, height;
    u16 tmem;                     /* TMEM word address this tile reads from */
    float uls, ult;
    int valid;
} TileState;

/* liboot v0.7: TMEM-keyed load tracking. Room materials load two textures at
   different TMEM addresses (tile 0 + tile 1); "last LOADBLOCK wins" bound
   tile 0 to the blend layer for ~24% of scene tris. Each LOADBLOCK records
   which TMEM address its data landed at (the load tile's tmem), and the bind
   resolves the render tile's tmem through this map. Link's materials all
   load at tmem 0, so the map degenerates to the old behavior for him. */
#define TMEM_MAP_SLOTS 8
typedef struct {
    u16 tmem;
    u32 addr;
    u8 valid;
} TmemSlot;
static TexEntry sTexCache[MAX_TEXTURES];
static int sTexCount;
/* Monotonic across the whole process, NOT reset by terminate: after a
   re-init a reused index gets a strictly larger revision, so consumers that
   cache uploads by (index, revision) always see "pixels changed". */
static u32 sTexRevisionCounter;

/* oot_global_terminate hook: drop every decoded texture (liboot.h contract:
   indices stay stable only until oot_global_terminate). */
void liboot_gfx_terminate( void )
{
    for( int i = 0; i < sTexCount; ++i )
        free( sTexCache[i].rgba );
    memset( sTexCache, 0, sizeof( sTexCache ));
    sTexCount = 0;
}

static struct {
    MtxF flexMtx[FLEX_MAX_MTX];
    int flexCount;
    MtxF cur;                     /* matrix used for vertex loads */
    XVtx vtx[VTX_CACHE];
    float envColor[4], primColor[4];
    int lighting;
    u8 cullMode;                  /* OoTTriangleFlags cull bits from G_GEOMETRYMODE */
    u8 otherFlags;                /* OoTTriangleFlags alpha-test/decal bits from othermode */
    int texgen;                   /* G_TEXTURE_GEN: derive UV from the normal (env-map) */
    u32 otherLo;                  /* tracked G_SETOTHERMODE_L word (alpha-compare/zmode/rendermode) */
    long cmdBudget;
    struct OoTLinkGeometryBuffers *out;
    /* the RSP executes DLs only after the whole flex matrix buffer is built,
       so record (dl, matrix) pairs during the walk and run them afterwards */
    struct { uintptr_t dl; int mtxIdx; int limb; } queue[FLEX_MAX_MTX];
    int queueCount;
    float baseColor[3];           /* per-limb material approximation */
    /* texture state */
    u32 timgAddr;                 /* last G_SETTIMG address token */
    u32 curDataAddr, curPalAddr;  /* committed by LOADBLOCK / LOADTLUT */
    u8 tileFmt, tileSiz, wrapS, wrapT, tilePalette;
    u8 maskS, maskT, shiftS, shiftT;
    u16 tileW, tileH;             /* physical dims (mask period wins over tilesize) */
    u16 tileTmem;                 /* render tile's TMEM address (mirror) */
    float tileUls, tileUlt;       /* SETTILESIZE origin, in texels */
    TileState tiles[8];
    u8 textureTile;               /* tile selected by G_TEXTURE */
    TmemSlot tmemMap[TMEM_MAP_SLOTS];
    int tmemNext;
    int maxTris;                  /* per-walk triangle cap (scene > Link) */
    float texScaleS, texScaleT;
    int texEnabled;               /* combiner references TEXEL0/TEXEL1 */
    int envMul;                   /* combiner multiplies texel by ENVIRONMENT */
    int primMul;                  /* combiner multiplies texel by PRIMITIVE */
    int texOn;                    /* G_TEXTURE on/off byte */
    int curTex;                   /* index into sTexCache or -1 */
    int texDirty;                 /* tile/TLUT state changed since last bind */
    const u8 *seg6;               /* stashed for lazy binds at TRI time */
    size_t seg6Size;
    u32 eyeTexAddr, mouthTexAddr; /* seg6 rewrites for seg 8/9, per frame */
} s_gfx;

/* liboot vNEXT: baked vanilla vertex lighting. Kept OUTSIDE s_gfx because both
   DL-begin paths memset s_gfx every frame; the active scene's light settings
   are applied once at load and must survive that. Directions are unit-length,
   world-space, pointing toward the light (same space as the emitted normals).
   `set` is 0 until a scene (or the static world) installs lights, in which case
   compute_shade falls back to a neutral daylight default so nothing goes black. */
static struct {
    float ambient[3];
    float dir[2][3];
    float col[2][3];
    int count;
    int set;
} sLights;

/* Neutral daylight fallback: soft ambient + one key light from the upper front,
   so Link and the arena stay legibly lit when no scene light settings exist. */
static const float kDefaultAmbient[3]   = { 0.46f, 0.46f, 0.52f };
static const float kDefaultLightDir[3]  = { 0.53f, 0.75f, 0.40f };   /* normalized below */
static const float kDefaultLightCol[3]  = { 0.72f, 0.72f, 0.68f };

void liboot_gfx_set_lights( const float ambient[3], const float dir0[3], const float col0[3],
                            const float dir1[3], const float col1[3], int count )
{
    memset( &sLights, 0, sizeof sLights );
    if( count <= 0 || ambient == NULL ) return;   /* set=0 -> compute_shade uses the default */
    if( count > 2 ) count = 2;
    for( int k = 0; k < 3; ++k ) sLights.ambient[k] = ambient ? ambient[k] : 0.0f;
    const float *sd[2] = { dir0, dir1 };
    const float *sc[2] = { col0, col1 };
    for( int l = 0; l < count; ++l ) {
        float dx = sd[l] ? sd[l][0] : 0.0f, dy = sd[l] ? sd[l][1] : 0.0f, dz = sd[l] ? sd[l][2] : 0.0f;
        float len = sqrtf( dx*dx + dy*dy + dz*dz );
        if( len > 1e-6f ) { dx/=len; dy/=len; dz/=len; }
        sLights.dir[l][0] = dx; sLights.dir[l][1] = dy; sLights.dir[l][2] = dz;
        for( int k = 0; k < 3; ++k ) sLights.col[l][k] = sc[l] ? sc[l][k] : 0.0f;
    }
    sLights.count = count;
    sLights.set = 1;
}

/* Replicates the N64 RSP diffuse model: shade = ambient + Σ max(N·Ldir,0)·Lcol,
   clamped to [0,1].  n is already unit-length world space (see mtx_mul_dir). */
static void compute_shade( const float n[3], float out[3] )
{
    float amb[3], dir[2][3], col[2][3];
    int count;
    if( sLights.set ) {
        for( int k = 0; k < 3; ++k ) amb[k] = sLights.ambient[k];
        for( int l = 0; l < sLights.count; ++l )
            for( int k = 0; k < 3; ++k ) { dir[l][k] = sLights.dir[l][k]; col[l][k] = sLights.col[l][k]; }
        count = sLights.count;
    } else {
        float len = sqrtf( kDefaultLightDir[0]*kDefaultLightDir[0] +
                           kDefaultLightDir[1]*kDefaultLightDir[1] +
                           kDefaultLightDir[2]*kDefaultLightDir[2] );
        for( int k = 0; k < 3; ++k ) {
            amb[k] = kDefaultAmbient[k];
            dir[0][k] = kDefaultLightDir[k] / len;
            col[0][k] = kDefaultLightCol[k];
        }
        count = 1;
    }
    for( int k = 0; k < 3; ++k ) out[k] = amb[k];
    for( int l = 0; l < count; ++l ) {
        float d = n[0]*dir[l][0] + n[1]*dir[l][1] + n[2]*dir[l][2];
        if( d < 0.0f ) d = 0.0f;
        for( int k = 0; k < 3; ++k ) out[k] += d * col[l][k];
    }
    for( int k = 0; k < 3; ++k ) out[k] = out[k] < 0.0f ? 0.0f : ( out[k] > 1.0f ? 1.0f : out[k] );
}

static const float sTunicColors[3][3] = {
    { 30/255.0f, 105/255.0f, 27/255.0f },   /* Kokiri */
    { 100/255.0f, 20/255.0f,  0/255.0f },   /* Goron  */
    {  0/255.0f,  60/255.0f, 100/255.0f },  /* Zora   */
};

/* No textures yet, so approximate each limb's dominant material color.
   Indexed by limb-table index (PLAYER_LIMB_* - 1); -1 entries take the tunic
   color. Skin/boots/tights sampled from the game's textures. */
typedef enum { MAT_TUNIC, MAT_SKIN, MAT_WHITE, MAT_BOOT, MAT_LEATHER } LimbMat;
static const LimbMat sLimbMat[21] = {
    MAT_TUNIC,   /*  0 ROOT       */
    MAT_TUNIC,   /*  1 WAIST      */
    MAT_TUNIC,   /*  2 LOWER      */
    MAT_WHITE,   /*  3 R_THIGH    */
    MAT_WHITE,   /*  4 R_SHIN     */
    MAT_BOOT,    /*  5 R_FOOT     */
    MAT_WHITE,   /*  6 L_THIGH    */
    MAT_WHITE,   /*  7 L_SHIN     */
    MAT_BOOT,    /*  8 L_FOOT     */
    MAT_TUNIC,   /*  9 UPPER      */
    MAT_SKIN,    /* 10 HEAD       */
    MAT_TUNIC,   /* 11 HAT        */
    MAT_TUNIC,   /* 12 COLLAR     */
    MAT_WHITE,   /* 13 L_SHOULDER */
    MAT_WHITE,   /* 14 L_FOREARM  */
    MAT_SKIN,    /* 15 L_HAND     */
    MAT_WHITE,   /* 16 R_SHOULDER */
    MAT_WHITE,   /* 17 R_FOREARM  */
    MAT_SKIN,    /* 18 R_HAND     */
    MAT_LEATHER, /* 19 SHEATH     */
    MAT_TUNIC,   /* 20 TORSO      */
};
static const float sMatColors[5][3] = {
    { 0, 0, 0 },                              /* MAT_TUNIC: filled at runtime */
    { 246/255.0f, 207/255.0f, 174/255.0f },   /* MAT_SKIN                     */
    { 225/255.0f, 225/255.0f, 225/255.0f },   /* MAT_WHITE (sleeves/tights)   */
    { 93/255.0f,  44/255.0f,  18/255.0f },    /* MAT_BOOT                     */
    { 90/255.0f,  60/255.0f,  30/255.0f },    /* MAT_LEATHER (sheath/straps)  */
};

/* face table used when the animation frame carries no eye/mouth index */
extern PlayerFaceIndices sPlayerFaces[PLAYER_FACE_MAX];

static const u8 *resolve_tex_addr( u32 addr, size_t need, const u8 *blob, size_t blobSize )
{
    u32 seg = addr >> 24, off = addr & 0x00FFFFFF;
    switch( seg ) {
    case 0x06:
        return ( blob && off <= blobSize && need <= blobSize - off ) ? blob + off : NULL;
    case 0x04: { /* gameplay_keep (head, gauntlets, mirror shield) */
        size_t kpSize;
        const u8 *kp = liboot_segment_base( 4, &kpSize );
        return ( kp && off <= kpSize && need <= kpSize - off ) ? kp + off : NULL;
    }
    case 0x02:   /* liboot v0.7: scene file (CI TLUTs, some room texel data) */
    case 0x03: { /* liboot v0.7: room file (most room texel data) */
        size_t sSize;
        const u8 *s = liboot_segment_base( seg, &sSize );
        return ( s && off <= sSize && need <= sSize - off ) ? s + off : NULL;
    }
    default:
        /* seg 8/9 (eyes/mouth) are rewritten to seg 6 at G_SETTIMG time */
        return NULL;
    }
}

/* TMEM map: addr most recently loaded at this TMEM word address */
static void tmem_record( u16 tmem, u32 addr )
{
    for( int i = 0; i < TMEM_MAP_SLOTS; ++i ) {
        if( s_gfx.tmemMap[i].valid && s_gfx.tmemMap[i].tmem == tmem ) {
            s_gfx.tmemMap[i].addr = addr;
            return;
        }
    }
    TmemSlot *slot = &s_gfx.tmemMap[s_gfx.tmemNext];
    s_gfx.tmemNext = ( s_gfx.tmemNext + 1 ) % TMEM_MAP_SLOTS;
    slot->tmem = tmem;
    slot->addr = addr;
    slot->valid = 1;
}

static u32 tmem_lookup( u16 tmem )
{
    for( int i = 0; i < TMEM_MAP_SLOTS; ++i )
        if( s_gfx.tmemMap[i].valid && s_gfx.tmemMap[i].tmem == tmem )
            return s_gfx.tmemMap[i].addr;
    return s_gfx.curDataAddr;   /* fallback: last LOADBLOCK (pre-map behavior) */
}

static u32 be32g( const u8 *p ) { return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]; }
static s16 be16g( const u8 *p ) { return (s16)(((u16)p[0]<<8) | p[1]); }

static void mtx_mul_point( const MtxF *m, float x, float y, float z, float *out )
{
    out[0] = m->xw + m->xx*x + m->xy*y + m->xz*z;
    out[1] = m->yw + m->yx*x + m->yy*y + m->yz*z;
    out[2] = m->zw + m->zx*x + m->zy*y + m->zz*z;
}

static void mtx_mul_dir( const MtxF *m, float x, float y, float z, float *out )
{
    out[0] = m->xx*x + m->xy*y + m->xz*z;
    out[1] = m->yx*x + m->yy*y + m->yz*z;
    out[2] = m->zx*x + m->zy*y + m->zz*z;
    float l = sqrtf( out[0]*out[0] + out[1]*out[1] + out[2]*out[2] );
    if( l > 1e-6f ) { out[0]/=l; out[1]/=l; out[2]/=l; }
}

static void expand_rgba16( u16 c, u8 *out )
{
    out[0] = (( c >> 11 ) & 0x1F ) * 255 / 31;
    out[1] = (( c >> 6 ) & 0x1F ) * 255 / 31;
    out[2] = (( c >> 1 ) & 0x1F ) * 255 / 31;
    out[3] = ( c & 1 ) ? 255 : 0;
}

/* decode any of Link's texel formats to RGBA32 */
static int decode_texture( TexEntry *t, const u8 *data, const u8 *pal )
{
    size_t n;

    if( t == NULL || data == NULL ) return 0;
    n = (size_t)t->width * t->height;
    if( n == 0u || n > MAX_TEXTURE_PIXELS || n > SIZE_MAX / 4u ) return 0;
    t->rgba = malloc( n * 4u );
    if( !t->rgba ) return 0;

    for( size_t i = 0; i < n; ++i ) {
        u8 *o = t->rgba + i * 4;
        switch(( t->fmt << 4 ) | t->siz ) {
        case 0x02: /* RGBA16 */
            expand_rgba16(( (u16)data[i*2] << 8 ) | data[i*2+1], o );
            break;
        case 0x21: { /* CI8 */
            u8 ci = data[i];
            expand_rgba16(( (u16)pal[ci*2] << 8 ) | pal[ci*2+1], o );
            break;
        }
        case 0x20: { /* CI4 */
            u8 ci = ( i & 1 ) ? data[i/2] & 0xF : data[i/2] >> 4;
            const u8 *pp = pal + ( t->palette * 16 + ci ) * 2;
            expand_rgba16(( (u16)pp[0] << 8 ) | pp[1], o );
            break;
        }
        case 0x32: /* IA16 */
            o[0] = o[1] = o[2] = data[i*2];
            o[3] = data[i*2+1];
            break;
        case 0x31: /* IA8: I4 A4 */
            o[0] = o[1] = o[2] = ( data[i] >> 4 ) * 17;
            o[3] = ( data[i] & 0xF ) * 17;
            break;
        case 0x30: { /* IA4: I3 A1 per nibble */
            u8 x = ( i & 1 ) ? data[i/2] & 0xF : data[i/2] >> 4;
            o[0] = o[1] = o[2] = ( x >> 1 ) * 255 / 7;
            o[3] = ( x & 1 ) ? 255 : 0;
            break;
        }
        case 0x41: /* I8 */
            o[0] = o[1] = o[2] = o[3] = data[i];
            break;
        case 0x40: { /* I4 */
            u8 x = ( i & 1 ) ? data[i/2] & 0xF : data[i/2] >> 4;
            o[0] = o[1] = o[2] = o[3] = x * 17;
            break;
        }
        default:
            o[0] = o[2] = 255; o[1] = 0; o[3] = 255; /* magenta = unsupported */
            break;
        }
    }
    return 1;
}

/* commit current tile state into a cache entry, decoding on first sight */
static void select_texture( const u8 *blob, size_t blobSize )
{
    u8 age = (u8)gSaveContext.save.linkAge;
    /* the TLUT only matters for CI formats; keying non-CI entries on stale
       LOADTLUT state would duplicate them */
    u32 palKey = s_gfx.tileFmt == 2 ? s_gfx.curPalAddr : 0;
    /* liboot v0.7: the render tile reads the load that landed at its TMEM
       address, not the last LOADBLOCK (two-tile room materials) */
    u32 dataAddr = tmem_lookup( s_gfx.tileTmem );
    /* seg-3 (per-room) textures alias across rooms in a full-dungeon load; key
       them by the current room. Scene-shared (seg-2) and others stay gen 0. */
    u32 gen = ( ( dataAddr >> 24 ) == 0x03u ) ? sRoomGen : 0u;

    s_gfx.curTex = -1;
    if( !dataAddr || !s_gfx.tileW || !s_gfx.tileH ||
        s_gfx.tileW > MAX_TEXTURE_DIMENSION ||
        s_gfx.tileH > MAX_TEXTURE_DIMENSION ||
        (size_t)s_gfx.tileW * s_gfx.tileH > MAX_TEXTURE_PIXELS ) return;

    /* Only formats decoded below are accepted. Reject malformed/unsupported
       pairs before computing source spans or allocating cache storage. */
    switch(( s_gfx.tileFmt << 4 ) | s_gfx.tileSiz ) {
    case 0x02: /* RGBA16 */
    case 0x20: /* CI4 */
    case 0x21: /* CI8 */
    case 0x30: /* IA4 */
    case 0x31: /* IA8 */
    case 0x32: /* IA16 */
    case 0x40: /* I4 */
    case 0x41: /* I8 */
        break;
    default:
        return;
    }

    for( int i = 0; i < sTexCount; ++i ) {
        TexEntry *t = &sTexCache[i];
        if( t->rgba && t->dataAddr == dataAddr && t->palAddr == palKey &&
            t->fmt == s_gfx.tileFmt && t->siz == s_gfx.tileSiz &&
            t->width == s_gfx.tileW && t->height == s_gfx.tileH &&
            t->wrapS == s_gfx.wrapS && t->wrapT == s_gfx.wrapT &&
            t->palette == s_gfx.tilePalette && t->age == age &&
            t->roomGen == gen ) {
            s_gfx.curTex = i;
            return;
        }
    }

    /* liboot v0.7: reuse slots vacated by liboot_gfx_evict_scene so scene
       reloads don't burn through the cache (indices stay stable, revision
       bumps tell consumers the pixels changed) */
    int slot = -1;
    for( int i = 0; i < sTexCount; ++i )
        if( !sTexCache[i].rgba ) { slot = i; break; }
    if( slot < 0 ) {
        if( sTexCount >= MAX_TEXTURES ) return;
        slot = sTexCount;
    }

    TexEntry *t = &sTexCache[slot];
    memset( t, 0, sizeof( *t ));
    t->dataAddr = dataAddr;
    t->palAddr = palKey;
    t->fmt = s_gfx.tileFmt;
    t->siz = s_gfx.tileSiz;
    t->width = s_gfx.tileW;
    t->height = s_gfx.tileH;
    t->wrapS = s_gfx.wrapS;
    t->wrapT = s_gfx.wrapT;
    t->palette = s_gfx.tilePalette;
    t->age = age;
    t->roomGen = gen;

    size_t bpt = t->siz == 2 ? 2 : 1;                 /* bytes per texel (>=4bpp) */
    size_t need = (size_t)t->width * t->height * bpt;
    if( t->siz == 0 ) need = ( (size_t)t->width * t->height + 1 ) / 2;
    const u8 *data = resolve_tex_addr( t->dataAddr, need, blob, blobSize );
    const u8 *pal = t->fmt == 2 ? resolve_tex_addr( t->palAddr, 512, blob, blobSize ) : NULL;
    if( !data || ( t->fmt == 2 && !pal ) || !decode_texture( t, data, pal )) {
        memset( t, 0, sizeof( *t ));   /* keep reused slots recognizably free */
        return;
    }
    t->revision = ++sTexRevisionCounter;

    if( getenv( "LIBOOT_TRACE" ))
        fprintf( stderr, "[T] tex%02d data=%08x pal=%08x fmt=%d siz=%d %dx%d cm=%d,%d\n",
                 slot, t->dataAddr, t->palAddr, t->fmt, t->siz,
                 t->width, t->height, t->wrapS, t->wrapT );
    s_gfx.curTex = slot;
    if( slot == sTexCount ) sTexCount++;
}

/* liboot v0.7 (scene loader hook): drop cache entries decoded from segments
   2/3 — their addresses would alias the newly loaded scene/room files. Slots
   are recycled by select_texture; indices of surviving entries are stable. */
void liboot_gfx_evict_scene( void )
{
    sRoomGen = 0;   /* next load starts scene-shared until set per room */
    for( int i = 0; i < sTexCount; ++i ) {
        u32 seg = sTexCache[i].dataAddr >> 24;
        if( sTexCache[i].rgba && ( seg == 0x02 || seg == 0x03 )) {
            free( sTexCache[i].rgba );
            memset( &sTexCache[i], 0, sizeof( sTexCache[i] ));
        }
    }
}

/* one interpretation stream: either blob offset or native Gfx* */
typedef struct {
    const u8 *blob; size_t blobSize;  /* domain for 0x06 tokens */
    int isBlob;
    const u8 *bp;                     /* big-endian stream cursor */
    const u8 *bpEnd;                  /* one-past mapped segment data */
    const Gfx *np;                    /* native stream cursor */
} Stream;

static int stream_open( Stream *st, uintptr_t addr, const u8 *blob, size_t blobSize )
{
    if( addr >= 0x06000000u && addr <= 0x06FFFFFFu ) {
        u32 off = addr & 0x00FFFFFF;
        if( !blob || off >= blobSize ) return 0;
        st->isBlob = 1;
        st->bp = blob + off;
        st->bpEnd = blob + blobSize;
    } else if( addr >= 0x04000000u && addr <= 0x04FFFFFFu ) {
        /* liboot v0.4: gameplay_keep DLs (fairy wings, reticle, shadows...)
           run from the extracted segment-4 blob; the stream keeps the seg-6
           domain for vertex loads, load_vtx resolves 0x04 itself. */
        size_t kpSize;
        const u8 *kp = liboot_segment_base( 4, &kpSize );
        u32 off = addr & 0x00FFFFFF;
        if( !kp || off >= kpSize ) return 0;
        st->isBlob = 1;
        st->bp = kp + off;
        st->bpEnd = kp + kpSize;
    } else if( addr >= 0x02000000u && addr <= 0x03FFFFFFu ) {
        /* liboot v0.7: scene (2) / room (3) mesh DLs; load_vtx and
           resolve_tex_addr resolve 0x02/0x03 tokens themselves. Animated
           draw-config segments (8/9/A/B...) stay unmapped: those G_DL calls
           fail here and the parent DL simply continues. */
        size_t sSize;
        const u8 *s = liboot_segment_base( (int)( addr >> 24 ), &sSize );
        u32 off = addr & 0x00FFFFFF;
        if( !s || off >= sSize ) return 0;
        st->isBlob = 1;
        st->bp = s + off;
        st->bpEnd = s + sSize;
    } else if( addr > 0xFFFFFFFFu ) {
        /* native pointer (generated wrapper array) */
        st->isBlob = 0;
        st->np = (const Gfx *)addr;
    } else {
        return 0; /* unmapped segment token */
    }
    st->blob = blob;
    st->blobSize = blobSize;
    return 1;
}

static int stream_fetch( Stream *st, u32 *w0, u32 *w1 )
{
    if( st->isBlob ) {
        if( st->bp > st->bpEnd || (size_t)( st->bpEnd - st->bp ) < 8u ) return 0;
        *w0 = be32g( st->bp );
        *w1 = be32g( st->bp + 4 );
        st->bp += 8;
    } else {
        *w0 = st->np->words.w0;
        *w1 = (u32)(uintptr_t)st->np->words.w1;  /* wrappers store tokens */
        st->np++;
    }
    return 1;
}

static void load_vtx( Stream *st, u32 w0, u32 w1 )
{
    int n = ( w0 >> 12 ) & 0xFF;
    int v0 = (( w0 >> 1 ) & 0x7F ) - n;
    const u8 *base;
    size_t baseSize;
    if( v0 < 0 || v0 + n > VTX_CACHE ) return;
    /* liboot v0.4: vertices live in seg 6 (link objects) or seg 4
       (gameplay_keep: fairy wings, reticle triangle, ...);
       liboot v0.7: scene (2) / room (3) mesh vertices */
    switch( w1 >> 24 ) {
    case 0x06: base = st->blob; baseSize = st->blobSize; break;
    case 0x04: base = liboot_segment_base( 4, &baseSize ); break;
    case 0x02: base = liboot_segment_base( 2, &baseSize ); break;
    case 0x03: base = liboot_segment_base( 3, &baseSize ); break;
    default:   return;
    }
    u32 off = w1 & 0x00FFFFFF;
    if( !base || off + (u32)n * 16 > baseSize ) return;

    const u8 *p = base + off;
    for( int i = 0; i < n; ++i, p += 16 ) {
        XVtx *v = &s_gfx.vtx[v0 + i];
        float x = be16g( p ), y = be16g( p+2 ), z = be16g( p+4 );
        mtx_mul_point( &s_gfx.cur, x, y, z, v->pos );
        v->st[0] = be16g( p+8 ) / 32.0f;   /* s10.5 -> texels */
        v->st[1] = be16g( p+10 ) / 32.0f;
        v->lit = s_gfx.lighting != 0;
        if( s_gfx.lighting ) {
            mtx_mul_dir( &s_gfx.cur, (s8)p[12], (s8)p[13], (s8)p[14], v->normal );
        } else {
            v->normal[0] = 0; v->normal[1] = 1; v->normal[2] = 0;
            v->color[0] = p[12]/255.0f; v->color[1] = p[13]/255.0f; v->color[2] = p[14]/255.0f;
        }
        v->color[3] = p[15]/255.0f;
    }
}

/* RDP tile-coordinate shift: 1..10 divide, 11..15 multiply */
static float shift_factor( u8 shift )
{
    if( shift == 0 ) return 1.0f;
    if( shift <= 10 ) return 1.0f / (float)( 1u << shift );
    return (float)( 1u << ( 16 - shift ));
}

static void emit_tri( int a, int b, int c )
{
    struct OoTLinkGeometryBuffers *o = s_gfx.out;
    if( !o || o->numTrianglesUsed >= s_gfx.maxTris ) return;
    if( a >= VTX_CACHE || b >= VTX_CACHE || c >= VTX_CACHE ) return;
    /* Lazy bind: real hardware pairs texel data + TLUT from TMEM state at
       draw time.  Several materials (hands/gauntlet/shield/sheath family)
       run LOADTLUT *after* SETTILESIZE but before their triangles, so
       binding at SETTILESIZE picked up the previous material's palette.
       Resolve only here, once all loads for this material have committed. */
    if( s_gfx.texDirty && s_gfx.texEnabled && s_gfx.texOn ) {
        select_texture( s_gfx.seg6, s_gfx.seg6Size );
        s_gfx.texDirty = 0;
    }
    int base = o->numTrianglesUsed * 3;
    const int idx[3] = { a, b, c };
    int tex = ( s_gfx.texEnabled && s_gfx.texOn && s_gfx.curTex >= 0 ) ? s_gfx.curTex : -1;
    for( int i = 0; i < 3; ++i ) {
        const XVtx *v = &s_gfx.vtx[idx[i]];
        if( o->position ) memcpy( o->position + ( base+i )*3, v->pos, 12 );
        if( o->normal )   memcpy( o->normal   + ( base+i )*3, v->normal, 12 );
        if( o->color ) {
            float *color = o->color + ( base+i )*3;
            if( v->lit ) {
                /* Vanilla shade: the RSP lights the vertex normal, and the
                   combiner then modulates the texel (or the per-limb material)
                   by that shade.  Replaces the old fullbright white/baseColor. */
                float shade[3];
                compute_shade( v->normal, shade );
                if( s_gfx.texEnabled && s_gfx.texOn ) {
                    color[0] = shade[0];
                    color[1] = shade[1];
                    color[2] = shade[2];
                    if( s_gfx.envMul ) {
                        color[0] *= s_gfx.envColor[0];
                        color[1] *= s_gfx.envColor[1];
                        color[2] *= s_gfx.envColor[2];
                    }
                    if( s_gfx.primMul ) {
                        color[0] *= s_gfx.primColor[0];
                        color[1] *= s_gfx.primColor[1];
                        color[2] *= s_gfx.primColor[2];
                    }
                } else {
                    color[0] = shade[0] * s_gfx.baseColor[0] * s_gfx.primColor[0];
                    color[1] = shade[1] * s_gfx.baseColor[1] * s_gfx.primColor[1];
                    color[2] = shade[2] * s_gfx.baseColor[2] * s_gfx.primColor[2];
                }
            } else {
                memcpy( color, v->color, 3 * sizeof( *color ));
            }
        }
        /* liboot vNEXT: optional parallel per-vertex shade alpha (color stays
           3 floats RGB for ABI). The alpha byte is already loaded into
           v->color[3]; emit it only when the caller supplied a buffer. */
        if( o->alpha ) o->alpha[base+i] = v->color[3];
        if( o->uv ) {
            float uu, vv;
            if( s_gfx.texgen ) {
                /* G_TEXTURE_GEN linear env-map: the RSP replaces ST with coords
                   from the transformed normal. v->normal is already unit and
                   world-space; map x/y into [0,1] (default look-at = world axes).
                   Emitted normalized, so it bypasses the texel/shift pipeline. */
                uu = v->normal[0] * 0.5f + 0.5f;
                vv = v->normal[1] * 0.5f + 0.5f;
            } else {
                uu = v->st[0] * s_gfx.texScaleS * shift_factor( s_gfx.shiftS ) - s_gfx.tileUls;
                vv = v->st[1] * s_gfx.texScaleT * shift_factor( s_gfx.shiftT ) - s_gfx.tileUlt;
                if( tex >= 0 ) { uu /= sTexCache[tex].width; vv /= sTexCache[tex].height; }
            }
            o->uv[( base+i )*2] = uu;
            o->uv[( base+i )*2+1] = vv;
        }
    }
    if( o->triTexture )
        o->triTexture[o->numTrianglesUsed] = tex >= 0 ? (u16)tex : 0xFFFF;
    if( o->triFlags )
        o->triFlags[o->numTrianglesUsed] = s_gfx.cullMode | s_gfx.otherFlags;
    o->numTrianglesUsed++;
}

static void run_dl( uintptr_t addr, int depth )
{
    Stream st;
    size_t blobSize;
    const u8 *blob = liboot_segment_base( 6, &blobSize );
    if( depth > MAX_DEPTH || !stream_open( &st, addr, blob, blobSize )) return;
    s_gfx.seg6 = blob;            /* for lazy texture binds in emit_tri */
    s_gfx.seg6Size = blobSize;

    while( s_gfx.cmdBudget-- > 0 ) {
        u32 w0, w1;
        if( !stream_fetch( &st, &w0, &w1 )) return;
        u8 op = w0 >> 24;

        switch( op ) {
        case 0x01: /* G_VTX */
            load_vtx( &st, w0, w1 );
            break;
        case 0x05: /* G_TRI1 */
            emit_tri(( w0 >> 17 ) & 0x7F, ( w0 >> 9 ) & 0x7F, ( w0 >> 1 ) & 0x7F );
            break;
        case 0x06: /* G_TRI2 */
            emit_tri(( w0 >> 17 ) & 0x7F, ( w0 >> 9 ) & 0x7F, ( w0 >> 1 ) & 0x7F );
            emit_tri(( w1 >> 17 ) & 0x7F, ( w1 >> 9 ) & 0x7F, ( w1 >> 1 ) & 0x7F );
            break;
        case 0xDE: /* G_DL */
            if((( w0 >> 16 ) & 0xFF ) == 0x01 ) {  /* branch, no return */
                if( !stream_open( &st, (uintptr_t)w1, blob, blobSize )) return;
            } else {
                run_dl(( uintptr_t )w1, depth + 1 );
            }
            break;
        case 0xDF: /* G_ENDDL */
            return;
        case 0xDA: /* G_MTX */
            if( w1 >= 0x0D000000u && w1 <= 0x0DFFFFFFu ) {
                u32 idx = ( w1 & 0x00FFFFFF ) / 0x40;
                if( idx < (u32)s_gfx.flexCount )
                    s_gfx.cur = s_gfx.flexMtx[idx];
            }
            break;
        case 0xD9: { /* G_GEOMETRYMODE: w0 low 24 = ~clearBits, w1 = set bits */
            u32 clearBits = ~w0 & 0x00FFFFFF;
            if( clearBits & 0x00020000 ) s_gfx.lighting = 0;  /* G_LIGHTING */
            if( w1 & 0x00020000 ) s_gfx.lighting = 1;
            /* G_CULL_FRONT 0x1000 / G_CULL_BACK 0x2000 (F3DEX2): clear then set. */
            if( clearBits & 0x00001000 ) s_gfx.cullMode &= (u8)~OOT_TRI_CULL_FRONT;
            if( clearBits & 0x00002000 ) s_gfx.cullMode &= (u8)~OOT_TRI_CULL_BACK;
            if( w1 & 0x00001000 ) s_gfx.cullMode |= OOT_TRI_CULL_FRONT;
            if( w1 & 0x00002000 ) s_gfx.cullMode |= OOT_TRI_CULL_BACK;
            /* G_TEXTURE_GEN 0x40000: reflective/env-mapped materials (Mirror
               Shield, metallic blades) derive their UV from the vertex normal. */
            if( clearBits & 0x00040000 ) s_gfx.texgen = 0;
            if( w1 & 0x00040000 ) s_gfx.texgen = 1;
            break;
        }
        case 0xE2: { /* G_SETOTHERMODE_L: partial write of the othermode low word.
                        w0 = (cmd<<24)|((32-sft-len)<<8)|(len-1), w1 = shifted data.
                        The low word carries alpha-compare (bits 0-1), z-mode
                        (bits 10-11) and the render/blend mode (bits 3-31). */
            u32 len = ( w0 & 0xFF ) + 1u;
            u32 sft = 32u - len - (( w0 >> 8 ) & 0xFF );
            u32 mask = ( len >= 32u ) ? 0xFFFFFFFFu : ((( 1u << len ) - 1u ) << sft );
            s_gfx.otherLo = ( s_gfx.otherLo & ~mask ) | ( w1 & mask );
            /* Alpha-test cutout: threshold alpha-compare, or a TEX_EDGE-style
               render mode that multiplies coverage by alpha (CVG_X_ALPHA). */
            u8 of = 0;
            if(( s_gfx.otherLo & 0x3u ) == 0x1u || ( s_gfx.otherLo & 0x1000u ))
                of |= OOT_TRI_ALPHA_TEST;
            if(( s_gfx.otherLo & 0x0C00u ) == 0x0C00u )   /* ZMODE_DEC */
                of |= OOT_TRI_DECAL;
            s_gfx.otherFlags = of;
            break;
        }
        case 0xFA: /* G_SETPRIMCOLOR */
            s_gfx.primColor[0] = (( w1 >> 24 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[1] = (( w1 >> 16 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[2] = (( w1 >> 8 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[3] = ( w1 & 0xFF ) / 255.0f;
            break;
        case 0xFB: /* G_SETENVCOLOR */
            s_gfx.envColor[0] = (( w1 >> 24 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[1] = (( w1 >> 16 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[2] = (( w1 >> 8 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[3] = ( w1 & 0xFF ) / 255.0f;
            break;
        case 0xFD: /* G_SETTIMG; seg 8/9 = per-frame eye/mouth -> seg6 offset */
            if(( w1 >> 24 ) == 0x08 )      w1 = s_gfx.eyeTexAddr + ( w1 & 0x00FFFFFF );
            else if(( w1 >> 24 ) == 0x09 ) w1 = s_gfx.mouthTexAddr + ( w1 & 0x00FFFFFF );
            s_gfx.timgAddr = w1;
            break;
        case 0xF3: { /* G_LOADBLOCK: commits the last SETTIMG as texel data,
                        keyed by the TMEM address the load tile points at */
            u8 loadTile = ( w1 >> 24 ) & 0x7;
            tmem_record( s_gfx.tiles[loadTile].tmem, s_gfx.timgAddr );
            s_gfx.curDataAddr = s_gfx.timgAddr;   /* map-miss fallback */
            s_gfx.texDirty = 1;
            break;
        }
        case 0xF0: /* G_LOADTLUT: commits the last SETTIMG as palette */
            s_gfx.curPalAddr = s_gfx.timgAddr;
            s_gfx.texDirty = 1;
            break;
        case 0xF5: { /* G_SETTILE */
            u8 tile = ( w1 >> 24 ) & 0x7;
            TileState *t = &s_gfx.tiles[tile];
            t->fmt = ( w0 >> 21 ) & 0x7;
            t->siz = ( w0 >> 19 ) & 0x3;
            t->tmem = w0 & 0x1FF;
            t->palette = ( w1 >> 20 ) & 0xF;
            t->wrapT = ( w1 >> 18 ) & 0x3;   /* bit0 mirror, bit1 clamp */
            t->maskT = ( w1 >> 14 ) & 0xF;
            t->shiftT = ( w1 >> 10 ) & 0xF;
            t->wrapS = ( w1 >> 8 ) & 0x3;
            t->maskS = ( w1 >> 4 ) & 0xF;
            t->shiftS = w1 & 0xF;
            t->valid = 1;
            if( tile == s_gfx.textureTile ) {
                s_gfx.tileFmt = t->fmt;
                s_gfx.tileSiz = t->siz;
                s_gfx.tileTmem = t->tmem;
                s_gfx.tilePalette = t->palette;
                s_gfx.wrapT = t->wrapT;
                s_gfx.maskT = t->maskT;
                s_gfx.shiftT = t->shiftT;
                s_gfx.wrapS = t->wrapS;
                s_gfx.maskS = t->maskS;
                s_gfx.shiftS = t->shiftS;
                s_gfx.texDirty = 1;
            }
            break;
        }
        case 0xF2: /* G_SETTILESIZE on render tile: dims known.  Physical size
                      is the mask period; the tilesize is often a mirrored/
                      wrapped multiple of it (never read past the end).  The
                      actual bind is deferred to emit_tri: some materials load
                      their TLUT only after this command. */
            {
                u8 tile = ( w1 >> 24 ) & 0x7;
                TileState *t = &s_gfx.tiles[tile];
                u16 uls = ( w0 >> 12 ) & 0xFFF, ult = w0 & 0xFFF;
                u16 lrs = ( w1 >> 12 ) & 0xFFF, lrt = w1 & 0xFFF;
                u16 tw = lrs >= uls ? (u16)(( lrs - uls ) / 4 + 1 ) : 0;
                u16 th = lrt >= ult ? (u16)(( lrt - ult ) / 4 + 1 ) : 0;
                t->uls = uls / 4.0f;
                t->ult = ult / 4.0f;
                t->width = t->maskS ? (u16)( 1u << t->maskS ) : tw;
                t->height = t->maskT ? (u16)( 1u << t->maskT ) : th;
                t->valid = 1;
                if( tile != s_gfx.textureTile ) break;
                s_gfx.tileUls = t->uls;
                s_gfx.tileUlt = t->ult;
                s_gfx.tileW = t->width;
                s_gfx.tileH = t->height;
                s_gfx.texDirty = 1;
            }
            break;
        case 0xD7: /* G_TEXTURE: on/off + st scale */
            s_gfx.texOn = (( w0 >> 1 ) & 0x7F ) != 0;   /* G_ON=1 */
            {
                u8 tile = ( w0 >> 8 ) & 0x7;
                if( tile != s_gfx.textureTile ) {
                    TileState *t = &s_gfx.tiles[tile];
                    s_gfx.textureTile = tile;
                    if( t->valid ) {
                        s_gfx.tileFmt = t->fmt; s_gfx.tileSiz = t->siz;
                        s_gfx.tileTmem = t->tmem;
                        s_gfx.tilePalette = t->palette;
                        s_gfx.wrapS = t->wrapS; s_gfx.wrapT = t->wrapT;
                        s_gfx.maskS = t->maskS; s_gfx.maskT = t->maskT;
                        s_gfx.shiftS = t->shiftS; s_gfx.shiftT = t->shiftT;
                        s_gfx.tileW = t->width; s_gfx.tileH = t->height;
                        s_gfx.tileUls = t->uls; s_gfx.tileUlt = t->ult;
                    }
                    s_gfx.texDirty = 1;
                }
            }
            s_gfx.texScaleS = (( w1 >> 16 ) & 0xFFFF ) / 65536.0f;
            s_gfx.texScaleT = ( w1 & 0xFFFF ) / 65536.0f;
            if( s_gfx.texScaleS == 0 ) s_gfx.texScaleS = 1.0f;
            if( s_gfx.texScaleT == 0 ) s_gfx.texScaleT = 1.0f;
            break;
        case 0xFC: { /* G_SETCOMBINE: does any slot reference TEXEL0/TEXEL1? */
            u32 a0 = ( w0 >> 20 ) & 0xF, c0 = ( w0 >> 15 ) & 0x1F;
            u32 a1 = ( w0 >> 5 ) & 0xF,  c1 = w0 & 0x1F;
            u32 b0 = ( w1 >> 28 ) & 0xF, b1 = ( w1 >> 24 ) & 0xF;
            u32 d0 = ( w1 >> 15 ) & 0x7, d1 = ( w1 >> 6 ) & 0x7;
            u32 Aa0 = ( w0 >> 12 ) & 0x7, Ac0 = ( w0 >> 9 ) & 0x7;
            u32 Ab0 = ( w1 >> 12 ) & 0x7, Ad0 = ( w1 >> 9 ) & 0x7;
            u32 Aa1 = ( w1 >> 21 ) & 0x7, Ac1 = ( w1 >> 18 ) & 0x7;
            u32 Ab1 = ( w1 >> 3 ) & 0x7,  Ad1 = w1 & 0x7;
#define REFS_TEX( x ) (( x ) == 1 || ( x ) == 2)   /* TEXEL0 / TEXEL1 */
            s_gfx.texEnabled =
                REFS_TEX( a0 ) || REFS_TEX( b0 ) || REFS_TEX( c0 ) || REFS_TEX( d0 ) ||
                REFS_TEX( a1 ) || REFS_TEX( b1 ) || REFS_TEX( c1 ) || REFS_TEX( d1 ) ||
                REFS_TEX( Aa0 ) || REFS_TEX( Ab0 ) || REFS_TEX( Ac0 ) || REFS_TEX( Ad0 ) ||
                REFS_TEX( Aa1 ) || REFS_TEX( Ab1 ) || REFS_TEX( Ac1 ) || REFS_TEX( Ad1 ) ||
                c0 == 8 || c0 == 9 || c1 == 8 || c1 == 9;  /* TEXELn_ALPHA in c-slot */
            /* c-slot multiplier source: ENVIRONMENT=5, PRIMITIVE=3 tint the texel */
            s_gfx.envMul  = ( c0 == 5 || c1 == 5 || a0 == 5 || a1 == 5 );
            s_gfx.primMul = ( c0 == 3 || c1 == 3 );
#undef REFS_TEX
            break;
        }
        default:
            /* sync, cull, branch-z, othermode: irrelevant or safely skipped */
            break;
        }
    }
}

/* ---- flex skeleton walk (mirrors SkelAnime_DrawFlexLod) ---------------- */

static PlayState *s_play;
static Player *s_player;

typedef struct {
    u32 visited;
    int limbCount;
    int walkCount;
    int invalid;
} SkeletonWalkGuard;

static int skeleton_walk_enter( SkeletonWalkGuard *guard, void **skeleton,
                                s32 limbIndex, int depth )
{
    if( guard->invalid || limbIndex < 0 || limbIndex >= guard->limbCount ||
        depth >= guard->limbCount || guard->walkCount >= guard->limbCount ||
        ( guard->visited & ( 1u << limbIndex )) != 0 || skeleton[limbIndex] == NULL ) {
        guard->invalid = 1;
        return 0;
    }
    guard->visited |= 1u << limbIndex;
    guard->walkCount++;
    return 1;
}

static SkeletonWalkGuard sLinkWalk;

static void draw_limb( void **skeleton, Vec3s *jointTable, s32 limbIndex, int depth )
{
    if( !skeleton_walk_enter( &sLinkWalk, skeleton, limbIndex, depth )) return;
    LodLimb *limb = skeleton[limbIndex];
    Vec3f pos = { limb->jointPos.x, limb->jointPos.y, limb->jointPos.z };
    Vec3s rot = jointTable[limbIndex + 1];
    Gfx *newDList, *limbDList;

    Matrix_Push();

    newDList = limbDList = limb->dLists[0];
    if( !Player_OverrideLimbDrawGameplayDefault( s_play, limbIndex + 1, &newDList, &pos, &rot, s_player )) {
        Matrix_TranslateRotateZYX( &pos, &rot );
        if( newDList != NULL ) {
            if( s_gfx.flexCount < FLEX_MAX_MTX && s_gfx.queueCount < FLEX_MAX_MTX ) {
                Matrix_Get( &s_gfx.flexMtx[s_gfx.flexCount] );
                s_gfx.queue[s_gfx.queueCount].dl = (uintptr_t)newDList;
                s_gfx.queue[s_gfx.queueCount].mtxIdx = s_gfx.flexCount;
                s_gfx.queue[s_gfx.queueCount].limb = limbIndex;
                s_gfx.queueCount++;
                s_gfx.flexCount++;
            }
        } else if( limbDList != NULL ) {
            if( s_gfx.flexCount < FLEX_MAX_MTX ) {
                Matrix_Get( &s_gfx.flexMtx[s_gfx.flexCount] );
                s_gfx.flexCount++;
            }
        }
    }

    if( limb->child != 0xFF )
        draw_limb( skeleton, jointTable, limb->child, depth + 1 );

    Matrix_Pop();

    if( limb->sibling != 0xFF )
        draw_limb( skeleton, jointTable, limb->sibling, depth + 1 );
}

/* ---- Navi wing/body mesh (liboot v0.4) --------------------------------- */

/* Standard (non-flex) skeleton walk mirroring SkelAnime_DrawOpa: one dList
   per limb, matrix committed right before the limb DL runs. Limb table index
   7 is the body-glow billboard (seg-8 per-frame color DL + camera-facing
   quad); skipped here, hosts billboard it from oot_navi_get. */
static int sNaviRenderEnabled;

void oot_navi_set_render( bool enabled )
{
    sNaviRenderEnabled = enabled;
}

#define NAVI_GLOW_LIMB 7

static void navi_walk_limb( void **skeleton, Vec3s *jointTable, s32 limbIndex,
                            int depth, SkeletonWalkGuard *guard )
{
    if( !skeleton_walk_enter( guard, skeleton, limbIndex, depth )) return;
    StandardLimb *limb = skeleton[limbIndex];
    Vec3f pos = { limb->jointPos.x, limb->jointPos.y, limb->jointPos.z };
    Vec3s rot = jointTable[limbIndex + 1];

    Matrix_Push();
    Matrix_TranslateRotateZYX( &pos, &rot );

    if( limb->dList != NULL && limbIndex != NAVI_GLOW_LIMB ) {
        Matrix_Get( &s_gfx.cur );
        run_dl(( uintptr_t )limb->dList, 0 );
    }

    if( limb->child != 0xFF )
        navi_walk_limb( skeleton, jointTable, limb->child, depth + 1, guard );

    Matrix_Pop();

    if( limb->sibling != 0xFF )
        navi_walk_limb( skeleton, jointTable, limb->sibling, depth + 1, guard );
}

static void liboot_render_navi( Actor *navi, SkelAnime *sk )
{
    void **skeleton = sk->skeleton;
    if( !skeleton || !sk->jointTable || sk->limbCount != NAVI_RENDER_LIMBS + 1 ||
        navi->scale.x <= 0.0f ) return;
    SkeletonWalkGuard guard = { 0, NAVI_RENDER_LIMBS, 0, 0 };
    int trianglesBefore = s_gfx.out ? s_gfx.out->numTrianglesUsed : 0;

    s_gfx.lighting = 0;
    s_gfx.curTex = -1;
    s_gfx.texDirty = 1;
    s_gfx.baseColor[0] = s_gfx.baseColor[1] = s_gfx.baseColor[2] = 1.0f;
    s_gfx.primColor[0] = s_gfx.primColor[1] = s_gfx.primColor[2] = s_gfx.primColor[3] = 1.0f;

    Matrix_SetTranslateRotateYXZ( navi->world.pos.x,
                                  navi->world.pos.y + navi->shape.yOffset * navi->scale.y,
                                  navi->world.pos.z, &navi->shape.rot );
    Matrix_Scale( navi->scale.x, navi->scale.y, navi->scale.z, MTXMODE_APPLY );

    Matrix_Push();

    if( !skeleton_walk_enter( &guard, skeleton, 0, 0 )) {
        Matrix_Pop();
        return;
    }
    StandardLimb *rootLimb = skeleton[0];
    Vec3f pos = { sk->jointTable[0].x, sk->jointTable[0].y, sk->jointTable[0].z };
    Vec3s rot = sk->jointTable[1];

    Matrix_TranslateRotateZYX( &pos, &rot );
    if( rootLimb->dList != NULL ) {
        Matrix_Get( &s_gfx.cur );
        run_dl(( uintptr_t )rootLimb->dList, 0 );
    }

    if( rootLimb->sibling != 0xFF ) guard.invalid = 1;
    if( rootLimb->child != 0xFF )
        navi_walk_limb( skeleton, sk->jointTable, rootLimb->child, 1, &guard );

    Matrix_Pop();
    if(( guard.invalid || guard.walkCount != guard.limbCount ) && s_gfx.out )
        s_gfx.out->numTrianglesUsed = trianglesBefore;
}

/* ---- Projectile actors: capture-replay (liboot v0.6) -------------------- */

/* The vendored projectile draws (EnBom_Draw, EnBoom_Draw, ArmsHook_Draw,
 * EnArrow_Draw via SkelAnime_DrawLod) write straight into the game's
 * GraphicsContext streams, so hijacking Gfx_DrawDListOpa captures nothing.
 * Instead each actor->draw runs against a fake GraphicsContext whose
 * polyOpa/polyXlu streams point at scratch arenas (DEBUG_FEATURES=0 makes
 * OPEN_DISPS pure aliasing and GRAPH_ALLOC carve Mtx allocations from
 * polyOpa.d downward), then the captured command buffer is replayed:
 *   0xDA G_MTX      w1 is a Mtx* TRUNCATED to u32 (Gwords.w1 is unsigned
 *                   int) -> offset rebuilt against the opa arena base,
 *                   Matrix_MtxToMtxF into the interpreter's current matrix
 *   0xDE G_DL       known native wrappers are matched by their truncated low
 *                   word first; otherwise only an in-bounds mapped segment
 *                   token is accepted (ASLR can make the two look alike)
 *   0xFA/0xFB       prim/env color for the material approximation
 */
static int sActorRenderEnabled;

void oot_actor_set_render( bool enabled )
{
    sActorRenderEnabled = enabled;
}

#define ACTOR_CAP_OPA_CMDS 1024
#define ACTOR_CAP_XLU_CMDS 256

static GraphicsContext sActorCapGfxCtx;
static Gfx sActorCapOpa[ACTOR_CAP_OPA_CMDS];
static Gfx sActorCapXlu[ACTOR_CAP_XLU_CMDS];

extern Gfx gBombBodyDL[];
extern Gfx gBombCapDL[];
extern Gfx gBoomerangRefDL[];
extern Gfx gEffSparklesDL[];
extern Gfx gLinkAdultHookshotTipDL[];
extern Gfx gLinkAdultHookshotChainDL[];

/* Gfx command words only retain the low 32 bits of native wrapper pointers.
   Match every wrapper used by the supported projectile actors before looking
   at the high byte as a segmented address; ASLR can put a native low word in
   the 0x01xxxxxx..0x0Fxxxxxx range by chance. */
static uintptr_t actor_wrapper_pointer( u32 word )
{
#define MATCH_WRAPPER( symbol ) \
    do { if((u32)(uintptr_t)( symbol ) == word ) return (uintptr_t)( symbol ); } while( 0 )
    MATCH_WRAPPER( gBombBodyDL );
    MATCH_WRAPPER( gBombCapDL );
    MATCH_WRAPPER( gBoomerangRefDL );
    MATCH_WRAPPER( gEffSparklesDL );
    MATCH_WRAPPER( gLinkAdultHookshotTipDL );
    MATCH_WRAPPER( gLinkAdultHookshotChainDL );
#undef MATCH_WRAPPER
    return 0;
}

static int replay_segment_valid( u32 address )
{
    u32 segment = address >> 24;
    if( segment != 0x02 && segment != 0x03 && segment != 0x04 && segment != 0x06 )
        return 0;
    size_t size = 0;
    const u8 *base = liboot_segment_base((int)segment, &size );
    return base != NULL && ( address & 0x00FFFFFFu ) < size;
}

static void liboot_replay_capture( const Gfx *buf, const Gfx *end,
                                   const void *allocationCursor, size_t capacity )
{
    uintptr_t beginAddress = (uintptr_t)buf;
    uintptr_t endAddress = (uintptr_t)end;
    uintptr_t limitAddress = beginAddress + capacity * sizeof( *buf );
    uintptr_t allocationAddress = (uintptr_t)allocationCursor;

    /* Actor draw code advances a command cursor upward while GRAPH_ALLOC moves
       a data cursor downward in the same scratch arena.  Never replay beyond
       either boundary, even if a future draw routine overproduces commands. */
    if( allocationAddress >= beginAddress && allocationAddress < limitAddress )
        limitAddress = allocationAddress;
    if( endAddress < beginAddress ) return;
    if( endAddress > limitAddress ) endAddress = limitAddress;
    size_t commandCount = ( endAddress - beginAddress ) / sizeof( *buf );

    for( size_t command = 0; command < commandCount; ++command ) {
        const Gfx *g = &buf[command];
        u32 w0 = g->words.w0;
        u32 w1 = (u32)g->words.w1;

        switch( w0 >> 24 ) {
        case 0xDA: { /* G_MTX: Mtx lives in the opa arena (GRAPH_ALLOC) */
            u32 off = (u32)( w1 - (u32)(uintptr_t)sActorCapOpa );
            if( off <= sizeof( sActorCapOpa ) - sizeof( Mtx ))
                Matrix_MtxToMtxF(( Mtx * )(( u8 * )sActorCapOpa + off ), &s_gfx.cur );
            break;
        }
        case 0xDE: { /* G_DL */
            uintptr_t wrapper = actor_wrapper_pointer( w1 );
            if( wrapper ) {
                run_dl( wrapper, 0 );
            } else if( replay_segment_valid( w1 )) {
                run_dl(( uintptr_t )w1, 0 );
            } else if( getenv( "LIBOOT_TRACE" )) {
                fprintf( stderr, "[G] ignored unknown captured DL %08x\n", w1 );
            }
            break;
        }
        case 0xFA: /* G_SETPRIMCOLOR */
            s_gfx.primColor[0] = (( w1 >> 24 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[1] = (( w1 >> 16 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[2] = (( w1 >> 8 ) & 0xFF ) / 255.0f;
            s_gfx.primColor[3] = ( w1 & 0xFF ) / 255.0f;
            break;
        case 0xFB: /* G_SETENVCOLOR */
            s_gfx.envColor[0] = (( w1 >> 24 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[1] = (( w1 >> 16 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[2] = (( w1 >> 8 ) & 0xFF ) / 255.0f;
            s_gfx.envColor[3] = ( w1 & 0xFF ) / 255.0f;
            break;
        default: /* pipeline state the interpreter reconstructs itself */
            break;
        }
    }
}

static void liboot_render_actor( PlayState *play, Actor *actor )
{
    GraphicsContext *gfxCtx = &sActorCapGfxCtx;

    memset( gfxCtx, 0, sizeof( *gfxCtx ));
    gfxCtx->polyOpa.p = sActorCapOpa;
    gfxCtx->polyOpa.d = ( u8 * )sActorCapOpa + sizeof( sActorCapOpa );
    gfxCtx->polyXlu.p = sActorCapXlu;
    gfxCtx->polyXlu.d = ( u8 * )sActorCapXlu + sizeof( sActorCapXlu );

    /* mirror Actor_Draw's matrix setup */
    Matrix_SetTranslateRotateYXZ( actor->world.pos.x,
                                  actor->world.pos.y + actor->shape.yOffset * actor->scale.y,
                                  actor->world.pos.z, &actor->shape.rot );
    Matrix_Scale( actor->scale.x, actor->scale.y, actor->scale.z, MTXMODE_APPLY );
    Matrix_Get( &s_gfx.cur ); /* fallback for DLs drawn before any G_MTX */

    play->state.gfxCtx = gfxCtx;
    actor->draw( actor, play );
    play->state.gfxCtx = NULL;

    /* interpreter material state per actor */
    s_gfx.lighting = 1;
    s_gfx.curTex = -1;
    s_gfx.texDirty = 1;
    s_gfx.texEnabled = 1;
    s_gfx.texOn = 1;
    s_gfx.texScaleS = s_gfx.texScaleT = 65535.0f / 65536.0f;
    /* OoT's opaque setup DL enables G_CULL_BACK before room/Link DLs run; they
       inherit it (and may clear/flip it per material), so start there. */
    s_gfx.cullMode = OOT_TRI_CULL_BACK;
    s_gfx.baseColor[0] = s_gfx.baseColor[1] = s_gfx.baseColor[2] = 1.0f;
    s_gfx.primColor[0] = s_gfx.primColor[1] = s_gfx.primColor[2] = s_gfx.primColor[3] = 1.0f;
    s_gfx.envColor[0] = s_gfx.envColor[1] = s_gfx.envColor[2] = s_gfx.envColor[3] = 1.0f;

    liboot_replay_capture( sActorCapOpa, gfxCtx->polyOpa.p,
                           gfxCtx->polyOpa.d, ACTOR_CAP_OPA_CMDS );
    liboot_replay_capture( sActorCapXlu, gfxCtx->polyXlu.p,
                           gfxCtx->polyXlu.d, ACTOR_CAP_XLU_CMDS );
}

static void liboot_render_actors( PlayState *play )
{
    for( int cat = 0; cat < ACTORCAT_MAX; cat++ ) {
        if( cat == ACTORCAT_PLAYER ) continue;
        for( Actor *a = play->actorCtx.actorLists[cat].head; a != NULL; a = a->next ) {
            if( a->draw == NULL || a->update == NULL ) continue;
            if( a->id == ACTOR_EN_ELF ) continue; /* Navi has her own path */
            liboot_render_actor( play, a );
        }
    }
}

void liboot_render_link( PlayState *play, Player *player, struct OoTLinkGeometryBuffers *out )
{
    SkelAnime *sk = &player->skelAnime;
    void **skeleton = sk->skeleton;

    out->numTrianglesUsed = 0;
    if( !skeleton || !sk->jointTable || sk->limbCount != LINK_RENDER_LIMBS + 1 ) return;

    memset( &s_gfx, 0, sizeof( s_gfx ));
    s_gfx.out = out;
    s_gfx.maxTris = OOT_GEO_MAX_TRIANGLES;
    s_gfx.cmdBudget = MAX_CMDS;
    s_gfx.lighting = 1;
    s_gfx.curTex = -1;
    s_gfx.texEnabled = 1;
    s_gfx.texOn = 1;
    s_gfx.texScaleS = s_gfx.texScaleT = 65535.0f / 65536.0f; /* gsSPTexture default */
    s_gfx.cullMode = OOT_TRI_CULL_BACK;  /* Link's setup enables G_CULL_BACK */

    /* Per-frame face selection (mirrors Player_DrawImpl, z_player_lib.c):
       eye/mouth indices ride 2 extra bytes per animation frame, read back as
       jointTable[22].x; negative means "use the actor's static face". The
       object file holds 8 CI8 64x32 eyes at idx*0x800 and 4 CI8 32x32 mouths
       at 0x4000+idx*0x400 (both ages), so seg 8/9 SETTIMGs rewrite to seg6. */
    {
        s16 f = sk->jointTable[22].x;
        s32 eye = ( f & 0xF ) - 1;
        s32 mouth = ( f >> 4 ) - 1;
        if( eye < 0 || mouth < 0 ) {
            s32 face = player->actor.shape.face;
            if( face < 0 || face >= PLAYER_FACE_MAX ) face = 0;
            if( eye < 0 ) eye = sPlayerFaces[face].eyeIndex;
            if( mouth < 0 ) mouth = sPlayerFaces[face].mouthIndex;
        }
        if( eye < 0 || eye >= PLAYER_EYES_MAX ) eye = PLAYER_EYES_OPEN;
        if( mouth < 0 || mouth >= PLAYER_MOUTH_MAX ) mouth = PLAYER_MOUTH_CLOSED;
        s_gfx.eyeTexAddr = 0x06000000u | (u32)( eye * 0x800 );
        s_gfx.mouthTexAddr = 0x06000000u | (u32)( 0x4000 + mouth * 0x400 );
    }
    s_gfx.primColor[0] = s_gfx.primColor[1] = s_gfx.primColor[2] = s_gfx.primColor[3] = 1.0f;
    int tunic = player->currentTunic;
    if( tunic < 0 || tunic > 2 ) tunic = 0;
    s_gfx.envColor[0] = sTunicColors[tunic][0];
    s_gfx.envColor[1] = sTunicColors[tunic][1];
    s_gfx.envColor[2] = sTunicColors[tunic][2];
    s_gfx.envColor[3] = 1.0f;

    s_play = play;
    s_player = player;

    Actor *actor = &player->actor;
    Matrix_SetTranslateRotateYXZ( actor->world.pos.x,
                                  actor->world.pos.y + actor->shape.yOffset * actor->scale.y,
                                  actor->world.pos.z, &actor->shape.rot );
    Matrix_Scale( actor->scale.x, actor->scale.y, actor->scale.z, MTXMODE_APPLY );

    Matrix_Push();

    /* root limb, same special-casing as SkelAnime_DrawFlexLod */
    memset( &sLinkWalk, 0, sizeof( sLinkWalk ));
    sLinkWalk.limbCount = LINK_RENDER_LIMBS;
    if( !skeleton_walk_enter( &sLinkWalk, skeleton, 0, 0 )) {
        Matrix_Pop();
        return;
    }
    LodLimb *rootLimb = skeleton[0];
    Vec3f pos = { sk->jointTable[0].x, sk->jointTable[0].y, sk->jointTable[0].z };
    Vec3s rot = sk->jointTable[1];
    Gfx *newDList = rootLimb->dLists[0], *limbDList = newDList;

    if( !Player_OverrideLimbDrawGameplayDefault( s_play, 1, &newDList, &pos, &rot, s_player )) {
        Matrix_TranslateRotateZYX( &pos, &rot );
        if( newDList != NULL ) {
            if( s_gfx.flexCount < FLEX_MAX_MTX && s_gfx.queueCount < FLEX_MAX_MTX ) {
                Matrix_Get( &s_gfx.flexMtx[s_gfx.flexCount] );
                s_gfx.queue[s_gfx.queueCount].dl = (uintptr_t)newDList;
                s_gfx.queue[s_gfx.queueCount].mtxIdx = s_gfx.flexCount;
                s_gfx.queue[s_gfx.queueCount].limb = 0;
                s_gfx.queueCount++;
                s_gfx.flexCount++;
            }
        } else if( limbDList != NULL ) {
            if( s_gfx.flexCount < FLEX_MAX_MTX ) {
                Matrix_Get( &s_gfx.flexMtx[s_gfx.flexCount] );
                s_gfx.flexCount++;
            }
        }
    }

    if( rootLimb->sibling != 0xFF ) sLinkWalk.invalid = 1;
    if( rootLimb->child != 0xFF )
        draw_limb( skeleton, sk->jointTable, rootLimb->child, 1 );

    Matrix_Pop();
    if( sLinkWalk.invalid || sLinkWalk.walkCount != sLinkWalk.limbCount ) {
        out->numTrianglesUsed = 0;
        return;
    }

    /* liboot v0.4: opt-in Navi wing/body mesh, appended after Link (walked
       first so its DLs cannot disturb the recorded flex queue state) */
    Actor *naviActor = player->naviActor;

    /* pass 2: matrix buffer complete, execute the recorded limb DLs */
    for( int i = 0; i < s_gfx.queueCount; ++i ) {
        s_gfx.cur = s_gfx.flexMtx[s_gfx.queue[i].mtxIdx];
        int limb = s_gfx.queue[i].limb;
        const float *mat = sLimbMat[limb] == MAT_TUNIC ? s_gfx.envColor
                                                       : sMatColors[sLimbMat[limb]];
        s_gfx.baseColor[0] = mat[0];
        s_gfx.baseColor[1] = mat[1];
        s_gfx.baseColor[2] = mat[2];
        s_gfx.primColor[0] = s_gfx.primColor[1] = s_gfx.primColor[2] = 1.0f;
        int before = out->numTrianglesUsed;
        run_dl( s_gfx.queue[i].dl, 0 );
        if( getenv( "LIBOOT_TRACE" ))
            fprintf( stderr, "[G] q%02d mtx%02d dl=%c%08x t=(%.1f %.1f %.1f) tris+%d\n",
                     i, s_gfx.queue[i].mtxIdx, s_gfx.queue[i].dl > 0xFFFFFFFFu ? 'N' : 'S',
                     (u32)( s_gfx.queue[i].dl & 0xFFFFFFFFu ),
                     s_gfx.cur.xw, s_gfx.cur.yw, s_gfx.cur.zw,
                     out->numTrianglesUsed - before );
    }

    if( sNaviRenderEnabled && naviActor != NULL && naviActor->id == ACTOR_EN_ELF )
        liboot_render_navi( naviActor, &(( EnElf * )naviActor )->skelAnime );

    /* liboot v0.6: opt-in projectile actor meshes (capture-replay), appended
       last so their state cannot disturb Link's or Navi's interpretation */
    if( sActorRenderEnabled )
        liboot_render_actors( play );
}

/* ---- scene/room mesh interpretation entry point (liboot v0.7) ---------- */

/* Room display lists are authored in world space and drawn with an identity
   model matrix (no G_MTX inside — verified across the probed scenes), so the
   walk is a plain begin + one run per mesh entry DL. RDP state persists
   between entries exactly like it does in the game's contiguous room stream.
   Called only by src/scene.c. */
void liboot_scene_dl_begin( struct OoTLinkGeometryBuffers *out, int maxTris )
{
    memset( &s_gfx, 0, sizeof( s_gfx ));
    s_gfx.out = out;
    s_gfx.maxTris = maxTris > 0 ? maxTris : OOT_GEO_MAX_TRIANGLES;
    s_gfx.cmdBudget = MAX_CMDS;
    s_gfx.lighting = 1;               /* room tris are lit (real normals) */
    s_gfx.curTex = -1;
    s_gfx.texDirty = 1;
    s_gfx.texEnabled = 1;
    s_gfx.texOn = 1;
    s_gfx.texScaleS = s_gfx.texScaleT = 65535.0f / 65536.0f;
    /* OoT's opaque setup DL enables G_CULL_BACK before room/Link DLs run; they
       inherit it (and may clear/flip it per material), so start there. */
    s_gfx.cullMode = OOT_TRI_CULL_BACK;
    s_gfx.baseColor[0] = s_gfx.baseColor[1] = s_gfx.baseColor[2] = 1.0f;
    s_gfx.primColor[0] = s_gfx.primColor[1] = s_gfx.primColor[2] = s_gfx.primColor[3] = 1.0f;
    s_gfx.envColor[0] = s_gfx.envColor[1] = s_gfx.envColor[2] = s_gfx.envColor[3] = 1.0f;
    /* identity: room vertices are already world-space */
    s_gfx.cur.xx = s_gfx.cur.yy = s_gfx.cur.zz = s_gfx.cur.ww = 1.0f;
}

void liboot_scene_dl_run( u32 dlAddr )
{
    run_dl( (uintptr_t)dlAddr, 0 );
}

/* ---- public texture accessors ------------------------------------------ */

int32_t oot_get_texture_count( void )
{
    return sTexCount;
}

bool oot_get_texture( int32_t index, struct OoTTextureInfo *info, const u8 **rgbaPixels )
{
    if( index < 0 || index >= sTexCount || !sTexCache[index].rgba ) return false;
    if( info ) {
        info->width = sTexCache[index].width;
        info->height = sTexCache[index].height;
        /* cm bit0 = MIRROR, bit1 = CLAMP. The RDP clamps to the tile extent
           BEFORE the mask/mirror stage, and every SETTILESIZE in both link
           objects spans exactly one mask period, so with the clamp bit set
           the mirror bit can never engage: clamp wins over mirror (cm=3
           behaves as pure clamp). Pure mirror (cm=1) masks with mirroring. */
        info->wrapS = ( sTexCache[index].wrapS & 2 ) ? 2 : ( sTexCache[index].wrapS & 1 ) ? 1 : 0;
        info->wrapT = ( sTexCache[index].wrapT & 2 ) ? 2 : ( sTexCache[index].wrapT & 1 ) ? 1 : 0;
        info->revision = sTexCache[index].revision;
    }
    if( rgbaPixels ) *rgbaPixels = sTexCache[index].rgba;
    return true;
}
