/*
 * The host's DirectDraw: IDirectDraw, IDirectDrawSurface and
 * IDirectDrawPalette over memory, for a game written for DirectX 3.
 *
 * POD asks for an exclusive 640x480 fullscreen mode.
 * On a current Windows that is an emulated mode at best, takes over every
 * screen, and cannot be captured at all from a session with no display
 * (RDP from a phone, a headless recording). So DirectDrawCreate is answered
 * here: every surface is a DIB section in this process (GetDC works on it,
 * and the guest can Lock it, since guest and host share one address space),
 * the "display mode" is only remembered, and a present converts the primary
 * surface through its palette into a 32-bit shadow, which goes to the
 * window (StretchDIBits), to ffmpeg (--record), or nowhere (--headless).
 *
 * Only what the game calls is implemented; every other method logs itself
 * and fails, so a new call shows up in the log rather than as a fault.
 * From Streets of SimCity's host (ssc src/runtime/ddraw.c); POD added
 * IDirectDraw2 and slack around every surface. docs/architecture.md.
 */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw_host.h"
#include "present.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)

#define VW 640
#define VH 480

static ddh_opts_t g_o;
static HWND     g_wnd;
static WNDPROC  g_prev;
static int      g_mode_w = VW, g_mode_h = VH, g_mode_bpp = 8;
static long     g_frames, g_written;
static FILE*    g_ffmpeg;
static DWORD    g_rec_t0;
static uint32_t* g_shadow;               /* the screen as BGRA, top-down: g_screen_dc's DIB */
static HDC      g_screen_dc;             /* GetDC of the game's window: the screen itself */
#define SHADOW_BYTES ((size_t)VW * VH * 4)
static CRITICAL_SECTION g_lock;          /* the shadow: game thread vs the window's WM_PAINT */

#define TRACE(...) do { if (g_o.trace) fprintf(stderr, "[ddraw] " __VA_ARGS__); } while (0)
#define UNIMPL(name) do { fprintf(stderr, "[ddraw] %s: not implemented\n", name); return DDERR_UNSUPPORTED; } while (0)

/* ------------------------------------------------------------ objects */

typedef struct Palette {
    IDirectDrawPaletteVtbl* vt;
    LONG ref;
    PALETTEENTRY e[256];
} Palette;

typedef struct Surface {
    IDirectDrawSurfaceVtbl* vt;
    LONG ref;
    DWORD caps;                          /* DDSCAPS_* the game asked for */
    int w, h, bpp, pitch;
    HBITMAP dib;                         /* the pixels, so GetDC works */
    uint8_t* bits;
    HDC dc;                              /* while GetDC is out */
    struct Surface* back;                /* a flipping primary's back buffer */
    Palette* pal;
    DDCOLORKEY ck_src;
    int has_ck_src;
} Surface;

typedef struct DDraw {
    IDirectDrawVtbl* vt;
    LONG ref;
} DDraw;

static Surface* g_primary;
static PALETTEENTRY g_sys_pal[256];     /* the 8-bit screen's hardware palette, as GDI last realized it */

/* ------------------------------------------------------------ presenting */

static void shadow_from_primary(void) {
    Surface* s = g_primary;
    if (!s) return;
    int w = s->w < VW ? s->w : VW, h = s->h < VH ? s->h : VH;
    EnterCriticalSection(&g_lock);
    if (s->bpp == 8) {
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) {
            PALETTEENTRY p = s->pal ? s->pal->e[i] : (PALETTEENTRY){ (BYTE)i, (BYTE)i, (BYTE)i, 0 };
            lut[i] = (uint32_t)p.peRed << 16 | (uint32_t)p.peGreen << 8 | p.peBlue;
        }
        for (int y = 0; y < h; y++) {
            const uint8_t* src = s->bits + (size_t)y * s->pitch;
            uint32_t* dst = g_shadow + (size_t)y * VW;
            for (int x = 0; x < w; x++) dst[x] = lut[src[x]];
        }
    } else if (s->bpp == 16) {                /* RGB565 */
        for (int y = 0; y < h; y++) {
            const uint16_t* src = (const uint16_t*)(s->bits + (size_t)y * s->pitch);
            uint32_t* dst = g_shadow + (size_t)y * VW;
            for (int x = 0; x < w; x++) {
                uint32_t c = src[x];
                dst[x] = ((c >> 11) * 255 / 31) << 16 | (((c >> 5) & 63) * 255 / 63) << 8 | ((c & 31) * 255 / 31);
            }
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void save_bmp(const char* path) {
    BITMAPFILEHEADER fh = { 0x4D42 };
    BITMAPINFOHEADER ih = { sizeof ih, VW, -VH, 1, 32, BI_RGB };
    DWORD bytes = VW * VH * 4;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + bytes;
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[capture] cannot write %s\n", path); return; }
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(g_shadow, 1, bytes, f);
    fclose(f);
    fprintf(stderr, "[capture] frame %ld saved to %s\n", g_frames, path);
}

/* A fixed 30 fps against the wall clock: a slow stretch holds the last
 * picture, a fast one drops frames, so the video plays at the game's speed
 * (hover's record_tick). */
static void record_tick(void) {
    if (!g_ffmpeg) return;
    long due = (long)((GetTickCount() - g_rec_t0) * 30 / 1000) + 1;
    EnterCriticalSection(&g_lock);
    while (g_written < due) {
        fwrite(g_shadow, 4, (size_t)VW * VH, g_ffmpeg);
        g_written++;
    }
    LeaveCriticalSection(&g_lock);
}

/* The screen changed: a flip, a write to the primary, or a palette change
 * (the game fades by palette alone). */
static void present_(const char* why, int from_primary) {
    if (from_primary) shadow_from_primary();
    g_frames++;
    if (g_frames == 1 || g_frames == 100 || g_frames % 1000 == 0)
        fprintf(stderr, "[capture] frame %ld presented (%s)\n", g_frames, why);
    if (!g_o.headless) present_frame(g_shadow);     /* present.c draws it, on its own thread */
    record_tick();
    if (g_o.shot_frame && g_frames == g_o.shot_frame) save_bmp(g_o.shot_path);
    if (g_o.stop_after && g_frames >= g_o.stop_after) {
        fprintf(stderr, "[capture] %ld frames: stopping\n", g_frames);
        ddh_finish();
        fflush(stderr);
        ExitProcess(0);
    }
}
#define present(why) present_(why, 1)

/* --diff A,B: how much of the screen changed from A ms to B ms after
 * start. By the clock, not by frame, because the game presents in bursts
 * (an animation) and not at all while it waits for a click. */
static void diff_tick(void) {
    static uint32_t* snap;
    static int done;
    DWORD t = GetTickCount() - g_rec_t0;
    if (!g_o.diff_b || done) return;
    if (!snap && t >= (DWORD)g_o.diff_a) {
        snap = (uint32_t*)malloc(SHADOW_BYTES);
        EnterCriticalSection(&g_lock);
        memcpy(snap, g_shadow, SHADOW_BYTES);
        LeaveCriticalSection(&g_lock);
    } else if (snap && t >= (DWORD)g_o.diff_b) {
        size_t changed = 0;
        EnterCriticalSection(&g_lock);
        for (size_t i = 0; i < VW * VH; i++) changed += snap[i] != g_shadow[i];
        LeaveCriticalSection(&g_lock);
        fprintf(stderr, "[diff] %u%% of the screen changed from %ld ms to %ld ms\n",
                (unsigned)(changed * 100 / (VW * VH)), g_o.diff_a, g_o.diff_b);
        done = 1;
    }
}

/* Recording also runs while the game shows a still picture and presents
 * nothing (a menu waiting for a click), so the video's clock keeps going. */
static DWORD WINAPI clock_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(33);
        record_tick();
        diff_tick();
    }
}

