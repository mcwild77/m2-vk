#!/usr/bin/env python3
# Push-speed / snap-back timing from a sticktest.log.
#
# sticktest itself only reports roundness/reach/drift; it does not time anything.
# This reads its raw per-change CSV (ms,a0,a1,...) and, per stick (axis pair),
# finds every excursion from centre past a deadzone and reports:
#   push_ms      time from leaving the deadzone to the excursion's peak
#   snapback_ms  time from that peak back to inside a tighter centre band
#
# Usage: sticktest-timing.py [logfile] [--dead 0.15] [--centre 0.08] [--full 0.85]

import sys, math

def parse(path):
    axes = None
    rows = []  # (ms, [raw...])
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("ms,"):
                axes = line.split(",")[1:]
                continue
            parts = line.split(",")
            ms = int(parts[0])
            vals = [int(x) for x in parts[1:]]
            rows.append((ms, vals))
    if axes is None:
        sys.exit("no header row found — not a sticktest.log")
    return axes, rows

def forward_fill(rows, naxes):
    # rows only log a line when something changed; reconstruct full state per row.
    state = [0] * naxes
    out = []
    for ms, vals in rows:
        for i, v in enumerate(vals):
            state[i] = v
        out.append((ms, list(state)))
    return out

def norm(raw):
    v = raw / 32767.0
    return max(-1.0, min(1.0, v))

def find_excursions(series, dead, centre):
    # series: list of (ms, r) where r = stick magnitude 0..1+
    # An excursion starts when r crosses above `dead` from below, peaks, and
    # "ends" the moment r first drops back under `centre` afterward. A push that
    # never comes back under `centre` before the log ends is reported open-ended.
    out = []
    i, n = 0, len(series)
    while i < n:
        ms, r = series[i]
        if r < dead:
            i += 1
            continue
        # walking start back to the exact threshold crossing (linear interp not
        # needed at this resolution — sample step is already sub-ms on a real pad)
        t_start = ms
        j = i
        t_peak, r_peak = ms, r
        while j < n and series[j][1] >= dead * 0.6:  # stay in the excursion with hysteresis
            if series[j][1] > r_peak:
                t_peak, r_peak = series[j][0], series[j][1]
            j += 1
        # from the peak, look for the first sample back under `centre`
        t_end = None
        k = j - 1
        # k currently sits at/after the last >=dead*0.6 sample; scan forward from peak
        p = i
        while series[p][0] < t_peak:
            p += 1
        for q in range(p, n):
            if series[q][1] < centre:
                t_end = series[q][0]
                break
        out.append({
            "t_start": t_start, "t_peak": t_peak, "peak": r_peak,
            "t_end": t_end,
            "push_ms": t_peak - t_start,
            "snapback_ms": (t_end - t_peak) if t_end is not None else None,
        })
        i = j if j > i else i + 1
    return out

def main():
    args = sys.argv[1:]
    path = "devnotes/tools/sticktest.log"
    dead, centre, full = 0.15, 0.08, 0.85
    pos = [a for a in args if not a.startswith("--")]
    if pos:
        path = pos[0]
    for a in args:
        if a.startswith("--dead="):
            dead = float(a.split("=", 1)[1])
        elif a.startswith("--centre="):
            centre = float(a.split("=", 1)[1])
        elif a.startswith("--full="):
            full = float(a.split("=", 1)[1])

    axes, rows = parse(path)
    naxes = len(axes)
    filled = forward_fill(rows, naxes)

    nsticks = naxes // 2
    for s in range(nsticks):
        ax, ay = s * 2, s * 2 + 1
        series = []
        for ms, state in filled:
            x, y = norm(state[ax]), norm(state[ay])
            series.append((ms, math.hypot(x, y)))
        # a stick that never moves has one flat value throughout — skip it
        if max(r for _, r in series) - min(r for _, r in series) < 0.05:
            continue

        exc = find_excursions(series, dead, centre)
        full_exc = [e for e in exc if e["peak"] >= full]
        print(f"stick {s} (a{ax}/a{ay}) — {len(exc)} excursion(s) past |r|>={dead}, "
              f"{len(full_exc)} reaching >={full:.0%} of full deflection")
        if not exc:
            continue
        print(f"  {'#':>3}  {'peak':>6}  {'push_ms':>8}  {'snapback_ms':>12}")
        for idx, e in enumerate(exc):
            sb = f"{e['snapback_ms']:>12d}" if e["snapback_ms"] is not None else "  (open — didn't return before log ended)"
            print(f"  {idx:>3}  {e['peak']:>6.3f}  {e['push_ms']:>8d}  {sb}")
        if full_exc:
            pushes = [e["push_ms"] for e in full_exc]
            backs = [e["snapback_ms"] for e in full_exc if e["snapback_ms"] is not None]
            print(f"  full-deflection pushes: min {min(pushes)} ms  max {max(pushes)} ms  "
                  f"mean {sum(pushes)/len(pushes):.0f} ms")
            if backs:
                print(f"  snap-back from full:    min {min(backs)} ms  max {max(backs)} ms  "
                      f"mean {sum(backs)/len(backs):.0f} ms")
        print()

if __name__ == "__main__":
    main()
