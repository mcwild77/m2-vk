# Feature survey — what the Vulkan renderer actually has to support

Measured 2026-07-25 against **mame0288**, using the P0 poly tap over **29 games** (65 emulated
seconds of attract mode each, ~2500 rendered frames per game). Method and caveats at the bottom —
**read them before trusting the ratio columns**.

Purpose: size the P3 renderer and settle the "is feature X in or out for v1" questions on data
rather than on one frame of VF2.

## The numbers

| game | frames | poly avg | poly max | solid% | tex% | trans% | v5+ | v6+ | micro | checker | tie% | bad z | 1/z min | 1/z max | bkt max | pages/f | pages | win |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bel | 3265 | 572 | 1144 | 2.6 | 89.0 | 8.4 | 2075 | 104 | · | 21,224 | 62.1 | · | 4.83e-05 | 284 | 54330 | 53 | 74 | 5 |
| daytona | 2842 | 1148 | 2182 | 17.2 | 50.8 | 32.0 | 39319 | 1427 | · | 109,024 | 38.7 | · | 0.002 | 501 | 32592 | 105 | 254 | 2 |
| desert | 1521 | 602 | 2816 | 12.2 | 80.6 | 7.2 | 5939 | 370 | 105,554 | 80,163 | 44.8 | · | 0.00197 | 411 | 32421 | 46 | 82 | 2 |
| doaa | 2631 | 1300 | 2076 | 5.6 | 84.9 | 9.6 | 24070 | 351 | · | 11,823 | 52.0 | · | 4.69e-10 | 20.1 | 37600 | 144 | 458 | 6 |
| dynabb97 | 2462 | 637 | 1462 | 4.6 | 80.0 | 15.4 | 30815 | 799 | · | 4,127 | 46.7 | · | 0.000567 | 2.48e+03 | 41822 | 131 | 355 | 4 |
| dynamcop | 2701 | 866 | 3127 | 1.3 | 85.5 | 13.2 | 31334 | 1304 | 54,245 | 11,560 | 38.0 | · | 0.000391 | 32 | 41977 | 205 | 522 | 3 |
| fvipers | 2662 | 1100 | 2282 | 0.1 | 88.3 | 11.6 | 21901 | 466 | · | 608 | 65.9 | · | 0.00669 | 9.23 | 25260 | 160 | 591 | 8 |
| gunblade | 2623 | 679 | 1511 | 4.8 | 75.9 | 19.3 | 31185 | 1939 | · | 116,713 | 48.0 | · | 1.77e-05 | 2.27e+03 | 56202 | 228 | 544 | 5 |
| hotdo | 2763 | 521 | 2153 | 14.0 | 73.4 | 12.6 | 29042 | 879 | 7,207 | 7,071 | 39.8 | · | 0.000124 | 1.8e+03 | 49057 | 102 | 432 | 3 |
| indy500 | 2744 | 1510 | 2250 | 2.1 | 84.0 | 13.9 | 27383 | 2040 | · | 127,818 | 31.8 | · | 0.000175 | 1.97e+03 | 46754 | 139 | 244 | 6 |
| lastbrnx | 2627 | 1652 | 1808 | 7.7 | 80.2 | 12.1 | 21964 | 561 | · | 374 | 66.7 | · | 0.00172 | 408 | 65535 | 188 | 392 | 9 |
| manxttc | 2146 | 605 | 1194 | 2.3 | 46.7 | 51.0 | 27352 | 1228 | · | 3,596 | 24.7 | · | 4.99e-05 | 2.05e+03 | 50065 | 117 | 202 | 2 |
| motoraid | 2867 | 902 | 1808 | 7.8 | 72.4 | 19.8 | 47275 | 1374 | · | 42,384 | 34.2 | · | -2.99e-13 | 1.86e+03 | 40199 | 105 | 241 | 5 |
| overrev | 3239 | 675 | 1577 | 8.1 | 0.0 | 91.9 | 31097 | 1249 | · | 27,040 | 34.8 | · | 0.00047 | 1.81e+03 | 41118 | 127 | 244 | 4 |
| pltkids | 3527 | 1296 | 3812 | 33.1 | 63.9 | 3.0 | 31512 | 853 | · | 1,596 | 66.8 | · | 0.0195 | 112 | 18888 | 85 | 291 | 6 |
| rchase2 | 1726 | 480 | 1278 | 8.4 | 68.5 | 23.1 | 12829 | 667 | · | 7,988 | 34.1 | 4,330 | 0 | 8.51e+37 | 46053 | 114 | 166 | 2 |
| schamp | 2967 | 1022 | 2331 | 0.2 | 95.0 | 4.9 | 42830 | 1271 | · | 44,600 | 41.9 | · | 0.00089 | 169 | 37262 | 41 | 210 | 12 |
| segawski | 2754 | 925 | 2303 | 7.0 | 63.0 | 30.0 | 60413 | 2067 | 391,392 | 380,758 | 31.5 | · | 6.03e-08 | 1.08e+04 | 36254 | 116 | 178 | 8 |
| sgt24h | 3247 | 811 | 3093 | 36.4 | 0.0 | 63.6 | 35691 | 1136 | · | 81,244 | 50.7 | · | 0.00022 | 2.01e+03 | 45513 | 137 | 310 | 3 |
| skytargt | 1301 | 1196 | 2914 | 2.8 | 85.6 | 11.6 | 28001 | 743 | · | 187,123 | 39.4 | · | 4.19e-05 | 2.01e+03 | 55116 | 62 | 139 | 5 |
| srallyc | 2385 | 1299 | 2137 | 15.0 | 48.9 | 36.1 | 34026 | 852 | · | 25,259 | 36.2 | · | 4.57e-06 | 572 | 64181 | 107 | 160 | 3 |
| stcc | 2547 | 1266 | 2158 | 8.0 | 57.4 | 34.6 | 26703 | 1140 | · | 146,630 | 35.4 | 292,320 | 9.96e-06 | 8.51e+37 | 59520 | 148 | 295 | 8 |
| vcop | 1445 | 1542 | 2565 | 0.0 | 94.8 | 5.2 | 19528 | 10 | · | 37,879 | 67.8 | · | 0.000801 | 1 | 45640 | 63 | 74 | 8 |
| vcop2 | 3043 | 1039 | 2025 | 0.0 | 86.6 | 13.4 | 59048 | 999 | · | 123,171 | 49.5 | · | 0.000802 | 1.29 | 45640 | 74 | 158 | 7 |
| vf2 | 2752 | 904 | 1622 | 0.2 | 84.1 | 15.8 | 16459 | 158 | · | 2,995 | 56.3 | · | 0.00102 | 976 | 36489 | 150 | 446 | 8 |
| vonj | 2575 | 704 | 4137 | 39.7 | 50.7 | 9.6 | 19594 | 1349 | · | 32,732 | 64.3 | · | 0.00011 | 822 | 45487 | 88 | 146 | 7 |
| vstriker | 1516 | 1053 | 2533 | 0.0 | 75.7 | 24.3 | 17404 | 594 | · | 67,823 | 60.7 | 99 | -3.77e+30 | 8.51e+37 | 45364 | 130 | 215 | 5 |
| waverunr | 2457 | 1151 | 2214 | 4.5 | 65.4 | 30.1 | 64289 | 1467 | 448,324 | 366,488 | 30.2 | · | 0.00103 | 776 | 36427 | 132 | 246 | 5 |
| zerogun | 2849 | 635 | 1749 | 1.7 | 90.2 | 8.1 | 32755 | 724 | · | 17,574 | 50.2 | · | 0.001 | 55.1 | 36672 | 105 | 514 | 2 |


