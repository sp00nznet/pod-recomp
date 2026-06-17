# Phase 3 — runtime bring-up

Phase 3 turns the compiled translation units into a **host executable that maps
`PODMMX.EXE` into a simulated address space and actually executes its code**,
starting from the Watcom CRT entry point. Architecture follows the sister project
`fury3` (the first in the family to boot into its main loop).

## What was built

| Piece | File | Role |
|-------|------|------|
| Runnable lifter | `run_lift.py` | global x87 FPU stack, `RECOMP_ENTER` tracing, intra-function switch/jump-table dispatch (vs the Phase-1 `run_pipeline.py`) |
| Runtime core | `src/runtime/recomp_runtime.c` | arena memory, PE image loader, dispatch, VEH, file/INI/display host I/O, entry call |
| Import bridges | `gen_imports.py` → `src/runtime/imports_gen.c` | a stdcall-correct shim per imported function + the `{iat_va → shim}` bridge table (231 bridges) |
| Hand-written shims | `src/runtime/shims_impl.c` | CRT-critical Win32 (`GetVersion`, command line, std handles, heap, file I/O, INI, window/DIB, TLS, `ExitProcess`, …) |
| DirectDraw/DirectInput COM shim | `src/runtime/ddraw_shim.c` | fake `IDirectDraw`/`IDirectDrawSurface`/`IDirectDrawPalette` + `IDirectInput`/`IDirectInputDevice` (synthetic-VA vtables); DirectDraw backed by a window + 16bpp present |
| Fn-pointer / OOB closure | `tools/seed_fptr_targets.py` | adds code addresses taken as pointers (handler tables) as entries; stubs out-of-range misdisassembly artifacts |
| CD/config/CPU shims | `src/runtime/shims_impl.c` + `game_files/UbiSoft/Ubi.ini` | CD-ROM drive + volume label, `VirtualQuery`, CPU-detect replacement, TLS, valid `Ubi.ini` |
| Threading + sync | `src/runtime/thread_shim.c` | real host threads (per-thread `RECOMP_TLS` registers), events/mutexes/semaphores/critical-sections |
| Closure pass | `tools/seed_call_targets.py` | adds direct **call and jump** targets IDA didn't catalog (incl. cross-function switch arms) as entries, iterating to a fixed point |
| Build | `CMakeLists.txt` | links runtime + bridges + 3,601 lifted functions into `pod_recomp.exe` |

```
py -3.11 run_lift.py            # x86 -> C (runnable variant)
py -3.11 tools/seed_call_targets.py   # close the direct-call set
py -3.11 gen_imports.py         # import bridge layer
cmake -B build -A Win32 && cmake --build build --config Debug
build/Debug/pod_recomp.exe analysis/PODMMX.EXE --run
```

## Memory model

The host EXE is linked at a high base (`/BASE:0x70000000 /DYNAMICBASE:NO`). At
startup the runtime reserves **one arena** wherever Windows has room and points
`g_mem_base` at it; every guest VA `v` lives at host `g_mem_base + v`, so the
whole guest space is addressed transparently through the `MEM`/`ADDR` macros:

| Region | Guest VA | Notes |
|--------|----------|-------|
| TIB/TEB | `0x00100000` | SEH chain head, stack base/limit, self — for `fs:[…]` |
| Stack | `0x00200000`–`0x00300000` | 1 MB; `esp` starts near the top |
| Image | `0x00400000`–`~0x0078E000` | PODMMX sections at their real VAs; `.bss` zeroed |
| Heap | `0x00800000`–`0x08800000` | 128 MB bump + free-list for shim allocations |

