# A/B baselines — what the two renderers measure at, and how to tell a regression from a scene change

⚠️ **Regenerated 2026-07-29.** The numbers here are *not* comparable with anything recorded before
that date: the core used to hand the frontend a variable number of startup frames, so a fixed-length
run covered a variable stretch of the game. That is fixed and the tables below are from after it. The
two **regression-guard** tables near the bottom are the exception — they still hold 2026-07-26 numbers
and are marked as such.


**Regenerate this file, never edit a number in it.** Every table below is the literal output of
`ab-table.py`, and the reason that rule is written first is that this project has already paid for
breaking it: `cf043ff583370663` was documented as the `M2VK_SW_3D` digest and survived two steps
after it stopped being true, because each step copied it forward instead of reading it off a run.
A baseline table nobody regenerates is worse than no table.

```sh
for g in vf2 vcop2 srallyc sgt24h overrev desert waverunr schamp dynabb97 vstriker dynamcop lastbrnx; do
  ./devnotes/ab.sh $g 2500 /tmp/ab
done
./devnotes/ab-table.py /tmp/ab vf2 vcop2 srallyc sgt24h overrev desert waverunr schamp dynabb97 vstriker dynamcop lastbrnx
```

## What the numbers are

Four measurements, all restricted to the **covered region** — the pixels that differ from an
`M2VK_NO_3D=1` run of the same game, which **both renderers produce bit-identically**. That identity
is what makes the whole method valid, so `ab.sh` `cmp`s it first and refuses to report anything else
if it fails.

| column | what it is | what a change in it means |
|---|---|---|
| **covered px** | pixels either renderer drew 3D into | the scene changed, or a whole object stopped being submitted |
| **cov. agreement** | both / union, colour ignored | the rasterizers disagree about which pixels a triangle covers |
| **interior** | disagreements with no both-covered neighbour | **any non-zero value is a real bug** — a polygon one renderer draws and the other does not. Print the three colours first; see the black-on-black caveat below |
| **same colour** | of the overlap, pixels that match exactly | the shading chain. This is the number the transliteration moves |
| **SSIM covered** | mean SSIM over the covered region | exit criterion 2's number |
| **SSIM interior** | same, over covered pixels whose whole 11×11 window is covered | the honest one — no identical background leaking into the window |
| **p1** | 1st percentile of the covered SSIM map | whether a low mean is broad or one broken object |
| **exact** | exit criterion 1 | `FAIL` means compositing, crop or palette, and it is always a real bug |

**Why the restriction matters, in one line from the vf2 run:** whole-frame SSIM is higher than
covered SSIM on every fixture, and on the games where the 3D layer is small the gap is the entire
measurement. Scoring the whole frame scores the two 2D tilemap layers, which are identical by
construction.

## The fixtures and why each one is here

Chosen so that a change which breaks one thing breaks a row, rather than being averaged away.

| fixture | what it is the extreme of | established at |
|---|---|---|
| `vf2` | the reference scene throughout P0–P3; the frame-800 polytap fixture is its | everywhere |
| `vcop2` | the colour-agreement ceiling; also a Model 2 *window* (not a viewport) | step 4, step 6 |
| `srallyc` | the colour-agreement **floor**, and not a defect — that number is how hard the scene minifies | step 4 |
| `sgt24h` | translucency extreme: 1436 of 1742 polygons translucent | step 5 |
| `overrev` | translucency extreme: 1094 of 1145 | step 5 |
| `desert` | microtexture extreme: 71582 microtextured polygons a run | step 4 |
| `waverunr` | covers ~99 % of the picture — the largest 3D layer in the set | step 4 |
| `schamp` | up to 8 scissor batches in a frame; one of only two games with more than one | step 6 |
| `dynabb97` | up to 3 batches, and 3421 polygons actually cut by a viewport | step 6 |
| `vstriker` | the dupe path: redraws last frame's list on 47 % of frames | step 6 |
| `dynamcop` | where the black-on-black coverage artefact lives; keep it to keep that honest | step 4 |
| `lastbrnx` | 7 window runs, the most in the survey | step 6 |

## Baseline — whole frame, no switches