void ddh_finish(void) {
    if (g_ffmpeg) {
        FILE* f = g_ffmpeg;
        g_ffmpeg = NULL;
        _pclose(f);
        fprintf(stderr, "[record] %ld frames written to %s\n", g_written, g_o.record);
    }
}

/* ------------------------------------------------------------ palette */

static HRESULT WINAPI pal_QueryInterface(IDirectDrawPalette* p, REFIID r, void** o) { (void)p; (void)r; *o = NULL; return E_NOINTERFACE; }
static ULONG WINAPI pal_AddRef(IDirectDrawPalette* p) { return InterlockedIncrement(&((Palette*)p)->ref); }
static ULONG WINAPI pal_Release(IDirectDrawPalette* p) {
    LONG n = InterlockedDecrement(&((Palette*)p)->ref);
    if (!n) free(p);
    return n;
}
static HRESULT WINAPI pal_GetCaps(IDirectDrawPalette* p, DWORD* c) { (void)p; *c = DDPCAPS_8BIT | DDPCAPS_ALLOW256; return DD_OK; }
static HRESULT WINAPI pal_GetEntries(IDirectDrawPalette* p, DWORD f, DWORD start, DWORD n, PALETTEENTRY* e) {
    (void)f;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(e, ((Palette*)p)->e + start, n * sizeof *e);
    return DD_OK;
}
static HRESULT WINAPI pal_Initialize(IDirectDrawPalette* p, IDirectDraw* d, DWORD f, PALETTEENTRY* e) { (void)p; (void)d; (void)f; (void)e; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI pal_SetEntries(IDirectDrawPalette* p, DWORD f, DWORD start, DWORD n, PALETTEENTRY* e) {
    (void)f;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(((Palette*)p)->e + start, e, n * sizeof *e);
    TRACE("Palette::SetEntries(%lu, %lu)\n", start, n);
    if (g_primary && g_primary->pal == (Palette*)p) present("palette");
    return DD_OK;
}
static IDirectDrawPaletteVtbl g_pal_vt = {
    pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries,
};

/* ------------------------------------------------------------ surface */

static IDirectDrawSurfaceVtbl g_surf_vt;

static Surface* surf_new(int w, int h, int bpp, DWORD caps) {
    Surface* s = (Surface*)calloc(1, sizeof *s);
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, (WORD)bpp } };
    if (bpp == 16) {                                 /* RGB565 needs its masks */
        bi.h.biCompression = BI_BITFIELDS;
        ((DWORD*)bi.c)[0] = 0xF800; ((DWORD*)bi.c)[1] = 0x07E0; ((DWORD*)bi.c)[2] = 0x001F;
    }
    for (int i = 0; bpp == 8 && i < 256; i++) bi.c[i].rgbRed = bi.c[i].rgbGreen = bi.c[i].rgbBlue = (BYTE)i;
    s->vt = &g_surf_vt;
    s->ref = 1;
    s->caps = caps;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->pitch = ((w * bpp / 8) + 3) & ~3;
    /* A page of slack on each side of the pixels. POD's 8-to-16-bit blit
     * (0x0049C8CD) stores its first pixel 2 bytes before the locked surface:
     * harmless in a 1997 card's video memory, a fault at the start of a DIB
     * section's own pages. The section is the DIB's memory; its view keeps
     * it alive once the handle is closed. */
    SIZE_T bytes = (SIZE_T)s->pitch * h;
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)(bytes + 2 * 4096), NULL);
    s->dib = sec ? CreateDIBSection(NULL, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&s->bits, sec, 4096) : NULL;
    if (sec) CloseHandle(sec);
    if (!s->dib) { free(s); return NULL; }
    memset(s->bits, 0, (size_t)s->pitch * h);
    return s;
}

#define S(p) ((Surface*)(p))

