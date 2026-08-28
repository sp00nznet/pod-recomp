/*
 * POD Gold Static Recompilation - runtime + entry point.
 *
 * Builds the simulated address space (loads the original PE's sections at their
 * original VAs), defines the global register model, implements indirect-call
 * dispatch, installs the import bridges, and (optionally) calls the recompiled
 * entry point.
 *
 * The host exe is linked at a high image base (see CMakeLists / build) so the
 * original game's VA range [0x00400000 ..] is free and we can map it fixed,
 * keeping g_mem_base == 0 (ADDR(va) == va) for both data and the simulated stack.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>   /* timeGetTime / timeBeginPeriod */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include "recomp_types.h"
#include "imports.h"

/* ---- register model: per-thread (RECOMP_TLS); memory (g_mem_base) is shared ---- */
RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0, g_ebp = 0;
RECOMP_TLS uint32_t g_cur_func = 0;       /* VA of the function now running */
#ifdef RECOMP_TRACE
RECOMP_TLS uint32_t g_call_stack[RECOMP_STACK_SIZE];
RECOMP_TLS uint32_t g_call_depth = 0;
#endif

#ifdef RECOMP_TRACE
/* Report each (caller, callee) pair that leaks stack once, so one bad lift shows
 * up as a single line instead of thousands. */
void recomp_stack_leak(uint32_t caller, uint32_t callee, uint32_t esp0, uint32_t esp1) {
    static uint32_t seen[64][2];
    static uint32_t n;
    for (uint32_t i = 0; i < n; i++)
        if (seen[i][0] == caller && seen[i][1] == callee) return;
    if (n < 64) { seen[n][0] = caller; seen[n][1] = callee; n++; }
    fprintf(stderr, "[stack-leak] 0x%08X called 0x%08X: esp %08X -> %08X (%d bytes)\n",
            caller, callee, esp0, esp1, (int)esp0 - (int)esp1);
}
#endif

void recomp_dump_stack(const char* why) {
#ifdef RECOMP_TRACE
    uint32_t d = g_call_depth, shown = d < RECOMP_STACK_SIZE ? d : RECOMP_STACK_SIZE;
    fprintf(stderr, "\n=== call stack (tid=%lu): %s ===\n  0x%08X  <- now\n",
            GetCurrentThreadId(), why, g_cur_func);
    for (uint32_t i = shown; i-- > 0; )
        fprintf(stderr, "  0x%08X\n", g_call_stack[i]);
    if (d > shown) fprintf(stderr, "  ... %u deeper frames not recorded\n", d - shown);
#else
    (void)why;
#endif
}
RECOMP_TLS uint16_t g_seg_cs = 0, g_seg_ds = 0, g_seg_es = 0;
RECOMP_TLS uint16_t g_seg_fs = 0, g_seg_gs = 0, g_seg_ss = 0;
ptrdiff_t g_mem_base = 0;                 /* shared address space */
RECOMP_TLS uint32_t g_fs_base = 0;        /* per-thread TIB base (set per thread) */
RECOMP_TLS uint32_t g_gs_base = 0;

/* MMX register file: eight 64-bit registers, per-thread like the rest. POD is
 * the MMX build -- its software rasterizer lives in these. */
RECOMP_TLS uint64_t g_mm[8] = {0};

/* Per-thread x87 FPU stack (shared across calls on one thread). */
RECOMP_TLS double   g_st[8] = {0};
RECOMP_TLS int      g_fp_top = 0;
RECOMP_TLS uint16_t g_fpu_cw = 0x037F;

uint32_t g_icall_trace[ICALL_TRACE_SIZE] = {0};
uint32_t g_icall_trace_idx = 0;
uint32_t g_icall_count = 0;

/* Function-entry tracer (active with -DRECOMP_TRACE). */
#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE] = {0};
uint32_t g_enter_idx = 0;
/* Per-thread call-history ring (race-free, unlike the shared trace above). */
#define TLS_RING 256
RECOMP_TLS uint32_t g_tls_ring[TLS_RING] = {0};
RECOMP_TLS uint32_t g_tls_ridx = 0;
/* Every guest thread registers its ring, so any thread can dump all of them.
 * A hang is a question about the threads you are NOT in. */
#define MAX_RINGS 16
static struct { DWORD tid; uint32_t* ring; uint32_t* idx;
                uint32_t* stack; uint32_t* depth; uint32_t* cur; } g_rings[MAX_RINGS];
static volatile LONG g_nrings = 0;
static RECOMP_TLS int g_ring_registered = 0;

void recomp_dump_all_rings(const char* why) {
    LONG n = g_nrings;
    for (LONG i = 0; i < n && i < MAX_RINGS; i++) {
        uint32_t idx = *g_rings[i].idx;
        uint32_t c = idx < 16 ? idx : 16;
        uint32_t d = *g_rings[i].depth, shown = d < 24 ? d : 24;
        fprintf(stderr, "[threads] tid=%lu (%s) in %08X, stack:", g_rings[i].tid, why,
                *g_rings[i].cur);
        for (uint32_t k = 0; k < shown; k++)
            fprintf(stderr, " %08X", g_rings[i].stack[d - 1 - k]);
        fprintf(stderr, "  (depth %u)\n          last:", d);
        for (uint32_t k = 0; k < c; k++)
            fprintf(stderr, " %08X", g_rings[i].ring[(idx - c + k) & (TLS_RING - 1)]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

void recomp_dump_tls_ring(const char* why) {
    fprintf(stderr, "\n=== per-thread call ring (tid=%lu): %s ===\n", GetCurrentThreadId(), why);
    uint32_t n = g_tls_ridx < TLS_RING ? g_tls_ridx : TLS_RING;
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, "  %2u 0x%08X\n", i, g_tls_ring[(g_tls_ridx - n + i) & (TLS_RING-1)]);
}
static DWORD g_start_tick;   /* defined below; needed by the arg hook */
uint32_t g_poll_va = 0;      /* POD_POLL_VA */
void recomp_trace_enter(uint32_t va) {
    /* Record only game-engine functions in the per-thread ring (skip the Watcom
     * CRT/runtime region >= 0x004C8000), so a dump at exit shows the game code that
     * triggered it, not the CRT teardown chain. */
    if (!g_ring_registered) {
        g_ring_registered = 1;
        LONG slot = InterlockedIncrement(&g_nrings) - 1;
        if (slot < MAX_RINGS) {
            g_rings[slot].tid = GetCurrentThreadId();
            g_rings[slot].ring = g_tls_ring;
            g_rings[slot].idx = &g_tls_ridx;
            g_rings[slot].stack = g_call_stack;
            g_rings[slot].depth = &g_call_depth;
            g_rings[slot].cur = &g_cur_func;
        }
    }
    if (va < 0x004C8000u) g_tls_ring[g_tls_ridx++ & (TLS_RING-1)] = va;
    /* memset-overrun watch: the Watcom memset wrapper (sub_004C8368) takes its
     * count in EBX; its core (sub_004CA160) and fill loop (sub_004CA197) take it
     * in ECX. Catch a count larger than the whole arena at entry -- that's the
     * bogus-size bug -- and dump full context before it runs off the heap. */
    if ((va == 0x004C8368u && g_ebx > 0x08800000u) ||
        ((va == 0x004CA160u || va == 0x004CA197u) && g_ecx > 0x08800000u)) {
        fprintf(stderr, "\n[memset-watch] enter 0x%08X count(ebx=%08X ecx=%08X) "
                "dst(eax=%08X) val(edx=%08X) esi=%08X edi=%08X ebp_esp=%08X\n",
                va, g_ebx, g_ecx, g_eax, g_edx, g_esi, g_edi, g_esp);
        recomp_dump_trace("memset with arena-overrunning count");
        fflush(stderr);
        _Exit(8);
    }
    /* POD_ARGS_AT=<va>: print that function's arguments on entry -- registers
     * first (Watcom passes in eax/edx/ebx/ecx) then the stack, rendering anything
     * that points at a printable string as one. POD's error reporter takes its
     * message that way, and it is the only place the game says what went wrong. */
    extern uint32_t g_args_at_va;
    if (g_args_at_va && va == g_args_at_va) {
        static uint32_t n = 0, after = 0xFFFFFFFFu;
        if (after == 0xFFFFFFFFu) { const char* a = getenv("POD_ARGS_AFTER_MS"); after = a ? (uint32_t)strtoul(a, NULL, 0) : 0; }
        if ((uint32_t)(timeGetTime() - g_start_tick) >= after && n++ < 12) {
            fprintf(stderr, "[args] 0x%08X eax=%08X edx=%08X ebx=%08X ecx=%08X\n",
                    va, g_eax, g_edx, g_ebx, g_ecx);
            uint32_t vals[6] = { g_eax, g_edx, g_ebx, g_ecx,
                                 MEM32(g_esp + 4), MEM32(g_esp + 8) };
            for (int i = 0; i < 6; i++) {
                uint32_t p = vals[i];
                if (p < 0x00400000u || p >= 0x00800000u) continue;
                char buf[80]; int k = 0;
                for (; k < 79; k++) {
                    uint8_t c = MEM8(p + k);
                    if (!c) break;
                    if (c < 0x20 || c > 0x7E) { k = 0; break; }
                    buf[k] = (char)c;
                }
                if (k > 2) { buf[k] = 0; fprintf(stderr, "        [%d] \"%s\"\n", i, buf); }
            }
            if (getenv("POD_ARGS_RING")) recomp_dump_tls_ring("args hook");
            { const char* mv = getenv("POD_ARGS_MEM");
              if (mv) { uint32_t a = (uint32_t)strtoul(mv, NULL, 0);
                  fprintf(stderr, "        mem 0x%08X ->", a);
                  for (int k = 0; k < 12; k++) fprintf(stderr, " %08X", MEM32(a + k * 4));
                  fprintf(stderr, "\n"); } }
            /* Registers that look like guest pointers: show what they point at. */
            for (int i = 0; i < 4; i++) {
                uint32_t p = vals[i];
                if (p < 0x00400000u || p >= 0x08800000u || (p & 3)) continue;
                fprintf(stderr, "        [%d] 0x%08X ->", i, p);
                for (int k = 0; k < 6; k++) fprintf(stderr, " %08X", MEM32(p + k * 4));
                fprintf(stderr, "\n");
            }
        }
    }
    /* POD_POLL_VA=<va>: sample three dwords on every function entry and report
     * each change. Slower than the page trap but it cannot miss a write, which
     * matters when the page is busy and the field is one of many. */
    extern uint32_t g_poll_va;
    if (g_poll_va) {
        static uint32_t prev[3], seen, hits;
        uint32_t now0 = MEM32(g_poll_va), now1 = MEM32(g_poll_va + 4), now2 = MEM32(g_poll_va + 8);
        if (!seen) { seen = 1; prev[0] = now0; prev[1] = now1; prev[2] = now2; }
        else if ((now0 != prev[0] || now1 != prev[1] || now2 != prev[2]) && hits < 40) {
            /* The change happened between the previous function entry and this
             * one, so the writer is the function that was running: the last ring
             * entry, not `va`. */
            fprintf(stderr, "[poll] 0x%08X: %08X %08X %08X -> %08X %08X %08X  after:",
                    g_poll_va, prev[0], prev[1], prev[2], now0, now1, now2);
            for (int q = 6; q >= 1; q--)
                fprintf(stderr, " %08X", g_tls_ring[(g_tls_ridx - q) & (TLS_RING - 1)]);
            fprintf(stderr, "  next %08X\n", va);
            prev[0] = now0; prev[1] = now1; prev[2] = now2; hits++;
        }
    }
    host_watch_tick();
    extern uint32_t g_dump_arm_va;
    /* POD_DUMP_ARM: hold the POD_DUMP_AFTER snapshot until this function has run
     * once, so a frame can be captured after a particular pass rather than on the
     * first call of the run. */
    if (va == g_dump_arm_va) { extern int g_dump_armed; g_dump_armed = 1; }
    g_enter_trace[g_enter_idx % RECOMP_ENTER_SIZE] = va;
    g_enter_idx++;
    /* Loop watchdog: if we run an absurd number of function entries without
     * exiting, we're stuck -- dump the recent trace so we can see where. */
    if (g_enter_idx == 2000000000u) {
        recomp_dump_trace("loop watchdog (2B entries)");
        fflush(stderr);
        _Exit(7);
    }
}
#endif
#ifndef RECOMP_TRACE
/* The ring dumps are called from the fault handler and the shims, which are
 * always built; without the tracer there is nothing to print. */
void recomp_dump_tls_ring(const char* why)  { (void)why; }
void recomp_dump_all_rings(const char* why) { (void)why; }
#endif
void recomp_dump_trace(const char* why) {
#ifdef RECOMP_TRACE
    fprintf(stderr, "\n=== function-entry trace (last 48 before: %s) ===\n", why);
    uint32_t n = g_enter_idx < 256 ? g_enter_idx : 256;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_enter_idx - n + i) % RECOMP_ENTER_SIZE;
        fprintf(stderr, "  #%-3u 0x%08X\n", g_enter_idx - n + i, g_enter_trace[idx]);
    }
    fprintf(stderr, "  (total function entries: %u)\n", g_enter_idx);
#else
    (void)why;
#endif
}

