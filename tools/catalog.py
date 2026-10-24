#!/usr/bin/env python3
"""Function catalogs for PODMMX.EXE and its video codec: pcrecomp disasm32, no IDA.

    py -3 tools/catalog.py      # -> work/functions.json, work/indeo/functions.json

Two modules are lifted (run_lift.py): the game, and IR41_32.DLL, Intel's
Indeo 4.1 codec, which the disc ships in INSTALL\\ for its videos and which
Windows no longer has (docs/movies.md). Each gets disasm32's recursive
descent, prologues and data scan, plus two kinds of extra seed:

* **Code addresses the program takes as immediates.** Watcom keeps switch
  tables in the code segment, right in front of the function that uses them.
  The data scan can decode one as code, and the made-up body then covers the
  real function after it: the table at 0x004A0DA4 swallowed the window
  procedure at 0x004A0DC8, which WinMain hands to RegisterClassA as `mov
  [esp+..], 0x4A0DC8`. Windows then called an address that was not lifted. So
  every `push imm32` / `mov ..., imm32` whose value is in the code segment,
  read from the bodies disasm32 found, is seeded and it runs again. The
  codec's hand-written assembly stores continuation addresses the same way.
* **work/seeds.json**, addresses runs found (indirect calls into code the
  scans missed), kept by pcrecomp's seed_from_log.py:

      py -3 ../tools/tools/disasm/seed_from_log.py scratch/run.log game/PODMMX.EXE --seeds work/seeds.json

Both are derived from the game's disc, so they live in work/ and are never
committed (REPO_RULES section 3).
"""
import json
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DEFAULT = os.path.join(_HERE, 'work', 'pcrecomp')
if not os.path.isdir(_DEFAULT):
    _DEFAULT = os.path.join(_HERE, '..', 'tools')
TOOLS = os.path.join(os.environ.get('PCRECOMP', _DEFAULT), 'tools')
WORK = os.path.join(_HERE, 'work')
RUN_SEEDS = os.path.join(WORK, 'seeds.json')
# (image, catalog it gets)
MODULES = [
    (os.path.join(_HERE, 'game', 'PODMMX.EXE'), os.path.join(WORK, 'functions.json')),
    (os.path.join(_HERE, 'game', 'INSTALL', 'IR41_32.DLL'), os.path.join(WORK, 'indeo', 'functions.json')),
]


def disasm32(exe, out, seeds):
    cmd = [sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'), exe, '-o', out]
    if seeds:
        path = out.replace('functions.json', 'seeds_all.json')
        json.dump(sorted(seeds), open(path, 'w'))
        cmd += ['--seed-functions', path]
    subprocess.run(cmd, check=True)
    return json.load(open(out))


def immediate_seeds(exe, cat):
    """Code-section addresses used as push/mov immediates in the cataloged bodies."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_IMM
    cs, ce = cat['code_start'], cat['code_end']
    sys.path.insert(0, os.path.join(TOOLS, 'pe'))
    from pe_analyze import analyze_pe
    info = analyze_pe(exe)
    text = next(s for s in info.sections if info.image_base + s.virtual_address == cs)
    code = open(exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = set()
    for f in cat['functions']:
        if f.get('entry_kind') == 'alias':
            continue
        lo, hi = f['address'], min(f['end'], ce)
        for i in md.disasm(code[lo - cs:hi - cs], lo):
            if i.mnemonic in ('push', 'mov'):
                out.update(op.imm & 0xFFFFFFFF for op in i.operands
                           if op.type == X86_OP_IMM and cs <= op.imm & 0xFFFFFFFF < ce)
    return out


def catalog(exe, out, run_seeds):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cat = disasm32(exe, out, run_seeds)
    imm = immediate_seeds(exe, cat) - {f['address'] for f in cat['functions']}
    name = os.path.basename(exe)
    print('[catalog] %s: %d code addresses taken as immediates are not entries%s'
          % (name, len(imm), (': ' + ', '.join('0x%08X' % a for a in sorted(imm))) if len(imm) <= 12 else ''))
    if imm:
        cat = disasm32(exe, out, run_seeds | imm)
        missing = imm - {f['address'] for f in cat['functions']}
        if missing:
            print('[catalog] %s: still not entries: %s' % (name, ', '.join('0x%08X' % a for a in sorted(missing))))
    print('[catalog] %s: %d functions -> %s' % (name, len(cat['functions']), out))


def main():
    seeds = set()
    if os.path.exists(RUN_SEEDS):
        seeds = {e['address'] if isinstance(e, dict) else int(e) for e in json.load(open(RUN_SEEDS))}
    for exe, out in MODULES:
        base = 0x10000000 if exe.lower().endswith('.dll') else 0x00400000
        catalog(exe, out, {a for a in seeds if base <= a < base + 0x01000000})


if __name__ == '__main__':
    main()
