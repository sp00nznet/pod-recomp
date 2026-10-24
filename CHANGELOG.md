# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/). No version has been tagged yet.

## [Unreleased]

### Added
- The videos play: the intro, the logo, the credits and the ending are Indeo 4.1, which
  Windows no longer decodes, so the codec on the disc (`INSTALL\IR41_32.DLL`, R4.11.15.91,
  the MMX build) is lifted with the game and installed with Video for Windows for this
  process. `tools/verify_indeo.py`: byte-identical to the original codec on all six videos,
  13,722 frames. The method is Twisted Metal's recomp's (docs/movies.md).
- `--decode-test`, `--codec-native` and `--no-codec`; `POD_INDEO_TRACE` and `POD_MCI_TRACE`.
- Stills and videos drawn with DrawDib reach the recording and the window.
- `tools/addr2name.py` and a linker map (`pod.map`) to name a host address in a report.
- The 3D world draws: the attract-mode race on Downtown renders the track, the cars and
  the HUD, and the game returns to its menu afterwards (docs/bringup.md, "The relift").
- `Setup.cmd` (`tools/setup.ps1`): one double-click from a downloaded folder to a
  shortcut, from the CD, `POD2_0.BIN` or an ISO.
- A headless mode that never touches a screen: `--headless`, `--record out.mp4`,
  `--frames`, `--shot`, and scripted keys with `--key NAME@MS[+HOLD]`.
- `tools/conformance.py` and `conformance.json`: thirteen milestones from two headless
  runs (20 s of the intro, 120 s of attract mode) plus lift health, failing on any drop.
  The attract events (menu, race, menu, second race) are found in order in the recording,
  not at fixed times, since their timing moves by seconds between builds. 13/13.
- `--original`: the shipping machine code under the same host, for comparison.
- `tools/catalog.py`: the function catalog from pcrecomp's disasm32, seeded with the code
  addresses the program uses as immediates.
- `docs/architecture.md`; `docs/bringup.md` (was `docs/PHASE3.md`) with the relift's log.
- A netlab recipe (`pod-recomp`), so builds and QA run on the farm.

### Changed
- Relifted on the current pcrecomp: disasm32 instead of IDA, `generate.py` + `lift32`
  instead of a private lift loop, and the `native32` runtime instead of hand-written
  import bridges, synthesized COM objects and a forked `recomp_types.h`. The host's
  DirectDraw, presenter and oracle come from Streets of SimCity's recomp.
- The toolkit is a sibling checkout (`..\pcrecomp`, or a worktree in `work\pcrecomp`),
  not a submodule. Needs pcrecomp#128.
- The extracted disc lives in `game\` (was `game_files\`); `tools/extract_disc.py` also
  reads plain ISOs and refuses names that would land outside the target folder.
- `LICENSE` is plain MIT; the scope note is in the README's License section.

### Fixed
- A game controller plugged into the machine drove headless runs: POD polls WINMM's
  joystick 0, and it walked the menus into a car select by itself. Headless, there is no
  joystick now.
- The game quit at start on any CPU newer than about 2004: CPUID answers as a Pentium MMX.
- Its intro thread and window procedure ran the original machine code instead of the
  lift (pcrecomp#128).
- A CD audio device that cannot open no longer stops the game at an error dialog.
- A video Windows cannot decode (Indeo 4.1: the intro and credits) no longer crashes the
  attract loop; it is refused at `AVIFileOpenA` and skipped.

### Removed
- The IDA path: `tools/ida_*.py`, the `seed_*` closure scripts, and the committed IDA
  catalog, names and analysis JSON (`config/`, `analysis/`). Facts derived from the
  binary are generated into the gitignored `work/` now (REPO_RULES section 3).
- `run_pipeline.py`, `gen_imports.py` and the old runtime in `src/runtime/`.
- `docs/PIPELINE.md`, replaced by `docs/architecture.md`.

## Before this changelog (August–September 2026)

The first bring-up, on an IDA catalog and its own runtime: the menu drew, took input and
started a race, and the race ran (laps, gears, standings) with the world not drawn. Its
log is the second half of docs/bringup.md.
