#!/usr/bin/env python3
"""Check the lifted Indeo 4.1 codec against the original, on every video on the disc.

Each AVI in game/DATA/BINARY/VIDEO is decoded three ways (docs/movies.md):

* lifted:   pod.exe --decode-test AVI OUT                 (the lift)
* original: pod.exe --codec-native --decode-test AVI OUT  (the DLL's own machine code, same host)
* ffmpeg:   its own Indeo 4 decoder, a separate program; none of its code is here

The lift must give the same bytes as the original for every frame. ffmpeg
converts YUV to RGB its own way, so it is compared as a correlation: the
same picture, not the same bytes.

    py -3 tools/verify_indeo.py [--exe build-farm/pod.exe] [NAME ...]

The decoded frames (hundreds of MB for the intro) go to work/indeo/verify/ and
are deleted after each video. Without game/ it prints SKIP and exits 0.
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIDEO = os.path.join(ROOT, 'game', 'DATA', 'BINARY', 'VIDEO')
TMP = os.path.join(ROOT, 'work', 'indeo', 'verify')


def host():
    have = [p for p in (os.path.join(ROOT, d, 'pod.exe') for d in ('build', 'build-farm')) if os.path.exists(p)]
    return max(have, key=os.path.getmtime) if have else None


def decode(exe, avi, out, native):
    cmd = [exe] + (['--codec-native'] if native else []) + ['--decode-test', avi, out]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace')
    if r.returncode:
        raise RuntimeError('%s: %s' % (' '.join(cmd[1:]), (r.stdout + r.stderr).strip()[-300:]))


def same(a, b, size):
    """Frames that differ between two raw decodes (None if identical)."""
    diff = []
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        i = 0
        while True:
            x, y = fa.read(size), fb.read(size)
            if not x and not y:
                return diff
            if x != y:
                diff.append(i)
            i += 1


def correlation(ours, avi, w, h):
    """Pearson correlation of our RGB with ffmpeg's, over every 10th frame."""
    row = (w * 3 + 3) & ~3
    p = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-i', avi, '-f', 'rawvideo', '-pix_fmt', 'bgr24',
                          '-vf', 'vflip', '-'], stdout=subprocess.PIPE)
    n = sx = sy = sxx = syy = sxy = 0
    with open(ours, 'rb') as f:
        k = 0
        while True:
            a, b = f.read(row * h), p.stdout.read(w * 3 * h)
            if len(a) < row * h or len(b) < w * 3 * h:
                break
            if k % 10 == 0:
                for y in range(0, h, 4):
                    xa, xb = a[y * row:y * row + w * 3:7], b[y * w * 3:(y + 1) * w * 3:7]
                    for u, v in zip(xa, xb):
                        n += 1; sx += u; sy += v; sxx += u * u; syy += v * v; sxy += u * v
            k += 1
    p.kill()
    if not n:
        return 0.0
    cov, vx, vy = sxy - sx * sy / n, sxx - sx * sx / n, syy - sy * sy / n
    return cov / (vx * vy) ** 0.5 if vx and vy else 0.0


def size_of(avi):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=width,height', '-of', 'csv=p=0', avi], capture_output=True, text=True).stdout
    w, h = (int(x) for x in out.strip().split(','))
    return w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exe', help='default: the newest of build/ and build-farm/')
    ap.add_argument('names', nargs='*', help='only these videos (LOGO, CREDITS, ...)')
    args = ap.parse_args()
    exe = os.path.abspath(args.exe) if args.exe else host()
    avis = sorted(glob.glob(os.path.join(VIDEO, '*.AVI')))
    if not exe or not avis:
        print('verify_indeo: SKIP -- needs game/ (your copy) and a build (README, Step by step)')
        return 0
    if args.names:
        avis = [a for a in avis if os.path.splitext(os.path.basename(a))[0].upper() in
                {n.upper() for n in args.names}]
    os.makedirs(TMP, exist_ok=True)
    bad = 0
    for avi in avis:
        name = os.path.basename(avi)
        w, h = size_of(avi)
        lift, orig = os.path.join(TMP, 'lift.rgb'), os.path.join(TMP, 'orig.rgb')
        try:
            decode(exe, avi, lift, False)
            decode(exe, avi, orig, True)
        except RuntimeError as e:
            print('%-13s FAIL %s' % (name, e))
            bad += 1
            continue
        frame = ((w * 3 + 3) & ~3) * h
        frames = os.path.getsize(lift) // frame
        diff = same(lift, orig, frame)
        r = correlation(lift, avi, w, h)
        print('%-13s %4d frames %dx%d   vs original: %s   vs ffmpeg: correlation %.4f'
              % (name, frames, w, h, 'identical' if not diff else '%d frames differ (first %d)'
                 % (len(diff), diff[0]), r))
        bad += bool(diff) or r < 0.95
        os.remove(lift)
        os.remove(orig)
    print('verify_indeo: %s' % ('ok' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
