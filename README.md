# POD Gold — Static Recompilation

```
   ____   ___   ____     ____  ___  _     ____
  |  _ \ / _ \ |  _ \   / ___|/ _ \| |   |  _ \
  | |_) | | | || | | | | |  _| | | | |   | | | |
  |  __/| |_| || |_| | | |_| | |_| | |___| |_| |
  |_|    \___/ |____/   \____|\___/|_____|____/
        " on Io, there is room for one "
```

A static recompilation of **POD Gold** (Ubi Soft, 1997) — lifting the original x86
machine code back into compilable C, on the way to a native build that runs on a modern
machine without an MMX Pentium and a 3dfx card under the desk.

> **POD was a gem.** A futuristic combat-racer with a genuinely nasty premise — the colony
> planet **Io** is dying from a fungus dug up out of its own crust, there's **one ship
> left**, and the survivors race for the only seat off-world. It was also one of the very
> first games to ship on the back of Intel's **MMX**, bundled with shiny new MMX PCs in
> 1996. This is the build that made MMX look like magic. Full story in
> [docs/HISTORY.md](docs/HISTORY.md). ✨

## Status

| Phase | Status | Result |
|-------|--------|--------|
| **Phase 0** — disc extract + IDA-seeded discovery | ✅ Complete | 951 disc files; **3,411 functions** found |
| **Phase 1** — x86 → C codegen | ✅ Complete | **3,405 functions, 0 lift errors**, 422K lines of C |
| **Phase 2** — compile | ✅ Complete | static lib, **0 compile errors** |
| **Phase 3** — runtime / boot | 🟢 **Menu on screen, race loading** | `pod_recomp.exe` draws POD's main menu ([docs/menu.png](docs/menu.png)), takes keyboard input, and on **ONE PLAYER** (or its own attract-mode timeout) loads the Downtown circuit for real — `DownTown.BL3`, the HUD skin, all seven cars' sound banks. Frontier: one face normal comes out with an x87 *integer-indefinite* component, which cascades into a zero camera basis and trips POD's own `("3D","NORM")` check. |
| Phase 3+ — gameplay | ⏳ Pending | first frame (intro is `DrawDibDraw`/Indeo video — stubbed; or reach the DirectDraw menu); harden WINMM/joystick/video stubs; the world of Io |

**It runs its game logic — multi-threaded.** The recompiled `PODMMX.EXE` runs its
whole real startup (Watcom CRT, `WinMain`, CD check, `Ubi.ini` config, CPUID renderer
select, **DirectDraw** + **DirectInput** through synthesized COM objects) and then its
**actual multi-threaded game logic** — the main thread driving WinMain's
`MsgWaitForMultipleObjects` loop while worker threads synchronize on events and
semaphores, **~34 million function calls** — through RDTSC timing calibration and the
FPU fast-`sqrt` vector math, **loading 14 real game assets** (`chrome.CH3`, `gamma.BV3`,
`gamma.MEG`, encrypted `.bin`/`.BMD` car & sound data) and re-initializing DirectDraw for
the game proper. Two broadly-impactful lifter fixes got it this deep: correct
`rep`/`repne` string-op (`memcpy`/`memset`) lifting, and correct FPU `st(n)` register
indexing (`X86_REG_ST0`).

**And it draws.** Four fixes got a clean menu on screen:

- Functions that **fall off their end** — Watcom shares tails and IDA carves each
  entry into its own function, so 565 of them had no `ret` and silently leaked 4
  bytes of the caller's stack — now tail-dispatch to the fall-through address.
- POD's **window procedure**, which had never once been called, is wired up:
  `RegisterClassA` captures it, `RegisterWindowMessageA` hands out real ids instead
  of `WM_NULL`, and `DispatchMessageA` calls into it.
- POD's 2D blit library is **self-modifying** — it writes the glyph width and the
  surface pitch into its own `cmp`/`add` immediates before running. Static C kept
  the author's `0x1234567` placeholders, so the font blitter sprayed the whole
  frame. The lifter now sources those 27 immediates from memory.
- **MMX**, on the MMX build, was entirely unimplemented in the lifter.

See [docs/PHASE3.md](docs/PHASE3.md).

