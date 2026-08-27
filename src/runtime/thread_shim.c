/*
 * POD Gold Recompilation - threading + synchronization.
 *
 * POD is multi-threaded: it CreateThreads worker(s) and the main loop waits on
 * their events (MsgWaitForMultipleObjects/WaitForSingleObject). With the CPU
 * register state now per-thread (RECOMP_TLS, see recomp_types.h), we can run each
 * guest thread on a REAL host thread: the launcher sets up that thread's stack,
 * FPU and TIB, pushes the thread argument, and calls the lifted thread proc
 * through the dispatch table. Synchronization objects (events/mutexes/semaphores/
 * critical sections) map to real Win32 objects so concurrent threads coordinate
 * exactly as the original did.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdio.h>
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "imports.h"

/* ---- guest handle table (threads + sync objects) ---- */
enum { OT_THREAD = 1, OT_EVENT, OT_MUTEX, OT_SEM };
typedef struct {
    int      type;
    HANDLE   h;            /* host object (thread/event/mutex/semaphore) */
    uint32_t proc, param;  /* thread: guest proc VA + argument */
    uint32_t stack_base, tib_base;
} HObj;

#define HOBJ_BASE  0x000B0000u
#define HOBJ_MAX   512
static HObj g_obj[HOBJ_MAX];
static int  g_obj_n = 0;
static CRITICAL_SECTION g_obj_cs;
static int g_obj_cs_init = 0;

static void obj_lock_init(void) { if (!g_obj_cs_init) { InitializeCriticalSection(&g_obj_cs); g_obj_cs_init = 1; } }

static uint32_t obj_alloc(int type, HANDLE h) {
    obj_lock_init();
    EnterCriticalSection(&g_obj_cs);
    uint32_t gh = 0;
    if (g_obj_n < HOBJ_MAX) { int i = g_obj_n++; g_obj[i].type = type; g_obj[i].h = h; gh = HOBJ_BASE + i; }
    LeaveCriticalSection(&g_obj_cs);
    return gh;
}
static HObj* obj_get(uint32_t gh) {
    uint32_t i = gh - HOBJ_BASE;
    return (gh >= HOBJ_BASE && i < (uint32_t)g_obj_n) ? &g_obj[i] : NULL;
}
int  pod_obj_is_handle(uint32_t gh) { return obj_get(gh) != NULL; }
void pod_obj_close(uint32_t gh) {       /* CloseHandle on a thread/event/mutex/sem */
    HObj* o = obj_get(gh);
    if (o && o->h) { CloseHandle(o->h); o->h = NULL; o->type = 0; }
}

/* ---- thread launcher: runs on a real host thread ---- */
#define POD_TSTACK_SIZE 0x00100000u   /* 1 MB guest stack per thread */
#define POD_TTIB_SIZE   0x00001000u

static unsigned __stdcall thread_launch(void* arg) {
    HObj* o = (HObj*)arg;
    /* This thread's own register/FPU state (TLS). */
    g_fpu_cw = 0x037F; g_fp_top = 0;
    g_seg_ds = g_seg_es = g_seg_ss = 0;
    g_fs_base = o->tib_base;
    /* TIB: SEH chain end, stack base/limit, self. */
    MEM32(g_fs_base + 0x00) = 0xFFFFFFFFu;
    MEM32(g_fs_base + 0x04) = o->stack_base + POD_TSTACK_SIZE;
    MEM32(g_fs_base + 0x08) = o->stack_base;
    MEM32(g_fs_base + 0x18) = g_fs_base;
    g_esp = o->stack_base + POD_TSTACK_SIZE - 64;
    /* Force this thread's Win32 message queue to exist now, so a PostThreadMessageA
     * from another thread reaches it (POD's sound thread is messaged this way). */
    { MSG _m; PeekMessageA(&_m, NULL, 0, 0, PM_NOREMOVE); }
    /* stdcall thread proc: [esp]=ret addr, [esp+4]=lpParameter */
    PUSH32(g_esp, o->param);
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t fn = recomp_lookup(o->proc);
    fprintf(stderr, "[thread] start tid=%lu proc=0x%08X param=0x%08X stack=0x%08X\n",
            GetCurrentThreadId(), o->proc, o->param, o->stack_base);
    if (fn) fn();
    fprintf(stderr, "[thread] proc 0x%08X returned (eax=0x%08X)\n", o->proc, g_eax);
    return g_eax;
}

void imp_CreateThread(void) {  /* (lpAttr, dwStackSize, lpStartAddress, lpParameter, dwFlags, lpThreadId) */
    uint32_t proc = ARG(2), param = ARG(3), flags = ARG(4), pTid = ARG(5);
    uint32_t gh = obj_alloc(OT_THREAD, NULL);
    HObj* o = obj_get(gh);
    o->proc = proc; o->param = param;
    o->stack_base = shim_alloc_aligned(POD_TSTACK_SIZE, 0x1000);
    o->tib_base   = shim_alloc_aligned(POD_TTIB_SIZE, 0x1000);
    unsigned tid = 0;
    /* CREATE_SUSPENDED (0x4): start the host thread suspended; ResumeThread runs it. */
    o->h = (HANDLE)_beginthreadex(NULL, 0, thread_launch, o,
                                  (flags & 0x4) ? CREATE_SUSPENDED : 0, &tid);
    if (pTid) MEM32(pTid) = tid;
    RET(gh); STDRET(6);
}
void imp_ResumeThread(void)  { HObj* o = obj_get(ARG(0)); if (o && o->type == OT_THREAD) ResumeThread(o->h); RET(0); STDRET(1); }
void imp_SuspendThread(void) { HObj* o = obj_get(ARG(0)); if (o && o->type == OT_THREAD) SuspendThread(o->h); RET(0); STDRET(1); }
void imp_GetCurrentThreadId(void) { RET(GetCurrentThreadId()); STDRET(0); }
void imp_ExitThread(void) { uint32_t code = ARG(0); fprintf(stderr, "[thread] ExitThread(%u)\n", code); _endthreadex(code); RET(0); }