/* ---- memory layout ----
 * The whole guest address space lives inside ONE host arena. We can't map fixed
 * at the guest VAs (0x400000+) because, with the host EXE linked high, the CRT
 * heap lands in that low region before our code runs. So we reserve one block
 * wherever Windows has room and set g_mem_base to it; every guest VA v is then at
 * host (g_mem_base + v). All guest memory (image, stack, TIB, heap) is addressed
 * through the MEM/ADDR macros, so a non-zero g_mem_base is transparent. */
#define POD_IMAGE_BASE 0x00400000u
#define POD_STACK_BASE 0x00200000u
#define POD_STACK_SIZE 0x00100000u  /* 1 MB */
#define POD_TIB_BASE   0x00100000u  /* simulated TIB/TEB (1 page) */
#define POD_TIB_SIZE   0x00001000u
#define POD_HEAP_BASE  0x00800000u  /* simulated heap (just above the image)     */
#define POD_HEAP_SIZE  0x08000000u  /* 128 MB                                    */
#define POD_ARENA_SIZE (POD_HEAP_BASE + POD_HEAP_SIZE)  /* covers all guest VAs   */

static void* g_arena = NULL;
static uint32_t g_heap_next = POD_HEAP_BASE;

uint32_t pod_arena_size(void) { return POD_ARENA_SIZE; }

/* The simulated heap is shared across guest threads; serialise allocations. */
static CRITICAL_SECTION g_alloc_cs;
static int g_alloc_cs_init = 0;
static void alloc_lock(void)   { if (g_alloc_cs_init) EnterCriticalSection(&g_alloc_cs); }
static void alloc_unlock(void) { if (g_alloc_cs_init) LeaveCriticalSection(&g_alloc_cs); }

/* Bump-allocate `n` bytes from the simulated heap, `align`-aligned; returns a VA. */
uint32_t shim_alloc_aligned(uint32_t n, uint32_t align) {
    alloc_lock();
    uint32_t mask = align - 1u;
    uint32_t va = (g_heap_next + mask) & ~mask;
    g_heap_next = va + ((n + 15u) & ~15u);
    if (g_heap_next > POD_HEAP_BASE + POD_HEAP_SIZE) {
        fprintf(stderr, "[shim_alloc] OUT OF SIMULATED HEAP\n");
        alloc_unlock();
        return 0;
    }
    alloc_unlock();
    return va;
}
uint32_t shim_alloc(uint32_t n) { return shim_alloc_aligned(n, 16); }

/* ---- host malloc/free/realloc/calloc: a simple free-list allocator that
 * replaces the lifted (and fragile) MSVC small-block heap. Each block carries a
 * 16-byte header [size, magic, free-next, pad]; data is 16-byte aligned. ---- */
#define HDR_MAGIC 0x46555259u  /* "FURY" */
static uint32_t g_free_head = 0;

uint32_t host_malloc(uint32_t n) {
    n = (n + 15u) & ~15u; if (!n) n = 16;
    alloc_lock();
    uint32_t prev = 0, cur = g_free_head;
    while (cur) {                                   /* first-fit over the free list */
        uint32_t hdr = cur - 16;
        if (MEM32(hdr) >= n) {
            if (prev) MEM32(prev - 16 + 8) = MEM32(hdr + 8); else g_free_head = MEM32(hdr + 8);
            alloc_unlock(); return cur;
        }
        prev = cur; cur = MEM32(hdr + 8);
    }
    uint32_t hdr = shim_alloc(16 + n);              /* recursive lock: safe */
    if (!hdr) { alloc_unlock(); return 0; }
    MEM32(hdr) = n; MEM32(hdr + 4) = HDR_MAGIC; MEM32(hdr + 8) = 0;
    alloc_unlock();
    return hdr + 16;
}
void host_free(uint32_t ptr) {
    if (!ptr) return;
    uint32_t hdr = ptr - 16;
    alloc_lock();
    if (MEM32(hdr + 4) == HDR_MAGIC) {              /* ours -> push on free list */
        MEM32(hdr + 8) = g_free_head; g_free_head = ptr;
    }
    alloc_unlock();
}
uint32_t host_realloc(uint32_t ptr, uint32_t n) {
    if (!ptr) return host_malloc(n);
    if (!n) { host_free(ptr); return 0; }
    uint32_t hdr = ptr - 16;
    if (MEM32(hdr + 4) == HDR_MAGIC && MEM32(hdr) >= ((n + 15u) & ~15u)) return ptr;
    uint32_t old = (MEM32(hdr + 4) == HDR_MAGIC) ? MEM32(hdr) : 0;
    uint32_t np = host_malloc(n);
    uint32_t cp = old < n ? old : n;
    for (uint32_t i = 0; i < cp; i++) MEM8(np + i) = MEM8(ptr + i);
    host_free(ptr);
    return np;
}
uint32_t host_calloc(uint32_t num, uint32_t sz) {
    uint32_t n = num * sz, p = host_malloc(n);
    for (uint32_t i = 0; i < n; i++) MEM8(p + i) = 0;
    return p;
}

