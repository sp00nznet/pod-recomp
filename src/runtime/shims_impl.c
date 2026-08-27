/*
 * POD Gold Recompilation - hand-written import shims.
 *
 * These replace the auto-generated stubs for imports that the MSVC CRT startup
 * and early game init actually depend on (command line, environment, std
 * handles, the heap). Each runs in the recomp stack model: read args with
 * ARG()/ARGP(), return via RET(), and clean the stack with STDRET(argc).
 *
 * Names listed in gen_imports.py's MANUAL set resolve here (the generator emits
 * an extern decl and the bridge table points at these).
 */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "imports.h"
#include <string.h>

/* ---- lifted-function replacements (listed in run_lift.py HOST_SHIM) ----
 * sub_004A2DBC is POD's CPU detector: it probes CPUID via the EFLAGS ID-bit
 * toggle, then runs CPUID and maps vendor+family to a renderer class in
 * dword_6E7290 (WinMain bails if it's not 1..8). Our recomp model carries no
 * EFLAGS, so the probe fails and the class stays 255 -> bail. Report a
 * Pentium-MMX-class Intel directly. */
void sub_004A2DBC(void) {
    MEM32(0x006E7298u) = 0x756E6547u;   /* vendor EBX "Genu" */
    MEM32(0x006E729Cu) = 0x49656E69u;   /* vendor EDX "ineI" */
    MEM32(0x006E72A0u) = 0x6C65746Eu;   /* vendor ECX "ntel" -> GenuineIntel */
    MEM32(0x006E72A8u) = 0x00000543u;   /* CPUID.1 EAX: family 5, model 4 = Pentium MMX */
    MEM32(0x006E7290u) = 2u;            /* renderer class 2 (Pentium MMX) */
    g_eax = 2;
}

/* ---- versions / locale ---- */
void imp_GetVersion(void)      { RET(0x00000004u); STDRET(0); }   /* Windows 4.0 */
void imp_GetACP(void)          { RET(1252); STDRET(0); }
void imp_GetOEMCP(void)        { RET(437);  STDRET(0); }
void imp_GetCPInfo(void) {       /* (CodePage, CPINFO* lpCPInfo) */
    uint32_t p = ARG(1);
    MEM32(p + 0) = 1;            /* MaxCharSize = 1 */
    MEM8(p + 4) = 0;            /* DefaultChar[0] */
    RET(1); STDRET(2);
}

/* ---- module / command line / environment ---- */
void imp_GetModuleHandleA(void) { RET(0x00400000u); STDRET(1); } /* hModule = image base */

void imp_GetCommandLineA(void) {
    static uint32_t va = 0;
    if (!va) va = shim_strdup("pod.exe");
    RET(va); STDRET(0);
}
void imp_GetModuleFileNameA(void) { /* (hModule, lpFilename, nSize) */
    const char* path = "C:\\POD\\POD.EXE";
    uint32_t buf = ARG(1), n = ARG(2), len = (uint32_t)strlen(path);
    if (len + 1 > n) len = n ? n - 1 : 0;
    for (uint32_t i = 0; i < len; i++) MEM8(buf + i) = (uint8_t)path[i];
    MEM8(buf + len) = 0;
    RET(len); STDRET(3);
}
/* CD-ROM presence: report a CD-ROM at D: (fixed at C:, absent elsewhere). POD's
 * loader enumerates drives looking for the disc. */
void imp_GetDriveTypeA(void) {
    uint32_t p = ARG(0); char c = p ? (char)MEM8(p) : 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    RET(c == 'D' ? 5u : c == 'C' ? 3u : 1u); STDRET(1);
}
void imp_GetVolumeInformationA(void) { /* (root,volBuf,volSz,serial,maxComp,fsFlags,fsBuf,fsSz) */
    uint32_t root = ARG(0), vb = ARG(1), vs = ARG(2);
    char c = root ? (char)MEM8(root) : 0; if (c >= 'a' && c <= 'z') c -= 32;
    const char* label = (c == 'D') ? "POD2_0" : "";    /* the disc volume label */
    if (vb) { uint32_t i = 0; for (; label[i] && i + 1 < vs; i++) MEM8(vb + i) = (uint8_t)label[i]; MEM8(vb + i) = 0; }
    if (ARG(3)) MEM32(ARG(3)) = 0x504F4432u;
    if (ARG(4)) MEM32(ARG(4)) = 255;
    if (ARG(5)) MEM32(ARG(5)) = 0;
    RET(c == 'D' ? 1u : 0u); STDRET(8);
}

