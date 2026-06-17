# The recompilation pipeline

POD Gold is recompiled with the shared **`pcrecomp`** toolkit (vendored under
`tools/pcrecomp/`), the same machinery used on the sister projects `recoil`,
`crimsonskies`, and `mw3`. The flow mirrors `recoil` (a BIN/CUE PC-CD game): three
phases get us from a raw disc to compiled C.

```
POD2_0.BIN  ──Phase 0──▶  PODMMX.EXE + functions.json
                            │
                Phase 1 (run_pipeline.py)
                            ▼
              src/recomp/gen/recomp_*.c   (x86 → C)
                            │
                Phase 2 (CMake, MSVC x86)
                            ▼
                   pod_recomp.lib
```

## Phase 0 — extract & discover ✅

1. **Disc extraction.** `POD2_0.BIN` is a **Mode1/2352** image (16-byte sync header +
   2048 data + 288 ECC per sector; track 2 is the redbook CD-audio music).
   `tools/extract_disc.py` walks the ISO9660 tree directly and pulls all **951 files
   (~450 MB)** into `game_files/`. The game builds sit loose on the disc root — no
   InstallShield cab to crack (unlike `recoil`).
2. **Triage.** `tools/pcrecomp/tools/pe/pe_analyze.py` identifies the Watcom build and
   the clean (un-DRM'd) import surface → `analysis/pe_analysis.json`. See
   [BINARIES.md](BINARIES.md).
3. **IDA-seeded discovery.** `tools/ida_export_funcs.py` runs headless IDA Pro 9.1
   (idalib) and exports **3,411 functions** → `config/functions.json`, plus 524
   FLIRT/MFC names → `config/ida_names.txt`. IDA follows **vtables**, so virtual methods
   reached only through a vtable pointer are captured up-front (a call-graph sweep misses
   those).

## Phase 1 — x86 → C ✅

`run_pipeline.py` seeds function entries from `config/functions.json` and lifts each one
via the shared capstone-based `lift32` lifter, emitting one `void sub_XXXXXXXX(void)` per
function across chunked translation units, plus a dispatch table.

| Result | Value |
|--------|-------|
| Functions lifted | **3,405** |
| Lift errors | **0** |
| Output | **422,632** lines of C (~21.9 MB), 7 `recomp_*.c` files |

```
py -3.11 run_pipeline.py            # -> src/recomp/gen/recomp_*.c
```

The generated `src/recomp/gen/` is **not committed** — it regenerates from `PODMMX.EXE`
plus the function list.

### POD-specific pipeline changes

The base pipeline targets MSVC binaries; two Watcom traits needed handling (all changes
are in this project's vendored copies, so the shared toolkit is untouched):

- **`AUTO` code segment with `VirtualSize == 0`.** Watcom names its code segment `AUTO`
  (not `.text`) and writes a zero VirtualSize into the section header. `run_pipeline.py`
  now selects the executable section by characteristics and sizes the code range from the
  **raw** size.
- **Stale capstone segment-register IDs.** The lifter's `fs`/`gs` mapping used hardcoded
  numeric IDs that don't match this capstone build (`fs`/`gs` arrived as 32/33), so
  `pop fs` / `pop gs` lifted to a non-l-value `0`. Fixed `lift32.py` to map segment
  registers via capstone's **symbolic** `X86_REG_*` constants. (`PODMMX`'s Watcom CRT/SEH
  prologues touch `fs`/`gs`; MSVC builds hit this path less.)
- **`FS_BASE` / `GS_BASE`.** The lifter emits `MEM32(FS_BASE + off)` for `fs:[…]`
  accesses (SEH chain, TLS). `recomp_types.h` now defines these (flat `0` for Phase 2;
  Phase 3 must point `FS_BASE` at a real per-thread TIB).
- **`_flag_a` / `_flag_b` locals.** Declared in the lifted-function prologue alongside the
  other CPU-state locals.

## Phase 2 — compile ✅

`CMakeLists.txt` compiles the generated TUs into a 32-bit static library — validating that
**every lifted function compiles**, before any runtime exists.

```
cmake -B build -A Win32
cmake --build build --config Debug
```

| Result | Value |
|--------|-------|
| Translation units | 8 objects |
| Compile errors | **0** |
| Output | `build/Debug/pod_recomp.lib` (~24.9 MB) |

**Why Watcom's calling convention doesn't bite here:** Phases 1–2 lift every function as
`void name(void)` operating on global CPU-state registers, then compile them in isolation.
Argument passing (`__watcall` puts args in `EAX/EDX/EBX/ECX` rather than on the stack) only
matters once functions actually *call* each other through a runtime — i.e. **Phase 3**. So
the Watcom convention is a Phase 3 concern, not a Phase 1/2 blocker.

## Phase 3 — runtime / boot 🟢 in progress

The lifted code now links into a host executable that maps `PODMMX.EXE` into a
simulated address space and **executes from the Watcom CRT entry point**. Full
detail (memory model, how far it boots, the current frontier) is in
[PHASE3.md](PHASE3.md). In brief:

- `pod_recomp.exe` = runtime + **231 import bridges** + **3,427 lifted functions**.
- One **arena** holds the whole guest space (`g_mem_base` maps every guest VA);
  TIB behind `FS_BASE` for SEH/TLS; 1 MB stack; 128 MB shim heap.
- Boots **~200 functions deep into CRT init** (TLS, exception filter, command
  line, the init/fini table walk) before a jump-table-across-function-boundary
  bug derails control flow.
- The `__watcall` register convention needs no special handling here — faithful
  register/stack emulation reproduces it; it only mattered conceptually.

### Pending

- **Jump-table analysis** (the current gate): recover `jmp [table+idx*4]` targets
  so intra-function switch dispatch resolves within the right function → finish
  CRT init → reach `WinMain`.
- Real HAL bodies for the Phase-0 surface: **DirectDraw** (→ a modern blit
  target), **DirectInput** (→ SDL/raw input), **WINMM** (CD-audio music, timers,
  joystick), optionally **AVIFIL32/MSVFW32** (Indeo video). Currently stubbed;
  the COM creators (`DirectDrawCreate`/`DirectInputCreateA`) return `E_FAIL` so
  device init fails gracefully rather than dereferencing a null interface.
- Asset loading from `game_files/DATA/` (tracks, cars, records).