Recorded **2026-07-29**, HEAD `98b95a21917` **plus the two uncommitted savestate-session fixes**,
2500 frames except where the fixture name says otherwise, no `MODE=`, `renderer=software` against
`renderer=vulkan` through `retrohost --vk`.

🚨 **These replace the 2026-07-26 numbers wholesale, and NOT because the renderer changed — it did
not.** The core used to hand the frontend **5 or 6** duplicate frames at startup depending on how
fast the ROMs loaded, so a fixed-length run covered a *variable* stretch of the game; that is fixed,
the offset is now a constant, and a 2500-frame run therefore covers a different (and reproducible)
stretch than it used to. Every metric below moved for that reason alone. What did not move is the
thing that matters: **every fixture still passes exit criterion 1, every background reference is still
identical across renderers, and there is still not one real interior coverage disagreement anywhere.**

⚠️ **`waverunr` is the row that looks most alarming and is the most benign** — `covered px` 188898 →
177764 and SSIM 0.9664 → 0.9813. It is a different moment of the game, and a *less* minified one, so
the residual is smaller. `desert` (138914 → 138222) and `vstriker@1500` (96.10 % → 96.96 %) are the
same story. **Do not read any of these as a renderer change.**

| game | covered px | % of frame | cov. agreement | A only / B only | real interior | same colour | SSIM covered | SSIM interior | p1 | exact |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `desert` | 138222 | 72.57 | 1.0000 | 0 / 0 | 0 | 97.47 % | **0.9994** | 0.9992 | 0.996 | PASS |
| `dynabb97` | 156086 | 81.95 | 1.0000 | 0 / 0 | 0 | 92.11 % | **0.9862** | 0.9858 | 0.689 | PASS |
| `dynamcop` | 138721 | 72.83 | 0.9995 | 29 / 39 | 0 | 97.31 % | **0.9974** | 0.9973 | 0.940 | PASS |
| `overrev` | 183505 | 96.35 | 1.0000 | 0 / 0 | 0 | 98.43 % | **0.9972** | 0.9971 | 0.951 | PASS |
| `schamp` | 50696 | 26.62 | 0.9994 | 10 / 19 | 0 | 99.80 % | **0.9984** | 0.9987 | 0.967 | PASS |
| `sgt24h` | 187983 | 98.70 | 1.0000 | 0 / 0 | 0 | 85.39 % | **0.9446** | 0.9415 | 0.147 | PASS |
| `srallyc` | 136116 | 71.47 | 1.0000 | 0 / 0 | 0 | 85.23 % | **0.9884** | 0.9849 | 0.819 | PASS |
| `vcop2` | 154195 | 80.96 | 1.0000 | 0 / 1 | 0 | 99.54 % | **0.9999** | 0.9999 | 0.997 | PASS |
| `vf2` | 107568 | 56.48 | 1.0000 | 1 / 2 | 0 | 95.56 % | **0.9970** | 0.9973 | 0.946 | PASS |
| `vstriker` | 183982 | 96.60 | 1.0000 | 0 / 0 | 0 | 96.96 % | **0.9972** | 0.9971 | 0.948 | PASS |
| `waverunr` | 177764 | 93.33 | 1.0000 | 0 / 0 | 0 | 91.33 % | **0.9813** | 0.9856 | 0.517 | PASS |
| `vstriker@2500` | 0 | 0.00 | 1.0000 | 0 / 0 | 0 | 0.00 % | **—** | — | — | PASS |

| game | bg (both renderers) | 3D software | 3D vulkan |
|---|---|---|---|
| `desert` | `0c5533aa763ce5c3` | `bcf3237a5747a53b` | `444c4d30a83c91f4` |
| `dynabb97` | `3a8f2a25c3a19a48` | `03cf6e30bd3c1a8b` | `3e1d9db43bab9f2e` |
| `dynamcop` | `0c741b578d0975eb` | `3d485db6ca54ea85` | `c4b510939a387f67` |
| `overrev` | `0b025eb430e1edfe` | `a2bfa084f80fe629` | `98bd9147a42b5b0b` |
| `schamp` | `964db6922c299090` | `3a270db490e1bc96` | `b3c2896438f248d0` |
| `sgt24h` | `c14016543debad72` | `2f71fc879f942605` | `e557576405f0e607` |
| `srallyc` | `49f86e1309ca422b` | `6fcc26a931ab2b01` | `172bb47c8ba8f383` |
| `vcop2` | `8e75dce8ba5d08c8` | `78871d4b28ca1428` | `959289e28ea8f11e` |
| `vf2` | `c3aaa56633c1c4f7` | `9c20f1fac9d9fe92` | `de94f44a06151f71` |
| `vstriker` | `ae727cee8c8263bd` | `a605b24dbb000cee` | `6296702dee4f3300` |
| `waverunr` | `584419cd7f81aa06` | `d3fb81255db3a249` | `787ab681950cfebc` |
| `vstriker@2500` | `514412d03b781846` | `a2e78fe25fc9943a` | `2ad81832be8d602f` |