/* Bring-up: log + suppress PostQuitMessage so an early error path doesn't end the
 * process before we reach the first frame. Dump the caller's ring to find it. */
void imp_PostQuitMessage(void) {
    static int n = 0;
    fprintf(stderr, "[quit] PostQuitMessage(%u) (suppressed for bring-up)\n", ARG(0));
    if (n++ < 3) recomp_dump_tls_ring("PostQuitMessage caller");
    RET(0); STDRET(1);
}

void imp_GetCurrentDirectoryA(void) { /* (nBufferLength, lpBuffer) */
    const char* d = "C:\\POD";
    uint32_t n = ARG(0), buf = ARG(1), len = (uint32_t)strlen(d);
    if (buf && len + 1 <= n) { for (uint32_t i=0;i<=len;i++) MEM8(buf+i)=(uint8_t)d[i]; }
    RET(len); STDRET(2);
}
void imp_GetEnvironmentStrings(void) {
    static uint32_t va = 0;
    if (!va) { va = shim_alloc(2); MEM8(va) = 0; MEM8(va + 1) = 0; }  /* "\0\0" */
    RET(va); STDRET(0);
}

/* ---- std handles / file output ---- */
void imp_GetStdHandle(void)  { RET(0xF0000000u | (ARG(0) & 0xFu)); STDRET(1); }
void imp_SetStdHandle(void)  { RET(1); STDRET(2); }
/* GetFileType + WriteFile are defined below (file-I/O aware). */

/* ---- startup info ---- */
void imp_GetStartupInfoA(void) { /* (LPSTARTUPINFOA) - 68 bytes */
    uint32_t p = ARG(0);
    for (int i = 0; i < 68; i += 4) MEM32(p + i) = 0;
    MEM32(p + 0) = 68;          /* cb */
    RET(0); STDRET(1);
}

/* ---- memory ---- */
/* VirtualAlloc returns page-aligned memory; the CRT heap relies on that. */
void imp_VirtualAlloc(void) {  /* (lpAddress, dwSize, flAllocationType, flProtect) */
    uint32_t addr = ARG(0), size = ARG(1);
    RET(addr ? addr : shim_alloc_aligned(size, 0x1000));
    STDRET(4);
}
void imp_VirtualFree(void)   { RET(1); STDRET(3); }
void imp_GlobalAlloc(void)   { /* (uFlags, dwBytes) */ RET(shim_alloc(ARG(1))); STDRET(2); }
void imp_GlobalLock(void)    { RET(ARG(0)); STDRET(1); }
void imp_GlobalFree(void)    { RET(0); STDRET(1); }
void imp_GlobalMemoryStatus(void) { /* (LPMEMORYSTATUS) - 32 bytes */
    uint32_t p = ARG(0);
    for (int i = 0; i < 32; i += 4) MEM32(p + i) = 0;
    MEM32(p + 0)  = 32;            /* dwLength */
    MEM32(p + 4)  = 50;            /* dwMemoryLoad % */
    MEM32(p + 8)  = 0x20000000u;   /* dwTotalPhys = 512 MB */
    MEM32(p + 12) = 0x10000000u;   /* dwAvailPhys = 256 MB */
    RET(0); STDRET(1);
}

/* VirtualQuery: fill a MEMORY_BASIC_INFORMATION for the guest arena, so the CRT's
 * memory-scan loops advance (RegionSize must be non-zero) and terminate. A stub
 * returning 0 left RegionSize unset -> the scan spun forever. */
