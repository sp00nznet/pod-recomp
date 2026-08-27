/*
 * POD Gold Recompilation - DirectDraw COM shim.
 *
 * POD's MMX build draws through DirectDraw: DirectDrawCreate -> IDirectDraw, then
 * SetCooperativeLevel / SetDisplayMode(640,480,16) / CreateSurface (a primary with
 * an attached back buffer), and per frame Lock the back buffer (the MMX rasterizer
 * writes 16bpp pixels straight into it), Unlock, and Flip.
 *
 * We synthesise the COM objects in *guest* memory: an object is { vtbl_va, fields... }
 * and a vtable is an array of *synthetic* method VAs (COM_VA_BASE + index). When the
 * lifted code does `call [vtbl+off]` it becomes RECOMP_ICALL(synthetic_va), which
 * recomp_lookup_import resolves (via pod_com_lookup) to the matching host shim here.
 * Each shim runs in the recomp stack model: this=ARG(0), args ARG(1..), RET(hr),
 * STDRET(total slots incl. this) -- COM methods are stdcall.
 */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "imports.h"

#define DD_OK 0u

/* ---- synthetic COM method VAs -> host shims ---- */
#define COM_VA_BASE 0xCD000000u
static recomp_func_t g_com_fn[512];
static uint32_t      g_com_n = 0;
static uint32_t com_reg(recomp_func_t fn) { uint32_t va = COM_VA_BASE + g_com_n; g_com_fn[g_com_n++] = fn; return va; }
recomp_func_t pod_com_lookup(uint32_t va) {
    if (va >= COM_VA_BASE && va < COM_VA_BASE + g_com_n) return g_com_fn[va - COM_VA_BASE];
    return NULL;
}

static int g_dbg = 1;
#define DLOG(...) do { if (g_dbg) { fprintf(stderr, "[ddraw] " __VA_ARGS__); } } while (0)

/* ---- surface object layout (guest memory) ---- */
#define SO_VTBL 0
#define SO_W    4
#define SO_H    8
#define SO_BPP  12
#define SO_PITCH 16
#define SO_BUF  20      /* VA of the 16bpp pixel buffer */
#define SO_BACK 24      /* VA of attached back-buffer surface (0 if none) */
#define SO_CAPS 28
#define SO_PRIMARY 32
#define SO_SIZE 40

/* DDSURFACEDESC field offsets */
#define DDSD_SIZE 0
#define DDSD_FLAGS 4
#define DDSD_HEIGHT 8
#define DDSD_WIDTH 12
#define DDSD_PITCH 16
#define DDSD_BACKBUFCNT 20
#define DDSD_LPSURFACE 36
#define DDSD_PIXFMT 72      /* DDPIXELFORMAT (32 bytes) */
#define DDSD_CAPS 104       /* DDSCAPS.dwCaps */

/* DDSCAPS bits */
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_FLIP           0x10u
#define DDSCAPS_BACKBUFFER     0x4u
/* DDSD flags */
#define DDSD_F_BACKBUFFERCOUNT 0x20u

static uint32_t g_dd_vtbl = 0, g_surf_vtbl = 0, g_pal_vtbl = 0;
static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;