Columns: `poly avg/max` per frame · `solid%` untextured · `tex%` textured opaque · `trans%` textured
translucent · `v5+`/`v6+` polys with more than 4/5 vertices · `micro` microtextured polys ·
`checker` stipple-transparency polys · `tie%` polys sharing a sort bucket with the poly drawn
immediately before · `bad z` vertices with non-finite or absurd (>1e6) `1/z` · `pages/f` distinct
texture pages in the busiest frame · `pages` distinct pages over the run · `win` max windows.

## What this settles

**1. Microtextures cannot be cut from v1.** Five games use them, two of them heavily:
`waverunr` (448k polys), `segawski` (391k), `desert` (106k), `dynamcop` (54k), `hotdo` (7k). The
water and ski titles lean on them for surface detail. VF2, Daytona and Sega Rally use none at all —
which is exactly why a VF2-only look would have got this wrong.

**2. `checker` stipple transparency is mandatory, not a P5 nicety.** *Every one of the 29 games*
uses it, several enormously: `segawski` 381k, `waverunr` 366k, `skytargt` 187k, `stcc` 147k,
`indy500` 128k. The single VF2 frame I first dumped had none, which was badly unrepresentative. This
is a per-pixel screen-door effect that will look wrong at enhanced internal resolution, so the
dither-vs-alpha decision moves from P5 into P3.

