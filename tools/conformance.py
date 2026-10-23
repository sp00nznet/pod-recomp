#!/usr/bin/env python3
"""POD Gold conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Milestones**: one headless run of the host with no input, 60 seconds.
  POD's menu starts a race on its own after a few seconds (attract mode), so
  the run covers the menu, the race and the return to the menu without a
  script. Scored by the lines the host prints and by the recording.
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, and RECOMP_ITAIL labels that cannot resolve at run time (their
  target is not in the dispatch table).

The game is not in the repo. Without game/ and a build this prints SKIP and
exits 0, so it can sit on netlab's test VMs without the corpus. Shape:
Streets of SimCity's tools/conformance.py.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
"""
import argparse
import glob
import json
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')
VIDEO = os.path.join(ROOT, 'work', 'conformance.mp4')
SECONDS = 60


def host():
    """POD_EXE, else the newest of build/ and build-farm/."""
    if os.environ.get('POD_EXE'):
        return os.environ['POD_EXE']
    have = [p for p in (os.path.join(ROOT, d, 'pod.exe') for d in ('build', 'build-farm')) if os.path.exists(p)]
    return max(have, key=os.path.getmtime) if have else None


def frame(t, crop=None):
    """The recording at t seconds, 64x48 grey, as bytes (empty if none)."""
    vf = ('crop=%d:%d:%d:%d,' % crop if crop else '') + 'scale=64:48'
    return subprocess.run(['ffmpeg', '-loglevel', 'error', '-ss', str(t), '-i', VIDEO, '-frames:v', '1',
                           '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                          capture_output=True).stdout


def lit(t):
    f = frame(t)
    return bool(f) and sum(f) / len(f) > 8


def differ(a, b):
    """Did a quarter of the picture change between a and b seconds?"""
    fa, fb = frame(a), frame(b)
    return bool(fa) and len(fa) == len(fb) and sum(abs(x - y) > 16 for x, y in zip(fa, fb)) > len(fa) // 4


def same(a, b):
    fa, fb = frame(a), frame(b)
    return bool(fa) and len(fa) == len(fb) and sum(abs(x - y) > 16 for x, y in zip(fa, fb)) < len(fa) // 50


def world(t):
    """The middle of the screen, away from the HUD, has detail: the track is
    drawn. Before the 2026 relift every polygon covered the screen and the
    world was one flat colour behind a working HUD."""
    f = frame(t, (320, 200, 160, 120))
    return bool(f) and statistics.pstdev(f) > 20


# (name, test). Times are seconds into the run: the menu is up by 10, the
# attract race runs from about 20 to 48, the menu is back by 50.
MILESTONES = [
    ('image mapped and imports bound', lambda out: re.search(r'mapped PODMMX\.EXE', out)),
    ('the game folder is the CD', lambda out: re.search(r'\[cd\] \w:\\ is the game folder', out)),
    ('DirectDraw created', lambda out: 'DirectDrawCreate -> host DirectDraw' in out),
    ('past the CD and sound checks (100 frames)', lambda out: 'frame 100 presented' in out),
    ('main menu drawn', lambda out: lit(14)),
    ('attract race starts by itself', lambda out: differ(14, 30)),
    ('the 3D world is drawn', lambda out: world(30) and world(40)),
    ('the race moves', lambda out: differ(30, 40)),
    ('back to the main menu', lambda out: same(14, 56)),
    ('no fault', lambda out: '=== fault' not in out),
]


def run(exe):
    cmd = [exe, '--run', '--headless', '--watchdog', str(SECONDS), '--record', VIDEO]
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=SECONDS + 60)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
    passed = [name for name, test in MILESTONES if test(out)]
    last = [l for l in out.splitlines() if l.startswith(('===', '  in lifted'))]
    return passed, last[:3]


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = sum(1 for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c'))
                     for t in re.findall(r'L_([0-9A-F]{8}): RECOMP_ITAIL', open(fn).read())
                     if t not in disp)
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--exe', help='the host to test (netlab passes its build); default POD_EXE, else the newest build')
    args = ap.parse_args()
    exe = args.exe or host()
    if not (exe and os.path.isdir(os.path.join(ROOT, 'game')) and os.path.exists(os.path.join(GEN, 'recomp_dispatch.c'))):
        print('conformance: SKIP -- needs game/ (your copy), the lift and a build (README, Step by step)')
        return 0
    os.makedirs(os.path.dirname(VIDEO), exist_ok=True)

    passed, last = run(os.path.abspath(exe))
    print('milestones: %d/%d  (%s)' % (len(passed), len(MILESTONES), os.path.relpath(exe, ROOT)))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  ' + l)

    health = lift_health()
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no terminator, '
          '%(unresolved_itail)d unresolvable ITAIL labels' % health)
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None
    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