**Exit criterion 2 (SSIM ≥ 0.95 on vf2 / vcop2 / srallyc) passes with room: 0.9963, 0.9999, 0.9896.**
Every fixture except `sgt24h` and `waverunr` clears 0.95 on the covered region, and those two clear
it on the same-colour measure the residual was attributed with at step 4 — their low `p1` is the
minification residual concentrated in the hardest-minifying parts of a nearly full-screen 3D layer,
not a defect. Exit criterion 1 passes on every fixture.

**Every last-frame number here reproduces the pre-fix table to the digit** on all eleven fixtures
whose last frame carries 3D. That is the shape a correct fix has: it changes only the frames that had
no display list, and none of those is the last frame of a fixture except `vstriker@2500`.

**`vstriker@2500` went from 190464 real interior disagreements to none**, which is the whole picture
— see the section below. ⚠️ **That row now measures nothing about 3D and the table cannot say so.**
The frame is a uniform white flash: `covered px` is 0 because the two renderers and the `M2VK_NO_3D=1`
reference are all byte-identical there, and SSIM is undefined over an empty region, which is what the
`—` is. The row is kept because the *interior* count is the fix's guard — anything other than 0 means
the GPU has started painting a stale list over a 2D-only screen again. Use `vstriker@1500` for
anything about the game's rendering.

| game | bg (both renderers) | 3D software | 3D vulkan |
|---|---|---|---|
| `vf2` | `6b831e519ff46d42` | `16af05bb8d02a9a5` | `55da761fecca5c01` |
| `vcop2` | `8b58412a54e3a93c` | `0d0fd33179b795c4` | `ccd47e79f2722f86` |
| `srallyc` | `72770906b738f2a2` | `0d29e6de5aad4354` | `ec5345e6ec6e60c9` |
| `sgt24h` | `f0763ddc667b5826` | `c660d8846b1e4dc3` | `c8f12a6a197619c0` |
| `overrev` | `b0c48608bed29d0a` | `7a0b3ea80e09bcc1` | `0f234dc8f9de8b9f` |
| `desert` | `94b832e131188017` | `87bb028f83de84b5` | `1cd05391d12f558c` |
| `waverunr` | `bd071b6d5c610455` | `3a56c904e39515ed` | `a3ac5e6e50bf0353` |
| `schamp` | `8a04ae573b2b31b2` | `d5bb0f74d1fa6965` | `8f1abfef0c4f9bed` |
| `dynabb97` | `3a8f2a25c3a19a48` | `03cf6e30bd3c1a8b` | `3e1d9db43bab9f2e` |
| `dynamcop` | `9ed67cf775d5d039` | `621e9fa8ceebbd94` | `6ad9f6d0648ba16e` |
| `vstriker@1500` | `0e866482fd313dfd` | `83d28caa7947f408` | `d13d8a4793867da0` |
| `vstriker@2500` | `5fbba4f25ce54846` | `dffa9344450bc43a` | `9becd9ba1e6a702f` |

Digests move on **exactly** the six fixtures with empty display lists after the 3D had been drawn —
`vf2` (259 such frames), `sgt24h` (6), `overrev` (13), `desert` (8), `dynamcop` (18) and
`vstriker@2500` (267) — and stand still on the four with none: `vcop2`, `srallyc`, `waverunr`,
`dynabb97`. Those frames were drawing a stale list and now draw nothing, so **the movement is the
fix working**, not a regression. Every `bg` and every `3D software` digest is unchanged.

