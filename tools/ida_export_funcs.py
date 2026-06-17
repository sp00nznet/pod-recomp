#!/usr/bin/env python3
"""
POD Gold — IDA-seeded function discovery (headless, idalib).

Exports config/functions.json (address, address_int, name, num_instructions)
for the recomp lifter, plus analysis/vtables.json and config/ida_names.txt.
IDA follows vtables, so virtual methods reached only via a vtable pointer are
captured up-front (a call-graph sweep would miss them).

Run with the Python idalib is installed into:
    py -3.11 tools/ida_export_funcs.py analysis/PODMMX.EXE
"""
import sys, os, json
import idapro  # MUST be first

import ida_auto, ida_funcs, ida_name, ida_bytes, ida_nalt, idautils, idc
import ida_xref, ida_segment, ida_ua


def code_range():
    for i in range(ida_segment.get_segm_qty()):
        s = ida_segment.getnseg(i)
        if s and (s.perm & ida_segment.SEGPERM_EXEC):
            return s.start_ea, s.end_ea
    return None, None


def recover_functions(cs, ce):
    """Make IDA functionise the orphan code reached only through pointers (handler
    tables / callbacks) so it computes TRUE [start, end) bounds, rather than us
    guessing [target, next-entry). Mis-guessed bounds were lifting garbage and
    causing spurious exits. Iterate add_func over pointer/call/jump targets until
    the set is closed."""
    total = 0
    for it in range(20):
        cands = set()
        # pointer/call/jump targets that are ALREADY disassembled code at an
        # instruction boundary but not yet inside any function. Non-destructive:
        # we never create_insn (that re-analyses and disrupts FLIRT) or touch data.
        for f in list(idautils.Functions()):
            for head in idautils.FuncItems(f):
                for xr in idautils.XrefsFrom(head, 0):
                    t = xr.to
                    if (cs <= t < ce and not ida_funcs.get_func(t)
                            and ida_bytes.is_code(ida_bytes.get_flags(t))
                            and ida_bytes.get_item_head(t) == t):
                        cands.add(t)
        added = 0
        for c in sorted(cands):
            if ida_funcs.add_func(c):
                added += 1
        ida_auto.auto_wait()
        total += added
        print(f"[recover] iter {it}: +{added} functions", flush=True)
        if added == 0:
            break
    return total


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "analysis/PODMMX.EXE"
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cfg = os.path.join(here, "config")
    ana = os.path.join(here, "analysis")
    os.makedirs(cfg, exist_ok=True)
    os.makedirs(ana, exist_ok=True)

    print(f"[*] opening {binary} (idalib auto-analysis) ...", flush=True)
    if idapro.open_database(binary, True) != 0:
        raise SystemExit("failed to open database")
    ida_auto.auto_wait()

    cs, ce = code_range()
    if cs is not None:
        n = recover_functions(cs, ce)
        print(f"[+] recovered {n} orphan functions (true IDA bounds)")

    funcs = []
    catalog = []   # ea/end bounds for the Phase-3 lifter (run_lift.py)
    named = []
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        if not f:
            continue
        n_ins = sum(1 for _ in idautils.FuncItems(ea))
        name = ida_funcs.get_func_name(ea) or f"sub_{ea:08X}"
        funcs.append({
            "address": f"0x{ea:08X}",
            "address_int": ea,
            "name": name,
            "num_instructions": n_ins,
        })
        catalog.append({"ea": ea, "end": f.end_ea, "name": name})
        # capture non-default (FLIRT / user) names
        if not name.startswith("sub_") and not name.startswith("nullsub"):
            named.append(f"0x{ea:08X}  {name}")

    funcs.sort(key=lambda e: e["address_int"])
    catalog.sort(key=lambda e: e["ea"])
    json.dump(funcs, open(os.path.join(cfg, "functions.json"), "w"), indent=1)
    json.dump({"functions": catalog}, open(os.path.join(ana, "ida_funcs.json"), "w"))
    open(os.path.join(cfg, "ida_names.txt"), "w").write("\n".join(named) + "\n")

    print(f"[+] functions: {len(funcs)}")
    print(f"[+] named (FLIRT/user): {len(named)}")
    print(f"[+] wrote config/functions.json, analysis/ida_funcs.json, config/ida_names.txt")

    idapro.close_database(False)


if __name__ == "__main__":
    main()
