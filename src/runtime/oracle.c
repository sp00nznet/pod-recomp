/*
 * --original: run the shipping PODMMX.EXE's own machine code, natively,
 * inside this host, as the reference the recompilation is compared against
 * (from Tiberian Sun's, itself civ3's src/runtime/oracle.c).
 *
 * The image is mapped at its base exactly as for the lifted run, but its code
 * is left executable and nothing is lifted: the IAT is bound to the real
 * functions, and the entry point is called on a thread of its own. The exe's
 * imports get the SAME shims the lifted run has -- headless DirectDraw, focus,
 * registration-free COM, scripted input -- so one script drives two machines
 * and any difference between them is the lift's.
 *
 * The shims are written for the lifted model: they read their arguments at
 * ARG(n), off g_esp, and pop them by advancing g_esp. A native caller reaches
 * one through a thunk (`mov eax, shim; jmp shim_stub`): the stub points g_esp
 * at the real stack, runs the shim, and returns with whatever purge the shim
 * applied. The register globals are saved and restored around it, because a
 * shim can re-enter (CreateWindowExA sends WM_CREATE to the game's window
 * procedure, which calls another shimmed import before the first returns).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "native32.h"
#include "image_loader.h"
#include "oracle.h"

/* One shim at a time, under native32's machine lock (recursive for
 * re-entry): the display shims hand it back around calls that can call the
 * window procedure (mach_leave in ddraw.c), and need it held to do so. */
static uint32_t __cdecl shim_dispatch(recomp_func_t fn, uint32_t esp, uint32_t* out_esp) {
    mach_enter();
    uint32_t se = g_esp, sa = g_eax;
    g_esp = esp;
    fn();
    *out_esp = g_esp;
    uint32_t r = g_eax;
    g_esp = se;
    g_eax = sa;
    mach_leave();
    return r;
}

/* eax = the shim. [esp] = the caller's return address, arguments above it. */
static __declspec(naked) void shim_stub(void) {
    __asm {
        mov ecx, esp            ; the stack as the caller left it
        sub esp, 4              ; room for the stack the shim leaves
        mov edx, esp
        push edx
        push ecx
        push eax
        call shim_dispatch
        add esp, 12
        pop edx                 ; where the shim left the stack
        mov ecx, [esp]          ; the return address
        mov esp, edx
        jmp ecx
    }
}

static const uint32_t* g_lift;
static int g_nlift;
void oracle_lift(const uint32_t* vas, int n) { g_lift = vas; g_nlift = n; }

/* `mov eax, imm32; jmp rel32`, one per shim, in memory that may execute. */
static uint8_t* g_thunks;
static int g_nthunks;

static void* thunk(recomp_func_t fn) {
    uint8_t* t = g_thunks + 10 * g_nthunks++;
    t[0] = 0xB8;
    *(uint32_t*)(t + 1) = (uint32_t)(uintptr_t)fn;
    t[5] = 0xE9;
    *(int32_t*)(t + 6) = (int32_t)((uintptr_t)shim_stub - (uintptr_t)(t + 10));
    return t;
}

static int bind(uint32_t base, native32_shim_t* shims, int nshims) {
    BYTE* b = (BYTE*)(uintptr_t)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int native = 0, shimmed = 0, missing = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress); d->Name; d++) {
        HMODULE h = LoadLibraryA((const char*)(b + d->Name));
        uint32_t* ilt = (uint32_t*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(b + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            int by_ord = (*ilt & 0x80000000u) != 0;
            const char* nm = by_ord ? (const char*)(uintptr_t)(*ilt & 0xFFFF) : (const char*)(b + *ilt + 2);
            void* fn = NULL;
            for (int i = 0; !by_ord && i < nshims; i++)
                if (!strcmp(nm, shims[i].name)) fn = thunk(shims[i].fn);
            if (fn) shimmed++;
            else if (h && (fn = (void*)GetProcAddress(h, nm))) native++;
            else missing++;
            *iat = (uint32_t)(uintptr_t)fn;
        }
    }
    printf("[original] imports: %d native, %d shimmed, %d unresolved\n", native, shimmed, missing);
    /* glide2x.dll's other 35, with no Voodoo: unused by the software renderer (host.c) */
    return missing == 35 && !GetModuleHandleA("glide2x.dll") ? 0 : missing;
}

