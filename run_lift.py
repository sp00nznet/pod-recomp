#!/usr/bin/env python3
"""POD Gold lift driver: the game and its video codec -> src/recomp/gen/.

Two modules, one dispatch table (Twisted Metal's run_lift.py does the same):

* game/PODMMX.EXE, a Watcom C/C++ build (linker 2.18): its code section is
  named AUTO, not .text, and every section has VirtualSize 0, so sizes come
  from the raw data.
* game/INSTALL/IR41_32.DLL, Intel's Indeo 4.1 codec R4.11.15.91 (MMX), which
  the disc ships for its videos and which Windows no longer has. The host maps
  it at its own base, 0x10000000 (docs/movies.md).

The catalogs are pcrecomp's disasm32 (tools/catalog.py); no IDA.

    py -3 tools/catalog.py      # work/functions.json, work/indeo/functions.json
    py -3 run_lift.py

Same shape as Streets of SimCity's run_lift.py, plus what this game needs:
self-modified immediates (find_patch_sites) and the CPU patches. Only the
chunks whose text changed are rewritten, so a lifter fix recompiles what it
touched. The generated C is recompiler output and is never committed
(REPO_RULES section 3); docs/bringup.md has how each fix was found.
"""
import argparse
import filecmp
import json
import os
import shutil
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_DEFAULT = os.path.join(_HERE, 'work', 'pcrecomp')
if not os.path.isdir(_DEFAULT):
    _DEFAULT = os.path.join(_HERE, '..', 'tools')
_TOOLS = os.path.join(os.environ.get('PCRECOMP', _DEFAULT), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32           # noqa: E402
from capstone.x86 import X86_OP_MEM                         # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                   # noqa: E402
from pe_analyze import analyze_pe, build_iat_map            # noqa: E402

OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# (image, catalog, the PE timestamp of the one build supported, what it is).
# Another build would lift, but the patches are at this build's addresses.
MODULES = [
    (os.path.join(_HERE, 'game', 'PODMMX.EXE'), os.path.join(_HERE, 'work', 'functions.json'),
     0x34538B18, "POD Gold's PODMMX.EXE (1997-10-26), docs/BINARIES.md"),
    (os.path.join(_HERE, 'game', 'INSTALL', 'IR41_32.DLL'),
     os.path.join(_HERE, 'work', 'indeo', 'functions.json'),
     0x326D51E5, "Indeo 4.1 R4.11.15.91 from the POD Gold disc's INSTALL\\, docs/movies.md"),
]

# Lines of C added after one lifted instruction (apply_patches), for what the
# lift cannot do by itself.
#
# The CPU. sub_004A2DBC picks the renderer class from CPUID: Intel family 5 or
# 6, or an AMD K5/K6 by exact model, and anything else leaves 0xFF, which
# WinMain answers by quitting without a word. Every CPU since about 2004 is
# "anything else" (a Ryzen reports family 0xF), the original exe included. So
# CPUID answers as the CPU the MMX build was written for, a Pentium MMX
# (GenuineIntel, family 5 model 4), and the game's own code picks class 2.
PATCHES = {
    0x004A2E11: 'eax = 0x00000543u; /* run_lift.py PATCHES: CPUID 1 is a Pentium MMX */',
    0x004A2E1D: ('ebx = 0x756E6547u; edx = 0x49656E69u; ecx = 0x6C65746Eu; '
                 '/* run_lift.py PATCHES: CPUID 0 is GenuineIntel */'),
}


def find_patch_sites(md, code, cs, ce, fns):
    """VAs inside the code that the program itself writes a dword to.

    POD's font blitter patches its own immediates before it runs: `mov
    [0x4D8499], ecx` writes the glyph width into the `cmp ecx, 0x1234567` a
    few bytes below, `mov [0x4D84A1], edx` the surface pitch into an `add
    edi`. The file holds placeholders, so a lift that bakes them in sprays the
    frame. Every absolute dword store into the code range is found here, and
    lift32 sources those immediates from memory. A store through a register
    could go anywhere; if one ever matters it shows up as a wrong constant,
    the way this one did.
    """
    sites = set()
    for addr, end in fns:
        end = min(end, ce)
        if end <= addr:
            continue
        for insn in md.disasm(code[addr - cs:end - cs], addr):
            if insn.mnemonic != 'mov' or not insn.operands:
                continue
            d = insn.operands[0]
            if (d.type == X86_OP_MEM and d.size == 4 and d.mem.base == 0
                    and d.mem.index == 0 and cs <= d.mem.disp < ce):
                sites.add(d.mem.disp & 0xFFFFFFFF)
    return sites


def apply_patches(body, patches):
    """Add each patch's C after the line that lifts its instruction."""
    for va, c in patches.items():
        k = body.find('/* 0x%08X:' % va)
        if k != -1:
            e = body.index('\n', k)
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of a module this is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


class Out:
    """Chunks of lifted C, shared by the modules: one dispatch table."""
    def __init__(self, out, split):
        self.out, self.split, self.chunk, self.files, self.entries = out, split, [], 0, []

    def add(self, body, addr, name):
        self.chunk.append((body, addr, name))
        self.entries.append((addr, name))
        if len(self.chunk) >= self.split:
            self.flush()

    def flush(self):
        if self.chunk:
            write_chunk(self.out, self.files, self.chunk)
            self.files += 1
            self.chunk = []


def lift(path, catalog, md, out):
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d catalog=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat), len(byaddr)))

    text = next(s for s in info.sections if cs == info.image_base + s.virtual_address)
    code = open(path, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    patch_sites = find_patch_sites(md, code, cs, ce, [(a, f['end']) for a, f in byaddr.items()])
    print('[*]   %d self-modified immediates%s' % (len(patch_sites), (': ' + ', '.join(
        '0x%08X' % s for s in sorted(patch_sites))) if len(patch_sites) <= 30 else ''))
    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    # precise carry: adc/sbb take their carry from the flag state that set it.
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), patch_sites=patch_sites,
                    precise_carry=True, precise_sbb=True)
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)

    stats = {'lifted': 0, 'errors': 0, 'no_terminator': 0}
    # A direct branch that leaves a body to an address nothing cataloged is a
    # tail call or a jump into a neighbor's shared tail (Watcom shares them
    # freely). Either way the target has to be dispatchable, so each round's
    # outside targets become entries of the next.
    todo, added = sorted(byaddr), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind = set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind)
            outside |= {t for t in behind if t not in reached}
            stats['no_terminator'] += not clean
            try:
                lo = min(reached) if reached else addr   # a chunk can sit below the entry
                insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
                body = apply_patches(body, PATCHES)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
                stats['errors'] += 1
            out.add(body, addr, name)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t}
        added += len(todo)
    stats['lifted'] = len(byaddr)
    print('[*]   %d lifted, %d outside branch targets added, %d errors'
          % (len(byaddr), added, stats['errors']))
    return info.image_base + info.entry_point_rva, stats


