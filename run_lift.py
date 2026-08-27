#!/usr/bin/env python3
"""
POD Gold Static Recompilation - Phase 3 lift driver.

Adapts the proven pcrecomp linear-lift pipeline (cf. fury3/run_lift.py) to POD:
seeds function entries AND exact end addresses from IDA's catalog
(analysis/ida_funcs.json), so each function is disassembled within its true
[start, end) bounds. Uses the GLOBAL x87 FPU stack + intra-function switch
dispatch (runnable variant), unlike the Phase-1 run_pipeline.py.

PODMMX.EXE is a Watcom C/C++ build: the code segment is named "AUTO" (not
".text") with VirtualSize == 0, so the code range is sized from the raw size.

Usage: py -3.11 run_lift.py [analysis/PODMMX.EXE] [src/recomp/gen]
"""
import sys, os, json, time, re

_here = os.path.dirname(os.path.abspath(__file__))
_pc = os.path.join(_here, 'tools', 'pcrecomp', 'tools')
sys.path.insert(0, os.path.join(_pc, 'pe'))
sys.path.insert(0, os.path.join(_pc, 'lift'))

from pe_analyze import analyze_pe, build_iat_map
from lift32 import Lifter
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM

COND_JUMPS = {'je','jne','jz','jnz','ja','jae','jb','jbe','jg','jge','jl','jle',
              'js','jns','jo','jno','jp','jnp','jcxz','jecxz'}

# CRT functions we replace with hand-written host shims (src/runtime/shims_impl.c)
# instead of lifting. Their gnarly state-machine internals (e.g. _output's format
# parser) are well-understood library code -- shimming to the host CRT is more
# robust than debugging the lift. run_lift emits an extern decl; the body is
# hand-written and the dispatch table still points at it.
# Functions replaced with hand-written host shims (src/runtime/shims_impl.c)
# instead of lifting. run_lift emits an `extern` decl; the body is hand-written
# and both direct calls and the dispatch table resolve to it.
HOST_SHIM = {
    0x004A2DBC,  # CPU detection (CPUID availability via EFLAGS ID-bit toggle).
                 # Our model carries no EFLAGS, so the probe fails and the game
                 # bails at WinMain's renderer switch; shim reports Pentium-MMX.
}
FPU_CMP = {'EQ':'==','NE':'!=','B':'<','BE':'<=','A':'>','AE':'>=',
           'L':'<','LE':'<=','G':'>','GE':'>='}


class LinearInstruction:
    __slots__ = ['address','size','mnemonic','op_str','bytes','operands',
                 'is_call','is_ret','is_cond_jump','is_uncond_jump','is_jump']
    def __init__(self, insn):
        self.address=insn.address; self.size=insn.size
        self.mnemonic=insn.mnemonic; self.op_str=insn.op_str
        self.bytes=bytes(insn.bytes)
        self.operands=list(insn.operands) if insn.operands else []
        self.is_call=insn.mnemonic=='call'
        self.is_ret=insn.mnemonic in ('ret','retn','retf')
        self.is_cond_jump=insn.mnemonic in COND_JUMPS
        self.is_uncond_jump=insn.mnemonic=='jmp'
        self.is_jump=self.is_cond_jump or self.is_uncond_jump
    @property
    def end_address(self): return self.address+self.size
    def get_branch_target(self):
        if self.operands and self.operands[0].type==X86_OP_IMM:
            return self.operands[0].imm & 0xFFFFFFFF
        return None


def linear_disassemble_function(md, code_data, code_start, func_start, func_end):
    offset=func_start-code_start; size=func_end-func_start
    if offset<0 or offset+size>len(code_data): return [], set()
    raw=code_data[offset:offset+size]; instructions=[]; leaders={func_start}
    for insn in md.disasm(raw, func_start):
        li=LinearInstruction(insn); instructions.append(li)
        if li.is_jump:
            t=li.get_branch_target()
            if t and func_start<=t<func_end: leaders.add(t)
            leaders.add(li.end_address)
        if li.mnemonic=='int3': break
    return instructions, leaders