/* Milliseconds since runtime start (for timeGetTime/GetTickCount shims). */
static DWORD g_start_tick = 0;
uint32_t host_millis(void) { return (uint32_t)(GetTickCount() - g_start_tick); }

/* ============================================================
 * Display: a real Win32 window + an 8-bit DIB the game renders into.
 *
 * The game CreateDIBSection's a 320x200 8bpp surface (we hand it a real pixel
 * buffer in simulated memory via ppvBits), renders into it, sets a 256-colour
 * palette, and StretchBlt's it to its window DC each frame. We map all that onto
 * one real window and StretchDIBits the buffer to it. Fake 32-bit handles stand
 * in for the GDI/USER objects (there's only ever one window/DC/DIB).
 * ============================================================ */
static void sim_read_str(uint32_t va, char* out, int outsz);  /* defined below */

#define H_HWND  0x00010001u
#define H_HDC   0x00020001u
#define H_DIB   0x00030001u
#define H_PAL   0x00040001u

static HWND g_hwnd = NULL;
static HDC  g_hdc  = NULL;
static uint32_t g_dib_buf = 0;          /* sim VA of the 8bpp pixel buffer */
static uint32_t g_dibs[8]; static int g_ndibs = 0;  /* all DIB surfaces (multi-buffered) */
static int g_dib_w = 320, g_dib_h = 200;
static RGBQUAD g_pal[256];
static int g_quit = 0;

static LRESULT CALLBACK pod_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { g_quit = 1; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_quit = 1; }
    return DefWindowProcA(h, m, w, l);
}

