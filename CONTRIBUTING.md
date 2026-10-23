# Contributing

This is a recompilation project, which changes what a useful contribution looks
like. Three rules matter more than the rest.

## No game files, ever

Nothing from the POD Gold disc goes in the repository, in any form: no
executable, DLL, track, car, sound, video or data file, no disc image, and
nothing regenerated from one: no lifted source, disassembly listing, function
catalog, symbol list, reconstructed header, or test file with a lifted function
in it.

The tool ships; the disc does not. Everyone brings their own copy and points
the tools at it. `.gitignore` covers `game/`, `work/` and `src/recomp/gen/`; if
something slips past it, that is a bug worth reporting on its own.

Screenshots and recordings of the game running are fine, and are the point of
the README.

## Where your code comes from

Contributions have to be your own work or under a licence compatible with MIT.
The live risk is a fix ported from a GPL or LGPL project, which would relicense
it by accident and is very hard to untangle later. The ones most likely to be
open next to this are **Wine** and **ReactOS** (LGPL/GPL, for any Win32 or
DirectX behaviour), **DOSBox** (GPL), and **dgVoodoo** (closed source). Reading
them to understand a behaviour is fine; copying code from them is not. If
something in your PR came from somewhere, say where.

## Claims are measured, not assumed

The oracle is `--original`: it runs the shipping `PODMMX.EXE` machine code
under the same host and the same scripted keys as the recompiled one. A change
that claims to fix a behaviour should say what the original does in the same
run, and `tools/conformance.py` should still pass. A change to the lifter or
the runtime belongs in [pcrecomp](https://github.com/sp00nznet/pcrecomp), with
a test there.

## The usual

- Imperative commit subjects; the body explains *why* when it is not obvious.
- Community pull requests are merged with a merge commit, never squashed or
  rebased, so your commits stay yours. Anything the maintainer adds goes in
  separate commits on top.
- Comments explain the reasoning, not the syntax: a comment recording how
  something was found out is worth more than one restating the line under it.
- AI-assisted contributions are welcome, provided a human understood and
  verified the change: ran it, and can say what it does.