static void fill_pixfmt(uint32_t pf) {       /* RGB 565 */
    MEM32(pf + 0) = 32; MEM32(pf + 4) = 0x40u /*DDPF_RGB*/; MEM32(pf + 8) = 0;
    MEM32(pf + 12) = 16; MEM32(pf + 16) = 0xF800u; MEM32(pf + 20) = 0x07E0u;
    MEM32(pf + 24) = 0x001Fu; MEM32(pf + 28) = 0;
}
static void fill_desc(uint32_t desc, uint32_t surf) {
    MEM32(desc + DDSD_SIZE) = 108;
    MEM32(desc + DDSD_FLAGS) = 0x1u|0x2u|0x4u|0x8u|0x800u|0x1000u; /* CAPS|HEIGHT|WIDTH|PITCH|LPSURFACE|PIXELFORMAT */
    MEM32(desc + DDSD_HEIGHT) = MEM32(surf + SO_H);
    MEM32(desc + DDSD_WIDTH)  = MEM32(surf + SO_W);
    MEM32(desc + DDSD_PITCH)  = MEM32(surf + SO_PITCH);
    MEM32(desc + DDSD_LPSURFACE) = MEM32(surf + SO_BUF);
    fill_pixfmt(desc + DDSD_PIXFMT);
    MEM32(desc + DDSD_CAPS) = MEM32(surf + SO_CAPS);
}
static uint32_t make_surface(int w, int h, uint32_t caps, int primary) {
    uint32_t s = shim_alloc(SO_SIZE);
    uint32_t pitch = (uint32_t)w * 2;
    MEM32(s + SO_VTBL) = g_surf_vtbl;
    MEM32(s + SO_W) = (uint32_t)w; MEM32(s + SO_H) = (uint32_t)h;
    MEM32(s + SO_BPP) = 16; MEM32(s + SO_PITCH) = pitch;
    MEM32(s + SO_BUF) = shim_alloc(pitch * (uint32_t)h + 64);
    MEM32(s + SO_BACK) = 0; MEM32(s + SO_CAPS) = caps; MEM32(s + SO_PRIMARY) = (uint32_t)primary;
    host_register_surface16(MEM32(s + SO_BUF), w, h, (int)pitch);  /* for pump auto-present */
    return s;
}
static void present(uint32_t surf) {
    /* Present only; the window's message pump runs on the main thread (via the
     * PeekMessage/MsgWait shims), and Flip/Blt may be called from a worker. */
    host_present16(MEM32(surf + SO_BUF), (int)MEM32(surf + SO_W),
                   (int)MEM32(surf + SO_H), (int)MEM32(surf + SO_PITCH));
}