def lift_function_linear(lifter, name, instructions, leaders):
    va = name[4:] if name.startswith('sub_') else '0'
    # _st/_fp_top/_fpu_cw are GLOBAL (x87 stack is shared across calls) -- see
    # recomp_types.h. Only per-function scratch is declared here.
    lines=[f'void {name}(void) {{',
           '    int _fpu_cmp = 0;','    uint32_t _cf = 0;','    int _df = 1;',
           '    uint32_t _flag_a = 0, _flag_b = 0;',
           '    uint32_t _flag_k = FK_NONE;  /* which instruction wrote them */',
           '    uint32_t _itail_tgt = 0;',
           f'    RECOMP_ENTER(0x{va}u);','']
    lifter._flag_state=None
    # If the function has an indirect jump (jump table / switch), a computed
    # target can land on ANY instruction in it -- so label every instruction so
    # the local dispatch below can reach it. Otherwise just label block leaders.
    has_indirect = any(i.is_uncond_jump and i.get_branch_target() is None
                       for i in instructions)
    # We walk the function linearly, so the flag state at each instruction is
    # whatever the PREVIOUS one left -- correct on a fallthrough edge (a jcc does
    # not touch the flags) and wrong at any block a jump can reach, whose real
    # predecessors may each have set the flags with a different instruction.
    # POD hits this in its resource dispatcher: `cmp edx, 0x50000000 / jae X /
    # xor edx, edx / X: jbe ...`, where the jbe belongs to the cmp but linearly
    # follows the xor, so it lifted to CMP_BE(0, 0) -- always true. Clearing the
    # state at those targets makes the lifter emit recomp_cond(), which evaluates
    # the condition from _flag_k at runtime. (pcrecomp 28738f0)
    # ponytail: an indirect jump can land anywhere, so those functions reset at
    # every instruction -- correct, but it costs a recomp_cond() call at each of
    # their jccs. Narrow it to the switch's real arms if it ever shows up hot.
    jump_targets = {t for t in (i.get_branch_target() for i in instructions
                                if i.is_jump) if t is not None}
    for insn in instructions:
        if has_indirect or insn.address in leaders:
            lines.append(f'L_{insn.address:08X}:')
        if insn.address in jump_targets or has_indirect:
            lifter._flag_state = None
        for line in lifter.lift_instruction(insn): lines.append(f'    {line}')
    last = instructions[-1] if instructions else None
    if last and not last.is_ret:
        if last.is_uncond_jump or last.mnemonic == 'int3':
            lines.append('    return; /* end of function */')
        else:
            # The function falls off its end into the next one. Watcom shares
            # tails, and IDA carves each entry into its own function, so this is
            # routine: sub_004C5734 is `add eax,0x19220 / lea eax,[eax]` and then
            # runs on into sub_004C573C. A bare `return` here skips the ret that
            # would have popped the dummy return address, so the CALLER's esp
            # stays 4 low -- every local it addresses off esp from then on is one
            # slot out, and it surfaces far away as a struct read at the wrong
            # offset. Tail-dispatch instead: no return address is pushed, and the
            # real ret at the end of the chain pops the caller's.
            lines.append(f'    RECOMP_ITAIL(0x{last.end_address:08X}u); '
                         f'return; /* falls through into 0x{last.end_address:08X} */')

    # Intra-function indirect jumps (jump tables / switch statements) lift to
    # RECOMP_ITAIL(<runtime expr>), but the dispatch table only knows function
    # entries -- it can't resolve a label *inside* this function, so the switch
    # breaks. Route indirect ITAILs through a local label dispatch first: a
    # standard-C switch over every label in this function (goto), falling back to
    # the global RECOMP_ITAIL for genuine cross-function tail calls.
    indirect = [i for i,l in enumerate(lines)
                if 'RECOMP_ITAIL(' in l and 'RECOMP_ITAIL(0x' not in l]
    body = '\n'.join(lines)
    defined = sorted(set(re.findall(r'(?m)^\s*(L_[0-9A-Fa-f]{8})\s*:', body)))
    if indirect and defined:
        for i in indirect:
            m = re.search(r'RECOMP_ITAIL\((.+?)\);\s*return;', lines[i])
            if m:
                lines[i] = (f'    {{ _itail_tgt = (uint32_t)({m.group(1)}); '
                            f'goto _ljump; }}')
        lines.append('  _ljump:')
        lines.append('    switch (_itail_tgt) {')
        for lbl in defined:
            lines.append(f'      case 0x{int(lbl[2:],16):08X}u: goto {lbl};')
        lines.append('      default: RECOMP_ITAIL(_itail_tgt); return;')
        lines.append('    }')

    refed = set(re.findall(r'goto\s+(L_[0-9A-Fa-f]{8})', '\n'.join(lines)))
    for lbl in sorted(refed - set(defined)):
        lines.append(f'    {lbl}: RECOMP_ITAIL(0x{int(lbl[2:],16):08X}u); return;')
    lines.append('}')
    out = '\n'.join(lines)
    out = re.sub(r'CMP_(\w+)\(_fpu_cmp\)',
                 lambda m: f'((_fpu_cmp) {FPU_CMP.get(m.group(1), "==")} 0)', out)
    return out


