#!/usr/bin/env python3
"""
Close the indirect-call set: find code addresses that are taken as POINTERS
(function pointers in handler tables, callbacks, etc.) but that IDA never turned
into functions, add them to analysis/ida_funcs.json as entries, and re-lift +
re-close direct targets. Such addresses are reached only at runtime through a
pointer, so they're invisible to the literal-scan closure -- but if they aren't
dispatch entries, the indirect call lands as "unresolved".

Sources of pointers:
  - dr_O (offset) xrefs from instructions (e.g. `push offset X` / `mov reg, X`)
  - data dwords anywhere that point into the code section (pointer tables)

Usage: py -3.11 tools/ida_decompile... ; py -3.11 tools/seed_fptr_targets.py
"""
import sys, os, json, subprocess
import idapro
import ida_auto, ida_funcs, ida_bytes, ida_segment, idautils, ida_xref

_here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT = os.path.join(_here, "analysis", "ida_funcs.json")

def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_here, "analysis", "PODMMX.EXE")
    idapro.open_database(binary, True)
    ida_auto.auto_wait()

    cat = json.load(open(CAT, encoding="utf-8"))
    funcs = cat["functions"]
    starts = {f["ea"] for f in funcs}
    cs = min(starts); ce = max(f["end"] for f in funcs)

    def in_code(v):
        return cs <= v < ce

    cand = set()
    # 1) offset (dr_O) xrefs from instructions -> code
    for f in idautils.Functions():
        for head in idautils.FuncItems(f):
            for xr in idautils.XrefsFrom(head, 0):
                if xr.type == ida_xref.dr_O and in_code(xr.to) and xr.to not in starts:
                    if ida_bytes.is_code(ida_bytes.get_flags(xr.to)):
                        cand.add(xr.to)
    # 2) data dwords that point into code, but ONLY where IDA marked the dword as
    # an offset (has a dr_O xref to code). Raw "any dword that looks like a code
    # pointer" is too loose -- false positives land mid-instruction and corrupt the
    # disassembly of the function they split.
    for s in range(ida_segment.get_segm_qty()):
        seg = ida_segment.getnseg(s)
        if not seg:
            continue
        ea = seg.start_ea
        while ea < seg.end_ea:
            if not ida_bytes.is_code(ida_bytes.get_flags(ea)):
                for xr in idautils.XrefsFrom(ea, 0):
                    if xr.type == ida_xref.dr_O and in_code(xr.to) and xr.to not in starts \
                       and ida_bytes.is_code(ida_bytes.get_flags(xr.to)):
                        cand.add(xr.to)
            ea += 4

    # Keep only candidates that sit at an instruction boundary (item head), so we
    # never split a real function mid-instruction.
    cand = sorted(c for c in cand if in_code(c) and c not in starts
                  and ida_bytes.is_code(ida_bytes.get_flags(c))
                  and c == ida_bytes.get_item_head(c))
    print(f"[*] {len(cand)} new function-pointer targets (not catalogued)")
    if not cand:
        idapro.close_database(False); return

    allstarts = sorted(starts | set(cand))
    nxt = {a: (allstarts[i + 1] if i + 1 < len(allstarts) else ce) for i, a in enumerate(allstarts)}
    for c in cand:
        funcs.append({"ea": c, "end": min(nxt[c], ce), "name": f"sub_{c:08X}"})
    funcs.sort(key=lambda f: f["ea"])
    json.dump(cat, open(CAT, "w"))
    idapro.close_database(False)
    print(f"[*] catalog now {len(funcs)} entries; re-lifting + closing direct targets ...")
    subprocess.run([sys.executable, os.path.join(_here, "run_lift.py")], cwd=_here, check=True)
    subprocess.run([sys.executable, os.path.join(_here, "tools", "seed_call_targets.py")], cwd=_here, check=True)

if __name__ == "__main__":
    main()