(An earlier fixed-map-at-`0x400000` attempt failed: with the host linked high, the
CRT's own heap lands in low memory before our code runs — hence the arena.)

## How far it boots ✅

From the Watcom CRT entry (`sub_004C9FD8`), the recompiled code runs **~200
function entries (57 distinct functions) of real CRT initialization** before
stalling, exercising 9 distinct Win32 imports along the way:

```
VirtualQuery, GetCurrentThreadId, GetModuleFileNameW, GetCommandLineW,
SetUnhandledExceptionFilter, TlsAlloc, InitializeCriticalSection, TlsSetValue, ...
```

That's TLS setup, exception-filter install, command-line/heap probing, critical
sections, and the start of the CRT's **init/fini function-pointer table walk**
(`sub_004CA4EA`/`sub_004CA536`, records `{type, priority, func}` sorted by
priority) — i.e. the C runtime is genuinely coming up.

## Jump-table / cross-function control flow ✅ resolved

The earlier derail around `0x004CB3xx` (registers holding code addresses; an
indirect call reading code bytes as a pointer, `ITAIL: unresolved 0x004CB4DC` …)
was a **direct jump whose target lands inside a *neighbouring* IDA function.** The
lifter can only `goto` a label in the *same* C function, so for an out-of-function
target it emits `L_X: RECOMP_ITAIL(0xXu)` — a global tail-call. That misses the
dispatch table whenever `X` isn't a catalogued entry (a switch arm / mid-function
landing), so the jump is silently dropped and control flow is lost.

**Fix — close the control-flow target set** (`tools/seed_call_targets.py`): scan
the generated code for every literal `RECOMP_CALL(sub_X)` **and**
`RECOMP_ITAIL(0xXu)` target, add any that isn't already a dispatch entry to the
IDA catalog with bounds `[X, next-known-start)`, re-lift, and iterate to a fixed
point. A mid-function target becomes its own `sub_X` (re-lifting the tail of the
host function); since registers/stack are global, tail-calling it continues
execution correctly. Closed at **3,601 functions** (≈190 added over 5 iterations).

**Result:** `ITAIL: unresolved` went from looping-forever to **0**. Execution
advanced from 57 → **76 distinct functions** and now runs cleanly through the
CRT init/fini table walk. (One stray `ICALL: unresolved 0x004D23A3` remains — an
init-table *function pointer* IDA didn't catalog; an indirect target, so it needs
table-content analysis rather than the literal-scan closure.)

**Other fixes landed this phase:**
- Unresolved-`ICALL` no longer leaks 4 bytes of `esp` per call (was `esp += 4`
  with nothing pushed) — cut the earlier stall's log from 425K lines to a clean stop.
- `test r,r` followed by `jbe/ja/jb/jae/jg/jge/jle` now lifts to the correct
  ZF/SF-vs-0 test instead of a bogus `CMP_*(a,b)` (after `test`, CF=OF=0).
- Watcom `.bss` (SizeOfRawData≠0, PointerToRawData=0) is left zeroed, not
  overwritten with file bytes.
- Segment registers `fs`/`gs` map via capstone's symbolic IDs (stale numeric IDs
  broke `pop fs`/`pop gs`).

## CRT-heap `memset` overrun ✅ resolved (a shift-flags lifter bug)

The access violation deeper in init was Watcom's `memset` fill loop
(`sub_004CA197`) running with `ECX = 0xFF800026` (~4 GB) off the end of the arena.
Root cause: **`shl`/`shr`/`sar` didn't update the lifter's flag state.** In
`memset`, `shr ecx, 2 ; je …` decides whether to enter the unrolled fill — but the
lifted `je` consumed the *previous* instruction's (`dec`) stale ZF, took the wrong
branch, and let `ecx` underflow (`dec` from 0 → `0xFFFFFFFF`) into a multi-GB
count. Fix: the shift handlers now capture their result and set a result-vs-0 flag
state, so a following `jcc` tests the shift's ZF/SF. (Broad win — shift-then-branch
is everywhere.) Also fixed: real **TLS** (`TlsAlloc/Free/Get/SetValue` back a slot
table — the stubs made every guest thread-data lookup fail in a tight loop,
"Thread has no thread-specific data").

## How far it boots now ✅ — full startup, graceful exit

The recompiled `PODMMX.EXE` runs its **entire boot sequence** — **378 function
entries** — with no crash:

```
Watcom CRT init  →  WinMain  →  game startup
  CreateMutexA, InitializeCriticalSection, GetDriveTypeA, GetLocaleInfoA,
  GetWindowsDirectoryA, RegisterWindowMessageA, LoadCursorA, SendMessageA,
  SetClassLongA  →  DirectDrawCreate
```