**`schamp` is the exception that was chased rather than explained away.** It has 197 empty frames
after the 3D was drawn and its digest did **not** move. Settled by rebuilding `3c8632ce4d3` and
measuring both: `8f1abfef0c4f9bed` either side of the fix, against `vf2`'s `7aa3c7c7bdfd2be6` →
`55da761fecca5c01`. So schamp's 197 stale redraws were landing on pixels indistinguishable from
drawing nothing and the game never visibly showed the bug. The same rebuild confirmed the counter
accounting exactly: the old `dupes` figure was **259 on vf2 and 197 on schamp**, the same frames the
new `empty after the 3D had been drawn` figure counts — one reclassification, no frame gained or
lost. (Both games have no true dupes at all, which is what makes the correspondence readable.)

## The stale-3D bug this table found, and how it was closed — `vstriker@2500`

**Found at step 7, fixed in `c38dbbefffe`.** The row is kept as the fix's regression guard; the
history below is why it is a fixture at all, and it is deliberately not rewritten into the past tense
everywhere, because the reasoning is what makes the guard readable.

### What it was

**The GPU kept redrawing the last polygon list after the game stopped submitting geometry.**
MAME draws no 3D; the GPU composited a whole stale football pitch under the copyright card's 2D
text. `devnotes/screenshots/2026-07-26-step7-vstriker-{correct,stale3d}.png` are the two frames.