/* ============================================================ IDirectDraw methods */
static void dd_QueryInterface(void){ MEM32(ARG(2)) = ARG(0); RET(DD_OK); STDRET(3); }   /* same object for any IID */
static void dd_AddRef(void)        { RET(2); STDRET(1); }
static void dd_Release(void)       { RET(1); STDRET(1); }
static void dd_Compact(void)       { RET(DD_OK); STDRET(1); }
static void dd_CreateClipper(void) { if(ARG(2)) MEM32(ARG(2))=0; RET(DD_OK); STDRET(4); }
static void dd_CreatePalette(void) {                          /* (this,flags,pCT,ppPal,pUnk) */
    uint32_t p = shim_alloc(8); MEM32(p + 0) = g_pal_vtbl;
    if (ARG(3)) MEM32(ARG(3)) = p; RET(DD_OK); STDRET(5);
}
static void dd_CreateSurface(void) {                          /* (this,pDesc,ppSurf,pUnk) */
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags = MEM32(desc + DDSD_FLAGS), caps = MEM32(desc + DDSD_CAPS);
    int w = (int)MEM32(desc + DDSD_WIDTH), h = (int)MEM32(desc + DDSD_HEIGHT);
    uint32_t bbc = MEM32(desc + DDSD_BACKBUFCNT);
    if (caps & DDSCAPS_PRIMARYSURFACE) { w = g_mode_w; h = g_mode_h; host_window_ensure(w, h); }
    if (w <= 0) w = g_mode_w; if (h <= 0) h = g_mode_h;
    uint32_t s = make_surface(w, h, caps, (caps & DDSCAPS_PRIMARYSURFACE) ? 1 : 0);
    if ((caps & DDSCAPS_PRIMARYSURFACE) && (flags & DDSD_F_BACKBUFFERCOUNT) && bbc > 0) {
        uint32_t back = make_surface(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP, 0);
        MEM32(s + SO_BACK) = back;
    }
    if (pp) MEM32(pp) = s;
    DLOG("CreateSurface %dx%d caps=0x%X bbc=%u -> surf=0x%08X back=0x%08X\n",
         w, h, caps, bbc, s, MEM32(s + SO_BACK));
    RET(DD_OK); STDRET(4);
}
static void dd_DuplicateSurface(void){ if(ARG(2)) MEM32(ARG(2))=ARG(1); RET(DD_OK); STDRET(3); }
static void dd_EnumDisplayModes(void){ DLOG("EnumDisplayModes (skipped)\n"); RET(DD_OK); STDRET(5); }
static void dd_EnumSurfaces(void)   { RET(DD_OK); STDRET(5); }
static void dd_FlipToGDISurface(void){ RET(DD_OK); STDRET(1); }
static void dd_GetCaps(void)        {                          /* (this,pDrvCaps,pHelCaps) */
    for (int i = 0; i < 2; i++) { uint32_t c = ARG(1 + i); if (c) { uint32_t sz = MEM32(c); if (sz < 4 || sz > 0x400) sz = 0x180; for (uint32_t o = 4; o < sz; o += 4) MEM32(c + o) = 0; MEM32(c + 4) = 0x00005000u; } }
    RET(DD_OK); STDRET(3);
}
static void dd_GetDisplayMode(void) { uint32_t d=ARG(1); if(d){ MEM32(d+DDSD_SIZE)=108; MEM32(d+DDSD_WIDTH)=g_mode_w; MEM32(d+DDSD_HEIGHT)=g_mode_h; MEM32(d+DDSD_PITCH)=g_mode_w*2; fill_pixfmt(d+DDSD_PIXFMT);} RET(DD_OK); STDRET(2); }
static void dd_GetFourCCCodes(void) { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(3); }
static void dd_GetGDISurface(void)  { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void dd_GetMonitorFreq(void) { if(ARG(1)) MEM32(ARG(1))=60; RET(DD_OK); STDRET(2); }
static void dd_GetScanLine(void)    { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void dd_GetVBStatus(void)    { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void dd_Initialize(void)     { RET(DD_OK); STDRET(2); }
static void dd_RestoreDisplayMode(void){ RET(DD_OK); STDRET(1); }
static void dd_SetCooperativeLevel(void){ DLOG("SetCooperativeLevel hwnd=0x%X flags=0x%X\n", ARG(1), ARG(2)); RET(DD_OK); STDRET(3); }
static void dd_SetDisplayMode(void) {                          /* (this,w,h,bpp) [v1] */
    g_mode_w = (int)ARG(1); g_mode_h = (int)ARG(2); g_mode_bpp = (int)ARG(3);
    DLOG("SetDisplayMode %dx%dx%d\n", g_mode_w, g_mode_h, g_mode_bpp);
    host_window_ensure(g_mode_w, g_mode_h);
    RET(DD_OK); STDRET(4);
}
static void dd_WaitForVB(void)      { RET(DD_OK); STDRET(3); }

/* ============================================================ IDirectDrawSurface methods */
static void s_QueryInterface(void){ MEM32(ARG(2)) = ARG(0); RET(DD_OK); STDRET(3); }
static void s_AddRef(void)        { RET(2); STDRET(1); }
static void s_Release(void)       { RET(1); STDRET(1); }
static void s_AddAttached(void)   { RET(DD_OK); STDRET(2); }
static void s_AddOverlayDirty(void){ RET(DD_OK); STDRET(2); }
static void s_Blt(void)           {                            /* (this,dRect,srcSurf,sRect,flags,fx) */
    uint32_t self = ARG(0), src = ARG(2);
    { static uint32_t n=0; if((n++%60)==0) DLOG("Blt #%u dst=0x%08X(prim=%u) src=0x%08X\n", n, self, MEM32(self+SO_PRIMARY), src); }
    if (MEM32(self + SO_PRIMARY) && src) present(src);
    RET(DD_OK); STDRET(6);
}
static void s_BltBatch(void)      { RET(DD_OK); STDRET(4); }
static void s_BltFast(void)       {                            /* (this,x,y,srcSurf,sRect,trans) */
    uint32_t self = ARG(0), src = ARG(3);
    if (MEM32(self + SO_PRIMARY) && src) present(src);
    RET(DD_OK); STDRET(6);
}
static void s_DeleteAttached(void){ RET(DD_OK); STDRET(3); }
static void s_EnumAttached(void)  { RET(DD_OK); STDRET(3); }
static void s_EnumOverlayZ(void)  { RET(DD_OK); STDRET(4); }
static void s_Flip(void)          {                            /* (this,targetOverride,flags) */
    uint32_t self = ARG(0), back = MEM32(self + SO_BACK);
    { static uint32_t n=0; if((n++%60)==0) DLOG("Flip #%u surf=0x%08X\n", n, self); }
    present(back ? back : self);
    RET(DD_OK); STDRET(3);
}
static void s_GetAttached(void)   {                            /* (this,pCaps,ppSurf) */
    uint32_t self = ARG(0), back = MEM32(self + SO_BACK);
    if (ARG(2)) MEM32(ARG(2)) = back ? back : self;
    RET(DD_OK); STDRET(3);
}
static void s_GetBltStatus(void)  { RET(DD_OK); STDRET(2); }
static void s_GetCaps(void)       { if(ARG(1)) MEM32(ARG(1))=MEM32(ARG(0)+SO_CAPS); RET(DD_OK); STDRET(2); }
static void s_GetClipper(void)    { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void s_GetColorKey(void)   { RET(DD_OK); STDRET(3); }
static void s_GetDC(void)         { if(ARG(1)) MEM32(ARG(1))=0x00020001u; RET(DD_OK); STDRET(2); }
static void s_GetFlipStatus(void) { RET(DD_OK); STDRET(2); }
static void s_GetOverlayPos(void) { RET(DD_OK); STDRET(3); }
static void s_GetPalette(void)    { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void s_GetPixelFormat(void){ if(ARG(1)) fill_pixfmt(ARG(1)); RET(DD_OK); STDRET(2); }
static void s_GetSurfaceDesc(void){ if(ARG(1)) fill_desc(ARG(1), ARG(0)); RET(DD_OK); STDRET(2); }
static void s_Initialize(void)    { RET(DD_OK); STDRET(3); }
static void s_IsLost(void)        { RET(DD_OK); STDRET(1); }   /* DD_OK = not lost */
static void s_Lock(void)          {                            /* (this,pRect,pDesc,flags,hEvent) */
    if (ARG(2)) fill_desc(ARG(2), ARG(0));
    { static uint32_t n=0; if((n++%120)==0) DLOG("Lock #%u surf=0x%08X buf=0x%08X\n", n, ARG(0), MEM32(ARG(0)+SO_BUF)); }
    RET(DD_OK); STDRET(5);
}
static void s_ReleaseDC(void)     { RET(DD_OK); STDRET(2); }
static void s_Restore(void)       { RET(DD_OK); STDRET(1); }
static void s_SetClipper(void)    { RET(DD_OK); STDRET(2); }
static void s_SetColorKey(void)   { RET(DD_OK); STDRET(3); }
static void s_SetOverlayPos(void) { RET(DD_OK); STDRET(3); }
static void s_SetPalette(void)    { RET(DD_OK); STDRET(2); }
static void s_Unlock(void)        { RET(DD_OK); STDRET(2); }
static void s_UpdateOverlay(void) { RET(DD_OK); STDRET(6); }
static void s_UpdateOverlayDisp(void){ RET(DD_OK); STDRET(2); }
static void s_UpdateOverlayZ(void){ RET(DD_OK); STDRET(3); }

/* ============================================================ IDirectDrawPalette (minimal) */
static void p_QueryInterface(void){ MEM32(ARG(2))=ARG(0); RET(DD_OK); STDRET(3); }
static void p_AddRef(void)  { RET(2); STDRET(1); }
static void p_Release(void) { RET(1); STDRET(1); }
static void p_GetCaps(void) { if(ARG(1)) MEM32(ARG(1))=0; RET(DD_OK); STDRET(2); }
static void p_GetEntries(void){ RET(DD_OK); STDRET(5); }
static void p_Initialize(void){ RET(DD_OK); STDRET(3); }
static void p_SetEntries(void){ RET(DD_OK); STDRET(5); }

/* ============================================================ DirectInput */
#define DI_OK 0u
static uint32_t g_di_vtbl = 0, g_didev_vtbl = 0;

/* IDirectInputA (v1): QI,AddRef,Release,CreateDevice,EnumDevices,GetDeviceStatus,
 * RunControlPanel,Initialize */
static void di_QueryInterface(void){ MEM32(ARG(2))=ARG(0); RET(DI_OK); STDRET(3); }
static void di_AddRef(void)  { RET(2); STDRET(1); }
static void di_Release(void) { RET(1); STDRET(1); }
static void di_CreateDevice(void){            /* (this,refguid,lplpDevice,pUnk) */
    uint32_t dev = shim_alloc(8); MEM32(dev + 0) = g_didev_vtbl;
    if (ARG(2)) MEM32(ARG(2)) = dev;
    DLOG("DirectInput CreateDevice -> 0x%08X\n", dev);
    RET(DI_OK); STDRET(4);
}
static void di_EnumDevices(void)    { RET(DI_OK); STDRET(5); }
static void di_GetDeviceStatus(void){ RET(DI_OK); STDRET(2); }
static void di_RunControlPanel(void){ RET(DI_OK); STDRET(3); }
static void di_Initialize(void)     { RET(DI_OK); STDRET(3); }

/* IDirectInputDeviceA (v1): 18 methods. No input is delivered (states zeroed). */
static void dv_QueryInterface(void){ MEM32(ARG(2))=ARG(0); RET(DI_OK); STDRET(3); }
static void dv_AddRef(void)  { RET(2); STDRET(1); }
static void dv_Release(void) { RET(1); STDRET(1); }
static void dv_GetCapabilities(void){ RET(DI_OK); STDRET(2); }
static void dv_EnumObjects(void)   { RET(DI_OK); STDRET(4); }
static void dv_GetProperty(void)   { RET(DI_OK); STDRET(3); }
static void dv_SetProperty(void)   { RET(DI_OK); STDRET(3); }
static void dv_Acquire(void)       { RET(DI_OK); STDRET(1); }
static void dv_Unacquire(void)     { RET(DI_OK); STDRET(1); }
/* A 256-byte state buffer is the keyboard; anything else (mouse, joystick) is
 * still reported as idle. */
static void dv_GetDeviceState(void){ uint32_t n=ARG(1),p=ARG(2);
    { static uint32_t k=0; if(k++<3) DLOG("GetDeviceState n=%u\n", n); }
    if (!host_key_state(p, n)) { for(uint32_t i=0;p&&i<n;i++) MEM8(p+i)=0; }
    RET(DI_OK); STDRET(3); }
static void dv_GetDeviceData(void) { { static uint32_t k=0; if(k++<3) DLOG("GetDeviceData sz=%u max=%u\n", ARG(1), ARG(3)?MEM32(ARG(3)):0); } if(ARG(3)) MEM32(ARG(3))=0; RET(DI_OK); STDRET(5); }
static void dv_SetDataFormat(void) { RET(DI_OK); STDRET(2); }
static void dv_SetEventNotif(void) { RET(DI_OK); STDRET(2); }
static void dv_SetCoopLevel(void)  { RET(DI_OK); STDRET(3); }
static void dv_GetObjectInfo(void) { RET(DI_OK); STDRET(4); }
static void dv_GetDeviceInfo(void) { RET(DI_OK); STDRET(2); }
static void dv_RunControlPanel(void){ RET(DI_OK); STDRET(3); }
static void dv_Initialize(void)    { RET(DI_OK); STDRET(4); }

static uint32_t build_vtbl(recomp_func_t* fns, int n) {
    uint32_t v = shim_alloc((uint32_t)n * 4);
    for (int i = 0; i < n; i++) MEM32(v + (uint32_t)i * 4) = com_reg(fns[i]);
    return v;
}
static void build_vtables(void) {
    if (g_dd_vtbl) return;
    recomp_func_t dd[] = {
        dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper,
        dd_CreatePalette, dd_CreateSurface, dd_DuplicateSurface, dd_EnumDisplayModes,
        dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
        dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFreq, dd_GetScanLine,
        dd_GetVBStatus, dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel,
        dd_SetDisplayMode, dd_WaitForVB,
    };
    recomp_func_t sf[] = {
        s_QueryInterface, s_AddRef, s_Release, s_AddAttached, s_AddOverlayDirty,
        s_Blt, s_BltBatch, s_BltFast, s_DeleteAttached, s_EnumAttached, s_EnumOverlayZ,
        s_Flip, s_GetAttached, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey,
        s_GetDC, s_GetFlipStatus, s_GetOverlayPos, s_GetPalette, s_GetPixelFormat,
        s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock, s_ReleaseDC, s_Restore,
        s_SetClipper, s_SetColorKey, s_SetOverlayPos, s_SetPalette, s_Unlock,
        s_UpdateOverlay, s_UpdateOverlayDisp, s_UpdateOverlayZ,
    };
    recomp_func_t pl[] = { p_QueryInterface, p_AddRef, p_Release, p_GetCaps,
                           p_GetEntries, p_Initialize, p_SetEntries };
    recomp_func_t di[] = { di_QueryInterface, di_AddRef, di_Release, di_CreateDevice,
                           di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel, di_Initialize };
    recomp_func_t dv[] = { dv_QueryInterface, dv_AddRef, dv_Release, dv_GetCapabilities,
                           dv_EnumObjects, dv_GetProperty, dv_SetProperty, dv_Acquire,
                           dv_Unacquire, dv_GetDeviceState, dv_GetDeviceData, dv_SetDataFormat,
                           dv_SetEventNotif, dv_SetCoopLevel, dv_GetObjectInfo, dv_GetDeviceInfo,
                           dv_RunControlPanel, dv_Initialize };
    g_dd_vtbl    = build_vtbl(dd, (int)(sizeof(dd) / sizeof(dd[0])));
    g_surf_vtbl  = build_vtbl(sf, (int)(sizeof(sf) / sizeof(sf[0])));
    g_pal_vtbl   = build_vtbl(pl, (int)(sizeof(pl) / sizeof(pl[0])));
    g_di_vtbl    = build_vtbl(di, (int)(sizeof(di) / sizeof(di[0])));
    g_didev_vtbl = build_vtbl(dv, (int)(sizeof(dv) / sizeof(dv[0])));
}

/* DirectDrawCreate(lpGUID, lplpDD, pUnkOuter) -> S_OK + *lplpDD = IDirectDraw obj */
void imp_DirectDrawCreate(void) {
    build_vtables();
    uint32_t obj = shim_alloc(8);
    MEM32(obj + 0) = g_dd_vtbl;
    if (ARG(1)) MEM32(ARG(1)) = obj;
    DLOG("DirectDrawCreate -> IDirectDraw 0x%08X (vtbl 0x%08X)\n", obj, g_dd_vtbl);
    RET(DD_OK); STDRET(3);
}

/* DirectInputCreateA(hinst, dwVersion, lplpDI, pUnkOuter) -> DI_OK + IDirectInput */
void imp_DirectInputCreateA(void) {
    build_vtables();
    uint32_t obj = shim_alloc(8);
    MEM32(obj + 0) = g_di_vtbl;
    if (ARG(2)) MEM32(ARG(2)) = obj;
    DLOG("DirectInputCreateA -> IDirectInput 0x%08X (vtbl 0x%08X)\n", obj, g_di_vtbl);
    RET(DI_OK); STDRET(4);
}