def find_patch_sites(md, code_data, cs, ce, fns):
    """VAs inside the code that the program itself writes a dword to.

    Only absolute stores count -- `mov dword ptr [0x4D8499], ecx`. A store
    through a register could go anywhere and is not something a static lift can
    resolve; if one ever matters it will show up as a wrong constant, the same
    way this one did.
    """
    from capstone.x86 import X86_OP_MEM
    sites=set()
    for addr, end in fns:
        end=min(end, ce)
        if end<=addr: continue
        off=addr-cs
        for insn in md.disasm(code_data[off:off+(end-addr)], addr):
            if insn.mnemonic!='mov' or not insn.operands: continue
            d=insn.operands[0]
            if (d.type==X86_OP_MEM and d.size==4 and d.mem.base==0
                    and d.mem.index==0 and cs <= d.mem.disp < ce):
                sites.add(d.mem.disp & 0xFFFFFFFF)
    return sites


def write_chunk(out, idx, funcs):
    with open(os.path.join(out, f'recomp_{idx:04d}.c'),'w') as f:
        f.write('/* Fury3 Recompilation - Auto-generated - DO NOT EDIT */\n')
        f.write(f'/* File {idx}: {len(funcs)} functions */\n\n')
        f.write('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n')
        f.write('#include "recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n')
        for code,_,_ in funcs: f.write(code+'\n\n')


