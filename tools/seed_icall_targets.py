#!/usr/bin/env python3
"""
Close the indirect-call set from a RUN, not from analysis.

A function whose only reference is a pointer stored in a table is invisible to
IDA's call-graph and to the literal scan in seed_call_targets.py -- POD has two
of them in its menu renderer (0x004931DC and 0x0049324C, each sitting one byte
past the end of the function IDA did catalogue). At runtime they show up as

    ICALL: unresolved VA 0x004931DC from 0x0049FB4C

and the call does nothing, so whatever they draw is simply missing.

This reads a run's stderr, adds every unresolved target that lands in the code
section to analysis/ida_funcs.json with bounds [ea, next-known-start), and
re-lifts. Repeat run -> seed -> run until no unresolved targets remain: each
pass can uncover pointers only reachable once the previous ones work.

Usage:
    pod_recomp.exe analysis/PODMMX.EXE --run 2> run.log
    py -3.11 tools/seed_icall_targets.py run.log
"""
import os, re, sys, json, subprocess

_here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT = os.path.join(_here, "analysis", "ida_funcs.json")
CODE_LO, CODE_HI = 0x00401000, 0x004DEA00      # AUTO segment of PODMMX.EXE

# "ICALL: unresolved VA 0x004931DC from 0x0049FB4C" (ITAIL too)
PAT = re.compile(r"I(?:CALL|TAIL): unresolved VA 0x([0-9A-Fa-f]{8})")


def main():
    log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_here, "run.log")
    text = open(log, encoding="utf-8", errors="replace").read()
    targets = {int(m, 16) for m in PAT.findall(text)}

    cat = json.load(open(CAT, encoding="utf-8"))
    funcs = cat["functions"]
    starts = sorted(f["ea"] for f in funcs)
    known = set(starts)

    new = sorted(t for t in targets
                 if CODE_LO <= t < CODE_HI and t not in known)
    skipped = sorted(t for t in targets if not (CODE_LO <= t < CODE_HI))
    if skipped:
        print(f"[*] {len(skipped)} target(s) outside the code range, ignored: "
              + ", ".join(f"0x{t:08X}" for t in skipped[:8]))
    if not new:
        print("[*] no new indirect-call targets -- the set is closed")
        return 0

    for t in new:
        # End at the next catalogued start; the lifter stops at the first ret
        # inside that anyway, and a too-generous bound only costs dead code.
        nxt = next((s for s in starts if s > t), CODE_HI)
        funcs.append({"ea": t, "end": nxt, "name": f"sub_{t:X}"})
        print(f"[+] 0x{t:08X} -> 0x{nxt:08X}")

    funcs.sort(key=lambda f: f["ea"])
    json.dump(cat, open(CAT, "w", encoding="utf-8"))
    print(f"[*] added {len(new)}; re-lifting")
    return subprocess.call([sys.executable, os.path.join(_here, "run_lift.py")])


if __name__ == "__main__":
    sys.exit(main())