void imp_VirtualQuery(void) {        /* (lpAddress, lpBuffer, dwLength) */
    uint32_t addr = ARG(0), buf = ARG(1), len = ARG(2);
    uint32_t base = addr & ~0xFFFu;
    extern uint32_t pod_arena_size(void);
    uint32_t arena = pod_arena_size();
    if (buf) {
        if (addr < arena) {                       /* committed guest memory */
            MEM32(buf + 0)  = base;               /* BaseAddress      */
            MEM32(buf + 4)  = 0x1000;             /* AllocationBase   */
            MEM32(buf + 8)  = 0x40;               /* AllocationProtect (RWX) */
            MEM32(buf + 12) = arena - base;       /* RegionSize       */
            MEM32(buf + 16) = 0x1000;             /* State = MEM_COMMIT */
            MEM32(buf + 20) = 0x40;               /* Protect          */
            MEM32(buf + 24) = 0x20000;            /* Type = MEM_PRIVATE */
        } else {                                  /* free, above the arena */
            MEM32(buf + 0)  = base;
            MEM32(buf + 4)  = 0;
            MEM32(buf + 8)  = 0;
            MEM32(buf + 12) = 0x10000000u;        /* big free region  */
            MEM32(buf + 16) = 0x10000;            /* State = MEM_FREE */
            MEM32(buf + 20) = 0x1;                /* PAGE_NOACCESS    */
            MEM32(buf + 24) = 0;
        }
    }
    RET(len >= 28 ? 28u : len);                   /* bytes written; wrapper wants == dwLength */
    STDRET(3);
}

/* ---- thread-local storage ----
 * We run the guest on one host thread, so a flat slot table is enough. The game
 * stashes a per-"thread" data pointer here; stubs that don't actually store it
 * make every lookup fail ("Thread has no thread-specific data" in a tight loop). */
#define POD_TLS_SLOTS 256
static uint32_t g_tls_val[POD_TLS_SLOTS];
static uint8_t  g_tls_used[POD_TLS_SLOTS];
void imp_TlsAlloc(void) {
    for (uint32_t i = 0; i < POD_TLS_SLOTS; i++)
        if (!g_tls_used[i]) { g_tls_used[i] = 1; g_tls_val[i] = 0; RET(i); STDRET(0); return; }
    RET(0xFFFFFFFFu); STDRET(0);            /* TLS_OUT_OF_INDEXES */
}
void imp_TlsFree(void)     { uint32_t i = ARG(0); if (i < POD_TLS_SLOTS) g_tls_used[i] = 0; RET(1); STDRET(1); }
void imp_TlsSetValue(void) { uint32_t i = ARG(0); if (i < POD_TLS_SLOTS) g_tls_val[i] = ARG(1); RET(1); STDRET(2); }
void imp_TlsGetValue(void) { uint32_t i = ARG(0); RET(i < POD_TLS_SLOTS ? g_tls_val[i] : 0); STDRET(1); }

/* ============================================================
 * Host-shimmed CRT functions (declared in the generated dispatch, defined here).
 * These replace lifted library code whose internals aren't worth debugging.
 * ============================================================ */

/* Shared null-safe formatter: reads a printf format + varargs from simulated
 * memory (args at sim VA `ap`), formats each conversion with the host CRT, and
 * writes the result into host buffer `dest`. A null %s prints "(null)" (the MSVC
 * behaviour) instead of dereferencing 0. Returns the length written. */
