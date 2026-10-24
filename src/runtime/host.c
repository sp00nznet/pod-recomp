/*
 * POD Gold - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). What is here is only what is
 * specific to this game: the guest image, where it finds its data and its CD,
 * the command line, scripted input and the fault report. The display is
 * ddraw.c, the keyboard dinput.c. docs/architecture.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (PODMMX.EXE) is
 * free when main() maps the image. Shape: Streets of SimCity's host.c.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "native32.h"
#include "recomp_trace.h"
#include "ddraw_host.h"
#include "dinput_host.h"
#include "oracle.h"
#include "indeo.h"

extern const uint32_t pod_entry_va;       /* recomp_dispatch.c */

#define POD_BASE 0x00400000u

static DWORD g_watchdog_s;
static int   g_headless;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is PODMMX.EXE in the game folder, not this host: its hInstance
 * (icon, strings) is GetModuleHandleA(NULL). */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)) : POD_BASE, 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == POD_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, g_guest_exe, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* --headless: nothing reaches the screen (REPO_RULES section 13). The game's
 * own message boxes go to stderr. */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

/* ------------------------------------------------------------ input */

/* --key NAME@MS[+HOLD]: scripted keys, in milliseconds from entry. POD reads
 * the keyboard through DirectInput (dinput.c answers GetDeviceState from
 * here) and its menus through window messages, so a scripted key is held for
 * the first and posted to the window for the second. Headless, the real
 * keyboard is not read at all. */
#define MAX_INPUT 64
static struct { int vk; DWORD at, hold; } g_in[MAX_INPUT];
static int g_nin;
static volatile LONG g_held[256];
int host_key_held(int vk) { return g_held[vk & 0xFF] != 0; }

static int vk_of(const char* s) {
    static const struct { const char* n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"ESC", VK_ESCAPE},
        {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);
    return (int)strtol(s, NULL, 0);
}
static int parse_key(const char* arg) {
    char name[16];
    unsigned at = 0, hold = 150;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_in[g_nin].vk = vk_of(name) & 0xFF;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = hold;
    return 1;
}
static DWORD WINAPI input_thread(LPVOID p) {
    int i = (int)(intptr_t)p;
    DWORD t0 = GetTickCount();
    while (!ddh_window() || GetTickCount() - t0 < g_in[i].at) Sleep(5);
    HWND w = ddh_window();
    int vk = g_in[i].vk;
    LPARAM sc = (LPARAM)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16;
    fprintf(stderr, "[input] key 0x%02X down at %lu ms\n", vk, GetTickCount() - t0);
    InterlockedIncrement(&g_held[vk]);
    dih_key_changed();
    PostMessageA(w, WM_KEYDOWN, vk, sc | 1);
    Sleep(g_in[i].hold);
    InterlockedDecrement(&g_held[vk]);
    dih_key_changed();
    PostMessageA(w, WM_KEYUP, vk, sc | 0xC0000001u);
    return 0;
}
static void shim_GetKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless ? 0 : GetKeyState(vk);
    RET((uint16_t)s, 1);
}
/* Windowed, a real key counts only while the game's window has the focus:
 * GetAsyncKeyState sees the whole desktop's keyboard. */
static void shim_GetAsyncKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless || !ddh_focused() ? 0 : GetAsyncKeyState(vk);
    RET((uint16_t)s, 1);
}

/* POD's WinMain sleeps in MsgWaitForMultipleObjects(INFINITE) and runs a
 * frame only when something wakes it. Fullscreen on a 1997 PC, paints and
 * input arrived all the time; a window nobody touches (headless, always)
 * gets nothing and the game sleeps for good. A 60 Hz WM_TIMER is the traffic
 * it expects. Posted, because SetTimer only works from the window's thread. */
static DWORD WINAPI heartbeat(LPVOID unused) {
    (void)unused;
    while (!ddh_window()) Sleep(5);
    for (;;) {
        PostMessageA(ddh_window(), WM_TIMER, 1, 0);
        Sleep(16);
    }
}