It reaches **graphics initialization**, and because `DirectDrawCreate` is stubbed
to return `E_FAIL`, the game does exactly what the original would on a machine with
no DirectDraw: pops its **own** `MessageBox` — *"POD Fatal error: DirectDrawCreate
failed"* — and calls `ExitProcess(-1)`. The recompiled game is executing its real
logic end-to-end through the HAL boundary.

## DirectDraw COM shim ✅ — graphics init runs, window opens

`DirectDrawCreate` now returns a real **fake COM object** (`src/runtime/ddraw_shim.c`).
COM objects are synthesised in *guest* memory as `{ vtbl_va, fields… }`; each vtable
is an array of **synthetic method VAs** (`0xCD000000 + index`). A virtual call
`call [vtbl+off]` lifts to `RECOMP_ICALL(synthetic_va)`, which `recomp_lookup_import`
resolves (via `pod_com_lookup`) to the matching host method shim — each running in
the recomp stack model (`this=ARG(0)`, args `ARG(1..)`, `RET(hr)`, `STDRET(slots)`).

Implemented `IDirectDraw` (23 methods), `IDirectDrawSurface` (36), and a minimal
`IDirectDrawPalette` (7). The real-behaviour ones: `SetDisplayMode` opens/sizes the
window; `CreateSurface` builds a primary (+ optional attached back buffer) and an
offscreen surface with a 16bpp guest pixel buffer; `Lock` hands back that buffer's
VA in the `DDSURFACEDESC` (565 pixel format) so the MMX rasterizer renders straight
into it; `Flip`/`Blt`/`BltFast` present it to the window via `StretchDIBits` (16bpp
`BI_BITFIELDS`).

**Result:** the recompiled POD runs its real graphics init end-to-end through the
shim — `DirectDrawCreate → SetCooperativeLevel(FULLSCREEN) → EnumDisplayModes →
CreateSurface(primary, 640×480) → CreateSurface(offscreen system-mem render target)`
— and a 640×480 window opens. The game is past the HAL boundary and into its loop.

## Full startup → game loop ✅ — reaches WinMain's message loop

A run of fixes took POD all the way through its real startup to the game loop:

- **Path translation bug.** `host_translate_path` stripped `(f-low)+6` for a
  `"pod/"` match (a `"fury3/"`=6 leftover) — corrupted any path containing `pod/`.
  Fixed to `+4`.
- **CD-ROM check** (`sub_4736E4`): scans drives A–Z for a CD-ROM whose volume label
  matches. Shimmed `GetDriveTypeA` (D: = CD-ROM) + `GetVolumeInformationA`
  (label `POD2_0`). Without it, POD loops "Please insert POD CD-ROM".