**3. Depth values must be clamped.** Four games emit `1/z` that is non-finite or absurd:
`stcc` (292k vertices!), `rchase2` (4.3k), `vstriker` (99). Worse, two games emit **negative**
`1/z` — `vstriker` at `-3.8e+30` and `motoraid` at `-3.0e-13` — i.e. geometry behind the eye
surviving the clip. The software rasterizer absorbs this silently; a Vulkan depth buffer will not.

**4. The untextured path matters after all.** VF2's 0.2 % solid is the outlier, not the rule:
`vonj` 39.7 %, `sgt24h` 36.4 %, `pltkids` 33.1 %, `daytona` 17.2 %, `srallyc` 15.0 %. An earlier
note here downgraded the solid path on VF2 evidence alone; that was wrong.

**5. For some games the translucent cutout path is the *primary* path.** `overrev` and `sgt24h`
have literally **zero** opaque textured polygons — every textured poly is flagged translucent
(91.9 % and 63.6 % of all polys respectively). Since the software renderer treats that as a
texel-alpha cutout with a 50 % discard, that discard behaviour is load-bearing for those titles, not
an edge case.

**6. The general n-gon fan path is required.** Every game produces 6+ vertex polygons after
clipping (104 in `bel`, 2067 in `segawski`). No shortcut for tris and quads only.

**7. Sizing.** Worst case across the set: **4137 polys/frame** (`vonj`), **228 distinct texture
pages in one frame** (`gunblade`), sort buckets reaching the full **65535** (`lastbrnx`, `srallyc`
64181), and up to **12 windows** (`schamp`) — not the 8 VF2 shows.

**8. Coplanar ties are pervasive in every game, not just VF2.** `tie%` runs 24.7 % (`manxttc`) to
67.8 % (`vcop`), clustering around 45 %. P4's resolve-ties-by-draw-order is the main depth path for
the whole library.

## Board variant per game

Taken from the driver's state classes (`src/mame/sega/model2.h`); note that games declared with
`GAMEL` rather than `GAME` are easy to miss when grepping — `indy500` (2B) and `stcc` (2C) both are.

| Variant | Copro in MAME | Surveyed games |
| --- | --- | --- |
| **Model 2** (original) | `mb86234` TGP — `model2o_state : model2_tgp_state` | daytona, desert, vcop |
| **2A-CRX** | `mb86234` TGP — `model2a_state : model2_tgp_state` | vf2, srallyc, vcop2, skytargt, doaa, manxttc, motoraid, dynamcop |
| **2B-CRX** | `adsp21062` SHARC — `model2b_state` | fvipers, lastbrnx, vonj, schamp, vstriker, gunblade, rchase2, dynabb97, zerogun, pltkids, sgt24h, indy500 |
| **2C-CRX** | `mb86235` TGPx4 — `model2c_state` | bel, hotdo, segawski, waverunr, overrev, stcc |