static int sim_format(uint32_t fmt, uint32_t ap, char* dest, int destsz) {
    char spec[64], tmp[1200], hoststr[4096];
    int out = 0;
    #define PUT(ch) do { if (out < destsz - 1) dest[out++] = (ch); } while (0)
    for (uint32_t f = fmt; MEM8(f); ) {
        char c = (char)MEM8(f++);
        if (c != '%') { PUT(c); continue; }
        int si = 0; spec[si++] = '%';
        int star_args = 0; uint32_t starv[2] = {0, 0};
        for (;;) {
            char d = (char)MEM8(f++);
            if (si < (int)sizeof(spec) - 2) spec[si++] = d;
            if (d == '*') { starv[star_args < 2 ? star_args : 1] = MEM32(ap); ap += 4; star_args++; continue; }
            if (d == '%' || d == 'c' || d == 's' || d == 'd' || d == 'i' || d == 'u' ||
                d == 'x' || d == 'X' || d == 'o' || d == 'p' || d == 'e' || d == 'E' ||
                d == 'f' || d == 'g' || d == 'G' || d == 0) break;
        }
        spec[si] = 0;
        char conv = spec[si - 1];
        for (int k = 0; spec[k]; k++) if (spec[k] == 'l' || spec[k] == 'h') {
            for (int j = k; spec[j]; j++) spec[j] = spec[j + 1];
            k--;
        }
        if (conv == '%') { PUT('%'); continue; }
        int n = 0;
        if (conv == 's') {
            uint32_t sva = MEM32(ap); ap += 4;
            uint32_t h = 0;
            if (sva) while (h < sizeof(hoststr) - 1 && MEM8(sva + h)) { hoststr[h] = (char)MEM8(sva + h); h++; }
            else { const char* nul = "(null)"; for (; nul[h]; h++) hoststr[h] = nul[h]; }
            hoststr[h] = 0;
            if (star_args == 2) n = snprintf(tmp, sizeof(tmp), spec, starv[0], starv[1], hoststr);
            else if (star_args == 1) n = snprintf(tmp, sizeof(tmp), spec, starv[0], hoststr);
            else n = snprintf(tmp, sizeof(tmp), spec, hoststr);
        } else if (conv == 'c') {
            uint32_t v = MEM32(ap); ap += 4;
            n = snprintf(tmp, sizeof(tmp), spec, (int)(v & 0xFF));
        } else if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'g' || conv == 'G') {
            union { uint64_t u; double d; } u; u.u = MEM32(ap) | ((uint64_t)MEM32(ap + 4) << 32); ap += 8;
            n = snprintf(tmp, sizeof(tmp), spec, u.d);
        } else { /* d i u x X o p */
            uint32_t v = MEM32(ap); ap += 4;
            if (star_args == 2) n = snprintf(tmp, sizeof(tmp), spec, starv[0], starv[1], v);
            else if (star_args == 1) n = snprintf(tmp, sizeof(tmp), spec, starv[0], v);
            else n = snprintf(tmp, sizeof(tmp), spec, v);
        }
        for (int k = 0; k < n && k < (int)sizeof(tmp); k++) PUT(tmp[k]);
    }
    dest[out] = 0;
    #undef PUT
    return out;
}

/* (CRT stdio/sprintf/stat: POD's Watcom CRT is lifted as-is. Replace specific
   routines with host shims here by address via HOST_SHIM if needed.) */

/* ---- file I/O (backed by the real game directory via runtime helpers) ---- */
void imp_CreateFileA(void) {   /* (name, access, share, sa, disp, flags, template) */
    RET(host_file_open(ARG(0), ARG(1), ARG(4))); STDRET(7);
}
void imp_ReadFile(void) {      /* (h, buf, n, *read, ovl) */
    uint32_t got = host_file_read(ARG(0), ARG(1), ARG(2));
    if (ARG(3)) MEM32(ARG(3)) = got;
    RET(1); STDRET(5);
}
void imp_WriteFile(void) {     /* (h, buf, n, *written, ovl) */
    uint32_t h = ARG(0), got;
    if (h >= 0x000A0000u && h < 0x000A0040u) got = host_file_write(h, ARG(1), ARG(2));
    else { uint32_t buf=ARG(1),n=ARG(2); for(uint32_t i=0;i<n;i++) fputc(MEM8(buf+i),stdout); got=n; }
    if (ARG(3)) MEM32(ARG(3)) = got;
    RET(1); STDRET(5);
}
void imp_SetFilePointer(void) { RET(host_file_seek(ARG(0), (int32_t)ARG(1), ARG(3))); STDRET(4); }
void imp_CloseHandle(void)    {                /* file handle, or thread/sync object */
    uint32_t h = ARG(0);
    if (pod_obj_is_handle(h)) pod_obj_close(h);
    else host_file_close(h);
    RET(1); STDRET(1);
}
void imp_GetFileSize(void)    { RET(host_file_size(ARG(0))); STDRET(2); }
void imp_GetFileType(void) {   /* real files = FILE_TYPE_DISK(1); std handles = CHAR(2) */
    uint32_t h = ARG(0);
    RET((h >= 0x000A0000u && h < 0x000A0040u) ? 1 : 2); STDRET(1);
}