- **`Ubi.ini` config.** POD reads `<WinDir>\UbiSoft\Ubi.ini` (`[POD2_0]`
  `Configuration`/`Language`/`Directory`, `[OSD]` `Valid`/`Directory`). Supplied a
  `game_files/UbiSoft/Ubi.ini` (`Configuration=2002` = "Playable config for MMX",
  per the disc's `INSTDATA/UBI.ENG`). The hardcoded `pod2_0\pod2_02.exe` /
  `startup.exe` / `Client.exe` are the Ubi Game-Service self-relaunch chain — absent
  = run in-process.
- **CPU detection** (`sub_4A2DBC`): probes CPUID via the EFLAGS ID-bit toggle, then
  maps vendor+family to a renderer class in `dword_6E7290` (WinMain bails if it's not
  1–8). Our model carries no EFLAGS, so the probe failed → bail. Replaced the
  function (`HOST_SHIM`) with a host shim reporting a Pentium-MMX Intel (class 2).
- **`VirtualQuery`**: a real implementation over the arena (the stub returned 0, so
  the CRT's memory-scan loop never advanced — an infinite spin in `sub_4A8560`).
- **Function-pointer closure** (`tools/seed_fptr_targets.py`): IDA leaves ~771 code
  addresses uncatalogued because they're reached only through pointers (handler
  tables). Added them as entries so indirect calls resolve. Out-of-range
  misdisassembly artifacts get empty stubs (`recomp_stubs.c`).
- **DirectInput COM shim** (in `ddraw_shim.c`): fake `IDirectInput` (8 methods) +
  `IDirectInputDevice` (18), so `DirectInputCreateA` succeeds (was `E_FAIL` →
  "Error loading Dinput.dll" fatal).

**Result — the recompiled POD runs its entire startup:** CD check → `Ubi.ini`
config → CPU detect → **DirectDraw** init (`SetDisplayMode 640×480×16`,
`CreateSurface` primary+backbuffer flip chain, 640×480 window) → **DirectInput**
(create device) → into **WinMain's `MsgWaitForMultipleObjects` game loop**.

## Threading ✅ — the game runs its real multi-threaded logic

POD is **multi-threaded**, so the runtime now runs each guest thread on a real host
thread (`src/runtime/thread_shim.c`):

- **Per-thread CPU state.** The register/FPU/segment model is now `RECOMP_TLS`
  (thread-local); the simulated memory (the arena) stays shared, like a real
  process. `ebp` and the flag temps were already per-call (C locals).
- **`CreateThread`** allocates a guest stack + TIB, then `_beginthreadex`s a launcher
  that sets that thread's `esp`/FPU/`fs_base`, pushes the thread argument, and calls
  the lifted thread proc through the dispatch table. `CREATE_SUSPENDED`/`ResumeThread`
  honoured.
- **Real synchronization.** Events, mutexes, semaphores, and `MsgWait`/`WaitFor*`
  map to real Win32 objects (a guest-handle table); critical sections map guest
  `CRITICAL_SECTION` pointers to host ones. The shim heap is lock-guarded.

**Result:** the recompiled POD runs its genuine multi-threaded logic — the main
thread drives **WinMain's `MsgWaitForMultipleObjects` message loop** while worker
threads synchronize on events/semaphores. In one run it executed **~30 million
function calls**, loaded its menu/track/car data (`circuits.bin`, `voitures.bin`,
`DIC_MNU/GAM/ERR.BMD`), and performed a **second DirectDraw init** (entering the
game proper) — all without crashing.

## Current frontier 🚧 — first rendered frame

The recompiled POD runs its **full engine** — up to **~34 million function calls** per
run: WinMain's message loop, worker threads, DirectDraw + DirectInput init, and the
sound/joystick subsystems (`waveOutGetDevCapsA`, `joyGetDevCapsA`). But no frame
reaches the screen yet, for two reasons found while chasing it:

1. **The intro/menu render is video, not DirectDraw.** POD draws its intro through
   the **`DrawDibDraw` (MSVFW32) video path** decoding an **Indeo AVI** — both stubbed
   — not by writing into a DirectDraw surface. Across a full run, no surface ever
   holds non-zero pixels (the pump's `auto_present_dd` finds nothing to show), and
   `Lock`/`Flip`/`Blt` are never called. So a visible frame needs the video subsystem
   to actually decode + present (Indeo is a whole codec), or the game advanced past
   the intro to a DirectDraw-rendered menu.
2. **Intermittent error-exit.** Some runs reach the menu loop and run indefinitely;
   others hit an error path — `PostQuitMessage` or a direct `ExitProcess(-1)` via the
   Watcom CRT exit chain — in subsystem init. The multi-threaded timing is
   non-deterministic, and several subsystems (WINMM audio, the video codec) are still
   stubs whose return values can trip the game's own error checks.

**Infrastructure in place for when rendering engages:** `host_present16` (16bpp 565
StretchDIBits), a DirectDraw-surface registry + `auto_present_dd` in the message pump
(presents whichever surface has content), and per-thread call-ring + null-call/quit
diagnostics.

**Next steps:** harden the WINMM/joystick/video stubs so the game stops error-exiting;
make the intro video either present real frames or report "finished" so POD advances
to its DirectDraw menu; then `auto_present_dd` should show the first frame.

**Minor:** `ICALL: unresolved 0x004D23A3` (init-table funcptr; indirect target).

## Bring-up instrumentation

- `-DRECOMP_TRACE` (on in the Debug build) records every function entry in a ring
  buffer; `recomp_dump_trace()` prints the last 256 on a fault or watchdog trip.
- A watchdog in `recomp_lookup_import` dumps the trace and exits after 64 repeated
  unresolved indirect calls (a wild-pointer loop), instead of spinning forever.
- A VEH handler reports access violations with register + indirect-call context.