static HRESULT WINAPI s_QueryInterface(IDirectDrawSurface* p, REFIID r, void** o) {
    (void)p;
    fprintf(stderr, "[ddraw] Surface::QueryInterface({%08lX-...}): no\n", r->Data1);
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_AddRef(IDirectDrawSurface* p) { return InterlockedIncrement(&S(p)->ref); }
static ULONG WINAPI s_Release(IDirectDrawSurface* p) {
    Surface* s = S(p);
    LONG n = InterlockedDecrement(&s->ref);
    if (!n) {
        TRACE("Surface %dx%d released\n", s->w, s->h);
        if (s == g_primary) g_primary = NULL;
        if (s->back) s_Release((IDirectDrawSurface*)s->back);
        if (s->pal) pal_Release((IDirectDrawPalette*)s->pal);
        DeleteObject(s->dib);
        free(s);
    }
    return n;
}
static HRESULT WINAPI s_AddAttachedSurface(IDirectDrawSurface* p, IDirectDrawSurface* a) { (void)p; (void)a; UNIMPL("AddAttachedSurface"); }
static HRESULT WINAPI s_AddOverlayDirtyRect(IDirectDrawSurface* p, RECT* r) { (void)p; (void)r; UNIMPL("AddOverlayDirtyRect"); }

/* The primary is the whole screen, and the game addresses it in screen
 * coordinates: its window's client area, from ClientToScreen. Its window
 * sat at 0,0 of a 640x480 desktop; this one is wherever the host put it, so
 * a destination on the primary is taken relative to the client area. */
static void to_primary(int* l, int* t, int* r, int* b) {   /* LONG is int on Win32 */
    POINT o = { 0, 0 };
    if (!g_wnd || !ClientToScreen(g_wnd, &o)) return;
    *l -= o.x; *t -= o.y;
    if (r) { *r -= o.x; *b -= o.y; }
}

/* Clip a src rect and a dst position to both surfaces; 0 if nothing is left. */
static int clip(Surface* d, Surface* s, RECT* sr, int* dx, int* dy) {
    if (sr->left < 0) { *dx -= sr->left; sr->left = 0; }
    if (sr->top < 0) { *dy -= sr->top; sr->top = 0; }
    if (sr->right > s->w) sr->right = s->w;
    if (sr->bottom > s->h) sr->bottom = s->h;
    if (*dx < 0) { sr->left -= *dx; *dx = 0; }
    if (*dy < 0) { sr->top -= *dy; *dy = 0; }
    if (*dx + sr->right - sr->left > d->w) sr->right = sr->left + d->w - *dx;
    if (*dy + sr->bottom - sr->top > d->h) sr->bottom = sr->top + d->h - *dy;
    return sr->right > sr->left && sr->bottom > sr->top;
}

static void copy_rect(Surface* d, int dx, int dy, Surface* s, RECT sr, int keyed) {
    int bpp = d->bpp / 8, w = sr.right - sr.left;
    uint32_t key = s->ck_src.dwColorSpaceLowValue;
    for (int y = sr.top; y < sr.bottom; y++) {
        uint8_t* dst = d->bits + (size_t)(dy + y - sr.top) * d->pitch + (size_t)dx * bpp;
        const uint8_t* src = s->bits + (size_t)y * s->pitch + (size_t)sr.left * bpp;
        if (!keyed) memmove(dst, src, (size_t)w * bpp);
        else if (bpp == 1) { for (int x = 0; x < w; x++) if (src[x] != key) dst[x] = src[x]; }
        else for (int x = 0; x < w; x++)
            if (((const uint16_t*)src)[x] != key) ((uint16_t*)dst)[x] = ((const uint16_t*)src)[x];
    }
}

static void fill_rect(Surface* d, RECT r, uint32_t c) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > d->w) r.right = d->w;
    if (r.bottom > d->h) r.bottom = d->h;
    for (int y = r.top; y < r.bottom; y++) {
        uint8_t* row = d->bits + (size_t)y * d->pitch;
        if (d->bpp == 8) memset(row + r.left, (int)c, r.right - r.left);
        else for (int x = r.left; x < r.right; x++) ((uint16_t*)row)[x] = (uint16_t)c;
    }
}

static HRESULT WINAPI s_Blt(IDirectDrawSurface* p, RECT* dr, IDirectDrawSurface* src, RECT* sr, DWORD f, DDBLTFX* fx) {
    Surface* d = S(p), *s = S(src);
    RECT drc = dr ? *dr : (RECT){ 0, 0, d->w, d->h };
    if (dr && d == g_primary) to_primary((int*)&drc.left, (int*)&drc.top, (int*)&drc.right, (int*)&drc.bottom);
    TRACE("Blt %dx%d <- %p flags %08lX\n", d->w, d->h, (void*)s, f);
    if (f & DDBLT_COLORFILL) {
        fill_rect(d, drc, fx ? fx->dwFillColor : 0);
    } else if (s) {
        RECT src_rc = sr ? *sr : (RECT){ 0, 0, s->w, s->h };
        int dw = drc.right - drc.left, dh = drc.bottom - drc.top;
        int sw = src_rc.right - src_rc.left, sh = src_rc.bottom - src_rc.top;
        int keyed = (f & DDBLT_KEYSRC) && s->has_ck_src;
        if (dw == sw && dh == sh) {
            int dx = drc.left, dy = drc.top;
            if (clip(d, s, &src_rc, &dx, &dy)) copy_rect(d, dx, dy, s, src_rc, keyed);
        } else if (dw > 0 && dh > 0 && sw > 0 && sh > 0) {
            /* ponytail: nearest-neighbour stretch, 8-bit only and unclipped
             * beyond the surfaces; the game's stretches are all on screen. */
            for (int y = 0; y < dh; y++) {
                int ty = drc.top + y, fy = src_rc.top + y * sh / dh;
                if (ty < 0 || ty >= d->h || fy < 0 || fy >= s->h) continue;
                for (int x = 0; x < dw; x++) {
                    int tx = drc.left + x, fx_ = src_rc.left + x * sw / dw;
                    if (tx < 0 || tx >= d->w || fx_ < 0 || fx_ >= s->w) continue;
                    uint8_t c = s->bits[(size_t)fy * s->pitch + fx_];
                    if (!keyed || c != s->ck_src.dwColorSpaceLowValue) d->bits[(size_t)ty * d->pitch + tx] = c;
                }
            }
        }
    } else {
        fprintf(stderr, "[ddraw] Blt flags %08lX with no source: ignored\n", f);
    }
    if (d == g_primary) present("blt");
    return DD_OK;
}
static HRESULT WINAPI s_BltBatch(IDirectDrawSurface* p, DDBLTBATCH* b, DWORD n, DWORD f) { (void)p; (void)b; (void)n; (void)f; UNIMPL("BltBatch"); }
static HRESULT WINAPI s_BltFast(IDirectDrawSurface* p, DWORD x, DWORD y, IDirectDrawSurface* src, RECT* sr, DWORD t) {
    Surface* d = S(p), *s = S(src);
    RECT r = sr ? *sr : (RECT){ 0, 0, s->w, s->h };
    int dx = (int)x, dy = (int)y;
    if (d == g_primary) to_primary(&dx, &dy, NULL, NULL);
    TRACE("BltFast %dx%d <- %dx%d at %d,%d trans %lX\n", d->w, d->h, s->w, s->h, dx, dy, t);
    if (clip(d, s, &r, &dx, &dy)) copy_rect(d, dx, dy, s, r, (t & DDBLTFAST_SRCCOLORKEY) && s->has_ck_src);
    if (d == g_primary) present("bltfast");
    return DD_OK;
}
static HRESULT WINAPI s_DeleteAttachedSurface(IDirectDrawSurface* p, DWORD f, IDirectDrawSurface* a) { (void)p; (void)f; (void)a; return DD_OK; }
static HRESULT WINAPI s_EnumAttachedSurfaces(IDirectDrawSurface* p, void* c, LPDDENUMSURFACESCALLBACK cb) { (void)p; (void)c; (void)cb; UNIMPL("EnumAttachedSurfaces"); }
static HRESULT WINAPI s_EnumOverlayZOrders(IDirectDrawSurface* p, DWORD f, void* c, LPDDENUMSURFACESCALLBACK cb) { (void)p; (void)f; (void)c; (void)cb; UNIMPL("EnumOverlayZOrders"); }

