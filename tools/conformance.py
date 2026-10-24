#!/usr/bin/env python3
"""POD Gold conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Milestones**: two headless runs of the host with no input (RUNS below),
  scored by the lines the host prints and by the recordings. 20 seconds of
  the intro, played through the lifted Indeo codec; and 120 seconds with the
  videos off, in which POD's menu starts a race on its own (attract mode),
  returns to the menu, and starts a four-way split-screen race.
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


def host():
    """POD_EXE, else the newest of build/ and build-farm/."""
    if os.environ.get('POD_EXE'):
        return os.environ['POD_EXE']
    have = [p for p in (os.path.join(ROOT, d, 'pod.exe') for d in ('build', 'build-farm')) if os.path.exists(p)]
    return max(have, key=os.path.getmtime) if have else None


def frames(path, crop=None):
    """The recording at one frame a second, each 64x48 gray bytes."""
    vf = ('crop=%d:%d:%d:%d,' % crop if crop else '') + 'fps=1,scale=64:48'
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-vf', vf, '-f', 'rawvideo',
                          '-pix_fmt', 'gray', '-'], capture_output=True).stdout
    n = 64 * 48
    return [raw[i:i + n] for i in range(0, len(raw) - n + 1, n)]


def changed(a, b):
    return sum(abs(x - y) > 16 for x, y in zip(a, b))


def differ(a, b):
    """A quarter of the picture changed."""
    return bool(a) and len(a) == len(b) and changed(a, b) > len(a) // 4


def same(a, b):
    return bool(a) and len(a) == len(b) and changed(a, b) < len(a) // 50


def detail(f, over):
    return bool(f) and statistics.pstdev(f) > over


class Recording:
    """A run's video, one frame a second: the whole screen, the middle away
    from the HUD (track crop), and the intro's 320x208 picture (video crop)."""

    def __init__(self, path):
        self.full = frames(path)
        self.mid = frames(path, (320, 200, 160, 120))
        self.vid = frames(path, (320, 208, 160, 136))

    def at(self, frames, t):
        return frames[t] if t < len(frames) else b''


def intro_plays(r):
    a, b = r.at(r.vid, 8), r.at(r.vid, 14)
    return detail(a, 12) and detail(b, 12) and differ(a, b)


def attract(r):
    """The attract-mode events in order, as seconds into the run (None if not
    seen). Found, not timed: how fast the menu hands over to a race varies by
    a few seconds from build to build.

    menu    the first lit picture that holds still for a second
    race    six seconds of the 3D world (the track crop has detail), none of
            them the menu
    moving  a race window in which at least three of the five steps change a
            quarter of the screen
    back    the menu again, after the first moving race
    second  a moving race after that"""
    f, m = r.full, r.mid
    n = len(f)
    ev = dict.fromkeys(('menu', 'race', 'moving', 'back', 'second'))
    ev['menu'] = next((t for t in range(5, n - 1)
                       if sum(f[t]) / len(f[t]) > 8 and same(f[t], f[t + 1])), None)
    if ev['menu'] is None:
        return ev
    menu = lambda t: same(f[ev['menu']], f[t])
    race = lambda t: all(detail(m[u], 20) and not menu(u) for u in range(t, t + 6))
    moves = lambda t: sum(differ(f[u], f[u + 1]) for u in range(t, t + 5)) >= 3
    first = lambda lo, test: next((t for t in range(lo, n - 6) if test(t)), None)
    ev['race'] = first(ev['menu'], race)
    ev['moving'] = first(ev['menu'], lambda t: race(t) and moves(t))
    if ev['moving'] is not None:
        ev['back'] = first(ev['moving'] + 6, menu)
    if ev['back'] is not None:
        ev['second'] = first(ev['back'] + 1, lambda t: race(t) and moves(t))
    return ev


# Two runs, no input. The intro is four minutes long, and skipping it with a
# key leaves the menu dead (ROADMAP.md), so one short run watches it play
# through the lifted codec, and the attract run goes without videos
# (--no-codec: POD then shows its "install Indeo" still and goes straight on).
# In the attract run the menu is up by about 10 s, a race runs to about 48 s,
# the menu returns, and a second (split-screen) race starts by about 75 s.
#
# (name, extra args, seconds, recording, milestones); a test takes the host's
# output and the Recording.
RUNS = [
    ('intro', [], 20, os.path.join(ROOT, 'work', 'conformance_intro.mp4'), [
        ('the codec starts (Indeo 4.1, lifted)', lambda out, r: '[indeo] Indeo 4.1 lifted' in out),
        ('the intro video plays', lambda out, r: intro_plays(r)),
        ('no fault (intro)', lambda out, r: '=== fault' not in out),
    ]),
    ('attract', ['--no-codec'], 120, os.path.join(ROOT, 'work', 'conformance.mp4'), [
        ('image mapped and imports bound', lambda out, r: re.search(r'mapped PODMMX\.EXE', out)),
        ('the game folder is the CD', lambda out, r: re.search(r'\[cd\] \w:\\ is the game folder', out)),
        ('DirectDraw created', lambda out, r: 'DirectDrawCreate -> host DirectDraw' in out),
        ('past the CD and sound checks (100 frames)', lambda out, r: 'frame 100 presented' in out),
        ('main menu drawn', lambda out, r: r.ev['menu'] is not None),
        ('attract race starts by itself, the 3D world drawn', lambda out, r: r.ev['race'] is not None),
        ('the race moves', lambda out, r: r.ev['moving'] is not None),
        ('back to the main menu', lambda out, r: r.ev['back'] is not None),
        ('a second attract race', lambda out, r: r.ev['second'] is not None),
        ('no fault', lambda out, r: '=== fault' not in out),
    ]),
]
MILESTONES = [m for r in RUNS for m in r[4]]


def run(exe):
    passed, last = [], []
    for name, args, seconds, path, milestones in RUNS:
        cmd = [exe, '--run', '--headless', '--watchdog', str(seconds), '--record', path] + args
        try:
            p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace',
                               timeout=seconds + 60)
            out = p.stdout + p.stderr
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
        r = Recording(path)
        r.ev = attract(r) if name == 'attract' else {}
        if r.ev:
            print('attract events (s): ' + '  '.join('%s %s' % kv for kv in r.ev.items()))
        passed += [m for m, test in milestones if test(out, r)]
        last += [l for l in out.splitlines() if l.startswith(('===', '  in lifted'))]
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
    os.makedirs(os.path.join(ROOT, 'work'), exist_ok=True)

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
