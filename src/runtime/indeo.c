/*
 * The videos' codec: Intel's Indeo 4.1 (IR41_32.DLL R4.11.15.91, the MMX
 * build), from the POD disc's INSTALL\ folder, lifted with the game.
 *
 * POD plays its videos itself, through Video for Windows: AVIFileOpen,
 * AVIStreamGetFrameOpen/GetFrame, DrawDibDraw. Windows has had no Indeo
 * decoder for years, so the frame decompressor is never found. Here the
 * lifted codec is installed as a VfW *function* driver for 'vidc'/'iv41'
 * (ICInstall with ICINSTALL_FUNCTION, for this process only): VfW sends it
 * its usual messages (DRV_OPEN, ICM_DECOMPRESS_QUERY, ICM_DECOMPRESS...),
 * and each one runs the codec's lifted DriverProc. POD's own player is
 * unchanged. Mapping and calling it is Twisted Metal's (twistedmetal-recomp
 * src/runtime/movie.c), which lifted the 4.11.15.62 build the same way.
 * docs/movies.md.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>

#include "native32.h"
#include "indeo.h"

extern const uint32_t pod_ir41_entry_va;     /* recomp_dispatch.c: its DllMain */

static void* g_hold;
static uint32_t g_driverproc;
static int g_native;                         /* --codec-native: the DLL's own code */

void indeo_hold(void) {
    g_hold = VirtualAlloc((void*)(uintptr_t)INDEO_BASE, INDEO_SPAN, MEM_RESERVE, PAGE_NOACCESS);
}

/* Every VfW message to the codec, run lifted on the calling thread. VfW calls
 * from inside a native call the game made (AVIStreamGetFrame), with the
 * machine lock handed back, so it is taken here; native32_call_guest keeps
 * the caller's registers. */
static uint32_t call(uint32_t id, uint32_t drv, uint32_t msg, uint32_t l1, uint32_t l2) {
    typedef LRESULT (CALLBACK *proc_t)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);
    if (g_native)
        return (uint32_t)((proc_t)(uintptr_t)g_driverproc)(id, (HDRVR)(uintptr_t)drv, msg, l1, l2);
    uint32_t a[5] = { id, drv, msg, l1, l2 };
    mach_enter();
    native32_call_guest(g_driverproc, 5, a);
    uint32_t r = g_eax;
    mach_leave();
    return r;
}

/* An instance for messages that come without one. Windows' VfW sends a
 * function driver ICM_GETINFO with driver id 0 and no DRV_OPEN before it
 * (an installable driver it would open first); the codec reads its
 * instance through that 0 and faults. So the codec is loaded once, and a
 * message with id 0 gets an instance opened for it, as VfW would have. */
static uint32_t shared_instance(void) {
    static uint32_t inst;
    if (!inst) {
        static ICOPEN open = { sizeof open, ICTYPE_VIDEO, mmioFOURCC('i', 'v', '4', '1'), 0, ICMODE_QUERY };
        inst = call(0, 1, DRV_OPEN, 0, (uint32_t)(uintptr_t)&open);
    }
    return inst;
}

static LRESULT CALLBACK driverproc(DWORD_PTR id, HDRVR drv, UINT msg, LPARAM l1, LPARAM l2) {
    static int trace = -1, loaded;
    if (trace < 0) trace = getenv("POD_INDEO_TRACE") != NULL;
    if (!loaded) {                       /* DRV_LOAD and DRV_ENABLE, once, before anything else */
        loaded = 1;
        call(0, 1, DRV_LOAD, 0, 0);
        call(0, 1, DRV_ENABLE, 0, 0);
    }
    if (msg == DRV_LOAD || msg == DRV_ENABLE || msg == DRV_DISABLE || msg == DRV_FREE) return 1;
    if (!id && msg >= DRV_USER) id = shared_instance();
    uint32_t r = call((uint32_t)id, (uint32_t)(uintptr_t)drv ? (uint32_t)(uintptr_t)drv : 1, msg,
                      (uint32_t)l1, (uint32_t)l2);
    if (trace) fprintf(stderr, "[indeo] DriverProc(%08X, %04X, %08X, %08X) -> %08X\n",
                       (unsigned)id, msg, (unsigned)l1, (unsigned)l2, r);
    return (LRESULT)(int32_t)r;
}