/* The front and back buffers trade pixels, as the hardware would trade
 * scan-out addresses. */
static HRESULT WINAPI s_Flip(IDirectDrawSurface* p, IDirectDrawSurface* to, DWORD f) {
    Surface* a = S(p), *b = to ? S(to) : a->back;
    (void)f;
    if (!b) return DDERR_NOTFLIPPABLE;
    HBITMAP dib = a->dib; uint8_t* bits = a->bits;
    a->dib = b->dib; a->bits = b->bits;
    b->dib = dib; b->bits = bits;
    if (a == g_primary) present("flip");
    return DD_OK;
}
static HRESULT WINAPI s_GetAttachedSurface(IDirectDrawSurface* p, DDSCAPS* c, IDirectDrawSurface** o) {
    Surface* s = S(p);
    (void)c;
    if (!s->back) { *o = NULL; return DDERR_NOTFOUND; }
    s_AddRef((IDirectDrawSurface*)s->back);
    *o = (IDirectDrawSurface*)s->back;
    return DD_OK;
}
static HRESULT WINAPI s_GetBltStatus(IDirectDrawSurface* p, DWORD f) { (void)p; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetCaps(IDirectDrawSurface* p, DDSCAPS* c) { c->dwCaps = S(p)->caps; return DD_OK; }
static HRESULT WINAPI s_GetClipper(IDirectDrawSurface* p, IDirectDrawClipper** c) { (void)p; *c = NULL; return DDERR_NOCLIPPERATTACHED; }
static HRESULT WINAPI s_GetColorKey(IDirectDrawSurface* p, DWORD f, DDCOLORKEY* k) {
    if (!(f & DDCKEY_SRCBLT) || !S(p)->has_ck_src) return DDERR_NOCOLORKEY;
    *k = S(p)->ck_src;
    return DD_OK;
}
static HRESULT WINAPI s_GetDC(IDirectDrawSurface* p, HDC* o) {
    Surface* s = S(p);
    if (s->dc) return DDERR_DCALREADYCREATED;
    s->dc = CreateCompatibleDC(NULL);
    SelectObject(s->dc, s->dib);
    Palette* pal = s->pal ? s->pal : g_primary ? g_primary->pal : NULL;
    if (s->bpp == 8 && pal) {              /* GDI maps colours through the DIB's table */
        RGBQUAD q[256];
        for (int i = 0; i < 256; i++) {
            q[i].rgbRed = pal->e[i].peRed; q[i].rgbGreen = pal->e[i].peGreen;
            q[i].rgbBlue = pal->e[i].peBlue; q[i].rgbReserved = 0;
        }
        SetDIBColorTable(s->dc, 0, 256, q);
    }
    *o = s->dc;
    return DD_OK;
}
static HRESULT WINAPI s_GetFlipStatus(IDirectDrawSurface* p, DWORD f) { (void)p; (void)f; return DD_OK; }
static HRESULT WINAPI s_GetOverlayPosition(IDirectDrawSurface* p, LONG* x, LONG* y) { (void)p; (void)x; (void)y; UNIMPL("GetOverlayPosition"); }
static HRESULT WINAPI s_GetPalette(IDirectDrawSurface* p, IDirectDrawPalette** o) {
    Surface* s = S(p);
    if (!s->pal) { *o = NULL; return DDERR_NOPALETTEATTACHED; }
    pal_AddRef((IDirectDrawPalette*)s->pal);
    *o = (IDirectDrawPalette*)s->pal;
    return DD_OK;
}
static void pixel_format(int bpp, DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwRGBBitCount = bpp;
    if (bpp == 8) pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    else {
        pf->dwFlags = DDPF_RGB;
        pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
    }
}
static HRESULT WINAPI s_GetPixelFormat(IDirectDrawSurface* p, DDPIXELFORMAT* pf) { pixel_format(S(p)->bpp, pf); return DD_OK; }
static void describe(Surface* s, DDSURFACEDESC* d) {
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    pixel_format(s->bpp, &d->ddpfPixelFormat);
}
static HRESULT WINAPI s_GetSurfaceDesc(IDirectDrawSurface* p, DDSURFACEDESC* d) { describe(S(p), d); return DD_OK; }
static HRESULT WINAPI s_Initialize(IDirectDrawSurface* p, IDirectDraw* dd, DDSURFACEDESC* d) { (void)p; (void)dd; (void)d; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI s_IsLost(IDirectDrawSurface* p) { (void)p; return DD_OK; }
static HRESULT WINAPI s_Lock(IDirectDrawSurface* p, RECT* r, DDSURFACEDESC* d, DWORD f, HANDLE e) {
    Surface* s = S(p);
    (void)f; (void)e;
    describe(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->bits + (r ? (size_t)r->top * s->pitch + (size_t)r->left * (s->bpp / 8) : 0);
    return DD_OK;
}
static HRESULT WINAPI s_ReleaseDC(IDirectDrawSurface* p, HDC dc) {
    Surface* s = S(p);
    (void)dc;
    if (!s->dc) return DDERR_NODC;
    GdiFlush();
    DeleteDC(s->dc);
    s->dc = NULL;
    if (s == g_primary) present("gdi");
    return DD_OK;
}
static HRESULT WINAPI s_Restore(IDirectDrawSurface* p) { (void)p; return DD_OK; }
static HRESULT WINAPI s_SetClipper(IDirectDrawSurface* p, IDirectDrawClipper* c) { (void)p; (void)c; return DD_OK; }
static HRESULT WINAPI s_SetColorKey(IDirectDrawSurface* p, DWORD f, DDCOLORKEY* k) {
    Surface* s = S(p);
    if (f & DDCKEY_SRCBLT) {
        s->has_ck_src = k != NULL;
        if (k) s->ck_src = *k;
        TRACE("SetColorKey src %lu\n", k ? k->dwColorSpaceLowValue : 0);
        return DD_OK;
    }
    fprintf(stderr, "[ddraw] SetColorKey flags %08lX ignored\n", f);
    return DD_OK;
}
static HRESULT WINAPI s_SetOverlayPosition(IDirectDrawSurface* p, LONG x, LONG y) { (void)p; (void)x; (void)y; UNIMPL("SetOverlayPosition"); }
static HRESULT WINAPI s_SetPalette(IDirectDrawSurface* p, IDirectDrawPalette* pal) {
    Surface* s = S(p);
    if (pal) pal_AddRef(pal);
    if (s->pal) pal_Release((IDirectDrawPalette*)s->pal);
    s->pal = (Palette*)pal;
    if (s == g_primary) present("setpalette");
    return DD_OK;
}
static HRESULT WINAPI s_Unlock(IDirectDrawSurface* p, void* r) {
    (void)r;
    if (S(p) == g_primary) present("unlock");
    return DD_OK;
}
static HRESULT WINAPI s_UpdateOverlay(IDirectDrawSurface* p, RECT* a, IDirectDrawSurface* d, RECT* b, DWORD f, DDOVERLAYFX* fx) { (void)p; (void)a; (void)d; (void)b; (void)f; (void)fx; UNIMPL("UpdateOverlay"); }
static HRESULT WINAPI s_UpdateOverlayDisplay(IDirectDrawSurface* p, DWORD f) { (void)p; (void)f; UNIMPL("UpdateOverlayDisplay"); }
static HRESULT WINAPI s_UpdateOverlayZOrder(IDirectDrawSurface* p, DWORD f, IDirectDrawSurface* r) { (void)p; (void)f; (void)r; UNIMPL("UpdateOverlayZOrder"); }

static IDirectDrawSurfaceVtbl g_surf_vt = {
    s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt, s_BltBatch,
    s_BltFast, s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip,
    s_GetAttachedSurface, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus,
    s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock,
    s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey, s_SetOverlayPosition, s_SetPalette, s_Unlock,
    s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder,
};

/* ------------------------------------------------------------ IDirectDraw */

static IDirectDrawVtbl g_dd_vt;
static IDirectDraw2Vtbl g_dd2_vt;
/* IDirectDraw2 (POD asks for it) is IDirectDraw with a SetDisplayMode that
 * takes a refresh rate and flags, and GetAvailableVidMem: the same object
 * behind a second vtable. */
static HRESULT WINAPI dd_QueryInterface(IDirectDraw* p, REFIID r, void** o) {
    if (IsEqualGUID(r, &IID_IDirectDraw) || IsEqualGUID(r, &IID_IDirectDraw2)) {
        DDraw* dd = (DDraw*)calloc(1, sizeof *dd);
        dd->vt = IsEqualGUID(r, &IID_IDirectDraw2) ? (IDirectDrawVtbl*)&g_dd2_vt : &g_dd_vt;
        dd->ref = 1;
        TRACE("DirectDraw::QueryInterface(IDirectDraw%s)\n", dd->vt == &g_dd_vt ? "" : "2");
        *o = dd;
        return DD_OK;
    }
    (void)p;
    fprintf(stderr, "[ddraw] DirectDraw::QueryInterface({%08lX-...}): no\n", r->Data1);
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI dd_AddRef(IDirectDraw* p) { return InterlockedIncrement(&((DDraw*)p)->ref); }
static ULONG WINAPI dd_Release(IDirectDraw* p) {
    LONG n = InterlockedDecrement(&((DDraw*)p)->ref);
    TRACE("DirectDraw::Release -> %ld\n", n);
    if (!n) free(p);
    return n;
}
static HRESULT WINAPI dd_Compact(IDirectDraw* p) { (void)p; return DD_OK; }
/* A clipper only remembers its window: the game clips to a fullscreen
 * primary, which never overlaps anything, so there is nothing to clip. */
typedef struct Clipper {
    IDirectDrawClipperVtbl* vt;
    LONG ref;
    HWND wnd;
} Clipper;
#define C(p) ((Clipper*)(p))
static HRESULT WINAPI c_QueryInterface(IDirectDrawClipper* p, REFIID r, void** o) { (void)p; (void)r; *o = NULL; return E_NOINTERFACE; }
static ULONG WINAPI c_AddRef(IDirectDrawClipper* p) { return InterlockedIncrement(&C(p)->ref); }
static ULONG WINAPI c_Release(IDirectDrawClipper* p) {
    LONG n = InterlockedDecrement(&C(p)->ref);
    if (!n) free(p);
    return n;
}
static HRESULT WINAPI c_GetClipList(IDirectDrawClipper* p, RECT* r, RGNDATA* d, DWORD* n) { (void)p; (void)r; (void)d; (void)n; UNIMPL("Clipper::GetClipList"); }
static HRESULT WINAPI c_GetHWnd(IDirectDrawClipper* p, HWND* w) { *w = C(p)->wnd; return DD_OK; }
static HRESULT WINAPI c_Initialize(IDirectDrawClipper* p, IDirectDraw* d, DWORD f) { (void)p; (void)d; (void)f; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI c_IsClipListChanged(IDirectDrawClipper* p, BOOL* b) { (void)p; *b = FALSE; return DD_OK; }
static HRESULT WINAPI c_SetClipList(IDirectDrawClipper* p, RGNDATA* d, DWORD f) { (void)p; (void)d; (void)f; return DD_OK; }
static HRESULT WINAPI c_SetHWnd(IDirectDrawClipper* p, DWORD f, HWND w) { (void)f; C(p)->wnd = w; return DD_OK; }
static IDirectDrawClipperVtbl g_clip_vt = {
    c_QueryInterface, c_AddRef, c_Release, c_GetClipList, c_GetHWnd, c_Initialize, c_IsClipListChanged,
    c_SetClipList, c_SetHWnd,
};
static HRESULT WINAPI dd_CreateClipper(IDirectDraw* p, DWORD f, IDirectDrawClipper** o, IUnknown* u) {
    Clipper* c = (Clipper*)calloc(1, sizeof *c);
    (void)p; (void)f; (void)u;
    c->vt = &g_clip_vt;
    c->ref = 1;
    *o = (IDirectDrawClipper*)c;
    return DD_OK;
}
static HRESULT WINAPI dd_CreatePalette(IDirectDraw* p, DWORD f, PALETTEENTRY* e, IDirectDrawPalette** o, IUnknown* u) {
    Palette* pal = (Palette*)calloc(1, sizeof *pal);
    (void)p; (void)u;
    pal->vt = &g_pal_vt;
    pal->ref = 1;
    if (e) memcpy(pal->e, e, sizeof pal->e);
    TRACE("CreatePalette(%08lX)\n", f);
    *o = (IDirectDrawPalette*)pal;
    return DD_OK;
}
static HRESULT WINAPI dd_CreateSurface(IDirectDraw* p, DDSURFACEDESC* d, IDirectDrawSurface** o, IUnknown* u) {
    DWORD caps = d->dwFlags & DDSD_CAPS ? d->ddsCaps.dwCaps : 0;
    int bpp = d->dwFlags & DDSD_PIXELFORMAT ? (int)d->ddpfPixelFormat.dwRGBBitCount : g_mode_bpp;
    Surface* s;
    (void)p; (void)u;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = surf_new(g_mode_w, g_mode_h, g_mode_bpp, caps | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER);
        if (d->dwFlags & DDSD_BACKBUFFERCOUNT && d->dwBackBufferCount)
            s->back = surf_new(g_mode_w, g_mode_h, g_mode_bpp, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_VIDEOMEMORY);
        g_primary = s;
    } else {
        s = surf_new((int)d->dwWidth, (int)d->dwHeight, bpp, caps ? caps : DDSCAPS_OFFSCREENPLAIN);
    }
    fprintf(stderr, "[ddraw] CreateSurface(caps %08lX, %dx%dx%d%s) -> %p\n", caps, s ? s->w : 0, s ? s->h : 0,
            s ? s->bpp : 0, s && s->back ? ", 1 back buffer" : "", (void*)s);
    if (!s) return DDERR_OUTOFMEMORY;
    if (d->dwFlags & DDSD_CKSRCBLT) { s->ck_src = d->ddckCKSrcBlt; s->has_ck_src = 1; }
    *o = (IDirectDrawSurface*)s;
    return DD_OK;
}
static HRESULT WINAPI dd_DuplicateSurface(IDirectDraw* p, IDirectDrawSurface* s, IDirectDrawSurface** o) { (void)p; (void)s; (void)o; UNIMPL("DuplicateSurface"); }
static HRESULT WINAPI dd_EnumDisplayModes(IDirectDraw* p, DWORD f, DDSURFACEDESC* d, void* c, LPDDENUMMODESCALLBACK cb) {
    static const int modes[][3] = { { 640, 480, 8 }, { 640, 480, 16 } };
    (void)p; (void)f; (void)d;
    TRACE("EnumDisplayModes\n");
    for (int i = 0; i < 2; i++) {
        DDSURFACEDESC m;
        memset(&m, 0, sizeof m);
        m.dwSize = sizeof m;
        m.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        m.dwWidth = modes[i][0];
        m.dwHeight = modes[i][1];
        m.lPitch = modes[i][0] * modes[i][2] / 8;
        pixel_format(modes[i][2], &m.ddpfPixelFormat);
        if (cb(&m, c) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
static HRESULT WINAPI dd_EnumSurfaces(IDirectDraw* p, DWORD f, DDSURFACEDESC* d, void* c, LPDDENUMSURFACESCALLBACK cb) { (void)p; (void)f; (void)d; (void)c; (void)cb; UNIMPL("EnumSurfaces"); }
static HRESULT WINAPI dd_FlipToGDISurface(IDirectDraw* p) { (void)p; return DD_OK; }
static HRESULT WINAPI dd_GetCaps(IDirectDraw* p, DDCAPS* drv, DDCAPS* hel) {
    (void)p;
    /* A card from 1997 that can do what the game asks: blits, colour keys,
     * a palette, flipping. */
    TRACE("GetCaps\n");
    DDCAPS* c[2] = { drv, hel };
    for (int i = 0; i < 2; i++) {
        if (!c[i]) continue;
        DWORD size = c[i]->dwSize ? c[i]->dwSize : sizeof(DDCAPS);
        memset(c[i], 0, size);
        c[i]->dwSize = size;
        c[i]->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_BLTSTRETCH;
        c[i]->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c[i]->dwPalCaps = DDPCAPS_8BIT;
        c[i]->dwVidMemTotal = c[i]->dwVidMemFree = 8u << 20;
        c[i]->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN |
                               DDSCAPS_PALETTE | DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY;
    }
    return DD_OK;
}
static HRESULT WINAPI dd_GetDisplayMode(IDirectDraw* p, DDSURFACEDESC* d) {
    (void)p;
    TRACE("GetDisplayMode\n");
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = g_mode_w;
    d->dwHeight = g_mode_h;
    d->lPitch = g_mode_w * g_mode_bpp / 8;
    pixel_format(g_mode_bpp, &d->ddpfPixelFormat);
    return DD_OK;
}
static HRESULT WINAPI dd_GetFourCCCodes(IDirectDraw* p, DWORD* n, DWORD* c) { (void)p; (void)c; *n = 0; return DD_OK; }
static HRESULT WINAPI dd_GetGDISurface(IDirectDraw* p, IDirectDrawSurface** o) { (void)p; (void)o; UNIMPL("GetGDISurface"); }
static HRESULT WINAPI dd_GetMonitorFrequency(IDirectDraw* p, DWORD* f) { (void)p; *f = 60; return DD_OK; }
static HRESULT WINAPI dd_GetScanLine(IDirectDraw* p, DWORD* l) { (void)p; *l = 0; return DD_OK; }
static HRESULT WINAPI dd_GetVerticalBlankStatus(IDirectDraw* p, BOOL* b) { (void)p; *b = TRUE; return DD_OK; }
static HRESULT WINAPI dd_Initialize(IDirectDraw* p, GUID* g) { (void)p; (void)g; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI dd_RestoreDisplayMode(IDirectDraw* p) { (void)p; return DD_OK; }
static HRESULT WINAPI dd_SetCooperativeLevel(IDirectDraw* p, HWND w, DWORD f) {
    (void)p;
    fprintf(stderr, "[ddraw] SetCooperativeLevel(%p, %08lX)\n", (void*)w, f);
    return DD_OK;
}
static HRESULT WINAPI dd_SetDisplayMode(IDirectDraw* p, DWORD w, DWORD h, DWORD bpp) {
    (void)p;
    fprintf(stderr, "[ddraw] SetDisplayMode(%lux%lux%lu)\n", w, h, bpp);
    if (w != VW || h != VH || (bpp != 8 && bpp != 16)) return DDERR_INVALIDMODE;
    g_mode_w = (int)w; g_mode_h = (int)h; g_mode_bpp = (int)bpp;
    return DD_OK;
}
/* Paces the game at 60 Hz, as the vertical blank it waits for did. */
static HRESULT WINAPI dd_WaitForVerticalBlank(IDirectDraw* p, DWORD f, HANDLE e) {
    static DWORD next;
    (void)p; (void)f; (void)e;
    DWORD now = timeGetTime();
    if ((LONG)(next - now) > 0 && next - now < 17) Sleep(next - now);
    next = (now > next ? now : next) + 16;
    return DD_OK;
}

static IDirectDrawVtbl g_dd_vt = {
    dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
    dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus,
    dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank,
};

static HRESULT WINAPI dd2_SetDisplayMode(IDirectDraw* p, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD f) {
    (void)hz; (void)f;
    return dd_SetDisplayMode(p, w, h, bpp);
}
static HRESULT WINAPI dd2_GetAvailableVidMem(IDirectDraw* p, DDSCAPS* c, DWORD* total, DWORD* free_) {
    (void)p; (void)c;
    if (total) *total = 8u << 20;
    if (free_) *free_ = 8u << 20;
    return DD_OK;
}
/* Every method but the two above has the same arguments in both versions. */
static IDirectDraw2Vtbl g_dd2_vt = {
    (void*)dd_QueryInterface, (void*)dd_AddRef, (void*)dd_Release, (void*)dd_Compact, (void*)dd_CreateClipper,
    (void*)dd_CreatePalette, (void*)dd_CreateSurface, (void*)dd_DuplicateSurface, (void*)dd_EnumDisplayModes,
    (void*)dd_EnumSurfaces, (void*)dd_FlipToGDISurface, (void*)dd_GetCaps, (void*)dd_GetDisplayMode,
    (void*)dd_GetFourCCCodes, (void*)dd_GetGDISurface, (void*)dd_GetMonitorFrequency, (void*)dd_GetScanLine,
    (void*)dd_GetVerticalBlankStatus, (void*)dd_Initialize, (void*)dd_RestoreDisplayMode,
    (void*)dd_SetCooperativeLevel, (void*)dd2_SetDisplayMode, (void*)dd_WaitForVerticalBlank,
    (void*)dd2_GetAvailableVidMem,
};

/* ------------------------------------------------------------ the window */

/* The game's window is a borderless popup the size of the screen, made for a
 * display that has just switched to 640x480. Windowed, the presenter
 * (present.c) makes it an ordinary resizable window and maps the mouse back
 * to 640x480; headless, it is a cloaked 640x480 window nobody sees. */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT r;
    if (present_message(h, m, w, &l, &r)) return r;
    if (m == WM_ERASEBKGND) return 1;
    /* The game stops drawing when it loses activation, as on alt-tab.
     * Headless, the window is never active, so it never hears of it; and
     * windowed, the player can turn the pause off (Video menu). */
    if ((g_o.headless || !present_pause_in_background()) &&
        ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)))
        return 0;
    return CallWindowProcA(g_prev, h, m, w, l);
}

static void headless_window(void) {
    DWORD style = WS_OVERLAPPED | WS_CAPTION;
    RECT r = { 0, 0, VW, VH };
    AdjustWindowRect(&r, style, FALSE);
    SetWindowLongA(g_wnd, GWL_STYLE, style);
    SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void ddh_shim_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    int top = !a[8];
    DWORD ex = a[0], style = a[3];
    if (top) {
        ex &= ~WS_EX_TOPMOST;
        if (g_o.headless) { ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW; style &= ~WS_VISIBLE; }
    }
    mach_leave();
    HWND h = CreateWindowExA(ex, (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], style, (int)a[4], (int)a[5],
                             (int)a[6], (int)a[7], (HWND)(uintptr_t)a[8], (HMENU)(uintptr_t)a[9],
                             (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]);
    if (top && h && !g_wnd) {
        g_wnd = h;
        if (g_o.headless) {
            BOOL on = TRUE;
            DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof on);
        }
        g_prev = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)wndproc);
        if (g_o.headless) headless_window();
        else present_start(h);
        /* The game runs only while it is the active app (WM_ACTIVATEAPP
         * sets its flag at 0x0042CF80, and its loop waits in WaitMessage
         * otherwise). A cloaked window is never activated: tell it once. */
        if (g_o.headless) {
            ShowWindow(h, SW_SHOWNOACTIVATE);
            PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        }
    }
    mach_enter();
    fprintf(stderr, "[window] CreateWindowExA(\"%s\", %dx%d, style %08X) -> %shwnd %p\n",
            a[2] >> 16 ? (const char*)(uintptr_t)a[2] : "#", (int)a[6], (int)a[7], a[3],
            g_o.headless && top ? "cloaked " : "", (void*)h);
    RET((uintptr_t)h, 12);
}

/* The window procedure runs inside ShowWindow: the machine lock is dropped. */
void ddh_shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    if (w == g_wnd && g_o.headless && cmd != SW_HIDE && cmd != SW_MINIMIZE) cmd = SW_SHOWNOACTIVATE;
    mach_leave();
    BOOL r = ShowWindow(w, cmd);
    mach_enter();
    RET(r, 2);
}

/* The intro and the other Smacker videos are drawn by SMACKW32 straight into
 * GetDC of the game's window, not through DirectDraw. The window stands for a
 * whole 640x480 screen the game owns, so its DC is the screen: a memory DC
 * over the shadow, presented when the game gives it back. Headless or not,
 * this is the picture; the real window only ever shows the presenter's copy.
 * Any other window's DC is the real one. */
void ddh_shim_GetDC(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    RET(w && w == g_wnd ? (uintptr_t)g_screen_dc : (uintptr_t)GetDC(w), 1);
}
void ddh_shim_ReleaseDC(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    HDC dc = (HDC)(uintptr_t)ARG(1);
    if (dc == g_screen_dc) {
        GdiFlush();
        present_("window dc", 0);
        RET(1, 2);
        return;
    }
    RET(ReleaseDC(w, dc), 2);
}

/* The hardware palette of the 8-bit screen, as GDI last realized it. The
 * game sets it for a video by selecting a palette into GetDC(NULL) and
 * realizing it (sub_00526410); SMACKW32 then draws with DIB_PAL_COLORS,
 * indices into it. */
static UINT WINAPI realize(HDC dc) {
    HPALETTE p = (HPALETTE)GetCurrentObject(dc, OBJ_PAL);
    if (p && p != GetStockObject(DEFAULT_PALETTE)) {
        UINT n = GetPaletteEntries(p, 0, 256, g_sys_pal);
        TRACE("RealizePalette: %u entries, [1] = %02X%02X%02X\n", n, g_sys_pal[1].peRed, g_sys_pal[1].peGreen, g_sys_pal[1].peBlue);
    }
    return RealizePalette(dc);
}
void ddh_shim_RealizePalette(void) { RET(realize((HDC)(uintptr_t)ARG(0)), 1); }

/* The menus take their DirectDraw palette from the system palette
 * (sub_0053A0D0). A 32-bit desktop has none and answers 0 entries, which
 * left every menu black; the 8-bit screen's is g_sys_pal. */
void ddh_shim_GetSystemPaletteEntries(void) {
    uint32_t start = ARG(1), n = ARG(2);
    PALETTEENTRY* out = (PALETTEENTRY*)(uintptr_t)ARG(3);
    if (start >= 256) { RET(0, 4); return; }
    if (n > 256 - start) n = 256 - start;
    if (out) memcpy(out, g_sys_pal + start, n * sizeof *out);
    RET(out ? n : 256, 4);
}

/* SMACKW32's own GetDC, ReleaseDC and StretchDIBits (host.c points its import
 * table here): it draws each video frame into GetDC of the window it was
 * given, whatever DC the game hands SmackBufferBlit. */
static HDC WINAPI smack_GetDC(HWND w) { return w && w == g_wnd ? g_screen_dc : GetDC(w); }
static int WINAPI smack_ReleaseDC(HWND w, HDC dc) {
    if (dc != g_screen_dc) return ReleaseDC(w, dc);
    GdiFlush();
    present_("video", 0);
    return 1;
}
static int WINAPI smack_StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                      const void* bits, const BITMAPINFO* bi, UINT usage, DWORD rop) {
    if (dc == g_screen_dc && usage == DIB_PAL_COLORS) {
        struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } rgb;
        const WORD* idx = (const WORD*)bi->bmiColors;
        TRACE("StretchDIBits(DIB_PAL_COLORS) idx %u %u %u\n", idx[0], idx[1], idx[100]);
        int n = bi->bmiHeader.biClrUsed ? (int)bi->bmiHeader.biClrUsed : 256;
        rgb.h = bi->bmiHeader;
        for (int i = 0; i < n && i < 256; i++) {
            PALETTEENTRY e = g_sys_pal[idx[i] & 0xFF];
            rgb.c[i] = (RGBQUAD){ e.peBlue, e.peGreen, e.peRed, 0 };
        }
        return StretchDIBits(dc, x, y, w, h, sx, sy, sw, sh, bits, (BITMAPINFO*)&rgb, DIB_RGB_COLORS, rop);
    }
    return StretchDIBits(dc, x, y, w, h, sx, sy, sw, sh, bits, bi, usage, rop);
}