(The driver header at `model2.cpp:4` sums it up: "i960KB + (5x TGP) or (2x SHARC) or (2x TGPx4)".)

### The seam is variant-independent — this is the important part

`geo_parse()` is called from **`model2_state::screen_vblank`** (`model2.cpp:2449`) — the *base*
class, unconditionally, for all four variants. And every call to `model2_3d_push()` (the rasterizer's
command entry point) comes from inside the HLE geometrizer in `model2_v.cpp`; the emulated copros
never push geometry themselves.

So in mame0288 **one HLE geometrizer feeds one rasterizer for the entire library**. The TGP / SHARC /
TGPx4 differences sit *upstream* of the display list and change what the games compute, not the shape
of the polygon stream we tap. Consequences:

- The tap and the Vulkan renderer need **no per-variant code paths**. One seam covers all of Model 2.
- The A/B harness does not need a per-variant matrix for *correctness* — only for coverage of
  content and features.
- The plan's open question #3 ("Model 2 sub-variant … different copros") is largely dissolved: the
  variants matter for which *features* appear (below) and for emulation maturity, not for the port's
  architecture.

### Feature correlation by variant

| Variant | Games | Uses microtextures | Microtex polys | Degenerate-depth games | Max polys/frame |
| --- | --- | --- | --- | --- | --- |
| Model 2 (original) | 3 | 1/3 (desert) | 105,554 | · | 2816 |
| 2A-CRX | 8 | 1/8 (dynamcop) | 54,245 | · | 3127 |
| 2B-CRX | 12 | 0/12 | · | vstriker (99), rchase2 (4,330) | 4137 |
| 2C-CRX | 6 | 3/6 (hotdo, segawski, waverunr) | 846,923 | stcc (292,320) | 2303 |

Two clean patterns:

- **Microtextures are overwhelmingly a 2C-CRX trait.** Three of the six 2C games use them, and they
  account for **846,923 of the 1,006,722 microtextured polys measured** (84 %) — `waverunr` and
  `segawski` alone are 840k of that. Outside 2C only `desert` (Model 2) and `dynamcop` (2A) use them,
  and **2B-CRX uses none at all across twelve games**. If v1 targeted 2B only, microtextures could be
  deferred; targeting 2C makes them unavoidable.
- **Every badly-behaved game in the set is 2C-CRX** — but "2C is broken" would overstate it. See
  the section below; 4 of the 6 2C games render fine, and the three failures have three different
  causes. Still: do not pick a 2C title as an early A/B target.

## How much should we trust 2C-CRX?

Evidence that 2C is the least mature variant:

- **100 % of 2C sets are `MACHINE_NOT_WORKING`** — 18/18 sets, 9/9 parents. Compare Model 2 33 %,
  2A 67 %, 2B 60 % of parents.
- **The TGPx4 core is explicitly incomplete.** `src/devices/cpu/mb86235/mb86235.cpp:7` lists: rewrite
  ALU integer/FP functions, rewrite FIFO hookups, *"illegal delay slots unsupported, and no idea
  about what is supposed to happen"*, externalize PDR.
- **Its DRC frontend hard-aborts on unimplemented instruction forms** — `fatalerror` in
  `describe_double_xfer1`, `describe_xfer2 MOV4`, `describe_double_xfer2`
  (`mb86235fe.cpp:668,729,737`). A plausible (unproven) cause of the `topskatr` SIGABRT.
- Our own sweep: 3 of 6 2C games misbehave, versus 0 of 23 on every other variant.

But the counter-evidence matters:

- **`MACHINE_NOT_WORKING` is not a 3D verdict.** It is widespread across all variants (2B: 71 % of
  sets) and usually concerns sound, analog inputs, link boards or protection. Every 2C game except
  `skisuprg` and `topskatr` rendered plausible 3D in the sweep — `bel`, `hotdo`, `segawski`,
  `waverunr` and `overrev` all look right.