/* Joysticks and wheels: POD polls WINMM's joystick 0 (joyGetPosEx) and drives
 * its menus with it. Headless, a controller plugged into the machine is real
 * input like the keyboard, and it walked the menus into a car select on its
 * own, so there is none; windowed, Windows' answers go through. */
static void shim_joyGetPosEx(void) {
    RET(g_headless ? JOYERR_UNPLUGGED : joyGetPosEx(ARG(0), (LPJOYINFOEX)(uintptr_t)ARG(1)), 2);
}
static void shim_joyGetDevCapsA(void) {
    RET(g_headless ? JOYERR_UNPLUGGED : joyGetDevCapsA(ARG(0), (LPJOYCAPSA)(uintptr_t)ARG(1), ARG(2)), 3);
}

/* ------------------------------------------------------------ the CD */

/* The game wants its CD: it looks for a drive GetDriveTypeA calls
 * DRIVE_CDROM whose volume label is the disc's, POD2_0. The game folder holds
 * the disc's files, so it is offered as that CD: a free drive letter mapped
 * onto it for this logon session (DefineDosDevice, what `subst` does, no
 * admin needed). A letter a crashed run left mapped onto the same folder is
 * reused. docs/architecture.md. */
static char g_cd[4];                     /* "X:\", or "" when no letter was free */
static char g_cd_target[MAX_PATH + 8];   /* what it maps to, for the removal */

static void cd_unmount(void) {
    char dev[3] = { g_cd[0], ':', 0 };
    if (g_cd[0] && DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH,
                                    dev, g_cd_target))
        g_cd[0] = 0;
}

static void cd_mount(void) {
    char folder[MAX_PATH], raw[MAX_PATH + 8], have[MAX_PATH + 8];
    strcpy(folder, g_game);
    folder[strlen(folder) - 1] = 0;                   /* no trailing '\' */
    _snprintf(raw, sizeof raw - 1, "\\??\\%s", folder);
    raw[sizeof raw - 1] = 0;
    for (char l = 'Z'; l >= 'D'; l--) {
        char dev[3] = { l, ':', 0 };
        int mine = QueryDosDeviceA(dev, have, sizeof have) && !_stricmp(have, raw);
        if (!mine && (QueryDosDeviceA(dev, have, sizeof have) || GetLastError() != ERROR_FILE_NOT_FOUND)) continue;
        if (mine || DefineDosDeviceA(DDD_RAW_TARGET_PATH, dev, raw)) {
            strcpy(g_cd_target, raw);
            g_cd[0] = l; g_cd[1] = ':'; g_cd[2] = '\\';
            fprintf(stderr, "[cd] %s is the game folder, as the POD Gold CD\n", g_cd);
            return;
        }
    }
    fprintf(stderr, "[cd] no free drive letter: the game will ask for its CD\n");
}

static int is_cd(const char* root) {
    return g_cd[0] && root && toupper((unsigned char)root[0]) == g_cd[0] && root[1] == ':';
}

static void shim_GetDriveTypeA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    RET(is_cd(root) ? DRIVE_CDROM : GetDriveTypeA(root), 1);
}

/* ...whose label is the pressed disc's. */
static void shim_GetVolumeInformationA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    if (!is_cd(root)) {
        RET(GetVolumeInformationA(root, (LPSTR)(uintptr_t)ARG(1), ARG(2), (LPDWORD)(uintptr_t)ARG(3),
                                  (LPDWORD)(uintptr_t)ARG(4), (LPDWORD)(uintptr_t)ARG(5),
                                  (LPSTR)(uintptr_t)ARG(6), ARG(7)), 8);
        return;
    }
    if (ARG(1) && ARG(2)) lstrcpynA((char*)(uintptr_t)ARG(1), "POD2_0", ARG(2));
    if (ARG(3)) MEM32(ARG(3)) = 0x504F4432u;          /* serial */
    if (ARG(4)) MEM32(ARG(4)) = 255;                  /* longest name */
    if (ARG(5)) MEM32(ARG(5)) = 0;                    /* flags */
    if (ARG(6) && ARG(7)) lstrcpynA((char*)(uintptr_t)ARG(6), "CDFS", ARG(7));
    RET(TRUE, 8);
}