uint32_t host_create_window(uint32_t title_va) {
    char title[128]; sim_read_str(title_va, title, sizeof(title));
    if (!title[0]) snprintf(title, sizeof(title), "POD Gold (recompiled)");
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = pod_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "POD GoldWnd";
    RegisterClassA(&wc);
    RECT r = { 0, 0, g_dib_w * 2, g_dib_h * 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(0, "POD GoldWnd", title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    g_hdc = GetDC(g_hwnd);
    /* POD's WinMain blocks in MsgWaitForMultipleObjects(INFINITE) and only runs
     * its frame when something wakes it. The original ran fullscreen-exclusive
     * with input and paints arriving constantly; a windowed build that nobody
     * touches gets nothing and sleeps forever. A 60 Hz timer is the message
     * traffic the game expects. */
    SetTimer(g_hwnd, 1, 16, NULL);
    fprintf(stderr, "[gfx] window created (%dx%d)\n", g_dib_w, g_dib_h);
    return H_HWND;
}

/* CreateDIBSection: parse the BITMAPINFOHEADER, allocate a sim pixel buffer, and
 * hand its VA back through ppvBits so the game renders into real memory. */
uint32_t host_create_dib(uint32_t pbmi_va, uint32_t ppvbits_va) {
    int w = (int32_t)MEM32(pbmi_va + 4);
    int h = (int32_t)MEM32(pbmi_va + 8);
    uint16_t bpp = MEM16(pbmi_va + 14);
    if (w) g_dib_w = w;
    if (h) g_dib_h = h < 0 ? -h : h;
    uint32_t stride = ((uint32_t)g_dib_w * (bpp ? bpp : 8) + 31) / 32 * 4;
    g_dib_buf = shim_alloc(stride * g_dib_h + 64);
    if (g_ndibs < 8) g_dibs[g_ndibs++] = g_dib_buf;
    if (ppvbits_va) MEM32(ppvbits_va) = g_dib_buf;
    printf("[gfx] CreateDIBSection %dx%d %dbpp -> buf 0x%08X\n", g_dib_w, g_dib_h, bpp, g_dib_buf);
    return H_DIB;
}

void host_set_dib_colors(uint32_t start, uint32_t n, uint32_t prgb_va) {
    for (uint32_t i = 0; i < n && start + i < 256; i++) {
        uint32_t e = prgb_va + i * 4;            /* RGBQUAD: B,G,R,reserved */
        g_pal[start + i].rgbBlue  = MEM8(e + 0);
        g_pal[start + i].rgbGreen = MEM8(e + 1);
        g_pal[start + i].rgbRed   = MEM8(e + 2);
        g_pal[start + i].rgbReserved = 0;
    }
}

/* Blit the 8-bit DIB to the window, scaled to the client area. */
void host_blit(void) {
    if (!g_hwnd || !g_dib_buf) return;
    struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = g_dib_w;
    bi.h.biHeight = -g_dib_h;                    /* top-down */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.h.biCompression = BI_RGB;
    bi.h.biClrUsed = 256;
    memcpy(bi.pal, g_pal, sizeof(g_pal));
    RECT rc; GetClientRect(g_hwnd, &rc);
    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, g_dib_w, g_dib_h,
                  (void*)(uintptr_t)ADDR(g_dib_buf), (BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

/* ---- DirectDraw 16bpp present path (used by ddraw_shim.c) ----
 * POD's MMX renderer draws straight into a DirectDraw surface (no GDI DIB), so we
 * present that 16bpp buffer to the game's window with StretchDIBits. */
static int g_dd_w = 640, g_dd_h = 480;

void host_window_ensure(int w, int h) {
    g_dd_w = w; g_dd_h = h;
    if (!g_hwnd) { host_create_window(0); }
    /* Size the client area to the display mode (no upscale during bring-up). */
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(g_hwnd, SW_SHOW);
    printf("[gfx] display mode %dx%d (16bpp)\n", w, h);
}

void host_present16(uint32_t buf_va, int w, int h, int pitch) {
    if (!g_hwnd || !buf_va) return;
    { static uint32_t n = 0; uint32_t nz = 0;
      for (int i = 0; i < w * h * 2; i += 64) if (MEM8(buf_va + i)) nz++;
      if ((n++ % 60) == 0) fprintf(stderr, "[gfx] present16 #%u %dx%d buf=0x%08X non-zero=%u\n", n, w, h, buf_va, nz); }
    struct { BITMAPINFOHEADER h; DWORD mask[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = pitch ? pitch / 2 : w;        /* width in pixels = pitch/2 (16bpp) */
    bi.h.biHeight = -h;                          /* top-down */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.mask[0] = 0xF800; bi.mask[1] = 0x07E0; bi.mask[2] = 0x001F;  /* RGB 565 */
    /* POD_FRAME_DUMP=<path>: write the Nth presented frame as a 24bpp BMP, so a
     * headless run can show what actually reached the screen. */
    { static uint32_t seq = 0; static int taken = 0;
      const char* path = getenv("POD_FRAME_DUMP");
      const char* nstr = getenv("POD_FRAME_DUMP_N");
      const char* mstr = getenv("POD_FRAME_DUMP_MS");   /* by wall clock instead */
      uint32_t want = nstr ? (uint32_t)strtoul(nstr, NULL, 10) : 0;
      const char* estr = getenv("POD_FRAME_EVERY_MS");  /* a filmstrip */
      static uint32_t next_ms = 0; static int shot = 0;
      char numbered[512];
      uint32_t now_ms = (uint32_t)(timeGetTime() - g_start_tick);
      int due;
      if (estr) {
          uint32_t iv = (uint32_t)strtoul(estr, NULL, 10);
          due = (now_ms >= next_ms && shot < 60);
          if (due && path) {
              next_ms = now_ms + (iv ? iv : 1000);
              snprintf(numbered, sizeof(numbered), "%s.%02d.bmp", path, shot++);
              path = numbered;
          }
      } else {
          due = mstr ? (!taken && now_ms >= (uint32_t)strtoul(mstr, NULL, 10))
                     : (seq++ == want);
      }
      if (path && due) { taken = 1;
        FILE* f = fopen(path, "wb");
        if (f) {
            int stride = (w * 3 + 3) & ~3, sz = stride * h, pitch2 = pitch ? pitch : w * 2;
            uint8_t hdr[54] = {0}; uint32_t v;
            hdr[0]='B'; hdr[1]='M'; v = 54 + sz; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4); v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
            hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &sz, 4);
            fwrite(hdr, 1, 54, f);
            uint8_t* row = (uint8_t*)calloc(1, (size_t)stride);
            for (int y = h - 1; y >= 0; y--) {          /* BMP rows are bottom-up */
                for (int x = 0; x < w; x++) {
                    uint16_t p = *(uint16_t*)(uintptr_t)ADDR(buf_va + y * pitch2 + x * 2);
                    row[x*3+2] = (uint8_t)(((p >> 11) & 0x1F) << 3);
                    row[x*3+1] = (uint8_t)(((p >> 5)  & 0x3F) << 2);
                    row[x*3+0] = (uint8_t)((p & 0x1F) << 3);
                }
                fwrite(row, 1, (size_t)stride, f);
            }
            free(row); fclose(f);
            fprintf(stderr, "[gfx] wrote frame %u to %s (%dx%d)\n", want, path, w, h);
        }
      }
    }
    RECT rc; GetClientRect(g_hwnd, &rc);
    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, h,
                  (void*)(uintptr_t)ADDR(buf_va), (BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

/* Registry of DirectDraw 16bpp surface buffers, so the pump can auto-present
 * whichever one the game has rendered into -- POD may write pixels via the
 * GetSurfaceDesc/Lock buffer pointer and present through a path we don't see. */
#define MAX_DDSURF 32
static uint32_t g_ddsurf[MAX_DDSURF][4]; static int g_ddnsurf = 0;
void host_register_surface16(uint32_t buf, int w, int h, int pitch) {
    if (g_ddnsurf < MAX_DDSURF && buf) {
        g_ddsurf[g_ddnsurf][0] = buf; g_ddsurf[g_ddnsurf][1] = (uint32_t)w;
        g_ddsurf[g_ddnsurf][2] = (uint32_t)h; g_ddsurf[g_ddnsurf][3] = (uint32_t)pitch;
        g_ddnsurf++;
    }
}
/* POD_DUMP_AFTER: snapshot the last-registered surface (the back buffer) the
 * moment the named function returns -- what that drawing pass alone produced. */
uint32_t g_after_call_va = 0;
uint32_t g_args_at_va = 0;    /* POD_ARGS_AT */
uint32_t g_dump_arm_va = 0;   /* POD_DUMP_ARM: gate for the snapshot below */
int g_dump_armed = 1;         /* no gate unless POD_DUMP_ARM is set */
void recomp_after_call(void) {
    /* POD_RET_PRINT: report the return value instead of snapshotting a frame.
     * The only way to see what a lifted leaf function actually computed. */
    if (getenv("POD_RET_PRINT")) {
        static uint32_t n = 0;
        if (n++ < 12)
            fprintf(stderr, "[ret] 0x%08X -> eax=%08X edx=%08X ebx=%08X st0=%f\n",
                    g_after_call_va, g_eax, g_edx, g_ebx, g_st[0]);
        return;
    }
    static int done = 0;
    if (done || !g_ddnsurf || !g_dump_armed) return;
    done = 1;
    int i = g_ddnsurf - 1;
    uint32_t buf = g_ddsurf[i][0], w = g_ddsurf[i][1], h = g_ddsurf[i][2], p = g_ddsurf[i][3];
    FILE* f = fopen("after_call.bin", "wb");
    if (!f) return;
    for (uint32_t y = 0; y < h; y++)
        fwrite((const void*)(uintptr_t)ADDR(buf + y * p), 1, w * 2, f);
    fclose(f);
    fprintf(stderr, "[after] 0x%08X returned; dumped surface 0x%08X %ux%u\n",
            g_after_call_va, buf, w, h);
    /* Everything that touches the frame from here on is a later drawing pass.
     * POD_WATCH_ROW picks which scanline to trap. */
    { const char* r = getenv("POD_WATCH_ROW");
      if (r) host_watch_page(buf + (uint32_t)atoi(r) * p); }
}

static void auto_present_dd(void) {
    uint32_t best = 0, bestnz = 0, bw = 0, bh = 0, bp = 0;
    for (int i = 0; i < g_ddnsurf; i++) {
        uint32_t buf = g_ddsurf[i][0], w = g_ddsurf[i][1], h = g_ddsurf[i][2], p = g_ddsurf[i][3];
        uint32_t nz = 0; for (uint32_t o = 0; o < w * h * 2u; o += 64) if (MEM8(buf + o)) nz++;
        if (nz > bestnz) { bestnz = nz; best = buf; bw = w; bh = h; bp = p; }
    }
    if (best && bestnz) {
        static uint32_t k = 0;
        if ((k++ % 120) == 0) fprintf(stderr, "[gfx] auto-present DD surf 0x%08X %ux%u non-zero=%u (of %d)\n", best, bw, bh, bestnz, g_ddnsurf);
        host_present16(best, (int)bw, (int)bh, (int)bp);
    }
}

/* ---- real Win32 message delivery (PeekMessageA/GetMessageA/PostThreadMessageA) ----
 * POD's sound engine PostThreadMessageA's its worker thread; the worker GetMessage's
 * those. We back the guest message calls with real per-thread Win32 queues: window
 * messages are dispatched to our real window here; *thread* messages are handed to
 * the game. (Stub message calls made SoundEngine init report a fatal error.) */
static void fill_guest_msg(uint32_t lpMsg, const MSG* m) {
    if (!lpMsg) return;
    MEM32(lpMsg + 0)  = (m->hwnd == g_hwnd) ? H_HWND : 0;   /* report the game's handle */
    MEM32(lpMsg + 4)  = m->message;
    MEM32(lpMsg + 8)  = (uint32_t)m->wParam;
    MEM32(lpMsg + 12) = (uint32_t)m->lParam;
    MEM32(lpMsg + 16) = m->time;
    MEM32(lpMsg + 20) = (uint32_t)m->pt.x;
    MEM32(lpMsg + 24) = (uint32_t)m->pt.y;
}
/* POD's own window procedure, captured from the WNDCLASS it registers. Until it
 * is known, window messages go to our host proc as before. */
uint32_t g_guest_wndproc = 0;

/* Call POD's WndProc. It is __stdcall (CALLBACK): arguments right-to-left on the
 * stack, and its `ret 0x10` pops them along with the dummy return address, so the
 * guest esp balances by itself. Runs on the calling thread's guest CPU state,
 * which is why this is only ever called from inside a guest import shim. */
uint32_t call_guest_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_guest_wndproc) return 0;
    recomp_func_t fn = recomp_lookup_manual(g_guest_wndproc);
    if (!fn) fn = recomp_lookup(g_guest_wndproc);
    if (!fn) return 0;
    PUSH32(g_esp, lp);
    PUSH32(g_esp, wp);
    PUSH32(g_esp, msg);
    PUSH32(g_esp, hwnd);
    PUSH32(g_esp, RECOMP_RETADDR);
    fn();
    return g_eax;
}

/* Return 1 when a message is available in *m, 2 on WM_QUIT, 0 if none
 * (non-blocking). Window messages for the game's window are handed to the game
 * so its own loop dispatches them into its WndProc -- POD drives its state
 * machine with private RegisterWindowMessage IDs posted to itself, so eating
 * them here (as we used to) meant the machine never advanced. Anything else
 * still goes to the host proc. */
static int pump_to_thread_msg(MSG* m, int block) {
    for (;;) {
        if (!PeekMessageA(m, NULL, 0, 0, PM_REMOVE)) {
            if (!block) return 0;
            if (!GetMessageA(m, NULL, 0, 0)) return 2;   /* WM_QUIT */
        }
        if (!m->hwnd) return 1;                                    /* thread msg */
        if (m->hwnd == g_hwnd && g_guest_wndproc) { TranslateMessage(m); return 1; }
        TranslateMessage(m); DispatchMessageA(m);                  /* not ours */
    }
}
/* ============================================================
 * Keyboard
 *
 * Two sources, because POD reads keys two ways and we need to drive it two ways.
 * The state array is indexed by PC set-1 scancode, which is exactly what
 * DirectInput's DIK_* constants are -- MapVirtualKey does the translation, so
 * there is no table to get wrong.
 *
 *   - real keys, whenever our window is the foreground one, so it is playable;
 *   - POD_KEYS="1500:DOWN,2000:RETURN", a script of millisecond timestamps, so a
 *     headless run can walk the menus. Scripted keys are also posted as
 *     WM_KEYDOWN/WM_KEYUP, since a menu may read either.
 * ============================================================ */
#define KEY_HOLD_MS 150u

static uint8_t g_keys[256];
static struct { uint32_t ms; uint8_t vk; uint8_t fired; } g_script[64];
static int g_script_n = 0;

static uint8_t vk_from_name(const char* s, size_t n) {
    static const struct { const char* name; uint8_t vk; } tab[] = {
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
        {"RETURN",VK_RETURN},{"ENTER",VK_RETURN},{"ESC",VK_ESCAPE},
        {"ESCAPE",VK_ESCAPE},{"SPACE",VK_SPACE},{"TAB",VK_TAB},
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F10",VK_F10},
        {"BACK",VK_BACK},{"DELETE",VK_DELETE},
    };
    for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if (strlen(tab[i].name) == n && _strnicmp(tab[i].name, s, n) == 0) return tab[i].vk;
    if (n == 1) return (uint8_t)toupper((unsigned char)s[0]);   /* letters and digits */
    return 0;
}

void host_keys_init(void) {
    const char* spec = getenv("POD_KEYS");
    if (!spec) return;
    while (*spec && g_script_n < 64) {
        char* endp;
        uint32_t ms = (uint32_t)strtoul(spec, &endp, 10);
        if (endp == spec || *endp != ':') break;
        const char* name = endp + 1;
        const char* comma = strchr(name, ',');
        size_t len = comma ? (size_t)(comma - name) : strlen(name);
        uint8_t vk = vk_from_name(name, len);
        if (vk) {
            g_script[g_script_n].ms = ms;
            g_script[g_script_n].vk = vk;
            g_script[g_script_n].fired = 0;
            g_script_n++;
            fprintf(stderr, "[key] scripted %ums: %.*s (vk 0x%02X)\n", ms, (int)len, name, vk);
        }
        if (!comma) break;
        spec = comma + 1;
    }
}

/* Refresh the scancode array; post messages on scripted edges. */
void host_keys_poll(void) {
    memset(g_keys, 0, sizeof(g_keys));
    if (g_hwnd && GetForegroundWindow() == g_hwnd) {
        for (int vk = 8; vk < 256; vk++)
            if (GetAsyncKeyState(vk) & 0x8000) {
                UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
                if (sc && sc < 256) g_keys[sc] = 0x80;
            }
    }
    uint32_t now = (uint32_t)(timeGetTime() - g_start_tick);
    for (int i = 0; i < g_script_n; i++) {
        UINT sc = MapVirtualKeyA(g_script[i].vk, MAPVK_VK_TO_VSC);
        int down = now >= g_script[i].ms && now < g_script[i].ms + KEY_HOLD_MS;
        if (down && sc && sc < 256) g_keys[sc] = 0x80;
        if (down && !g_script[i].fired) {
            g_script[i].fired = 1;
            if (g_hwnd) {
                PostMessageA(g_hwnd, WM_KEYDOWN, g_script[i].vk, (LPARAM)(sc << 16) | 1);
                PostMessageA(g_hwnd, WM_KEYUP,   g_script[i].vk,
                             (LPARAM)(sc << 16) | 0xC0000001u);
            }
            fprintf(stderr, "[key] press vk 0x%02X at %ums\n", g_script[i].vk, now);
        }
    }
}

/* DirectInput keyboard state: 256 bytes, high bit set on the keys held. */
int host_key_state(uint32_t buf, uint32_t n) {
    if (n < 256 || !buf) return 0;
    host_keys_poll();
    for (uint32_t i = 0; i < 256; i++) MEM8(buf + i) = g_keys[i];
    return 1;
}

/* The game addresses its window by our fake handle; these translate to the real
 * one so its posts and default handling reach Windows. */
uint32_t host_post_window_message(uint32_t msg, uint32_t wp, uint32_t lp) {
    return g_hwnd ? (uint32_t)PostMessageA(g_hwnd, msg, wp, lp) : 0;
}
uint32_t host_def_window_proc(uint32_t msg, uint32_t wp, uint32_t lp) {
    return g_hwnd ? (uint32_t)DefWindowProcA(g_hwnd, msg, wp, lp) : 0;
}

void imp_PeekMessageA(void) {   /* (lpMsg, hWnd, min, max, removeFlag) */
    host_keys_poll();   /* scripted keys post their messages from here */
    auto_present_dd();
    MSG m; int r = pump_to_thread_msg(&m, 0);
    if (r == 1) fill_guest_msg(ARG(0), &m);
    RET(r == 1 ? 1 : 0); STDRET(5);
}
void imp_GetMessageA(void) {    /* (lpMsg, hWnd, min, max) */
    auto_present_dd();
    MSG m; int r = pump_to_thread_msg(&m, 1);
    if (r == 2) { if (ARG(0)) MEM32(ARG(0) + 4) = 0x0012; RET(0); STDRET(4); return; }  /* WM_QUIT */
    fill_guest_msg(ARG(0), &m);
    RET(1); STDRET(4);
}
/* Real high-resolution, STRICTLY INCREASING millisecond clock. POD calibrates the
 * CPU speed by dividing an RDTSC delta by a timeGetTime delta over a tiny fixed
 * delay; on fast hardware that delay is sub-millisecond, so two raw timeGetTime
 * reads can be equal -> divide-by-zero. Guaranteeing the value advances by >=1 per
 * call kills that (and any timeGetTime spin-wait), at the cost of time running a
 * hair fast under extremely tight polling -- negligible for POD. */
void imp_timeGetTime(void) {
    static volatile LONG last = 0;
    uint32_t t = (uint32_t)(timeGetTime() - g_start_tick);
    LONG cur;
    do { cur = last; if ((LONG)t <= cur) t = (uint32_t)(cur + 1); }
    while (InterlockedCompareExchange(&last, (LONG)t, cur) != cur);
    RET(t); STDRET(0);
}

void imp_PostThreadMessageA(void) {  /* (idThread, Msg, wParam, lParam) */
    DWORD tid = ARG(0), msg = ARG(1), w = ARG(2), l = ARG(3);
    BOOL ok = 0;
    /* The target thread's message queue may not exist yet (it's created on the
     * thread's first message call) -- PostThreadMessage fails with
     * ERROR_INVALID_THREAD_ID(1444) until then. Retry briefly so the post lands. */
    for (int i = 0; i < 200; i++) {
        ok = PostThreadMessageA(tid, msg, w, l);
        if (ok || GetLastError() != 1444) break;
        Sleep(1);
    }
    RET(ok ? 1 : 0); STDRET(4);
}

/* Pump the window's messages, and present the game's DIB. Returns 1 to quit. */
int host_pump(void) {
    MSG msg;
    while (g_hwnd && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    auto_present_dd();
    /* Auto-present: the game renders into the DIB but may drive the blit through a
     * path we don't see; presenting here each pump shows the frame regardless. */
    static uint32_t pumps = 0;
    if ((pumps++ % 4) == 0) {
        /* present whichever surface has the most content (the active buffer);
         * also consider the game's 320x200 work buffer at [0x4EBC48]. */
        uint32_t best = g_dib_buf, bestnz = 0;
        uint32_t cands[10]; int nc = 0;
        for (int i = 0; i < g_ndibs; i++) cands[nc++] = g_dibs[i];
        for (int c = 0; c < nc; c++) {
            uint32_t nz = 0, b = cands[c];
            for (int i = 0; i < g_dib_w * g_dib_h; i += 16) if (MEM8(b + i)) nz++;
            if (nz > bestnz) { bestnz = nz; best = b; }
        }
        if ((pumps % 240) == 1)
            fprintf(stderr, "[gfx] present #%u  best surface 0x%08X non-zero=%u (of %d DIBs)\n",
                    pumps, best, bestnz, g_ndibs);
        if (best) { g_dib_buf = best; host_blit(); }
    }
    return g_quit;
}

/* ============================================================
 * Host file + INI I/O (backed by the real game directory).
 *
 * The game uses paths like ".\system\pod.ini", "C:\POD\data\...", or bare
 * "mapback.raw". We translate them all to live under POD_GAME_ROOT so reads/
 * writes hit real files. Implemented here (where windows.h is available);
 * shims_impl.c calls these.
 * ============================================================ */
#define POD_GAME_ROOT "game_files"

/* Read a NUL-terminated string out of simulated memory into a host buffer. */
static void sim_read_str(uint32_t va, char* out, int outsz) {
    int i = 0;
    if (!va) { out[0] = 0; return; }
    for (; i < outsz - 1; i++) { uint8_t c = MEM8(va + i); if (!c) break; out[i] = (char)c; }
    out[i] = 0;
}

/* ---- basename index: the game opens many assets by bare name (e.g. "laser.bin"
 * really MODELS/LASER.BIN). We scan the game tree once and resolve a bare name to
 * its real path when the literal path doesn't exist. ---- */
#define POD_MAX_INDEX 8192
static char  g_idx_name[POD_MAX_INDEX][28];   /* lowercase basename */
static char  g_idx_path[POD_MAX_INDEX][260];  /* full host path     */
static int   g_idx_count = 0;

static void index_dir(const char* dir) {
    char pat[300]; snprintf(pat, sizeof(pat), "%s/*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char full[300]; snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) index_dir(full);
        else if (g_idx_count < POD_MAX_INDEX) {
            int i = 0; for (; fd.cFileName[i] && i < 27; i++)
                g_idx_name[g_idx_count][i] = (char)tolower((unsigned char)fd.cFileName[i]);
            g_idx_name[g_idx_count][i] = 0;
            snprintf(g_idx_path[g_idx_count], 260, "%s", full);
            g_idx_count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
void host_index_build(void) { g_idx_count = 0; index_dir(POD_GAME_ROOT); }

/* If `host` doesn't exist, try to resolve its basename via the index. */
static void index_resolve(char* host, int hostsz) {
    DWORD a = GetFileAttributesA(host);
    if (a != INVALID_FILE_ATTRIBUTES) return;            /* exact path exists */
    const char* b = host; for (const char* p = host; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    char low[64]; int i = 0; for (; b[i] && i < 63; i++) low[i] = (char)tolower((unsigned char)b[i]); low[i] = 0;
    for (int k = 0; k < g_idx_count; k++)
        if (!strcmp(g_idx_name[k], low)) { snprintf(host, hostsz, "%s", g_idx_path[k]); return; }
}

/* Translate a game path to a host path under the game root. */
void host_translate_path(uint32_t va, char* out, int outsz) {
    char p[512]; sim_read_str(va, p, sizeof(p));
    for (char* q = p; *q; q++) if (*q == '\\') *q = '/';
    const char* rel = p;
    /* strip a drive letter ("c:/...") */
    if (rel[0] && rel[1] == ':') rel += 2;
    /* strip everything up to and incl. a "pod/" segment, else a leading "./" or "/" */
    char low[512]; int i; for (i = 0; rel[i] && i < 511; i++) low[i] = (char)tolower((unsigned char)rel[i]); low[i] = 0;
    char* f = strstr(low, "pod/");
    if (f) rel += (f - low) + 4;   /* skip the "pod/" segment (4 chars) */
    else { if (rel[0] == '.' && rel[1] == '/') rel += 2; else if (rel[0] == '/') rel += 1; }
    snprintf(out, outsz, "%s/%s", POD_GAME_ROOT, rel);
    index_resolve(out, outsz);   /* fall back to basename match if exact path missing */
    /* POD_IO_LOG=<n> raises the cap (0 = every open). The default hid the whole
     * race load behind the startup chatter. */
    { static int n = 0, cap = -1;
      if (cap < 0) { const char* e = getenv("POD_IO_LOG"); cap = e ? atoi(e) : 80; }
      if (cap == 0 || n++ < cap) fprintf(stderr, "[io] path '%s' -> '%s'\n", p, out); }
}

/* File handle table: sim handle = index + base. */
#define POD_HANDLE_BASE 0x000A0000u
#define POD_MAX_FILES   64
static HANDLE g_files[POD_MAX_FILES];

uint32_t host_file_open(uint32_t name_va, uint32_t access, uint32_t disp) {
    char host[600]; host_translate_path(name_va, host, sizeof(host));
    DWORD acc = (access & 0x40000000u) ? GENERIC_WRITE : GENERIC_READ;
    if (access & 0x80000000u) acc = GENERIC_READ;
    DWORD cd = OPEN_EXISTING;
    if (disp == 2) cd = CREATE_ALWAYS;            /* CREATE_ALWAYS */
    else if (disp == 1) cd = CREATE_NEW;
    else if (disp == 4) cd = OPEN_ALWAYS;
    /* Write streams seek back to rewrite a header, so they need read access too. */
    if (acc == GENERIC_WRITE) acc = GENERIC_READ | GENERIC_WRITE;
    HANDLE h = CreateFileA(host, acc, FILE_SHARE_READ, NULL, cd, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[io] open FAILED: %s\n", host);
        return 0xFFFFFFFFu;
    }
    for (int i = 0; i < POD_MAX_FILES; i++) if (!g_files[i]) {
        g_files[i] = h;
        return POD_HANDLE_BASE + i;
    }
    CloseHandle(h); return 0xFFFFFFFFu;
}
static HANDLE handle_of(uint32_t sh) {
    uint32_t i = sh - POD_HANDLE_BASE;
    return (i < POD_MAX_FILES) ? g_files[i] : NULL;
}
uint32_t host_file_read(uint32_t sh, uint32_t buf_va, uint32_t n) {
    HANDLE h = handle_of(sh); if (!h) return 0;
    DWORD got = 0; ReadFile(h, (void*)(uintptr_t)ADDR(buf_va), n, &got, NULL);
    return got;
}
uint32_t host_file_write(uint32_t sh, uint32_t buf_va, uint32_t n) {
    HANDLE h = handle_of(sh); if (!h) return 0;
    DWORD put = 0; WriteFile(h, (void*)(uintptr_t)ADDR(buf_va), n, &put, NULL);
    return put;
}
uint32_t host_file_seek(uint32_t sh, int32_t dist, uint32_t method) {
    HANDLE h = handle_of(sh); if (!h) return 0xFFFFFFFFu;
    return (uint32_t)SetFilePointer(h, dist, NULL, method);
}
uint32_t host_file_size(uint32_t sh) {
    HANDLE h = handle_of(sh); if (!h) return 0xFFFFFFFFu;
    return (uint32_t)GetFileSize(h, NULL);
}
void host_file_close(uint32_t sh) {
    uint32_t i = sh - POD_HANDLE_BASE;
    if (i < POD_MAX_FILES && g_files[i]) { CloseHandle(g_files[i]); g_files[i] = NULL; }
}
int host_file_getc(uint32_t sh) {
    HANDLE h = handle_of(sh); if (!h) return -1;
    uint8_t b; DWORD got = 0;
    if (!ReadFile(h, &b, 1, &got, NULL) || got != 1) return -1;
    return b;
}
int host_is_file(uint32_t sh) { return sh >= POD_HANDLE_BASE && sh < POD_HANDLE_BASE + POD_MAX_FILES; }

/* fopen returns a pointer to a real (mapped) MSVC-style FILE struct, because the
 * game dereferences it (the getc() macro reads _cnt/_ptr, code reads _file). We
 * keep _cnt=0 so getc always falls through to _filbuf, and stash our file-slot
 * index in _file (+16). Stream->handle conversion reads _file back. */
#define FILE_CNT  4    /* int _cnt   */
#define FILE_FLAG 12   /* int _flag  */
#define FILE_FILE 16   /* int _file  */
#define FILE_STRUCT_SIZE 32

uint32_t host_fopen(uint32_t name_va, uint32_t mode_va) {
    char m[16]; sim_read_str(mode_va, m, sizeof(m));
    uint32_t access = 0x80000000u, disp = 3 /*OPEN_EXISTING*/;
    for (char* p = m; *p; p++) {
        if (*p == 'w') { access = 0x40000000u; disp = 2 /*CREATE_ALWAYS*/; }
        else if (*p == 'a') { access = 0x40000000u; disp = 4 /*OPEN_ALWAYS*/; }
        else if (*p == '+') { access = 0xC0000000u; }
    }
    uint32_t h = host_file_open(name_va, access, disp);
    if (h == 0xFFFFFFFFu) return 0;                 /* fopen -> NULL on failure */
    uint32_t i = h - POD_HANDLE_BASE;
    uint32_t f = shim_alloc(FILE_STRUCT_SIZE);
    for (int o = 0; o < FILE_STRUCT_SIZE; o += 4) MEM32(f + o) = 0;
    MEM32(f + FILE_CNT) = 0;                         /* getc -> _filbuf */
    MEM32(f + FILE_FILE) = i;                        /* our file-slot index */
    MEM32(f + FILE_FLAG) = (access == 0x80000000u) ? 1 : 2;
    return f;
}
/* Map a FILE* stream (our struct) to its sim file handle; 0 for console/_iob. */
uint32_t host_stream_handle(uint32_t stream_va) {
    if (stream_va < POD_HEAP_BASE) return 0;      /* _iob stdout/stderr/stdin */
    uint32_t i = MEM32(stream_va + FILE_FILE);
    return (i < POD_MAX_FILES) ? POD_HANDLE_BASE + i : 0;
}
/* Set/clear the EOF flag on a stream after a read. */
void host_stream_set_eof(uint32_t stream_va, int eof) {
    if (stream_va >= POD_HEAP_BASE) {
        uint32_t fl = MEM32(stream_va + FILE_FLAG);
        MEM32(stream_va + FILE_FLAG) = eof ? (fl | 0x10) : (fl & ~0x10u);
    }
}

/* stat(): fill an MSVC _stat struct (st_mode@+4, st_size@+16) for a game path.
 * Returns 0 if the file exists, -1 otherwise. */
uint32_t host_stat(uint32_t name_va, uint32_t buf_va) {
    char host[600]; host_translate_path(name_va, host, sizeof(host));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(host, GetFileExInfoStandard, &fad)) {
        fprintf(stderr, "[io] stat FAILED: %s\n", host);
        return 0xFFFFFFFFu;
    }
    for (int o = 0; o < 32; o += 4) MEM32(buf_va + o) = 0;
    int dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    MEM16(buf_va + 4) = (uint16_t)(dir ? 0x4000 : 0x8000) | 0x1B6;  /* st_mode */
    MEM32(buf_va + 16) = fad.nFileSizeLow;                          /* st_size */
    return 0;
}

/* Length of a game file by path (the game's own POD length lookup isn't wired up,
 * so we answer from the extracted loose files). Returns size, or -1 if missing. */
uint32_t host_filelength(uint32_t name_va) {
    char host[600]; host_translate_path(name_va, host, sizeof(host));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(host, GetFileExInfoStandard, &fad)) return 0xFFFFFFFFu;
    return fad.nFileSizeLow;
}

/* INI: delegate to the real Win32 profile API with a translated file path. */
uint32_t host_ini_string(uint32_t sect, uint32_t key, uint32_t def,
                         uint32_t buf, uint32_t bufsz, uint32_t file) {
    char s[128], k[128], d[256], host[600];
    sim_read_str(sect, s, sizeof(s)); sim_read_str(key, k, sizeof(k));
    sim_read_str(def, d, sizeof(d));  host_translate_path(file, host, sizeof(host));
    char tmp[1024];
    DWORD n = GetPrivateProfileStringA(s, key ? k : NULL, d, tmp, sizeof(tmp), host);
    n = n < bufsz ? n : (bufsz ? bufsz - 1 : 0);
    for (DWORD i = 0; i < n; i++) MEM8(buf + i) = (uint8_t)tmp[i];
    if (bufsz) MEM8(buf + n) = 0;
    return n;
}
uint32_t host_ini_int(uint32_t sect, uint32_t key, uint32_t def, uint32_t file) {
    char s[128], k[128], host[600];
    sim_read_str(sect, s, sizeof(s)); sim_read_str(key, k, sizeof(k));
    host_translate_path(file, host, sizeof(host));
    return (uint32_t)GetPrivateProfileIntA(s, k, (int)def, host);
}
uint32_t host_ini_write(uint32_t sect, uint32_t key, uint32_t val, uint32_t file) {
    char s[128], k[128], v[256], host[600];
    sim_read_str(sect, s, sizeof(s)); sim_read_str(key, k, sizeof(k));
    sim_read_str(val, v, sizeof(v));  host_translate_path(file, host, sizeof(host));
    return WritePrivateProfileStringA(s, key ? k : NULL, val ? v : NULL, host) ? 1 : 0;
}

/* Copy a host C string into simulated memory; returns its VA. */
uint32_t shim_strdup(const char* s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = shim_alloc(n);
    if (va) memcpy((void*)(uintptr_t)ADDR(va), s, n);
    return va;
}

/* The recompiled entry point (Watcom CRT startup -> WinMain). PODMMX.EXE EP is
 * RVA 0x000C9FD8 (VA 0x004C9FD8). */
extern void sub_004C9FD8(void);

/* ============================================================
 * Dispatch
 * ============================================================ */
recomp_func_t recomp_lookup(uint32_t va) {
    int lo = 0, hi = (int)recomp_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t m = recomp_dispatch_table[mid].address;
        if (m == va) return recomp_dispatch_table[mid].func;
        else if (m < va) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}
/* Manual dispatch overrides (none yet; WinMain address TBD from analysis). */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    for (uint32_t i = 0; i < pod_import_bridge_count; i++)
        if (pod_import_bridges[i].address == va)
            return pod_import_bridges[i].func;
    /* DirectDraw/COM method VAs (synthetic) resolve to host method shims. */
    recomp_func_t com = pod_com_lookup(va);
    if (com) return com;
#ifdef RECOMP_TRACE
    /* A call through a null pointer is an uninitialised callback/COM method --
     * the usual cause of an early quit. Dump the calling thread's ring (first few). */
    if (va == 0) {
        static volatile long n = 0;
        if (n++ < 8) recomp_dump_tls_ring("null indirect call (call through 0)");
    }
#endif
    /* Bring-up watchdog: a wild indirect call that misses every table, repeated,
     * means we're stuck calling a garbage pointer. Dump the function-entry trace
     * (the last N lifted functions) once so we can see where, then bail. */
    static uint32_t last_va = 0, miss = 0;
    if (va == last_va) {
        if (++miss == 64) {
            fprintf(stderr, "\n[watchdog] %u repeated unresolved indirect calls to 0x%08X\n", miss, va);
            fprintf(stderr, "[watchdog] eax=%08X esp=%08X esi=%08X edi=%08X ebx=%08X edx=%08X\n",
                    g_eax, g_esp, g_esi, g_edi, g_ebx, g_edx);
            recomp_dump_trace("repeated unresolved indirect call");
            fflush(stderr);
            _Exit(9);
        }
    } else { last_va = va; miss = 0; }
    return NULL;
}

/* ============================================================
 * VEH crash handler
 * ============================================================ */
static void dump_state(void) {
    fprintf(stderr, "  EAX=%08X ECX=%08X EDX=%08X EBX=%08X\n", g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "  ESP=%08X ESI=%08X EDI=%08X\n", g_esp, g_esi, g_edi);
    fprintf(stderr, "  last indirect-call targets:\n");
    for (int i = 0; i < ICALL_TRACE_SIZE; i++) {
        uint32_t idx = (g_icall_trace_idx - ICALL_TRACE_SIZE + i) & (ICALL_TRACE_SIZE - 1);
        if (g_icall_trace[idx]) fprintf(stderr, "    0x%08X\n", g_icall_trace[idx]);
    }
}
/* Name a host address: the module it lies in, or the guest VA it maps to inside
 * the arena. A fault deep in kernel32 says nothing on its own; "inside ReadFile,
 * on a pointer the game passed us" says everything. */
static void describe_addr(const char* what, uintptr_t p) {
    if (g_arena && p >= (uintptr_t)g_arena && p < (uintptr_t)g_arena + POD_ARENA_SIZE) {
        fprintf(stderr, "  %s 0x%p = guest VA 0x%08X (in arena)\n",
                what, (void*)p, (uint32_t)(p - (uintptr_t)g_arena));
        return;
    }
    HMODULE m = NULL;
    char name[MAX_PATH] = {0};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)p, &m) && GetModuleFileNameA(m, name, MAX_PATH)) {
        const char* base = strrchr(name, '\\');
        fprintf(stderr, "  %s 0x%p = %s+0x%X\n", what, (void*)p,
                base ? base + 1 : name, (unsigned)(p - (uintptr_t)m));
    } else {
        /* Not in a module and not in the arena: a guest VA used as a host
         * pointer without ADDR(), or plain garbage. */
        fprintf(stderr, "  %s 0x%p = unmapped\n", what, (void*)p);
    }
}

/* One-shot memory watchpoint. PAGE_GUARD faults on the next access to the page
 * and Windows clears it for us, so the guarded code runs on afterwards -- the
 * cost is one exception, and the reward is the call stack of whoever touched a
 * pixel. Nothing in a lifted binary announces which function is the blitter. */
static uintptr_t g_watch_page = 0;
static uint32_t  g_watch_rearm = 0;   /* function entries until the trap is reset */
static uint32_t  g_watch_hits  = 0;
#define WATCH_MAX_HITS 40

static void watch_arm(uintptr_t p) {
    DWORD old;
    if (VirtualProtect((void*)p, 0x1000, PAGE_READONLY, &old)) g_watch_page = p;
}
void host_watch_page(uint32_t va) {
    uintptr_t p = ADDR(va) & ~(uintptr_t)0xFFF;
    watch_arm(p);
    if (g_watch_page)
        fprintf(stderr, "[watch] armed on guest VA 0x%08X (host %p)\n", va, (void*)p);
}
/* Narrow the trap to an exact guest range, so a busy page can be watched for the
 * few bytes that matter: POD_WATCH_VA=<va> POD_WATCH_LEN=<n>. Writes elsewhere in
 * the page are let through silently. POD_WATCH_AFTER_MS delays arming, which is
 * how you skip the init that zeroes the field you care about. */
static uint32_t g_watch_lo, g_watch_hi, g_watch_after_ms;
static int g_watch_pending;

void host_watch_va(uint32_t va, uint32_t len, uint32_t after_ms) {
    g_watch_lo = va; g_watch_hi = va + (len ? len : 4);
    g_watch_after_ms = after_ms;
    g_watch_pending = 1;
    fprintf(stderr, "[watch] will arm on guest VA 0x%08X..0x%08X at %ums\n",
            g_watch_lo, g_watch_hi, after_ms);
}

/* Re-arm a little after each hit: the faulting instruction has to retire first,
 * so we count function entries rather than resetting straight away. Sampling
 * like this lists the writers of one page in the order they run. */
void host_watch_tick(void) {
    if (g_watch_pending &&
        (uint32_t)(timeGetTime() - g_start_tick) >= g_watch_after_ms) {
        g_watch_pending = 0;
        host_watch_page(g_watch_lo);
    }
    if (g_watch_rearm && --g_watch_rearm == 0 && g_watch_hits < WATCH_MAX_HITS)
        watch_arm(g_watch_page);
}

static LONG WINAPI veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Ignore benign codes: C++ EH unwinding, thread-name set, debugger prints. */
    if (code == 0xE06D7363u || code == 0x406D1388u || code == 0x40010006u || code == 0x4001000Au)
        return EXCEPTION_CONTINUE_SEARCH;
    /* Write-watchpoint: the page is READONLY, so reads (our own present scan)
     * still work and only a write faults. Log it, unprotect, carry on. */
    if (code == (DWORD)0xC0000005L && g_watch_page &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t hit = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (hit >= g_watch_page && hit < g_watch_page + 0x1000) {
            DWORD old;
            VirtualProtect((void*)g_watch_page, 0x1000, PAGE_READWRITE, &old);
            /* Outside the range of interest: unprotect so the faulting store can
             * retire (leaving the page read-only would fault the same
             * instruction forever), and re-arm a couple of entries later. */
            uint32_t guest = (uint32_t)(hit - (uintptr_t)g_arena);
            if (g_watch_hi && (guest < g_watch_lo || guest >= g_watch_hi)) {
                DWORD old;
                VirtualProtect((void*)g_watch_page, 0x1000, PAGE_READWRITE, &old);
                g_watch_rearm = 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* One line per distinct writer: the background blit hits this page
             * every frame and would otherwise drown out the pass we're after. */
            static uint32_t last = 0;
            if (g_cur_func != last) {
                last = g_cur_func;
                fprintf(stderr, "[watch] write +0x%03X from 0x%08X (caller 0x%08X)\n",
                        (unsigned)(hit - g_watch_page), g_cur_func,
#ifdef RECOMP_TRACE
                        g_call_depth ? g_call_stack[g_call_depth - 1] : 0);
#else
                        0u);
#endif
                fflush(stderr);
                g_watch_hits++;
            }
            g_watch_rearm = 8;               /* resume, then trap the next writer */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    static volatile long once = 0;
    if (InterlockedIncrement(&once) > 3) return EXCEPTION_CONTINUE_SEARCH;  /* avoid spam */
    fprintf(stderr, "\n=== EXCEPTION 0x%08lX at %p (tid=%lu, in 0x%08X) ===\n",
            code, (void*)ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),
            g_cur_func);
    describe_addr("pc  ", (uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        describe_addr(ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                      (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
    dump_state();
    recomp_dump_stack("exception");
    recomp_dump_tls_ring("exception");
    recomp_dump_trace("exception");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ============================================================
 * PE image loader (parses section headers at runtime)
 * ============================================================ */
#pragma pack(push,1)
typedef struct { char name[8]; uint32_t vsize, vaddr, rsize, roff; uint32_t a,b; uint16_t c,d; uint32_t chr; } SecHdr;
#pragma pack(pop)

static int load_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FATAL: cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return 0; }
    fclose(f);

    uint32_t e_lfanew = *(uint32_t*)(buf + 0x3C);
    uint8_t* nt = buf + e_lfanew;                 /* "PE\0\0" */
    uint16_t nsec = *(uint16_t*)(nt + 6);
    uint16_t opt_sz = *(uint16_t*)(nt + 20);
    SecHdr* sec = (SecHdr*)(nt + 24 + opt_sz);

    /* compute image span */
    uint32_t max_end = 0;
    for (int i = 0; i < nsec; i++) {
        uint32_t end = sec[i].vaddr + (sec[i].vsize ? sec[i].vsize : sec[i].rsize);
        if (end > max_end) max_end = end;
    }
    uint32_t span = (max_end + 0xFFF) & ~0xFFFu;  /* page round */
    if (POD_IMAGE_BASE + span > POD_ARENA_SIZE) {
        fprintf(stderr, "FATAL: image span 0x%X overflows arena\n", span);
        free(buf); return 0;
    }

    /* Copy each section's raw bytes to its guest VA (arena is already zeroed, which
     * handles .bss). Watcom marks .bss with SizeOfRawData != 0 but
     * PointerToRawData == 0 -- skip roff==0 so it stays zeroed instead of being
     * overwritten with file bytes. Cap the copy to what's actually in the file. */
    for (int i = 0; i < nsec; i++) {
        if (sec[i].rsize == 0 || sec[i].roff == 0) continue;
        uint32_t va = POD_IMAGE_BASE + sec[i].vaddr;
        uint32_t n = (sec[i].vsize && sec[i].vsize < sec[i].rsize) ? sec[i].vsize : sec[i].rsize;
        if ((long)(sec[i].roff + n) > sz) n = (uint32_t)(sz - sec[i].roff);
        memcpy((void*)(uintptr_t)ADDR(va), buf + sec[i].roff, n);
    }
    free(buf);
    printf("[*] image: %d sections, span 0x%X at guest 0x%08X (host %p)\n",
           nsec, span, POD_IMAGE_BASE, (void*)(uintptr_t)ADDR(POD_IMAGE_BASE));
    return 1;
}

/* ============================================================
 * Init: stack, IAT self-patch, bridge install
 * ============================================================ */
static int setup(const char* exe) {
    InitializeCriticalSection(&g_alloc_cs); g_alloc_cs_init = 1;
    /* One arena for the whole guest address space. Let Windows choose the base
     * (it lands high, clear of the CRT's low-memory heap), then g_mem_base maps
     * guest VA 0 -> arena. PAGE_EXECUTE_READWRITE: lifted code is host code, but
     * self-modifying guest code or data-as-code reads stay valid. */
    g_arena = VirtualAlloc(NULL, POD_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_arena) {
        fprintf(stderr, "FATAL: arena alloc (0x%X bytes) failed (err=%lu)\n",
                POD_ARENA_SIZE, GetLastError());
        return 0;
    }
    g_mem_base = (ptrdiff_t)(uintptr_t)g_arena;   /* guest VA v -> g_arena + v */
    /* stderr, not stdout: stdout is block-buffered when redirected, so a crash
     * loses the one line that makes every host address in the dump readable. */
    fprintf(stderr, "[*] arena: %u MB at host %p (g_mem_base=0x%p)\n",
            POD_ARENA_SIZE >> 20, g_arena, (void*)g_mem_base);

    if (!load_image(exe)) return 0;

    g_esp = POD_STACK_BASE + POD_STACK_SIZE - 64;

    /* Simulated TIB so fs:[...] accesses (SEH setup in CRT startup) work.
     * fs:[0]=SEH chain head (-1 = end), fs:[4]=stack base (top), fs:[8]=stack
     * limit (bottom), fs:[0x18]=self (linear TEB address). */
    g_fs_base = POD_TIB_BASE;
    MEM32(g_fs_base + 0x00) = 0xFFFFFFFFu;                      /* ExceptionList */
    MEM32(g_fs_base + 0x04) = POD_STACK_BASE + POD_STACK_SIZE; /* StackBase   */
    MEM32(g_fs_base + 0x08) = POD_STACK_BASE;                 /* StackLimit    */
    MEM32(g_fs_base + 0x18) = g_fs_base;                        /* Self          */

    g_heap_next = POD_HEAP_BASE;

    /* Self-patch IAT slots so both call paths resolve through the bridge table:
     *   direct : RECOMP_ICALL(slot_va)         -> lookup_import(slot_va)
     *   thunk  : RECOMP_ITAIL(MEM32(slot_va))  -> lookup_import(slot_va)   */
    for (uint32_t i = 0; i < pod_iat_slot_count; i++)
        MEM32(pod_iat_slots[i]) = pod_iat_slots[i];

    printf("[*] stack: 0x%08X..0x%08X (ESP=0x%08X)\n",
           POD_STACK_BASE, POD_STACK_BASE + POD_STACK_SIZE, g_esp);
    printf("[*] dispatch table: %u functions\n", recomp_dispatch_count);
    printf("[*] import bridges: %u (%u IAT slots patched)\n",
           pod_import_bridge_count, pod_iat_slot_count);
    return 1;
}

int main(int argc, char** argv) {
    printf("=== POD Gold Static Recompilation - runtime ===\n");
    if (argc < 2) {
        printf("usage: %s <path-to-POD.EXE> [--run]\n", argv[0]);
        return 2;
    }
    timeBeginPeriod(1);                 /* 1 ms timer resolution for timeGetTime */
    g_start_tick = timeGetTime();
    host_index_build();
    AddVectoredExceptionHandler(1, veh);
    host_keys_init();
    { const char* a = getenv("POD_DUMP_AFTER");
      if (a) g_after_call_va = (uint32_t)strtoul(a, NULL, 0);
#ifdef RECOMP_TRACE
      { const char* pv = getenv("POD_POLL_VA"); if (pv) g_poll_va = (uint32_t)strtoul(pv, NULL, 0); }
#endif
      { const char* wv = getenv("POD_WATCH_VA");
        if (wv) host_watch_va((uint32_t)strtoul(wv, NULL, 0),
                              getenv("POD_WATCH_LEN") ? (uint32_t)strtoul(getenv("POD_WATCH_LEN"), NULL, 0) : 12,
                              getenv("POD_WATCH_AFTER_MS") ? (uint32_t)strtoul(getenv("POD_WATCH_AFTER_MS"), NULL, 0) : 0); }
      const char* w = getenv("POD_ARGS_AT");
      if (w) g_args_at_va = (uint32_t)strtoul(w, NULL, 0);
      const char* g = getenv("POD_DUMP_ARM");
      if (g) { g_dump_arm_va = (uint32_t)strtoul(g, NULL, 0); g_dump_armed = 0; } }
    if (!setup(argv[1])) return 1;
    printf("[*] runtime initialized.\n");

    int run = (argc > 2 && strcmp(argv[2], "--run") == 0);
    if (run) {
        printf("[*] calling entry point sub_004C9FD8 ...\n\n");
        PUSH32(g_esp, 0xDEAD0000u);  /* dummy return address */
        sub_004C9FD8();
        printf("\n[*] entry returned. EAX=0x%08X\n", g_eax);
    } else {
        printf("[*] ready. Re-run with --run to call the entry point.\n");
    }
    return 0;
}