- **The three failures have three unrelated causes**, only one of which implicates the copro:
  - `skisuprg` — flagged `MACHINE_UNEMULATED_PROTECTION`; sits on a drive-board error screen. An
    I/O-board problem, not a renderer or copro one.
  - `topskatr` — `model2_v.cpp:32` states Top Skater uses *"a slightly different geometry code that
    has a secondary transformation matrix"* which MAME's HLE geometrizer does not implement. That is
    a **geometrizer** limitation, sitting directly upstream of our seam.
  - `stcc` — 292k non-finite `1/z` vertices, cause not established.

**The upstream-sync risk this exposes.** The reason our seam is variant-independent is that MAME
currently uses the *HLE* geometrizer for every variant. But the driver's stated intent
(`model2_v.cpp:24-26`) is that 2B and 2C should eventually run their **real geometry DSP** with data
pushed into the Z-sort/clip/render stage, and the Top Skater note says that is the fix for its
breakage. If upstream ever lands that, the geometry *source* changes for 2B/2C even though the seam
itself (the rasterizer entry) should stay put. Worth watching on each release-tag merge.

Also note the Model 2 video code is **actively moving**: microtextures were only added in Oct 2025
(`#14433`) and front-to-back rendering with the fill buffer in Nov 2025 (`#14452`). Behaviour we
measure now may shift under us.

Highest polygon counts are on **2B-CRX** (`vonj` 4137/frame, `pltkids` 3812) — so 2B sets the buffer
sizing even though 2C sets the feature requirements.

## Method, and what not to trust

Each game ran 65 emulated seconds headless (`-video none -window -nomaximize -nothrottle -str 65`)
with the tap's run-summary enabled, plus PNG snapshots at 20/30/40/50/60 s via
[snap.lua](snap.lua). **Every game was then checked visually** against those snapshots — because a
Model 2 service screen or attract text card is itself drawn as hundreds of textured translucent
quads, and would otherwise pass as plausible geometry.

That check matters. Three games had to be handled specially:

| Game | What the pixels showed | Disposition |
| --- | --- | --- |
| `skisuprg` | **"DRIVE BOARD TROUBLE — PRESS TEST BUTTON"** for the entire run | Excluded. Rendered zero 3D, so it also wrote no summary at all. |
| `topskatr` | Renders ~87 frames of degenerate geometry (`1/z` 0…8.5e+37, buckets 0…65535) then stops; **aborts MAME (SIGABRT)** when snapshotting | Excluded. `model2_v.cpp:32` already notes Top Skater uses different geometry code that MAME does not implement properly. Not a valid A/B target. |
| `manxttc` | All five 20–60 s shots are text/logo cards. A later run (80/100/120 s) does reach real track geometry under leaderboard overlays | **Kept, but its ratios are unreliable** — UI-dominated. |

**Trust these columns:** `poly max`, `micro`, `checker`, `bad z`, `v5+`/`v6+`, `pages/f`, `pages`,
`bkt max`, `win`, `1/z` extremes. They are presence/maximum metrics, so attract-mode UI screens can
only make them conservative.

**Treat these as approximate:** `solid%`, `tex%`, `trans%`, `tie%`, `poly avg`. Every attract mode
mixes gameplay with 2D title cards, ranking tables and text, and text is drawn as translucent quads
— so the translucency and tie ratios are inflated by an unknown amount. `bel`, `manxttc`, `motoraid`,
`rchase2` and `waverunr` spent the most time on non-gameplay screens. Getting clean ratios needs
per-frame classification (or savestate fixtures parked in gameplay), which is a P1 job once the A/B
harness has fixtures.

**One gap in the instrumentation:** a game that renders no 3D at all never constructs the tap
singleton, so it writes *no* summary file rather than one saying `no_3d=1`. A missing summary is
therefore itself the signal — as it was for `skisuprg`. Worth closing when the tap moves behind the
P1 sink.

Per-game single-frame dumps (frame 800) are in [fixtures/](fixtures/).