/* The music is Red Book audio on the disc (POD2_0.CUE: track 1 data, track
 * 2 audio), played through MCI's cdaudio device. The game folder standing in
 * for the CD has no audio tracks, so a real open fails (MCI error 0x10A) and
 * POD stops at its "CD error" dialog. The host answers cdaudio itself: it
 * opens, reports the disc's two tracks with track 2 as audio, and takes
 * play/stop/seek without sound. Every other MCI device is Windows'.
 * ponytail: silent; playing track 2 out of the user's POD2_0.BIN is the
 * upgrade (ROADMAP.md). */
#define POD_CD_DEVICE 0xCDu
static int g_cd_playing;

static void shim_mciSendCommandA(void) {
    uint32_t dev = ARG(0), msg = ARG(1), flags = ARG(2), p = ARG(3);
    if (getenv("POD_MCI_TRACE"))
        fprintf(stderr, "[mci] dev %X msg %04X flags %08X item %08X\n", dev, msg, flags,
                msg == MCI_STATUS && p ? MEM32(p + 8) : 0);
    if (msg == MCI_OPEN && p && (flags & MCI_OPEN_TYPE)) {
        uint32_t type = MEM32(p + 8);         /* MCI_OPEN_PARMSA.lpstrDeviceType */
        int cd = (flags & MCI_OPEN_TYPE_ID) ? LOWORD(type) == MCI_DEVTYPE_CD_AUDIO
                                            : type && !_stricmp((const char*)(uintptr_t)type, "cdaudio");
        if (cd) {
            MEM32(p + 4) = POD_CD_DEVICE;     /* wDeviceID */
            RET(0, 4);
            return;
        }
    }
    if (dev != POD_CD_DEVICE) {
        RET(mciSendCommandA(dev, msg, flags, p), 4);
        return;
    }
    if (msg == MCI_STATUS && p && (flags & MCI_STATUS_ITEM)) {
        uint32_t item = MEM32(p + 8), track = MEM32(p + 12), r = 0;   /* dwItem, dwTrack */
        switch (item) {
        case MCI_STATUS_NUMBER_OF_TRACKS: r = 2; break;
        case MCI_STATUS_MEDIA_PRESENT:
        case MCI_STATUS_READY:            r = TRUE; break;
        case MCI_STATUS_MODE:             r = g_cd_playing ? MCI_MODE_PLAY : MCI_MODE_STOP; break;
        case MCI_CDA_STATUS_TYPE_TRACK:   r = track == 2 ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER; break;
        }
        MEM32(p + 4) = r;                     /* dwReturn */
    }
    if (msg == MCI_PLAY) g_cd_playing = 1;
    if (msg == MCI_STOP || msg == MCI_PAUSE || msg == MCI_CLOSE) g_cd_playing = 0;
    RET(0, 4);
}

/* The videos are Indeo 4.1, which Windows has had no decoder for in years.
 * Normally the disc's own codec is lifted and installed (indeo.c), and this
 * passes everything through. Without it (--no-codec, or a disc with no
 * INSTALL\IR41_32.DLL) the file opens, AVIStreamGetFrameOpen returns NULL,
 * and POD reads a frame through that NULL and faults, as the original exe
 * does on a current PC. So a video with no decoder is refused here, the
 * error POD expects for a missing file, and it goes on without it. */
typedef HRESULT (WINAPI *avi_open_t)(void**, const char*, UINT, void*);
typedef HRESULT (WINAPI *avi_stream_t)(void*, void**, DWORD, LONG);
typedef void* (WINAPI *avi_getframe_open_t)(void*, void*);
typedef LONG (WINAPI *avi_release_t)(void*);
#define AVI_STREAMTYPE_VIDEO mmioFOURCC('v', 'i', 'd', 's')

