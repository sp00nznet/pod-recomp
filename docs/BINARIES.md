# The binaries on the POD Gold disc

POD predates a dependable Direct3D HAL, so Ubi Soft shipped **one hand-tuned executable
per graphics chip**. The disc root carries the whole zoo:

## The renderer matrix

| Executable | Renderer / target | Notes |
|------------|-------------------|-------|
| **`PODMMX.EXE`** | **MMX software rasterizer** | **Our recomp target.** No 3D card; integer SIMD draws everything. The historically iconic build. |
| `PODX3DFX.EXE` | 3dfx Voodoo (Glide) | The "premium" look in 1997. |
| `PODFXM64.EXE` | 3dfx (Glide, 64-bit-bus / Voodoo variant) | |
| `PODPVRX.EXE` | PowerVR (PCX1/PCX2) | The one build that runs **800×600** (per disc readme). |
| `PODS3DX.EXE` | S3 ViRGE (S3D) | |
| `PODATIX.EXE` | ATI Rage (ATI3D) | |
| `PODD3DX.EXE` | generic Direct3D | The forward-looking path. |
| `PODMAX64.EXE` | Matrox / "MAX" variant | paired with the `MAXI64/` dir. |
| `WINPOD.EXE` | DirectDraw "Windows" build | Same import surface as `PODMMX`. |
| `WPOD3DFX` / `WPODATI` / `WPODD3D` / `WPODPVR` / `WPODS3D` | windowed per-renderer builds | the `W`-prefixed family. |
| `MOSAICO.EXE` | **CD autorun / setup front-end** | What `AUTORUN.INF` launches; MSVC + InstallShield-style (registry imports, 2.8 MB overlay). *Not* a game build. |
| `PODTHEME.EXE` | "Theme POD" desktop theme installer | cursors / icons / sounds / wallpaper. |
| `UBISETUP.EXE`, `SETUP.EXE` | installers | |

## PE triage (the game builds)

All the `POD*`/`W*POD*` game executables share a fingerprint:

| Property | Value |
|----------|-------|
| Format | PE32, machine **i386**, image base `0x00400000`, subsystem **Windows GUI** |
| **Compiler** | **Watcom C/C++** — Linker stamp **2.18**; segments `AUTO` (code), `DGROUP`, `.bss`, `.idata`, `.reloc`, `.rsrc` |
| Build date | late **October 1997** (e.g. `PODMMX` 1997-10-26) |
| DRM | **none** — clean, analyzes directly |

### `PODMMX.EXE` specifically

| Property | Value |
|----------|-------|
| Size | 1,317,888 bytes |
| Entry point | `0x004C9FD8` |
| Code segment | `AUTO` — `0x00401000`–`0x004DEA00` (~890 KB) |
| Imports | **231** functions from **9** DLLs |
| Functions | **3,725** starts in the disasm32 catalog (`tools/catalog.py`); **9,257** dispatchable entries once Watcom's shared tails and switch arms are counted |

**Import surface:**

| DLL | Role |
|-----|------|
| `DDRAW.dll` | framebuffer (the MMX rasterizer blits a surface) |
| `DINPUT.dll` | keyboard / joystick / wheel |
| `WINMM.dll` | CD-audio music, MIDI, joystick, multimedia timers |
| `AVIFIL32.dll` + `MSVFW32.dll` | Indeo-coded intro / cutscene video |
| `GDI32` / `USER32` / `KERNEL32` | windowing, CRT, OS |
| `VERSION.dll` | version stamp checks |

No Glide, no PowerVR, no DirectSound — `PODMMX` is the cleanest possible target.

## The Watcom + MFC wrinkle

IDA's FLIRT pass named ~524 functions, and the names are revealing — e.g.
`W?OnQuery3dControls$:CWnd$n(uil)l`. That `W?…$:…$n(…)` shape is **Watcom name
mangling**, and `CWnd` / `CSyncObject` are **MFC** classes. So POD's UI/launcher layer
is **MFC compiled with the Watcom toolchain** — an unusual combination, and the reason
the project's MSVC-oriented FLIRT signatures only partially match. None of it matters to the
lift, which needs no names: see [architecture.md](architecture.md).
