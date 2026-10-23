# Architecture

## The game

POD Gold (Ubi Soft, 1997) shipped one executable per graphics chip. This
project recompiles `PODMMX.EXE`, the MMX software renderer: no 3D card, the
world is drawn by integer and MMX code into a DirectDraw surface.
[BINARIES.md](BINARIES.md) has the whole zoo.

| Property | Value |
|---|---|
| Compiler | Watcom C/C++, linker 2.18 (MFC on Watcom for the launcher layer) |
| Image | PE32 at `0x00400000`, code segment `AUTO` `0x00401000`-`0x004DEA00`, entry `0x004C9FD8` |
| Timestamp | `0x34538B18` (1997-10-26); `run_lift.py` refuses any other build |
| Imports | 231 functions: DirectDraw, DirectInput, WINMM (CD audio, waveOut, joystick), MSVFW32 (intro stills), GDI/USER/KERNEL |
| Protection | none |

Three things about the binary shape everything below:

- **Watcom leaves `VirtualSize` 0** on every section and puts the size in
  `SizeOfRawData`. Every tool that sizes a section has to know (image_loader
  for `.bss`, native32 since pcrecomp#128).
- **Watcom keeps switch tables in the code segment**, in front of the function
  that uses them, and shares function tails freely. A disassembler can take a
  table for code, and a lifted body can fall off its end into a neighbour.
- **The 2D blit library rewrites its own code** (27 immediates in
  `0x004D81xx`-`0x004D85xx`), and calls `VirtualProtect` on its code section
  to be allowed to.

## The pipeline

```
POD2_0.BIN / ISO / CD ── tools\extract_disc.py ── game\  (+ game\UbiSoft\Ubi.ini, from Setup)
                                                    │
                     game\PODMMX.EXE ── tools\catalog.py ── work\functions.json
                                        (pcrecomp disasm32 + seeds)      │
                                                                run_lift.py
                                                    (pcrecomp generate.py + lift32)
                                                                         │
                                                        src\recomp\gen\*.c  (1.9 M lines)
                                                                         │
           src\runtime\*.c + pcrecomp runtime\native32 ── build.cmd / netlab ── pod.exe
```

Everything under `game\`, `work\` and `src\recomp\gen\` is derived from the
user's disc and is never committed (REPO_RULES section 3).

**The catalog** (`tools/catalog.py`) is pcrecomp's disasm32, run twice: the
second time with every code address the program itself uses as a `push` or
`mov` immediate as a seed. That is how the window procedure (`0x004A0DC8`),
which a switch table decoded as code had swallowed, gets its own entry
([bringup.md](bringup.md)). Earlier runs' unresolved targets can be fed back
through `work/seeds.json` (pcrecomp `seed_from_log.py`). Against IDA's catalog
of the same exe, disasm32 misses 517 functions, none of which anything in the
binary refers to: dead library code IDA's linear sweep found.

**The lift** (`run_lift.py`) is pcrecomp's `generate.py` loop: each function's
extent is walked by its branches, a branch that leaves a body becomes an entry
of its own (Watcom's shared tails), and the bodies go to `lift32` with precise
carries and the self-modified immediates (`find_patch_sites`), which it reads
from memory instead of the file. Two `PATCHES` make CPUID answer as a Pentium
MMX. Only chunks whose text changed are rewritten, so a lifter fix recompiles
what it touched.

## At run time

`pod.exe` is a 32-bit process linked at `0x60000000`, so the guest image maps
at its own base and a guest pointer is a host pointer. pcrecomp's native32
binds the guest's imports to real Windows, runs lifted code under one machine
lock across POD's threads, and turns every Windows call into the guest (window
procedure, thread start) into a call to the lift. The guest's code pages stay
non-executable so that this can never silently run the original instead.

What the host adds, in `src/runtime/`:

| File | Owns |
|---|---|
| `host.c` | The command line, the guest's module and command line, the CD (a drive letter, the `POD2_0` label, a silent `cdaudio`), `Ubi.ini` via `GetWindowsDirectoryA`, the 60 Hz message heartbeat, scripted keys, the watchdog and fault report |
| `ddraw.c` | DirectDraw (v1 and v2) over memory surfaces: no mode change, no exclusive screen. A Flip is a present: to the window, to ffmpeg (`--record`), or nowhere (`--headless`) |
| `present.c` | The window's picture when there is a screen: scaling, aspect, a Direct3D 11 presenter |
| `dinput.c` | Windows' DirectInput with three slots replaced: Acquire, GetDeviceState (scripted keys) and SetEventNotification (to wake the game when one changes) |
| `oracle.c` | `--original`: the shipping machine code under the same host, for comparing |

Why each piece exists, with the symptom that called for it, is in
[bringup.md](bringup.md). The ones a reader would not guess:

- **Why a heartbeat.** WinMain waits in `MsgWaitForMultipleObjects(INFINITE)`
  and runs a frame only when woken. Nobody touches a headless window.
- **Why the game folder is the Windows folder.** Only for `Ubi.ini`: it is
  the one thing POD reads from there, and answering that way keeps the real
  `C:\Windows` untouched.
- **Why `cdaudio` is the host's.** The music is the disc's audio track; the
  game folder has none, and a failed open is a modal error dialog.
- **Why surfaces have slack.** The blitter writes 2 bytes before the surface
  it locked.

### --original

`--original` maps the same `PODMMX.EXE` with its code executable and runs it
natively under the same shims, so one command line drives two machines. It
needs the CPU hook too (`hook_cpu_detect`), or it quits at start on a modern
CPU. Comparing the two is how the relift showed that a stall was the host's
and not the lift's: both stalled the same way.

## Testing

`tools/conformance.py` (REPO_RULES section 9) runs the host headless for 60
seconds with no input and scores ten milestones from its output and its
recording, plus lift health from the generated tree; `conformance.json` is the
baseline, and a drop in either fails. netlab's recipe runs it as QA; a test
VM without `game\` reports SKIP.
