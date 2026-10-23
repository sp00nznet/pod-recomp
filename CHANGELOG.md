# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/). No version has been tagged yet.

## [Unreleased]

### Added
- The 3D world draws: the attract-mode race on Downtown renders the track, the cars and
  the HUD, and the game returns to its menu afterwards (docs/bringup.md, "The relift").
- `Setup.cmd` (`tools/setup.ps1`): one double-click from a downloaded folder to a
  shortcut, from the CD, `POD2_0.BIN` or an ISO.
- A headless mode that never touches a screen: `--headless`, `--record out.mp4`,
  `--frames`, `--shot`, and scripted keys with `--key NAME@MS[+HOLD]`.
- `tools/conformance.py` and `conformance.json`: eleven milestones from a 120-second
  headless run plus lift health, failing on any drop. 11/11.
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
