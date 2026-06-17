#!/usr/bin/env python3
"""
Close the control-flow target set so every direct call AND every direct
(cross-function) jump resolves to a real dispatch entry.

The lifter emits, for a target it can't reach as a local C label:
  * direct call  -> RECOMP_CALL(sub_XXXXXXXX)
  * direct jump  -> L_XXXXXXXX: RECOMP_ITAIL(0xXXXXXXXXu); return;
When the target address isn't a catalogued function (e.g. it lands inside a
*neighbouring* IDA function -- a switch case, a jump-table arm, or a tail jump),
the call/tail-call misses the dispatch table and control flow is lost.

This scans the generated code for both forms, adds any target that isn't already
an entry to analysis/ida_funcs.json with bounds [ea, next-known-start), re-lifts,
and repeats until the set is closed.

Usage: py -3.11 tools/seed_call_targets.py
"""
import os, re, json, subprocess, sys

_here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(_here, 'src', 'recomp', 'gen')
CAT = os.path.join(_here, 'analysis', 'ida_funcs.json')

REF_CALL  = re.compile(r'RECOMP_CALL\(sub_([0-9A-Fa-f]{8})\)')
REF_ITAIL = re.compile(r'RECOMP_ITAIL\(0x([0-9A-Fa-f]{8})u\)')

def referenced_targets():
    refs = set()
    for fn in os.listdir(GEN):
        if not (fn.startswith('recomp_') and fn.endswith('.c')):
            continue
        txt = open(os.path.join(GEN, fn), encoding='utf-8', errors='replace').read()
        refs |= {int(h, 16) for h in REF_CALL.findall(txt)}
        refs |= {int(h, 16) for h in REF_ITAIL.findall(txt)}
    return refs

def relift():
    subprocess.run([sys.executable, os.path.join(_here, 'run_lift.py')],
                   cwd=_here, check=True)

def write_oob_stubs(cs, ce):
    """A few function-pointer candidates are actually misaligned code; lifting them
    yields `call` targets OUTSIDE the code range (garbage). Those can never be real
    entries, so emit empty defs for them — the build links and the (dead) path is a
    no-op rather than an unresolved symbol."""
    oob = {t for t in referenced_targets() if not (cs <= t < ce)}
    path = os.path.join(GEN, 'recomp_stubs.c')
    with open(path, 'w') as f:
        f.write('/* POD Gold - out-of-range (misaligned) call targets - auto-generated */\n')
        f.write('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n\n')
        for t in sorted(oob):
            f.write(f'void sub_{t:08X}(void) {{ }}  /* OOB target (dead) */\n')
    print(f'[+] wrote {len(oob)} out-of-range stub(s) to recomp_stubs.c')

def main():
    cat = json.load(open(CAT, encoding='utf-8'))
    funcs = cat['functions']
    cs = min(f['ea'] for f in funcs)
    ce = max(f['end'] for f in funcs)
    for it in range(1, 25):
        entries = {f['ea'] for f in funcs}
        missing = {t for t in referenced_targets()
                   if cs <= t < ce and t not in entries}
        if not missing:
            print(f'[+] closed: every call/jump target is a dispatch entry ({len(funcs)} funcs)')
            write_oob_stubs(cs, ce)
            return
        starts = sorted(entries | missing)
        nxt = {a: (starts[i + 1] if i + 1 < len(starts) else ce)
               for i, a in enumerate(starts)}
        for m in sorted(missing):
            funcs.append({'ea': m, 'end': min(nxt[m], ce), 'name': f'sub_{m:08X}'})
        funcs.sort(key=lambda f: f['ea'])
        json.dump(cat, open(CAT, 'w'))
        print(f'[*] iter {it}: added {len(missing)} targets -> re-lifting ...', flush=True)
        relift()
    print('[!] still open after 24 iterations:',
          sorted(f'0x{m:08X}' for m in missing)[:40])

if __name__ == '__main__':
    main()