static void shim_AVIFileOpenA(void) {
    static avi_open_t open; static avi_stream_t get_stream; static avi_getframe_open_t gf_open;
    static avi_release_t gf_close, stream_release, file_release;
    if (!open) {
        HMODULE m = LoadLibraryA("avifil32.dll");
        open = (avi_open_t)GetProcAddress(m, "AVIFileOpenA");
        get_stream = (avi_stream_t)GetProcAddress(m, "AVIFileGetStream");
        gf_open = (avi_getframe_open_t)GetProcAddress(m, "AVIStreamGetFrameOpen");
        gf_close = (avi_release_t)GetProcAddress(m, "AVIStreamGetFrameClose");
        stream_release = (avi_release_t)GetProcAddress(m, "AVIStreamRelease");
        file_release = (avi_release_t)GetProcAddress(m, "AVIFileRelease");
    }
    void** out = (void**)(uintptr_t)ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    HRESULT r = open(out, name, ARG(2), (void*)(uintptr_t)ARG(3));
    void* vs = NULL;
    if (SUCCEEDED(r) && SUCCEEDED(get_stream(*out, &vs, AVI_STREAMTYPE_VIDEO, 0))) {
        void* gf = gf_open(vs, NULL);
        if (gf) gf_close(gf);
        stream_release(vs);
        if (!gf) {
            fprintf(stderr, "[avi] %s: no decoder for its video (Indeo), not played\n", name);
            file_release(*out);
            *out = NULL;
            r = 0x80044067;                           /* AVIERR_FILEOPEN */
        }
    }
    RET((uint32_t)r, 4);
}

/* Before it plays a video POD asks SYSTEM.INI whether an Indeo 4.1 driver is
 * installed ([Drivers32] VIDC.IV41), and shows "You must install Indeo"
 * instead when there is none. With the lifted codec installed (indeo.c),
 * the answer is yes. */
static int g_have_codec;
static void shim_GetPrivateProfileStringA(void) {
    const char *sect = (const char*)(uintptr_t)ARG(0), *key = (const char*)(uintptr_t)ARG(1);
    char* buf = (char*)(uintptr_t)ARG(3);
    uint32_t size = ARG(4);
    if (g_have_codec && sect && key && !_stricmp(sect, "Drivers32") && !_stricmp(key, "VIDC.IV41") && size) {
        lstrcpynA(buf, "ir41_32.dll", size);
        RET(strlen(buf), 6);
        return;
    }
    RET(GetPrivateProfileStringA(sect, key, (const char*)(uintptr_t)ARG(2), buf, size,
                                 (const char*)(uintptr_t)ARG(5)), 6);
}

/* ...and then that the driver file is in the system folder
 * (GetSystemDirectoryA + "\IR41_32.DLL", FindFirstFileA). The lifted one is
 * the disc's own copy, so that lookup finds game\INSTALL\IR41_32.DLL. */
static void shim_FindFirstFileA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    size_t n = name ? strlen(name) : 0;
    if (g_have_codec && n >= 12 && !_stricmp(name + n - 12, "\\IR41_32.DLL")) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path - 1, "%sINSTALL\\IR41_32.DLL", g_game);
        path[sizeof path - 1] = 0;
        RET((uintptr_t)FindFirstFileA(path, (LPWIN32_FIND_DATAA)(uintptr_t)ARG(1)), 2);
        return;
    }
    RET((uintptr_t)FindFirstFileA(name, (LPWIN32_FIND_DATAA)(uintptr_t)ARG(1)), 2);
}

/* POD's installer records the install in <Windows>\UbiSoft\Ubi.ini
 * ([POD2_0] Directory, Language, Configuration), and the game quits quietly
 * without it. The game folder stands in for the Windows folder, so
 * game\UbiSoft\Ubi.ini (tools/setup.ps1 writes it) is read and the real
 * Windows folder is never touched. POD asks it for nothing else. */
