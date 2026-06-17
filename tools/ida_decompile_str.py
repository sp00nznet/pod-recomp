#!/usr/bin/env python3
"""Find the function(s) referencing a given string and decompile them.
Usage: py -3.11 tools/ida_decompile_str.py analysis/PODMMX.EXE "pod2_02.exe"
"""
import sys
import idapro
import ida_auto, ida_funcs, ida_hexrays, idautils, ida_bytes, ida_nalt, idc, ida_xref

def main():
    binary = sys.argv[1]
    needle = sys.argv[2]
    idapro.open_database(binary, True)
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()

    # find the string
    targets = []
    for s in idautils.Strings():
        if needle.lower() in str(s).lower():
            targets.append((s.ea, str(s)))
    print(f"[*] {len(targets)} string match(es) for {needle!r}")
    seen = set()
    for ea, text in targets:
        print(f"\n=== string @0x{ea:08X}: {text!r} ===")
        for xr in idautils.XrefsTo(ea):
            f = ida_funcs.get_func(xr.frm)
            if not f or f.start_ea in seen:
                continue
            seen.add(f.start_ea)
            name = ida_funcs.get_func_name(f.start_ea)
            print(f"\n----- referenced by sub_{f.start_ea:08X} ({name}) @0x{xr.frm:08X} -----")
            try:
                cf = ida_hexrays.decompile(f.start_ea)
                print(str(cf))
            except Exception as e:
                print(f"(decompile failed: {e})")
    idapro.close_database(False)

if __name__ == "__main__":
    main()