def sync(new, out):
    """Move new's files over out's where their text differs, drop out's chunks
    the new lift no longer has. Returns how many files changed."""
    os.makedirs(out, exist_ok=True)
    names = set(os.listdir(new))
    changed = 0
    for fn in names:
        src, dst = os.path.join(new, fn), os.path.join(out, fn)
        if not (os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False)):
            os.replace(src, dst)
            changed += 1
    for fn in set(os.listdir(out)) - names:
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(out, fn))
            changed += 1
    shutil.rmtree(new)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--split', type=int, default=200, help='functions per .c file')
    args = ap.parse_args()
    for path, catalog, stamp, what in MODULES:
        if not os.path.exists(catalog):
            sys.exit('no catalog at %s -- run tools/catalog.py first (README, Step by step)' % catalog)
        if exe_stamp(path) != stamp:
            sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                     'expected 0x%08X: %s.' % (path, exe_stamp(path), stamp, what))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    final, args.out = args.out, args.out + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out)

    t0 = time.time()
    out = Out(args.out, args.split)
    entries_va, stats = [], {'lifted': 0, 'errors': 0, 'no_terminator': 0}
    for path, catalog, _, _ in MODULES:
        entry, s = lift(path, catalog, md, out)
        entries_va.append(entry)
        for k in stats:
            stats[k] += s[k]
    out.flush()
    stats['files'] = out.files

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* PODMMX.EXE + IR41_32.DLL - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n')
        for a, n in out.entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* PODMMX.EXE + IR41_32.DLL - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(out.entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t pod_entry_va = 0x%08Xu;\n'
                'const uint32_t pod_ir41_entry_va = 0x%08Xu;   /* the codec\'s DllMain */\n'
                % (len(out.entries), entries_va[0], entries_va[1]))

    # A patch that found no instruction to follow is a fix that silently isn't there.
    gen = ''.join(open(os.path.join(args.out, fn), encoding='utf-8', errors='replace').read()
                  for fn in os.listdir(args.out) if fn.endswith('.c'))
    for va, c in PATCHES.items():
        if c not in gen:
            sys.exit('patch at 0x%08X was not applied: is 0x%08X lifted?' % (va, va))

    stats['lines'] = gen.count('\n')
    stats['changed'] = sync(args.out, final)
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   errors %(errors)d   no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files (%d changed), %.1fs' % (format(stats['lines'], ','), stats['files'],
                                                               stats['changed'], time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