static void shim_GetWindowsDirectoryA(void) {
    char* out = (char*)(uintptr_t)ARG(0);
    uint32_t size = ARG(1), n = (uint32_t)strlen(g_game) - 1;   /* no trailing '\' */
    if (n < size) { memcpy(out, g_game, n); out[n] = 0; RET(n, 2); }
    else RET(n + 1, 2);
}

/* ------------------------------------------------------------ exit */

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), ddh_frames());
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    ExitProcess(ARG(0));
}

static native32_shim_t g_shims[] = {
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetKeyState", shim_GetKeyState },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "ExitProcess", shim_ExitProcess },
    { "GetDriveTypeA", shim_GetDriveTypeA },
    { "GetVolumeInformationA", shim_GetVolumeInformationA },
    { "GetWindowsDirectoryA", shim_GetWindowsDirectoryA },
    { "mciSendCommandA", shim_mciSendCommandA },
    { "AVIFileOpenA", shim_AVIFileOpenA },
    { "GetPrivateProfileStringA", shim_GetPrivateProfileStringA },
    { "FindFirstFileA", shim_FindFirstFileA },
    { "joyGetPosEx", shim_joyGetPosEx },
    { "joyGetDevCapsA", shim_joyGetDevCapsA },
    DDH_SHIMS,
    DIH_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },          /* headless only: the last entry */
};

/* --original: the original's CPU detector (0x004A2DBC) answered as the
 * lift's run_lift.py PATCHES answer it, a Pentium MMX (GenuineIntel, family
 * 5 model 4: renderer class 2). On a CPU from after 2004 the original would
 * otherwise quit at start, so there would be nothing to compare against. */
static void hook_cpu_detect(void) {
    MEM32(0x006E7298u) = 0x756E6547u;    /* "Genu" */
    MEM32(0x006E729Cu) = 0x49656E69u;    /* "ineI" */
    MEM32(0x006E72A0u) = 0x6C65746Eu;    /* "ntel" */
    MEM32(0x006E72A8u) = 0x00000543u;    /* CPUID 1: family 5, model 4 */
    MEM32(0x006E7290u) = 2u;             /* the renderer class */
    RET(2, 0);
}
static const oracle_hook_t g_hooks[] = { { 0x004A2DBCu, hook_cpu_detect } };

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    cd_unmount();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Where every other thread is: its eip and the host return addresses on its
 * stack, newest first. `py -3 tools/addr2line.py` names them. */
