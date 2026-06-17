#!/usr/bin/env python3
"""Recursively extract the ISO9660 tree from a Mode1/2352 BIN into game_files/."""
import struct, os, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "POD2_0.BIN"
OUT = sys.argv[2] if len(sys.argv) > 2 else "game_files"
SEC, OFF = 2352, 16  # Mode1/2352: 16-byte sync header, then 2048 data

fh = open(BIN, "rb")

def read_sector(lba):
    fh.seek(lba * SEC + OFF)
    return fh.read(2048)

def read_run(lba, length):
    n = (length + 2047) // 2048
    return b"".join(read_sector(lba + i) for i in range(n))[:length]

def list_dir(lba, length):
    buf = read_run(lba, length)
    out, i = [], 0
    while i < len(buf):
        rl = buf[i]
        if rl == 0:
            i = (i // 2048 + 1) * 2048
            if i >= len(buf):
                break
            continue
        rec = buf[i:i + rl]
        el = struct.unpack("<I", rec[2:6])[0]
        ln = struct.unpack("<I", rec[10:14])[0]
        flags = rec[25]
        nl = rec[32]
        name = rec[33:33 + nl].decode("latin1", "replace")
        out.append((name, flags, el, ln))
        i += rl
    return out

def clean(name):
    return name.split(";")[0]

count = [0]
total = [0]

def walk(lba, length, path):
    for name, flags, el, ln in list_dir(lba, length):
        if name in ("\x00", "\x01"):  # . and ..
            continue
        cn = clean(name)
        dst = os.path.join(path, cn)
        if flags & 2:
            os.makedirs(dst, exist_ok=True)
            walk(el, ln, dst)
        else:
            data = read_run(el, ln)
            with open(dst, "wb") as o:
                o.write(data)
            count[0] += 1
            total[0] += len(data)

pvd = read_sector(16)
root = pvd[156:156 + 34]
root_lba = struct.unpack("<I", root[2:6])[0]
root_len = struct.unpack("<I", root[10:14])[0]
os.makedirs(OUT, exist_ok=True)
walk(root_lba, root_len, OUT)
print(f"[+] extracted {count[0]} files, {total[0]/1048576:.1f} MB -> {OUT}/")