![POD Gold's main menu, recompiled](docs/menu.png)

## The target

POD shipped **one executable per graphics chip** — there was no dependable Direct3D HAL
in 1997, so Ubi Soft hand-tuned a separate build for 3dfx Glide, PowerVR, S3 ViRGE, ATI
Rage, generic D3D… and an **MMX software rasterizer**. We recompile the iconic one:

| Property | Value |
|----------|-------|
| **Target** | **`PODMMX.EXE`** — the MMX software renderer (no 3D card; integer SIMD draws the world) |
| **Compiler** | **Watcom C/C++** (Linker 2.18; `AUTO`/`DGROUP`/`.bss` segments) — *not* MSVC |
| **Arch** | x86-32, PE32, image base `0x00400000`, Windows GUI |
| **DRM** | none — clean, analyzes directly |
| **HAL surface** | DirectDraw, DirectInput, WINMM, AVIFIL32/MSVFW32, GDI/USER/KERNEL |

The full renderer zoo and PE triage live in [docs/BINARIES.md](docs/BINARIES.md).

## Build it

You supply your own legally-obtained POD Gold disc (`POD2_0.BIN`/`.CUE`); **no game code
or data is committed here.**

```bash
# Phase 0 — pull the files off the disc, then find the functions with headless IDA
py -3.11 tools/extract_disc.py POD2_0.BIN game_files
py -3.11 tools/ida_export_funcs.py analysis/PODMMX.EXE     # -> config/functions.json

# Phase 1 — lift x86 -> C
py -3.11 run_pipeline.py                                   # -> src/recomp/gen/recomp_*.c

# Phase 2/3 — build the host executable (32-bit MSVC) and run it
py -3.11 tools/seed_call_targets.py                        # close the direct-call set
py -3.11 gen_imports.py                                    # -> src/runtime/imports_gen.c
cmake -B build -A Win32
cmake --build build --config Debug                         # -> build/Debug/pod_recomp.exe
build/Debug/pod_recomp.exe analysis/PODMMX.EXE --run       # boots the Watcom CRT
```

`src/recomp/gen/` is generated and **not committed** — it regenerates from `PODMMX.EXE`
plus the IDA function list.

## Layout

```
analysis/      target EXE + pe_analysis.json (gitignored: *.exe)
config/        functions.json (IDA discovery) + ida_names.txt
src/recomp/    recomp_types.h runtime header + gen/ (generated)
tools/         extract_disc.py, ida_export_funcs.py, + vendored pcrecomp toolkit
game_files/    extracted disc (gitignored — bring your own disc)
docs/          history & deep-dives  ── see below
CMakeLists.txt run_pipeline.py
```

## Docs

- [docs/HISTORY.md](docs/HISTORY.md) — POD, Io, MMX, and the race off a dying planet
- [docs/BINARIES.md](docs/BINARIES.md) — the one-EXE-per-GPU matrix + PE triage
- [docs/PIPELINE.md](docs/PIPELINE.md) — the three phases + the Watcom-specific fixes
- [docs/PHASE3.md](docs/PHASE3.md) — runtime bring-up: memory model, how far it boots, the frontier
- [docs/DISC.md](docs/DISC.md) — what's on the CD

## License

The code in this repository is released under the [MIT License](LICENSE).

That covers **this project's own source** — the runtime, the import bridges, the
DirectDraw and DirectInput shims, the threading and input layers, the lift drivers,
the tooling and the docs. Everything here was written for this project.

It does **not** cover POD itself. The game's executable, data and assets remain
Ubi Soft's and are not distributed here. Neither is the lifted C: that is
machine-translated from the original `PODMMX.EXE` and carries whatever rights the
executable does, so it is generated on your machine, from your copy, and never
checked in. The screenshots in `docs/` are the recompilation's own output against a
disc you supply — the artwork in them is the game's, shown only to record how far
this gets.

## Family

Part of a set of PC static-recomp projects sharing the `pcrecomp` toolkit — sister
projects include `recoil`, `crimsonskies`, and `mw3`. POD is the first **Watcom**-built
target in the family.

---

*POD and POD Gold are © Ubi Soft Entertainment. This project recompiles a binary you must
own; it ships no copyrighted game code or assets.*