/* First-chance exceptions, logged and passed on: the original handles its
 * own with SEH, so this only reports. */
static LONG CALLBACK note(EXCEPTION_POINTERS* ep) {
    /* Each place once: the guarded divide raises many times a second, and
     * the exception that matters comes after. */
    static void* seen[64];
    static volatile LONG n;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    int fresh = 1;
    for (LONG i = 0; i < n && i < 64; i++) fresh &= seen[i] != r->ExceptionAddress;
    if ((r->ExceptionCode & 0xF0000000u) == 0xC0000000u && fresh && n < 64) {
        seen[InterlockedIncrement(&n) - 1] = r->ExceptionAddress;
        CONTEXT* c = ep->ContextRecord;
        fprintf(stderr, "[original] exception 0x%08lX at %p (%s 0x%08lX) eax=%08lX ecx=%08lX esp=%08lX [esp]=%08lX\n",
                r->ExceptionCode, r->ExceptionAddress,
                r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 ? "write" : "read",
                r->NumberParameters >= 2 ? (unsigned long)r->ExceptionInformation[1] : 0,
                c->Eax, c->Ecx, c->Esp, IsBadReadPtr((void*)(uintptr_t)c->Esp, 4) ? 0 : *(DWORD*)(uintptr_t)c->Esp);
        /* The SEH chain, innermost first: who would catch it, and from which
         * frame (a C++ try/catch's handler is `mov eax, funcinfo; jmp ...`). */
        fprintf(stderr, "  SEH chain:");
        EXCEPTION_REGISTRATION_RECORD* e = ((NT_TIB*)NtCurrentTeb())->ExceptionList;
        for (int k = 0; k < 6 && e && e != (void*)-1 && !IsBadReadPtr(e, 8); k++, e = e->Next)
            fprintf(stderr, " %p(frame %p)", (void*)e->Handler, (void*)e);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI entry(LPVOID va) {
    ((void (*)(void))va)();               /* the CRT's entry: ends in ExitProcess */
    return 0;
}

int oracle_run(const char* exe_full, uint32_t base, native32_shim_t* shims, int nshims,
               const oracle_hook_t* hooks, int nhooks) {
    g_thunks = (uint8_t*)VirtualAlloc(NULL, 10 * (nshims * 4 + nhooks + 16), MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    uint32_t span = recomp_load_image(exe_full, base);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, base); return 1; }
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(uintptr_t)(base + ((IMAGE_DOS_HEADER*)(uintptr_t)base)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        DWORD old;
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            VirtualProtect((void*)(uintptr_t)(base + s->VirtualAddress), s->Misc.VirtualSize,
                           PAGE_EXECUTE_READWRITE, &old);
    }
    printf("[original] mapped %s: 0x%08X-0x%08X, running the original machine code\n", exe_full, base, base + span);
    if (bind(base, shims, nshims)) return 1;
    /* The lift's HOOKS, as a jmp at the function's entry (the debug printf is
     * a bare `ret` in the original, five bytes are there to overwrite). */
    for (int i = 0; i < nhooks; i++) {
        uint8_t* at = (uint8_t*)(uintptr_t)hooks[i].va;
        if (!at) continue;                       /* an address not found yet */
        uint8_t* t = (uint8_t*)thunk(hooks[i].fn);
        at[0] = 0xE9;
        *(int32_t*)(at + 1) = (int32_t)((uintptr_t)t - (uintptr_t)(at + 5));
    }
    if (g_nlift)
        printf("[original] %d of %d functions run lifted\n", native32_lift_in_native(g_lift, g_nlift), g_nlift);
    uint32_t ep = base + nt->OptionalHeader.AddressOfEntryPoint;
    AddVectoredExceptionHandler(1, note);
    printf("  entering 0x%08X (original)\n\n", ep);
    fflush(stdout);
    HANDLE t = CreateThread(NULL, 16u << 20, entry, (LPVOID)(uintptr_t)ep, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    return 0;
}
