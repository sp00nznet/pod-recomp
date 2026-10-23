#!/usr/bin/env python3
"""Function catalog for PODMMX.EXE (POD Gold): pcrecomp disasm32, no IDA.

    py -3 tools/catalog.py            # -> work/functions.json

The entry point is the seed; the rest is disasm32's recursive descent,
prologues and data scan, plus two kinds of extra seed:

* **Code addresses the program takes as immediates.** Watcom keeps switch
  tables in the code segment, right in front of the function that uses them.
  The data scan can decode one as code, and the made-up body then covers the
  real function after it: the table at 0x004A0DA4 swallowed the window
  procedure at 0x004A0DC8, which WinMain hands to RegisterClassA as `mov
  [esp+..], 0x4A0DC8`. Windows then called an address that was not lifted. So
  every `push imm32` / `mov ..., imm32` whose value is in the code segment,
  read from the bodies disasm32 found, is seeded and it runs again.
* **work/seeds.json**, addresses runs found (indirect calls into code the
  scans missed), kept by pcrecomp's seed_from_log.py:

      py -3 ../tools/tools/disasm/seed_from_log.py scratch/run.log game/PODMMX.EXE --seeds work/seeds.json

Both are derived from the game binary, so they live in work/ and are never
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
EXE = os.path.join(_HERE, 'game', 'PODMMX.EXE')
WORK = os.path.join(_HERE, 'work')
OUT = os.path.join(WORK, 'functions.json')
RUN_SEEDS = os.path.join(WORK, 'seeds.json')
ALL_SEEDS = os.path.join(WORK, 'seeds_all.json')


def disasm32(seeds):
    cmd = [sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'), EXE, '-o', OUT]
    if seeds:
        json.dump(sorted(seeds), open(ALL_SEEDS, 'w'))
        cmd += ['--seed-functions', ALL_SEEDS]
    subprocess.run(cmd, check=True)
    return json.load(open(OUT))


def immediate_seeds(cat):
    """Code-segment addresses used as push/mov immediates in the catalogued bodies."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_IMM
    cs, ce = cat['code_start'], cat['code_end']
    sys.path.insert(0, os.path.join(TOOLS, 'pe'))
    from pe_analyze import analyze_pe
    info = analyze_pe(EXE)
    text = next(s for s in info.sections if info.image_base + s.virtual_address == cs)
    code = open(EXE, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
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


def main():
    os.makedirs(WORK, exist_ok=True)
    seeds = set()
    if os.path.exists(RUN_SEEDS):
        seeds = {e['address'] if isinstance(e, dict) else int(e) for e in json.load(open(RUN_SEEDS))}
    cat = disasm32(seeds)
    known = {f['address'] for f in cat['functions']}
    imm = immediate_seeds(cat) - known
    print('[catalog] %d code addresses taken as immediates are not entries: %s'
          % (len(imm), ', '.join('0x%08X' % a for a in sorted(imm))))
    if imm:
        cat = disasm32(seeds | imm)
        missing = imm - {f['address'] for f in cat['functions']}
        if missing:
            print('[catalog] still not entries: %s' % ', '.join('0x%08X' % a for a in sorted(missing)))
    print('[catalog] %d functions -> %s' % (len(cat['functions']), OUT))


if __name__ == '__main__':
    main()
