# Roadmap

## Next

- **A race of your own.** Drive One Player → a track → a car → the race with `--key`, and
  add it to the conformance run as milestones.
- **CD music.** The host's `cdaudio` is silent. The music is track 2 of the user's
  `POD2_0.BIN` (raw 2352-byte audio sectors after the data track): play it through
  `waveOut` when the game asks MCI to play, and loop it as the CD did.
- **Skipping a video by key.** Esc, Space or Enter during the intro stops it, but the menu
  after it never flips and takes no keys, in the original code under this host too; Esc on
  the main menu does the same. Until then a player has to let the intro play out, and the
  conformance run cannot drive a race of its own. Leads are in docs/movies.md.
- **Windowed play checked on a real screen**, through offstage: focus, the real keyboard,
  the presenter's scaling, and the mouse cursor POD clips.
- **A clean-machine run of Setup.cmd** on a netlab test VM with a disc image.

## Later

- **Upstream the catalog seeds.** disasm32 can take a Watcom switch table in the code
  segment for a function; seeding code addresses used as immediates (`tools/catalog.py`)
  is general and belongs in pcrecomp.
- **Upstream the self-modified immediate scan** (`run_lift.py find_patch_sites`): lift32
  has the mechanism, every self-modifying title needs the same scan.
- Joysticks and wheels through DirectInput (POD reads them; nothing has been tried).
- Multiplayer: POD's split screen, then its network game.
- The other renderers on the disc (Direct3D, 3dfx) are separate executables; not planned.

## Out of scope

- Distributing anything of the game: the executable, its data, the lifted C.
- Changing the game itself, beyond what it needs to run on a current PC.