int indeo_start(const char* dll, native32_shim_t* shims, int nshims, int native) {
    if (g_hold) VirtualFree(g_hold, 0, MEM_RELEASE);
    g_hold = NULL;
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES ||
        !native32_map(dll, INDEO_BASE) || native32_bind(INDEO_BASE, shims, nshims)) {
        fprintf(stderr, "[indeo] no codec at %s: the videos are skipped\n", dll);
        return 0;
    }
    uint32_t a[3] = { INDEO_BASE, DLL_PROCESS_ATTACH, 0 }, ok;
    g_driverproc = native32_export(INDEO_BASE, "DriverProc");
    if (native) {                        /* the reference: the DLL's own machine code */
        native32_set_native(INDEO_BASE);
        g_native = 1;
        typedef BOOL (WINAPI *dllmain_t)(uint32_t, uint32_t, uint32_t);
        ok = ((dllmain_t)(uintptr_t)pod_ir41_entry_va)(a[0], a[1], a[2]);
    } else {
        mach_enter();
        native32_call_guest(pod_ir41_entry_va, 3, a);
        ok = g_eax;
        mach_leave();
    }
    if (!ok || !g_driverproc ||
        !ICInstall(ICTYPE_VIDEO, mmioFOURCC('i', 'v', '4', '1'), (LPARAM)driverproc, NULL, ICINSTALL_FUNCTION)) {
        fprintf(stderr, "[indeo] the codec did not start (DllMain %u): the videos are skipped\n", ok);
        return 0;
    }
    fprintf(stderr, "[indeo] Indeo 4.1 %s from %s, DriverProc 0x%08X\n",
            native ? "ORIGINAL machine code (--codec-native)" : "lifted", dll, g_driverproc);
    return 1;
}

/* --decode-test AVI OUT: every frame of AVI's video, through Video for
 * Windows and so through the installed codec, as 24-bit RGB (bottom-up
 * rows, as GetFrame returns them) appended to OUT. tools/verify_indeo.py
 * compares the lifted codec's output with the original's and with ffmpeg's. */
int indeo_decode_test(const char* avi, const char* out) {
    PAVIFILE f;
    PAVISTREAM s;
    AVIFileInit();
    if (AVIFileOpenA(&f, avi, OF_READ, NULL) || AVIFileGetStream(f, &s, streamtypeVIDEO, 0)) {
        fprintf(stderr, "[indeo] cannot open %s\n", avi);
        return 1;
    }
    AVISTREAMINFOA si;
    AVIStreamInfoA(s, &si, sizeof si);
    BITMAPINFOHEADER want = { sizeof want, si.rcFrame.right - si.rcFrame.left,
                              si.rcFrame.bottom - si.rcFrame.top, 1, 24, BI_RGB };
    PGETFRAME gf = AVIStreamGetFrameOpen(s, &want);
    if (!gf) { fprintf(stderr, "[indeo] %s: no decoder\n", avi); return 1; }
    FILE* o = fopen(out, "wb");
    if (!o) return 1;
    long n = 0, first = AVIStreamStart(s), end = AVIStreamEnd(s);
    for (long i = first; i < end; i++) {
        BITMAPINFOHEADER* b = (BITMAPINFOHEADER*)AVIStreamGetFrame(gf, i);
        if (!b) { fprintf(stderr, "[indeo] %s: frame %ld did not decode\n", avi, i); break; }
        size_t row = ((size_t)b->biWidth * 3 + 3) & ~(size_t)3;
        fwrite((uint8_t*)b + b->biSize + b->biClrUsed * 4, 1, row * (size_t)abs(b->biHeight), o);
        n++;
    }
    fclose(o);
    AVIStreamGetFrameClose(gf);
    AVIStreamRelease(s);
    AVIFileRelease(f);
    printf("%s: %ld of %ld frames, %ldx%ld -> %s\n", avi, n, end - first, want.biWidth, want.biHeight, out);
    return n == end - first ? 0 : 1;
}