/* ---- INI config (real POD.INI via runtime helpers) ---- */
void imp_GetPrivateProfileStringA(void) { /* (sect,key,def,buf,size,file) */
    RET(host_ini_string(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); STDRET(6);
}
void imp_GetPrivateProfileIntA(void) {    /* (sect,key,def,file) */
    RET(host_ini_int(ARG(0), ARG(1), ARG(2), ARG(3))); STDRET(4);
}
void imp_WritePrivateProfileStringA(void){ /* (sect,key,val,file) */
    RET(host_ini_write(ARG(0), ARG(1), ARG(2), ARG(3))); STDRET(4);
}

/* POD registers a private window message and posts it to itself to drive its
 * state machine. The stub returned 0, which is WM_NULL: every such post was
 * swallowed and the machine never advanced. Hand out real IDs from the system
 * range instead -- same string, same ID, as the API promises. */
void imp_RegisterWindowMessageA(void) {  /* (lpString) */
    static struct { uint32_t va; uint32_t id; } seen[16];
    static uint32_t n = 0, next_id = 0xC000u;
    uint32_t s = ARG(0);
    for (uint32_t i = 0; i < n; i++)
        if (seen[i].va == s) { RET(seen[i].id); STDRET(1); return; }
    uint32_t id = next_id++;
    if (n < 16) { seen[n].va = s; seen[n].id = id; n++; }
    fprintf(stderr, "[msg] RegisterWindowMessageA(\"%s\") -> 0x%04X\n",
            (const char*)(uintptr_t)ADDR(s), id);
    RET(id); STDRET(1);
}

/* ---- window messages: route them into POD's own WndProc ----
 * DispatchMessageA was a stub, so the game's window procedure never ran: every
 * WM_ and every private UserMsg_* it posted to itself was dropped. */
extern uint32_t g_guest_wndproc;
extern uint32_t call_guest_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

void imp_RegisterClassA(void) {   /* (const WNDCLASSA*) */
    uint32_t wc = ARG(0);
    if (wc) {
        g_guest_wndproc = MEM32(wc + 4);   /* lpfnWndProc */
        fprintf(stderr, "[msg] RegisterClassA: WndProc = 0x%08X\n", g_guest_wndproc);
    }
    RET(0xC0DE); STDRET(1);           /* a non-zero class atom */
}
void imp_DispatchMessageA(void) {  /* (const MSG*) */
    uint32_t m = ARG(0);
    uint32_t r = m ? call_guest_wndproc(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12)) : 0;
    RET(r); STDRET(1);
}
void imp_SendMessageA(void) {      /* (hwnd, msg, wParam, lParam) -- synchronous */
    RET(call_guest_wndproc(ARG(0), ARG(1), ARG(2), ARG(3))); STDRET(4);
}
void imp_PostMessageA(void) {      /* (hwnd, msg, wParam, lParam) -- queued */
    /* Onto the real queue, so it comes back through PeekMessage in order with
     * the input and paint messages rather than jumping ahead of them. */
    RET(host_post_window_message(ARG(1), ARG(2), ARG(3))); STDRET(4);
}
void imp_DefWindowProcA(void) {    /* (hwnd, msg, wParam, lParam) */
    RET(host_def_window_proc(ARG(1), ARG(2), ARG(3))); STDRET(4);
}

/* ---- display: real window + DIB blit ---- */
void imp_CreateWindowExA(void) {  /* (exStyle, cls, name, style, x,y,w,h, parent, menu, inst, param) */
    RET(host_create_window(MEM32(g_esp + 12))); STDRET(12);
}
void imp_GetDC(void)      { RET(0x00020001u); STDRET(1); }
void imp_ReleaseDC(void)  { RET(1); STDRET(2); }
void imp_CreateDIBSection(void) { /* (hdc, pbmi, usage, ppvBits, hSection, offset) */
    RET(host_create_dib(MEM32(g_esp + 8), MEM32(g_esp + 16))); STDRET(6);
}
void imp_SetDIBColorTable(void) { /* (hdc, start, n, prgbq) */
    host_set_dib_colors(MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16));
    RET(MEM32(g_esp + 12)); STDRET(4);
}
void imp_StretchBlt(void) { host_blit(); RET(1); STDRET(11); }
void imp_BitBlt(void)     { host_blit(); RET(1); STDRET(9); }
/* PeekMessageA/GetMessageA/PostThreadMessageA are in recomp_runtime.c (real Win32
 * message queues): POD posts thread messages to its sound thread, which must
 * actually receive them, else SoundEngine init reports a fatal error and quits. */

/* (heap: POD's Watcom malloc/free are lifted as-is; add host_malloc shims
   here by address via HOST_SHIM in run_lift.py only if they prove fragile.) */