/* ---- events / mutexes / semaphores ---- */
void imp_CreateEventA(void) {  /* (lpAttr, bManualReset, bInitialState, lpName) */
    HANDLE h = CreateEventA(NULL, (BOOL)ARG(1), (BOOL)ARG(2), NULL);
    RET(obj_alloc(OT_EVENT, h)); STDRET(4);
}
void imp_SetEvent(void)   { HObj* o = obj_get(ARG(0)); if (o) SetEvent(o->h);   RET(1); STDRET(1); }
void imp_ResetEvent(void) { HObj* o = obj_get(ARG(0)); if (o) ResetEvent(o->h); RET(1); STDRET(1); }
void imp_CreateMutexA(void) {  /* (lpAttr, bInitialOwner, lpName) */
    HANDLE h = CreateMutexA(NULL, (BOOL)ARG(1), NULL);
    RET(obj_alloc(OT_MUTEX, h)); STDRET(3);
}
void imp_ReleaseMutex(void) { HObj* o = obj_get(ARG(0)); if (o) ReleaseMutex(o->h); RET(1); STDRET(1); }
void imp_CreateSemaphoreA(void) {  /* (lpAttr, lInitialCount, lMaximumCount, lpName) */
    HANDLE h = CreateSemaphoreA(NULL, (LONG)ARG(1), (LONG)ARG(2), NULL);
    RET(obj_alloc(OT_SEM, h)); STDRET(4);
}
void imp_ReleaseSemaphore(void) {  /* (hSem, lReleaseCount, lpPreviousCount) */
    HObj* o = obj_get(ARG(0)); LONG prev = 0;
    if (o) ReleaseSemaphore(o->h, (LONG)ARG(1), &prev);
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)prev;
    RET(1); STDRET(3);
}

/* ---- waits ---- */
void imp_WaitForSingleObject(void) {  /* (hHandle, dwMs) */
    HObj* o = obj_get(ARG(0));
    static uint32_t n = 0; int log = (n++ < 40);
    if (log) fprintf(stderr, "[wait] WFSO tid=%lu h=0x%08X ms=%u type=%d ...\n",
                     GetCurrentThreadId(), ARG(0), ARG(1), o ? o->type : -1);
    DWORD r = o ? WaitForSingleObject(o->h, ARG(1)) : 0;
    if (log) fprintf(stderr, "[wait] WFSO tid=%lu -> %lu\n", GetCurrentThreadId(), r);
    RET(r); STDRET(2);
}
void imp_WaitForMultipleObjects(void) {  /* (nCount, lpHandles, bWaitAll, dwMs) */
    uint32_t n = ARG(0), ph = ARG(1);
    HANDLE hh[64]; if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) { HObj* o = obj_get(MEM32(ph + i * 4)); hh[i] = o ? o->h : NULL; }
    DWORD r = WaitForMultipleObjects(n, hh, (BOOL)ARG(2), ARG(3));
    RET(r); STDRET(4);
}
void imp_MsgWaitForMultipleObjects(void) {  /* (nCount, lpHandles, bWaitAll, dwMs, dwWakeMask) */
    uint32_t n = ARG(0), ph = ARG(1);
    HANDLE hh[64]; if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) { HObj* o = obj_get(MEM32(ph + i * 4)); hh[i] = o ? o->h : NULL; }
    static uint32_t m = 0; int log = (m++ < 20);
    /* The pump is where the game sits when it is alive but drawing nothing.
     * Dump what ran between waits, once, so an idle loop names its own code. */
    if (m == 500 || m == 3000 || m == 6000) recomp_dump_all_rings("pump tick");
    if (log) fprintf(stderr, "[wait] MsgWait tid=%lu n=%u ms=%u ...\n", GetCurrentThreadId(), n, ARG(3));
    DWORD r = MsgWaitForMultipleObjects(n, hh, (BOOL)ARG(2), ARG(3), ARG(4));
    if (log) fprintf(stderr, "[wait] MsgWait tid=%lu -> %lu\n", GetCurrentThreadId(), r);
    RET(r); STDRET(5);
}

/* ---- critical sections: guest CS pointer -> host CRITICAL_SECTION ---- */
#define CS_MAX 256
static struct { uint32_t va; CRITICAL_SECTION cs; } g_cs[CS_MAX];
static int g_cs_n = 0;
static CRITICAL_SECTION* cs_for(uint32_t va, int create) {
    obj_lock_init();
    EnterCriticalSection(&g_obj_cs);
    CRITICAL_SECTION* r = NULL;
    for (int i = 0; i < g_cs_n; i++) if (g_cs[i].va == va) { r = &g_cs[i].cs; break; }
    if (!r && create && g_cs_n < CS_MAX) {
        int i = g_cs_n++; g_cs[i].va = va; InitializeCriticalSection(&g_cs[i].cs); r = &g_cs[i].cs;
    }
    LeaveCriticalSection(&g_obj_cs);
    return r;
}
void imp_InitializeCriticalSection(void) { cs_for(ARG(0), 1); STDRET(1); }
void imp_DeleteCriticalSection(void)     { STDRET(1); }   /* leave the host CS allocated */
void imp_EnterCriticalSection(void) { CRITICAL_SECTION* c = cs_for(ARG(0), 1); if (c) EnterCriticalSection(c); STDRET(1); }
void imp_LeaveCriticalSection(void) { CRITICAL_SECTION* c = cs_for(ARG(0), 0); if (c) LeaveCriticalSection(c); STDRET(1); }