def main():
    exe = sys.argv[1] if len(sys.argv)>1 else os.path.join(_here,'analysis','PODMMX.EXE')
    out = sys.argv[2] if len(sys.argv)>2 else os.path.join(_here,'src','recomp','gen')
    split = int(sys.argv[3]) if len(sys.argv)>3 else 400
    os.makedirs(out, exist_ok=True)

    print('=== POD Gold Static Recompilation (Phase 3 lift) ===', flush=True)
    info=analyze_pe(exe); iat=build_iat_map(info)
    pe_data=open(exe,'rb').read()
    # Watcom names the code segment "AUTO" with VirtualSize == 0; size from raw.
    text=next(s for s in info.sections if s.is_code)
    vsize = text.virtual_size or text.raw_size
    cs = info.image_base + text.virtual_address
    ce = cs + vsize
    code_data=pe_data[text.raw_offset: text.raw_offset+text.raw_size]
    print(f'[*] base=0x{info.image_base:08X} code=0x{cs:08X}-0x{ce:08X} '
          f'seg={text.name!r} IAT={len(iat)}')

    # Seed entries + exact end bounds from IDA's catalog.
    cat=json.load(open(os.path.join(_here,'analysis','ida_funcs.json'),encoding='utf-8'))
    fns=[(f['ea'], f['end']) for f in cat['functions'] if cs <= f['ea'] < ce]
    fns.sort()
    print(f'[*] seeded {len(fns)} functions from IDA catalog (with exact bounds)')

    md=Cs(CS_ARCH_X86, CS_MODE_32); md.detail=True

    # Self-modifying code. POD's font blitter patches two of its own immediates
    # before it runs -- `mov [0x4D8499], ecx` writes the glyph width into the
    # `cmp ecx, 0x1234567` a few bytes below, and `mov [0x4D84A1], edx` writes
    # the surface pitch into `add edi, 0x12d687`. Those placeholder constants are
    # what is in the file, so lifted C ran the inner loop 19 million times per row
    # and advanced the destination by 1.2 MB, spraying the whole frame. Find every
    # store into the code range first; the lifter then sources those immediates
    # from memory instead of baking the placeholder in.
    patch_sites=find_patch_sites(md, code_data, cs, ce, fns)
    if patch_sites:
        print(f'[*] {len(patch_sites)} self-modified immediate(s): '
              + ', '.join(f'0x{s:08X}' for s in sorted(patch_sites)))

    lifter=Lifter(iat_map=iat, patch_sites=patch_sites)
    all_entries=[]; errors=0; idx=0; chunk=[]; t0=time.time()
    for addr, end in fns:
        end = min(end, ce)
        if end-addr < 1: continue
        name=f'sub_{addr:08X}'
        if addr in HOST_SHIM:
            # body provided by hand in shims_impl.c; just declare + register it
            chunk.append((f'/* {name}: host-shimmed in shims_impl.c */\n'
                          f'extern void {name}(void);\n', addr, name))
            all_entries.append((addr,name)); continue
        try:
            insns,leaders=linear_disassemble_function(md, code_data, cs, addr, end)
            if not insns:
                chunk.append((f'void {name}(void) {{ }} /* empty */\n',addr,name))
                all_entries.append((addr,name)); continue
            code=lift_function_linear(lifter, name, insns, leaders)
            chunk.append((code,addr,name)); all_entries.append((addr,name))
        except Exception as e:
            chunk.append((f'/* ERROR {name}: {e} */\nvoid {name}(void) {{}}\n',addr,name))
            all_entries.append((addr,name)); errors+=1
        if len(chunk)>=split:
            write_chunk(out, idx, chunk); idx+=1; chunk=[]
            print(f'[*]   {len(all_entries)}/{len(fns)} ({errors} err)', flush=True)
    if chunk: write_chunk(out, idx, chunk); idx+=1

    with open(os.path.join(out,'recomp_funcs.h'),'w') as f:
        f.write('/* Fury3 Recompilation - Auto-generated */\n#pragma once\n#include <stdint.h>\n\n')
        for a,n in all_entries: f.write(f'void {n}(void);  /* 0x{a:08X} */\n')
    with open(os.path.join(out,'recomp_dispatch.c'),'w') as f:
        f.write('/* Fury3 Recompilation - Auto-generated */\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write('const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a,n in sorted(all_entries): f.write(f'    {{ 0x{a:08X}u, {n} }},\n')
        f.write('};\n\n'+f'const uint32_t recomp_dispatch_count = {len(all_entries)};\n')

    total_lines=total_bytes=0
    for fn in os.listdir(out):
        fp=os.path.join(out,fn)
        if os.path.isfile(fp):
            total_bytes+=os.path.getsize(fp)
            total_lines+=sum(1 for _ in open(fp, encoding='utf-8', errors='replace'))
    json.dump({'functions':len(all_entries),'errors':errors,'files':idx,
               'lines':total_lines,'bytes':total_bytes},
              open(os.path.join(_here,'analysis','phase3_codegen.json'),'w'), indent=1)
    print('='*56)
    print(f'  functions: {len(all_entries):,}  errors: {errors}  files: {idx}')
    print(f'  lines of C: {total_lines:,}  size: {total_bytes/1048576:.1f} MB  time: {time.time()-t0:.1f}s')
    print('='*56)


if __name__=='__main__':
    main()
