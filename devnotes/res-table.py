#!/usr/bin/env python3
"""Turn a directory of res.sh reports into the table P4 step 2 quotes.

    ./devnotes/res-table.py /tmp/res [/tmp/res-point ...]

Same rule as ab-table.py and for the same reason: the numbers in the write-up are generated from the
reports, never retyped.  A hand-copied figure survives a rerun that would have changed it.

One row per (game, scale).  The columns are chosen so that a resolution-invariance claim can be read
off them directly:

    covered 1x / Nx   how much of the picture each render's 3D layer touched
    A only            pixels the 1x render drew over and the supersampled one did not.  This is the
                      one that must be ~0: supersampling can only ADD partially-covered edge pixels,
                      so a pixel the 1x render covered and the Nx render did not is a polygon that
                      stopped being drawn.
    B only            the antialiased fringe.  Expected, and it grows with the scale.
    interior          coverage disagreements with no both-covered neighbour, after the drew-black
                      artefact is discounted.  A filled region of these is a missing polygon.
    same colour       of the pixels both covered, how many are bit-identical.  Meaningful only for
                      the point resolve; under a box filter every edge and every minified texel
                      legitimately changes.
    ssim covered      the headline SSIM over the covered region.
"""

import pathlib
import re
import sys


def parse(path):
    """One res.sh report -> a list of per-scale dicts.  The reports are read, not trusted: a
    section with no `coverage agreement` line is one whose run died and it is skipped."""
    text = path.read_text()
    game = re.search(r"^game\s+(\S+)", text, re.M)
    resolve = re.search(r"^resolve\s+(\S+)", text, re.M)
    mode = re.search(r"^mode\s+(.+)$", text, re.M)
    if not game:
        return []

    rows = []
    # Each scale's block starts at its "background reference identical to 1x at Nx" line.
    for block in re.split(r"^background reference identical to 1x at (\d+)x.*$", text, flags=re.M)[1:]:
        if block.strip().isdigit():
            scale = int(block)
            continue
        def grab(pattern, cast=float, default=None):
            m = re.search(pattern, block, re.M)
            return cast(m.group(1)) if m else default

        agreement = grab(r"^coverage agreement ([\d.]+)")
        if agreement is None:
            continue
        rows.append(dict(
            game=game.group(1),
            resolve=resolve.group(1) if resolve else "?",
            mode=(mode.group(1).strip() if mode else ""),
            scale=scale,
            cov_a=grab(r"^covered by A\s+(\d+)", int),
            cov_b=grab(r"^covered by B\s+(\d+)", int),
            both=grab(r"^both\s+(\d+)", int),
            same=grab(r"^\s+same colour\s+(\d+)", int),
            a_only=grab(r"^A only\s+(\d+)", int),
            b_only=grab(r"^B only\s+(\d+)", int),
            agreement=agreement,
            interior=grab(r"real interior disagreements\s+(\d+)", int,
                          grab(r"^\s+interior disagreements (\d+)", int, 0)),
            ssim=grab(r"^ssim covered\s+([\d.]+)"),
            exact=("exit criterion 1 holds" in block),
        ))
    return rows


def main(argv):
    dirs = argv[1:] or ["/tmp/res"]
    rows = []
    for d in dirs:
        for report in sorted(pathlib.Path(d).glob("*.txt")):
            rows.extend(parse(report))

    if not rows:
        print("no res.sh reports found in " + ", ".join(dirs), file=sys.stderr)
        return 1

    # Grouped by (resolve, mode) and not by resolve alone: a MODE= run and a plain one are different
    # measurements of the same game at the same scale, and merging them puts two rows with the same
    # (game, scale) in one table with nothing to tell them apart.
    for resolve, mode in sorted({(r["resolve"], r["mode"]) for r in rows}):
        group = [r for r in rows if (r["resolve"], r["mode"]) == (resolve, mode)]
        print(f"\n### {resolve} resolve" + ("" if mode == "(none)" else f"  —  {mode}"))
        print()
        print("| game | scale | covered 1x | covered Nx | A only | B only | interior | same colour | agreement | ssim covered | exact |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for r in sorted(group, key=lambda r: (r["game"], r["scale"])):
            same = f"{100.0 * r['same'] / r['both']:.3f} %" if r["both"] else "—"
            print(f"| {r['game']} | {r['scale']}x | {r['cov_a']} | {r['cov_b']} | **{r['a_only']}** | "
                  f"{r['b_only']} | **{r['interior']}** | {same} | {r['agreement']:.4f} | "
                  f"{r['ssim']:.4f} | {'pass' if r['exact'] else 'FAIL'} |")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