/* ---- audio (WINMM waveOut): present a device, accept everything, play silently ---- */
void imp_waveOutGetDevCapsA(void) {   /* (uDeviceID, lpCaps, cbCaps) -> WAVEOUTCAPS (52 bytes) */
    uint32_t c = ARG(1);
    if (c) {
        for (int i = 0; i < 52; i += 4) MEM32(c + i) = 0;
        MEM16(c + 0) = 1;            /* wMid  */
        MEM16(c + 2) = 1;            /* wPid  */
        MEM32(c + 4) = 0x0400;       /* vDriverVersion */
        const char* nm = "POD Audio";
        for (int i = 0; nm[i] && i < 31; i++) MEM8(c + 8 + i) = (uint8_t)nm[i];
        MEM32(c + 40) = 0x0FFF;      /* dwFormats (all PCM formats) */
        MEM16(c + 44) = 2;           /* wChannels = stereo */
        MEM32(c + 48) = 0;           /* dwSupport */
    }
    RET(0); STDRET(3);               /* MMSYSERR_NOERROR */
}
void imp_waveOutOpen(void)            { if (ARG(0)) MEM32(ARG(0)) = 0x00C00001u; RET(0); STDRET(6); } /* phwo = fake handle */
void imp_waveOutClose(void)           { RET(0); STDRET(1); }
void imp_waveOutReset(void)           { RET(0); STDRET(1); }
void imp_waveOutPause(void)           { RET(0); STDRET(1); }
void imp_waveOutRestart(void)         { RET(0); STDRET(1); }
void imp_waveOutPrepareHeader(void)   { RET(0); STDRET(3); }
void imp_waveOutUnprepareHeader(void) { uint32_t h = ARG(1); if (h) MEM32(h + 16) |= 0x1; /* WHDR_DONE */ RET(0); STDRET(3); }
void imp_waveOutWrite(void)           { uint32_t h = ARG(1); if (h) MEM32(h + 16) |= 0x1; /* mark played */ RET(0); STDRET(3); }
void imp_waveOutGetPosition(void)     { RET(0); STDRET(3); }
void imp_waveOutGetErrorTextA(void)   { uint32_t b = ARG(1); if (b) MEM8(b) = 0; RET(0); STDRET(3); }

/* ---- no joystick, no mixer (the game falls back to keyboard, skips the mixer) ---- */
void imp_joyGetDevCapsA(void) { RET(165); STDRET(3); }   /* JOYERR_PARMS = no device */
void imp_joyGetPosEx(void)    { RET(165); STDRET(2); }
void imp_mixerGetNumDevs(void){ RET(0);   STDRET(0); }   /* no mixers */

/* ---- diagnostics ---- */
void imp_MessageBoxA(void) {   /* (hWnd, lpText, lpCaption, uType) */
    uint32_t text = ARG(1), cap = ARG(2);
    char t[512], c[128]; uint32_t i;
    for (i = 0; i < sizeof(t) - 1 && MEM8(text + i); i++) t[i] = (char)MEM8(text + i);
    t[i] = 0;
    for (i = 0; i < sizeof(c) - 1 && MEM8(cap + i); i++) c[i] = (char)MEM8(cap + i);
    c[i] = 0;
    fprintf(stderr, "\n=== MessageBox [%s] ===\n%s\n=====================\n", c, t);
    RET(1 /*IDOK*/); STDRET(4);
}

/* timeGetTime is in recomp_runtime.c (real high-res, strictly increasing) so POD's
 * RDTSC CPU-speed calibration never divides by a zero time-delta on fast hardware. */
void imp_timeBeginPeriod(void){ RET(0); STDRET(1); }
void imp_timeEndPeriod(void)  { RET(0); STDRET(1); }

/* ---- dynamic loading (no real DLLs; report failure cleanly) ---- */
void imp_LoadLibraryA(void)  { RET(0); STDRET(1); }
void imp_GetProcAddress(void){ RET(0); STDRET(2); }

/* ---- process exit: actually terminate so we don't run into garbage ---- */
void imp_ExitProcess(void) {   /* (uExitCode) */
    uint32_t code = ARG(0);
    fprintf(stderr, "\n[ExitProcess] game called ExitProcess(%u) - terminating\n", code);
    recomp_dump_tls_ring("ExitProcess caller");
    recomp_dump_trace("ExitProcess");
    fflush(stdout); fflush(stderr);
    exit((int)code);
}