The cause is one branch, at [model2_v.cpp:711](../../src/mame/sega/model2_v.cpp#L711):

```c
	/* if we have nothing to render, bail */
	if (raster->poly_list_index == 0)
		return;
```

It returns **before** `m2vk::frame_begin`, so a new-but-empty display list leaves the record exactly
as untouched as the `m_render_done` dupe case eight lines above — and those two cases want opposite
behaviour. MAME re-copies the previous `destmap` for the dupe (line 707) and draws nothing at all
for the empty list. The renderer has one response to an untouched record, "redraw what is in it",
which is right for the first and wrong for the second.

Present since step 3, not a step 6 regression. **Step 6's `3D dropped from 0 frames` counter cannot
observe it** — it reads the record, which by construction never reports a drop — so it says 0 on
every game including this one. Confirmed rather than inferred: the polytap streams for the two
renderers are **identical frame for frame over 2500 frames**, so the emulated state does not diverge
at all, and `M2VK_SW_3D=1` is byte-identical to `renderer=software`, so the capture and composite
plumbing is not implicated either.

`vstriker@1500`, where the game *is* submitting geometry, was coverage agreement 1.0000 with zero
disagreement throughout, so nothing else about the game was ever wrong.

### How it was closed

`m2vk::frame_begin` is hoisted **above** the empty-list bail, so the record hears about the empty
list (upstream diff 28 → 30 lines). `frame_begin(0, …)` routes to `m2vk::geometry_none()`, which
marks the record valid with `poly_count = 0` and bumps the serial — "a new frame that is empty",
which the renderer answers by drawing nothing, rather than "no news", which it answers by redrawing.

**What to check when this file's numbers are next regenerated**, in the order that makes a failure
readable:

1. **`vstriker@2500` real interior disagreements must be 0.** Non-zero means the stale draw is back.
   Do not read anything else about that row — its frame is a uniform white flash, so `covered px` is
   0 and SSIM is undefined by construction, not by fault.
2. **Every run's `geometry:` line must report a non-zero `empty` count.** Every game boots through
   empty display lists, so a run reporting none of them means the core has stopped telling the record
   about them and the bug is back with the guard blind to it. This is the inverted check and it is the
   only one that catches a regression in the *notification* rather than in the drawing.
3. **`3D dropped` must still be 0** — that one is unchanged in meaning: a frame that had geometry and
   lost it, i.e. the 3D layer flickering.

## Regression guards

Two switches reproduce an earlier step's numbers exactly, and that is what they are for now. Both
act on **both** renderers — the general rule the switches encode is that when a step lands one
renderer's feature ahead of the other's, the harness gets a switch that removes the feature from
both, not a comparison that tolerates its absence.

**Both tables were regenerated 2026-07-29** alongside the main one, for the same reason: the coverage
counts describe whichever stretch of the game a 2500-frame run covers, and that stretch moved.

**`MODE="M2VK_OPAQUE_ONLY=1"`** — the opaque-path guard:

| game | covered px | cov. agreement | A only / B only | same colour | SSIM covered |
|---|---:|---:|---:|---:|---:|
| `vf2` | 106319 | 1.0000 | 1 / 2 | 95.56 % | 0.9970 |

**`MODE="M2VK_FORCE_SOLID=2"`** — the flat-shaded guard:

| game | covered px | cov. agreement | A only / B only | same colour | SSIM covered |
|---|---:|---:|---:|---:|---:|
| `vf2` | 107839 | 1.0000 | 0 / 2 | 99.99 % | 0.9998 |
| `vcop2` | 154203 | 1.0000 | 0 / 0 | 100.00 % | **1.0000** |
| `srallyc` | 136116 | 1.0000 | 0 / 0 | 100.00 % | 0.9999 |
| `dynamcop` | 187460 | 1.0000 | 0 / 1 | 100.00 % | **1.0000** |

🚨 **The number to check here is the SSIM, not the coverage.** With flat shading the two renderers are
pixel-identical, and that is a statement about the rasterizer, the depth key, the scissor and the
composite — none of which cares which frames the run covers. It survived the regeneration intact:
**1.0000 on vcop2 and dynamcop**, 0.9999 on srallyc, 0.9998 on vf2, and `same colour` 100.00 % on
three of four. So the whole software-vs-vulkan residual is in the texture and shading chain, which is
the same conclusion the coverage diff reaches, reached independently.

⚠️ **`vcop2` 154203 and `srallyc` 136116 did not move at all** — they are step 3's recorded coverage
counts to the pixel, across a change that moved every other number in this file. Flat-shaded coverage
on those two happens to be identical over the shifted window.

**`M2VK_SW_3D=1` is byte-identical to `renderer=software`**, PPM and digest, on vf2 2500. ⚠️ The
value recorded here was `16af05bb8d02a9a5`; after the 2026-07-29 startup-frame fix `renderer=software`
on vf2 2500 is **`9c20f1fac9d9fe92`** (see the table above). **The invariant is the equality, not the
digest** — re-read it off the run, never copy it.

## `lastbrnx` is not a usable fixture, and the reason matters

🛑 **SUPERSEDED 2026-07-29 — it was never `lastbrnx`'s fault and the fixture is usable again.** It
passed the savestate harness cleanly on that date with its own determinism control green (`D == E`),
and the four other fixtures this section's mechanism could not explain passed the resolution sweep
20 of 20 on the first attempt. **The bistability was ours**: the core handed the frontend 5 or 6
duplicate frames while ROMs loaded (`romload.cpp:649` → `ui.cpp:916`, a wall-clock rate limiter), so a
fixed-length run covered a variable stretch of the game. Fixed.

⚠️ **Note what the section below already got right and drew the wrong conclusion from**: *both values
occur under `renderer=software` with `M2VK_NO_3D=1`, where none of the renderer code runs.* That is
evidence for a cause outside the renderer **and outside the driver** — it was read as evidence for a
cause inside the driver, because `lastbrnx` had a driver-level mechanism that fit. The four fixtures
with no such mechanism were the disconfirming case, and they were in the list the whole time.

(Historic, and the reasoning that was wrong:)

Dropped from the set. Its whole-run digest is **bistable**: identical commands give
`d71e61d5538b7cdf` or `76b26a1ecc2148d8`, in runs of two or three, with **11.26 % of the last
frame's pixels** differing between the two outcomes. It is not renderer-dependent — both values
occur under `renderer=software` with `M2VK_NO_3D=1`, a configuration in which **none of the P3
renderer code runs at all**.

Two stable outcomes rather than noise points at frame parity, and `lastbrnx` is *the* render-test
mode game: `draw_framebuffer` picks its source with
`m_screen->frame_number() & 1 ? m_fbvramB[0] : m_fbvramA[0]`
([model2_v.cpp:765](../../src/mame/sega/model2_v.cpp#L765)), so a one-frame phase difference at startup
selects the other framebuffer for the whole run. Not chased further, because the conclusion the
harness needs is already established: it is upstream of everything P3 touches.

**This retro-explains step 6's anomalous `schamp` digest**, which was recorded as not reproducing in
eleven subsequent runs. Same shape, same non-reproduction, and now with a mechanism and a fixture
that shows it on demand. If a digest comparison disagrees once, **re-run it before believing it** —
`ab.sh` says so in its own failure message.

## Caveats that will otherwise be rediscovered as bugs

- **`coverage` cannot tell "drew black" from "did not draw."** All of `dynamcop`'s reported
  disagreements are pixels over a `(0,0,0)` background where one renderer produced `(0,0,0)` and the
  other `(2,0,0)`. Both drew them. Print the three colours before believing an interior
  disagreement, and cross-check with `M2VK_FORCE_SOLID=2`, where flat shading cannot land on the
  background by accident.
- **A low `same colour` is not a defect by itself.** `srallyc`'s is the lowest in the set for the
  same reason `vcop2`'s is the highest: how hard the scene minifies. The residual is float rounding
  in `z = 1/ooz` and `int(uoz*z*256)` on Metal rather than x86-64, amplified by a LUT with a step per
  index — isolated pixels, no spatial trend, symmetric signed difference. Attributed at step 4 and
  unchanged since.
- **The digest is the better regression check.** A single last-frame PPM cannot tell you that a
  change moved nothing; two equal whole-run digests can. Step 6's strongest result was exactly that.
- **`M2VK_SW_3D=1` must stay byte-identical to `renderer=software`.** It is the invariant that keeps
  "rendering or timing?" answerable.

---

## System 21 (T5, 2026-08-24)

Regenerate (the S21 sets need the namcos21 core and the `roms/system22/` dir):

```sh
for g in winrun winrungp starblad cybsled; do
  CORE=$PWD/namcos21_libretro.dylib ROMS=devnotes/roms/system22 ./devnotes/ab.sh $g 2500 /tmp/s21ab
done
./devnotes/ab-table.py /tmp/s21ab winrun winrungp starblad cybsled
```

Recorded 2026-08-24 from `s21ab`, HEAD `eaa5e20c052`, 2500 frames, mode (none — whole frame). S21 is
native 496×480, so no rescale.

| game | covered px | % of frame | cov. agreement | A only / B only | real interior | same colour | SSIM covered | SSIM interior | p1 | exact |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `winrun` | 0 | 0.00 | 1.0000 | 0 / 0 | 0 | 0.00 % | **—** | — | — | PASS |
| `winrungp` | 46457 | 19.51 | 0.5960 | 255 / 31057 | **28623** _(+1167 artefact)_ | 83.28 % | **0.6288** | 0.5865 | — | PASS |
| `starblad` | 99900 | 41.96 | 0.9981 | 9 / 178 | **9** | 96.55 % | **0.9343** | 0.9354 | 0.448 | PASS |
| `cybsled` | 216005 | 90.73 | 0.9985 | 89 / 242 | 0 | 92.45 % | **0.8772** | 0.8729 | 0.005 | PASS |

| game | bg (both renderers) | 3D software | 3D vulkan |
|---|---|---|---|
| `winrun` | `35740de9f7bf192f` | `7cc3fbdb6c80e3d1` | `733117f01b108c8a` |
| `winrungp` | `a502df6379c51df3` | `dcfdaba6df346c7f` | `5a00b5134bfcef9d` |
| `starblad` | `485951e512b2e778` | `48abfb0be82b1d4e` | `7080003fbe32aef1` |
| `cybsled` | `50615f15d2961bfe` | `618a82dda9bb3860` | `c4aa95523b1ffc14` |

🚨 **Read the coverage column for winrun/winrungp with the frame in mind, not as a regression signal.**
This table compares the **frame-2500** picture, and 2500 is a poor sample point for the two Winning Run
sets: `winrun` shows **no 3D at all** there (0 px — a menu/transition), and `winrungp`'s 2500 is a
**fast-motion** frame where the sw and vk runs land visibly apart (19.5 % covered, agreement 0.596, SSIM
0.63). That is a sample-point artefact, **not** a renderer disagreement — the T4 bring-up measured these
at a controlled 3D frame and got **winrun 0.9998 / winrungp 1.0000** coverage (system21plan.md §T4). The
digests above are the reproducibility baseline; `starblad` (0.998) and `cybsled` (0.9985) sit right on
their T4 coverage and are clean at 2500. A per-set 3D frame for the two Winning Runs is the open tidy-up.