/* Point a native module's imports of these names at the host's. */
void ddh_hook_module(const char* dll) {
    static const struct { const char* name; void* fn; } hooks[] = {
        { "GetDC", (void*)smack_GetDC }, { "ReleaseDC", (void*)smack_ReleaseDC },
        { "StretchDIBits", (void*)smack_StretchDIBits }, { "RealizePalette", (void*)realize },
    };
    uint8_t* m = (uint8_t*)GetModuleHandleA(dll);
    if (!m) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* d =
        (IMAGE_IMPORT_DESCRIPTOR*)(m + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    int n = 0;
    for (; d->Name; d++) {
        uint32_t* ilt = (uint32_t*)(m + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(m + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            if (*ilt & 0x80000000u) continue;
            for (int i = 0; i < (int)(sizeof hooks / sizeof hooks[0]); i++)
                if (!strcmp((const char*)(m + *ilt + 2), hooks[i].name)) {
                    DWORD old;
                    VirtualProtect(iat, 4, PAGE_READWRITE, &old);
                    *iat = (uint32_t)(uintptr_t)hooks[i].fn;
                    VirtualProtect(iat, 4, old, &old);
                    n++;
                }
        }
    }
    fprintf(stderr, "[ddraw] %s: %d imports drawn into the host's screen\n", dll, n);
}

void ddh_shim_DirectDrawCreate(void) {
    uint32_t* out = (uint32_t*)(uintptr_t)ARG(1);
    DDraw* dd = (DDraw*)calloc(1, sizeof *dd);
    dd->vt = &g_dd_vt;
    dd->ref = 1;
    *out = (uint32_t)(uintptr_t)dd;
    fprintf(stderr, "[ddraw] DirectDrawCreate -> host DirectDraw %p\n", (void*)dd);
    RET(DD_OK, 3);
}

HWND ddh_window(void) { return g_wnd; }
int  ddh_focused(void) { return g_wnd && GetForegroundWindow() == g_wnd; }
long ddh_frames(void) { return g_frames; }

/* Headless, every top-level window on the game's thread is cloaked, not only
 * its main one: a window made some other way than the CreateWindowExA shim
 * (a system dialog) must not reach the screen either (REPO_RULES section 13).
 * A CBT hook catches them all as they are created. */
static LRESULT CALLBACK cloak_hook(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND && !(((CBT_CREATEWNDA*)l)->lpcs->style & WS_CHILD)) {
        BOOL on = TRUE;
        DwmSetWindowAttribute((HWND)w, DWMWA_CLOAK, &on, sizeof on);
    }
    return CallNextHookEx(NULL, code, w, l);
}

void ddh_init(const ddh_opts_t* o) {
    g_o = *o;
    InitializeCriticalSection(&g_lock);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), VW, -VH, 1, 32, BI_RGB } };
    HBITMAP dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&g_shadow, NULL, 0);
    g_screen_dc = CreateCompatibleDC(NULL);
    SelectObject(g_screen_dc, dib);
    if (o->headless) SetWindowsHookExA(WH_CBT, cloak_hook, NULL, GetCurrentThreadId());
    if (!o->headless) {
        /* pod.ini lives beside the exe: settings belong to
         * this build of the host, not to the game folder (Hover!'s rule). */
        char ini[MAX_PATH];
        GetModuleFileNameA(NULL, ini, MAX_PATH);
        strcpy(strrchr(ini, '\\') + 1, "pod.ini");
        present_init(ini, o->scale);
    }
    if (o->record) {
        char cmd[MAX_PATH + 256];
        _snprintf(cmd, sizeof cmd - 1,
                  "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
                  "-framerate 30 -i - -pix_fmt yuv420p \"%s\"", VW, VH, o->record);
        cmd[sizeof cmd - 1] = 0;
        g_ffmpeg = _popen(cmd, "wb");
        if (!g_ffmpeg) fprintf(stderr, "[record] cannot start ffmpeg (is it on PATH?)\n");
    }
    g_rec_t0 = GetTickCount();
    if (o->record || o->diff_b) CloseHandle(CreateThread(NULL, 0, clock_thread, NULL, 0, NULL));
}
