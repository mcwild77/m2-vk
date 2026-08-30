#!/usr/bin/env python3
"""Turn a directory of ab.sh reports into the markdown baseline tables.

    ab-table.py <dir-of-ab.sh-reports> [game ...]

The point of this existing at all is that a baseline table nobody can regenerate goes stale
silently, and this project has already paid for that once: `cf043ff583370663` was documented as the
M2VK_SW_3D digest and survived two steps after it stopped being true, because each step copied it
forward instead of reading it off a run.  Paste this tool's output into ab-baselines.md; never edit
a number in that file by hand.
"""

import os
import re
import sys


def parse(path):
    text = open(path).read()

    def one(pattern, cast=str, default=None):
        m = re.search(pattern, text, re.M)
        return cast(m.group(1)) if m else default

    row = {
        'game': one(r'^game\s+(\S+)'),
        'frames': one(r'^frames\s+(\d+)', int),
        'mode': one(r'^mode\s+(.*?)\s*$'),
        'head': one(r'^head\s+(\S+)'),
        'date': one(r'^date\s+(\S+)'),
        'covered': one(r'^covered by A\s+(\d+)', int),
        'covered_pct': one(r'^covered by A\s+\d+\s+\(([\d.]+)', float),
        'agreement': one(r'^coverage agreement\s+([\d.]+)', float),
        'a_only': one(r'^A only\s+(\d+)', int),
        'b_only': one(r'^B only\s+(\d+)', int),
        'interior': one(r'^\s+real interior disagreements (\d+)', int, 0),
        'artefact': one(r'of which (\d+) differ by <=', int, 0),
        'same_colour': one(r'^\s+same colour\s+\d+\s+\(([\d.]+)', float),
        'ssim_frame': one(r'^ssim whole frame\s+([\d.]+)', float),
        'ssim_covered': one(r'^ssim covered\s+([\d.]+)', float),
        'ssim_interior': one(r'^ssim interior\s+([\d.]+)', float),
        'p1': one(r'covered p1 ([\d.]+)', float),
        'p5': one(r'p5 ([\d.]+)', float),
        'exact': 'PASS' if 'exit criterion 1 holds' in text else 'FAIL',
        'heat_max': one(r'max (\d+), mean', int),
    }
    for layer in ('bg', '3d'):
        for r in ('software', 'vulkan'):
            row['%s_%s' % (layer, r)] = one(r'^%s\s+%s\s+digest (\S+)' % (layer, r))
    row['no_disagreement'] = 'no coverage disagreement at all' in text
    return row


def fmt(value, spec, missing='—'):
    return missing if value is None else format(value, spec)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    directory = argv[1]
    wanted = argv[2:]
    rows = []
    for name in sorted(os.listdir(directory)):
        if not name.endswith('.txt'):
            continue
        row = parse(os.path.join(directory, name))
        if row['game'] and (not wanted or row['game'] in wanted):
            rows.append(row)
    if wanted:  # keep the order the caller asked for; it is usually meaningful
        rows.sort(key=lambda r: wanted.index(r['game']))

    if not rows:
        print("no reports in %s" % directory)
        return 1

    print("Recorded %s from `%s`, HEAD `%s`, %d frames, mode %s.\n"
          % (rows[0]['date'], os.path.basename(directory.rstrip('/')), rows[0]['head'],
             rows[0]['frames'], rows[0]['mode']))

    print("| game | covered px | % of frame | cov. agreement | A only / B only | real interior | same colour | SSIM covered | SSIM interior | p1 | exact |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for r in rows:
        interior = '**%d**' % r['interior'] if r['interior'] else '0'
        if r['artefact']:
            interior += ' _(+%d artefact)_' % r['artefact']
        print("| `%s` | %s | %s | %s | %d / %d | %s | %s | **%s** | %s | %s | %s |"
              % (r['game'], fmt(r['covered'], 'd'), fmt(r['covered_pct'], '.2f'),
                 fmt(r['agreement'], '.4f'), r['a_only'] or 0, r['b_only'] or 0, interior,
                 fmt(r['same_colour'], '.2f') + ' %', fmt(r['ssim_covered'], '.4f'),
                 fmt(r['ssim_interior'], '.4f'), fmt(r['p1'], '.3f'), r['exact']))

    print("\n| game | bg (both renderers) | 3D software | 3D vulkan |")
    print("|---|---|---|---|")
    for r in rows:
        bg = r['bg_software'] if r['bg_software'] == r['bg_vulkan'] else (
            '%s / %s DIFFER' % (r['bg_software'], r['bg_vulkan']))
        print("| `%s` | `%s` | `%s` | `%s` |" % (r['game'], bg, r['3d_software'], r['3d_vulkan']))

    bad = [r['game'] for r in rows if r['exact'] != 'PASS' or r['interior']]
    print("\n%d fixtures; exit criterion 1 %s; interior coverage disagreements %s."
          % (len(rows),
             'passes on all' if all(r['exact'] == 'PASS' for r in rows) else 'FAILS on ' + ', '.join(bad),
             'none anywhere' if not any(r['interior'] for r in rows) else 'ON ' + ', '.join(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