static void dump_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    uintptr_t lo = (uintptr_t)GetModuleHandleA(NULL), hi = lo + 0x4000000;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_CONTROL;
        if (t && SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                fprintf(stderr, "  thread %lu: eip=%08lX esp=%08lX host frames:", te.th32ThreadID, c.Eip, c.Esp);
                MEMORY_BASIC_INFORMATION mbi;
                int n = 0;
                if (VirtualQuery((void*)(uintptr_t)c.Esp, &mbi, sizeof mbi))
                    for (uint32_t* p = (uint32_t*)(uintptr_t)c.Esp;
                         (uint8_t*)p + 4 <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize && n < 16; p++)
                        if (*p >= lo && *p < hi) { fprintf(stderr, " %08X", *p); n++; }
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    dump_threads();
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, ddh_frames());
    native32_dump_icalls(8);
    recomp_dump_trace("watchdog");          /* with -DPOD_TRACE=ON: the last function entries */
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game";
    int run = 0, scale = 0, original = 0, nlift = 0, codec_native = 0, no_codec = 0;
    const char *test_avi = NULL, *test_out = NULL;
    static uint32_t lift[256];
    ddh_opts_t o = { 0 };
    indeo_hold();                        /* before anything else can land on 0x10000000 */
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            o.record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.stop_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            static char shot[MAX_PATH];
            char* colon = strchr(argv[++i], ':');
            o.shot_frame = atol(argv[i]);
            if (!colon || o.shot_frame < 1) { fprintf(stderr, "bad --shot %s (want N:file.bmp)\n", argv[i]); return 1; }
            GetFullPathNameA(colon + 1, MAX_PATH, shot, NULL);
            o.shot_path = shot;
        }
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld,%ld", &o.diff_a, &o.diff_b) != 2 || o.diff_b <= o.diff_a) {
                fprintf(stderr, "bad --diff %s (want A,B in ms, A < B)\n", argv[i]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--ddraw-trace")) o.trace = 1;
        else if (!strcmp(argv[i], "--original")) original = 1;
        else if (!strcmp(argv[i], "--codec-native")) codec_native = 1;
        else if (!strcmp(argv[i], "--no-codec")) no_codec = 1;
        else if (!strcmp(argv[i], "--decode-test") && i + 2 < argc) { test_avi = argv[i + 1]; test_out = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--lifted") && i + 1 < argc)      /* with --original */
            for (char* t = strtok(argv[++i], ","); t && nlift < 256; t = strtok(NULL, ","))
                lift[nlift++] = strtoul(t, NULL, 16);
        else {
            printf("usage: pod [--run] [--headless] [--record out.mp4] [--frames N]\n"
                   "           [--shot N:file.bmp] [--diff A,B] [--key NAME@MS[+HOLD]]\n"
                   "           [--scale N] [--game game] [--watchdog S] [--native-trace]\n"
                   "           [--callbacks] [--ddraw-trace] [--original [--lifted VA,...]]\n"
                   "           [--codec-native] [--no-codec] [--decode-test AVI OUT]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sPODMMX.EXE", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    o.headless = g_headless;
    o.scale = scale;
    ddh_init(&o);
    dih_init(g_headless);
    native32_init();
    printf("POD Gold recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
    int nshims = (int)(sizeof g_shims / sizeof g_shims[0]) - !g_headless;
    if (original && run) {
        /* The shipping machine code under the same host, shims and input
         * (oracle.c): the reference a lifted run is compared against. */
        if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   /* a crash ends it, no dialog */
        cd_mount();
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        for (int i = 0; i < g_nin; i++)
            CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, heartbeat, NULL, 0, NULL));
        {   /* the codec too, as original code, so the reference plays the videos */
            char codec[MAX_PATH];
            _snprintf(codec, sizeof codec - 1, "%sINSTALL\\IR41_32.DLL", g_game);
            codec[sizeof codec - 1] = 0;
            g_have_codec = indeo_start(codec, g_shims, nshims, 1);
        }
        oracle_lift(lift, nlift);
        return oracle_run(g_guest_exe, POD_BASE, g_shims, nshims, g_hooks, 1);
    }
    /* Lifted runs only: the original may handle its own exceptions with SEH. */
    AddVectoredExceptionHandler(0, crash);
    uint32_t span = native32_map(g_guest_exe, POD_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, POD_BASE); return 1; }
    printf("  mapped PODMMX.EXE: 0x%08X-0x%08X\n", POD_BASE, POD_BASE + span);
    if (native32_bind(POD_BASE, g_shims, nshims)) return 1;
    /* The videos' Indeo 4.1 codec, from the disc, lifted with the game (indeo.c). */
    char codec[MAX_PATH];
    _snprintf(codec, sizeof codec - 1, "%sINSTALL\\IR41_32.DLL", g_game);
    codec[sizeof codec - 1] = 0;
    g_have_codec = !no_codec && indeo_start(codec, g_shims, nshims, codec_native);
    if (test_avi) return g_have_codec ? indeo_decode_test(test_avi, test_out) : 1;
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", pod_entry_va);
        return 0;
    }
    /* The game opens its data by relative path (DATA\...), from the current folder. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    cd_mount();
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    for (int i = 0; i < g_nin; i++)
        CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, heartbeat, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", pod_entry_va);
    fflush(stdout);
    native32_call_guest(pod_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    ddh_finish();
    cd_unmount();
    return (int)g_eax;
}
