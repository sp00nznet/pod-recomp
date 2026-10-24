# Videos: the Indeo 4.1 codec, lifted

POD's videos are Indeo 4.1 (`IV41`), which Windows 10 and 11 no longer decode:

| File | Size | Length | What it is |
|---|---|---|---|
| `INTROPOD.AVI`, `INTROCD.AVI` | 320x208, 25 fps | 3 min 59 s | the intro: Io, the pods, the city |
| `LOGO.AVI` | 640x480 | 2 s | Ubi Soft's logo |
| `CREDITS.AVI` | 280x180 | 4 s | the background of the credits |
| `FINPOD.AVI`, `FINPOD2.AVI` | 320x240 | 57 s, 5 s | the ending |

Without a decoder POD shows `NOINDEO.BBM` ("You must install Indeo to play the video")
in place of the intro, and its attract loop faults on the first video after a race. So
this project lifts the codec the disc came with, as Twisted Metal's recomp lifted the
one on its disc (twistedmetal-recomp `docs/movies.md`, the method this follows).

## The codec

`INSTALL\IR41_32.DLL` on the POD disc: Intel Indeo Video Interactive **R4.11.15.91**,
PE timestamp `0x326D51E5` (1996), base `0x10000000`, 390 KB of code. It is the
**MMX build**: 2,522 MMX instructions and a CPUID check, where Twisted Metal's
4.11.15.62 has none. lift32 handles MMX (POD's own renderer is MMX), and the codec
lifts with no changes to the toolkit. It exports `DriverProc`, the Video for Windows
installable-driver entry.

## How it runs

- `tools/catalog.py` catalogs it with disasm32 (`work/indeo/functions.json`, 2,608
  functions), and `run_lift.py` lifts it with the game into one dispatch table (11,865
  functions, 0 errors). `run_lift.py` refuses any other build of the DLL.
- The host holds `0x10000000` from the first line of `main` (the runtime's guest stacks
  would otherwise land across it), then maps the DLL there, binds its imports like the
  game's and runs its lifted `DllMain` (`src/runtime/indeo.c`).
- **POD's own player is unchanged.** It uses AVIFile and DrawDib
  (`AVIStreamGetFrameOpen`, `AVIStreamGetFrame`, `DrawDibDraw`), so the codec is
  installed with Video for Windows as a *function* driver for `vidc`/`iv41`
  (`ICInstall(..., ICINSTALL_FUNCTION)`, this process only). Every message VfW sends it
  runs the lifted `DriverProc`. (Twisted Metal plays through MCI `avivideo` and drives
  `DriverProc` from its own AVI reader instead.)
- Windows' VfW sends a function driver `ICM_GETINFO` with driver id 0 and no `DRV_OPEN`
  first; the codec reads its instance through that 0 and faults. `indeo.c` loads the
  codec once (`DRV_LOAD`, `DRV_ENABLE`) and opens an instance for any message that comes
  without one.
- Before it plays a video, POD checks that an Indeo driver is installed:
  `[Drivers32] VIDC.IV41` in `SYSTEM.INI`, then `IR41_32.DLL` in the system folder.
  The host answers both from the disc's copy (`host.c`).
- The frames reach the screen through `DrawDibDraw` on the game window's DC. The host
  hands POD its own screen DC for that window, so each `DrawDibDraw` is a present: in the
  window, in `--record`, in `--shot`. That also brought the intro stills (`MMX.BBM`,
  `NOINDEO.BBM`) into recordings.
- The video is paced by its sound (`waveOutGetPosition`), which plays through Windows.

## How it is checked

`tools/verify_indeo.py` decodes every AVI on the disc three ways: the lift
(`pod.exe --decode-test AVI OUT`), the DLL's original machine code in the same host
(`--codec-native`), and FFmpeg's own Indeo 4 decoder as an outside reference.

```
CREDITS.AVI    131 frames 280x180   vs original: identical   vs ffmpeg: correlation 0.9984
FINPOD.AVI    1413 frames 320x240   vs original: identical   vs ffmpeg: correlation 0.9975
FINPOD2.AVI    156 frames 320x240   vs original: identical   vs ffmpeg: correlation 0.9914
INTROCD.AVI   5981 frames 320x208   vs original: identical   vs ffmpeg: correlation 0.9813
INTROPOD.AVI  5981 frames 320x208   vs original: identical   vs ffmpeg: correlation 0.9813
LOGO.AVI        60 frames 640x480   vs original: identical   vs ffmpeg: correlation 0.9992
verify_indeo: ok
```

The lift gives the same bytes as Intel's code for all 13,722 frames. FFmpeg converts YUV
to RGB its own way, so it is compared as a correlation: the same picture, not the same
bytes. None of FFmpeg's code is in this project; it runs as a separate program.

The conformance run (`tools/conformance.py`) watches the intro play for 20 seconds
through the lifted codec. Its attract run uses `--no-codec`, because the intro is four
minutes long and skipping it by key is broken (below).

## Not working yet

**Skipping a video with a key leaves the menu dead.** Esc, Space or Enter during the
intro stops it and the main menu appears, but POD never flips again: it keeps calling
`DrawDibDraw` about 2,000 times a second from its video code, and the menu takes no
keys. The original machine code does exactly the same under this host
(`--original` with `--codec-native`), so the difference is in the host or in Windows,
not in the lift. When the intro plays to its end, the menu, the attract races and the
keys all work. Leads: the video loop (`sub_0046094C`) paces itself by the sound clock
and polls the keyboard (`sub_0049CC24`) and joystick (`sub_004801F4`); on the skip
path `waveOutClose` fails with buffers still queued. ROADMAP.md.

Esc on the main menu, with no video involved, freezes it the same way; Space or Enter
there does not.

## Host flags for this

| Flag | Effect |
|---|---|
| `--no-codec` | do not install the codec: POD shows "You must install Indeo" and skips its videos |
| `--decode-test AVI OUT` | decode every frame of an AVI through VfW to raw 24-bit RGB and exit |
| `--codec-native` | the DLL's original machine code instead of its lift (the reference) |
| `POD_INDEO_TRACE=1` | log every message VfW sends the codec, and its answer |
| `POD_MCI_TRACE=1` | log every MCI command POD sends |
