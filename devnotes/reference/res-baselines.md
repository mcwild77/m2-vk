# Resolution invariance — what the depth path measures at 2×, 3× and 4×

⚠️ **Regenerated 2026-07-29** — both the box and point tables, 20 runs, all passing. The numbers are
*not* comparable with anything recorded before that date: the core used to hand the frontend a
variable number of startup frames, so a fixed-length run covered a variable stretch of the game. That
is fixed.


**Regenerated 2026-07-28 after the `checker` stipple fix**; originally P4 step 2's measurement, taken
2026-07-27 at HEAD `11b5ce4ca1c`. 10 fixtures, 2500 frames each, `renderer=vulkan` throughout — this
compares the Vulkan renderer against *itself* at another internal resolution, not against the software
one. That is [ab-baselines.md](ab-baselines.md)'s job and the two files answer different questions.

⚠️ **Regenerate with `res-table.py`, never retype a number.** Same rule and the same reason as
ab-baselines.md: a hand-copied figure survives a rerun that would have changed it, and it is the
copied one that gets quoted.

## 🚨 Read this before regenerating: FIVE of the ten fixtures are bistable

🛑 **FIXED 2026-07-29, AND THE DIAGNOSIS BELOW WAS WRONG.** The 2026-07-29 regeneration ran **20 of
20 clean on the first attempt**, including all five fixtures named below — where the previous
regeneration had 4 of 23 runs fail outright and a fifth pass while measuring nothing. The section is
kept because its *observations* were right and its rules are still good practice; only the mechanism
was misidentified.

**It was not frame parity.** `draw_framebuffer`'s `m_screen->frame_number() & 1` only runs in render
test mode, which is `lastbrnx` — it never could have explained `waverunr`, `dynamcop`, `overrev` or
`schamp`, and that mismatch was visible in the evidence at the time. The real cause was ours and it hit
**every** set: the core handed the frontend **5 or 6** duplicate frames while ROMs loaded
(`romload.cpp:649` → `ui.cpp:916`, a tenth-of-a-wall-clock-second rate limiter), so a fixed-length run
covered a *variable* stretch of the game. See the savestate NEXT STEP block in CLAUDE.md.

⚠️ **The tell the section already named — a moved `covered 1x` — is exactly right and is now a clean
check**: every `covered 1x` in the tables below agrees with `ab-baselines.md` to the pixel.

(Historic, and the reasoning that was wrong:)
`lastbrnx`, `schamp`, `waverunr`, `dynamcop` and `overrev` each have **two** stable whole-run digests
and pick between them per run — frame parity, `draw_framebuffer` choosing its source with
`m_screen->frame_number() & 1` (`model2_v.cpp:765`). `res.sh` compares a 1× run with an n× run, so
**an affected fixture fails whenever the two land on opposite sides, about half the time.** The
2026-07-28 regeneration had 4 of 23 runs fail outright and a fifth pass while measuring nothing.

- **Re-run until the 1× and n× runs agree.** That is all it takes; the values themselves are stable.
- 🚨 **`res.sh` guards the background reference and NOT the 3D 1× reference**, so a bistable fixture
  can produce a *passing*, plausible, meaningless row — `overrev`'s 3× point read 36.801 % against a
  99.853 % baseline that way. **The tell is a moved `covered 1x`**: nothing about a supersampling
  change can touch a 1× frame, so if that column moved, the run is bad rather than the renderer.
  Worth fixing in `res.sh`.
- ⚠️ **Two samples cannot distinguish bistable from deterministic.** Two re-runs reproducing a failure
  byte-identically is not evidence of a real bug. **The decisive test is building the parent commit
  into a second dylib and running both** — if both values occur on both binaries, it is the coin.

```sh
./devnotes/res.sh vf2 2500 "2 4" /tmp/res              # box resolve, the "what does it look like" run
POINT=1 ./devnotes/res.sh vf2 2500 3 /tmp/res-point    # point resolve, the run that carries the claim
MODE=M2VK_FORCE_SOLID=2 POINT=1 ./devnotes/res.sh vcop2 2500 3 /tmp/res-solid
./devnotes/res-table.py /tmp/res /tmp/res-point /tmp/res-solid > tables
```

## How to read these

`A` is the 1× render and `B` the supersampled one, both resolved to 496×384, both compared against the
same `M2VK_NO_3D=1` background reference — which `res.sh` requires to come back **bit-identical at
every scale** before it measures anything else.

- **`A only`** — pixels the 1× render's 3D covered and the supersampled one did not.
- **`B only`** — the antialiased fringe. Expected, grows with the scale.
- **`interior`** — coverage disagreements with no both-covered neighbour, after the drew-black
  artefact is discounted.
- **`same colour`** — of the pixels both covered, how many are bit-identical.

🚨 **Quote a POINT run when claiming invariance. A box resolve moves the sample points and cannot
carry the claim.** The 2× subpixel centres are at ±0.25 and the 1× centre at +0.5 is not among them,
so sub-sample-width geometry can be gained *or lost*: desert loses a one-pixel-wide mast, 7 pixels at
2× and 1 at 4×, and 0 at 3× point. An **odd** scale resolved by the centre subpixel keeps the 1×
sample point as one of its samples — `n*x + (n-1)/2` has centre `x + 0.5` — so its coverage is
comparable directly. `res.sh` enforces `ppmdiff`'s interior verdict only on point runs for this
reason, and prints it as information on box runs.

⚠️ **`same colour` is meaningful only for the point resolve.** Under a box filter every polygon edge
and every minified texel legitimately changes, so 12–83 % is the expected range and says nothing.

## The result

**The depth path is resolution-invariant.** At 3× point, on the 2026-07-28 regeneration: `A only` is 0
on **9 of 10** fixtures, coverage agreement is 1.0000 on **9 of 10**, 98.97–99.95 % of covered pixels
are bit-identical, and there are **0 interior coverage disagreements anywhere**. The 12 `A only` pixels
are all dynamcop's, each on a silhouette edge with 2–7 both-covered neighbours.

⚠️ **That is 9 of 10 where P4 step 2 measured 8 of 10, and the difference is not an improvement** —
`schamp`'s 2/1 became 0/0 because this run landed on the other side of its frame parity. Both are
correct measurements of that fixture; it simply has two.

**The residual has no shape, which is the part that rules out an ordering change** — a depth change
takes a region, not scattered pixels. Connected components of the 3× point residual, **measured at P4
step 2 and not regenerated on 2026-07-28**: the point-resolve rows are unchanged by the stipple fix
(for odd n the parity survived by accident, so those runs never saw the bug), so the clustering it
describes still holds.

| | vf2 | vcop2 | srallyc | sgt24h | desert | waverunr | overrev | dynabb97 | schamp | dynamcop |
|---|---|---|---|---|---|---|---|---|---|---|
| differing px | 774 | 402 | 705 | 1089 | 1426 | 1000 | 270 | 983 | 19 | 252 |
| clusters | 688 | 247 | 645 | 1010 | 1259 | 873 | 261 | 885 | 19 | 248 |
| **largest** | **5** | 129 | **6** | **4** | **5** | **7** | **2** | **7** | **1** | **2** |

vcop2's 129 is a **one-pixel-wide vertical line at x=485** going `(125,125,125)` → `(133,133,133)`:
one step along the `colorxlat` ramp on a sliver seen edge-on, i.e. the float-rounding residual
ab-baselines.md already attributes. Shading, not depth — and the flat-shaded vcop2 run below has one
differing pixel in the entire frame.

## ✅ The `checker` stipple WAS not resolution-invariant — fixed 2026-07-28

One vcop2 checkered quad (`M2VK_ONLY_POLY=114`, frame 1804), drawn alone, before and after:

| | pixels drawn (before) | same colour as 1× | pixels drawn (**after**) | same colour as 1× |
|---|---|---|---|---|
| 1× | 78968 — half the hull, the screen door | — | 78968 | — |
| **2× box** | **157945 — exactly twice, the whole hull** | **0.000 %** | **78968** | **100.000 %** |
| 3× point | 78968 | 100.000 % | 78968 | 100.000 % |

**The symptom was worse than "a grey dither"**, which is how `p3-hw-geometry.md` §9 described it: at an
**even** scale a box resolve of a finer checkerboard covers *every* output pixel, so the polygon went
fully opaque. The screen door did not soften, it disappeared — so the dither-vs-real-alpha decision that
was supposed to be forced here never existed.

**The fix is one integer divide in `poly.frag`**: test the parity in **picture** pixels,
`ivec2(gl_FragCoord.xy) / scale`, so all n² subpixels of a picture pixel share a parity. The scale
arrives on the push constant block, which grew a third word and both stage flags. At scale 1 the divide
is by 1, so 1× output is bit-exact unchanged — checked, not argued: `ab.sh vf2 2500` reproduces both
digests byte-exactly.

⚠️ **The 3× point row was never evidence of invariance and still is not.** The stipple is `(x ^ y) & 1`
and for odd n, `(n*x + (n-1)/2) + (n*y + (n-1)/2) ≡ x + y (mod 2)` — the parity survived by accident of
the odd scale, which is why that row did not move. **Use a box resolve to see the stipple; the point
resolve hides it.** Before the fix, what sat on the oversized attachment was a stipple n times finer
than the hardware's; now it is the hardware's, at every scale.

**Where it shows in the tables below:** the fixtures with checkered polygons gained, and only under a
box resolve — `vcop2` 2× box `same colour` 44.158 % → 71.973 % with `B only` 3749 → 38 and SSIM
0.5754 → 0.9828; `overrev` and `desert` gained several points; the fixtures with no checkered polygons
(`dynabb97`, `dynamcop`, `vf2`) reproduce their old rows to the digit, which is the guard.

### ✅ The hand-off to P5 is answered, and the answer is that the divisor is per-frame

**Everything above is still exactly right for `M2VK_SS`, and it is now the only thing it is right
for.** P5 (2026-07-28, [p5-internal-resolution.md](../plan_finished/p5-internal-resolution.md)) made
`model2_internal_res` a *real* internal resolution — the frame is handed to the frontend at the size
it was drawn — and there the same divide would be wrong: nothing is averaged, so a picture-pixel door
would just be a magnified screen door.

So `pc.scale` became **`pc.stipple_div`** and the renderer sets it per frame: `s_ss` when resolving
back down (this file's case, unchanged), **1** when presenting as drawn. Measured on this section's
own fixture — the quad covers **57.401 %** of the frame at native and **57.422 %** at 1440×1080, so it
is still a half-covering door, at three times the frequency.

⚠️ **The tables below are unaffected and were not regenerated for P5.** The check that says so is
stronger than a re-run: a real 3× frame point-downsampled is **bit-identical to
`M2VK_SS=3 M2VK_SS_POINT=1` on all ten fixtures**, checkered polygons included — for odd n the centre
subpixel's parity is the 1× pixel's under *either* divisor, which is the same accident this section
already warns about.


### box resolve

| game | scale | covered 1x | covered Nx | A only | B only | interior | same colour | agreement | ssim covered | exact |
|---|---|---|---|---|---|---|---|---|---|---|
| desert | 2x | 138222 | 138462 | **5** | 245 | **5** | 14.459 % | 0.9982 | 0.9030 | pass |
| desert | 4x | 138222 | 138577 | **1** | 356 | **16** | 13.100 % | 0.9974 | 0.9218 | pass |
| dynabb97 | 2x | 156086 | 156086 | **0** | 0 | **0** | 39.479 % | 1.0000 | 0.8857 | pass |
| dynabb97 | 4x | 156086 | 156086 | **0** | 0 | **0** | 34.992 % | 1.0000 | 0.9100 | pass |
| dynamcop | 2x | 138731 | 143059 | **176** | 4504 | **41** | 42.612 % | 0.9673 | 0.9657 | pass |
| dynamcop | 4x | 138731 | 143695 | **138** | 5102 | **45** | 38.705 % | 0.9636 | 0.9715 | pass |
| overrev | 2x | 183505 | 183505 | **0** | 0 | **0** | 52.773 % | 1.0000 | 0.9626 | pass |
| overrev | 4x | 183505 | 183505 | **0** | 0 | **0** | 48.288 % | 1.0000 | 0.9685 | pass |
| schamp | 2x | 50705 | 52013 | **0** | 1308 | **7** | 83.246 % | 0.9749 | 0.9547 | pass |
| schamp | 4x | 50705 | 52596 | **0** | 1891 | **12** | 76.296 % | 0.9640 | 0.9596 | pass |
| sgt24h | 2x | 187983 | 187983 | **0** | 0 | **0** | 37.141 % | 1.0000 | 0.8795 | pass |
| sgt24h | 4x | 187983 | 187983 | **0** | 0 | **0** | 33.754 % | 1.0000 | 0.9099 | pass |
| srallyc | 2x | 136116 | 136116 | **0** | 0 | **0** | 37.853 % | 1.0000 | 0.9246 | pass |
| srallyc | 4x | 136116 | 136116 | **0** | 0 | **0** | 36.341 % | 1.0000 | 0.9345 | pass |
| vcop2 | 2x | 154196 | 154232 | **2** | 38 | **0** | 71.973 % | 0.9997 | 0.9828 | pass |
| vcop2 | 4x | 154196 | 154263 | **0** | 67 | **0** | 69.253 % | 0.9996 | 0.9878 | pass |
| vf2 | 2x | 107569 | 107929 | **0** | 360 | **0** | 31.358 % | 0.9967 | 0.9018 | pass |
| vf2 | 4x | 107569 | 108136 | **0** | 567 | **1** | 25.875 % | 0.9948 | 0.9202 | pass |
| waverunr | 2x | 177764 | 177764 | **0** | 0 | **0** | 41.625 % | 1.0000 | 0.9611 | pass |
| waverunr | 4x | 177764 | 177764 | **0** | 0 | **0** | 41.624 % | 1.0000 | 0.9682 | pass |

### point resolve

| game | scale | covered 1x | covered Nx | A only | B only | interior | same colour | agreement | ssim covered | exact |
|---|---|---|---|---|---|---|---|---|---|---|
| desert | 3x | 138222 | 138222 | **0** | 0 | **0** | 99.076 % | 1.0000 | 0.9997 | pass |
| dynabb97 | 3x | 156086 | 156086 | **0** | 0 | **0** | 99.370 % | 1.0000 | 0.9994 | pass |
| dynamcop | 3x | 138731 | 138731 | **8** | 8 | **0** | 99.757 % | 0.9999 | 0.9998 | pass |
| overrev | 3x | 183505 | 183505 | **0** | 0 | **0** | 99.791 % | 1.0000 | 0.9997 | pass |
| schamp | 3x | 50705 | 50704 | **2** | 1 | **0** | 99.968 % | 0.9999 | 0.9999 | pass |
| sgt24h | 3x | 187983 | 187983 | **0** | 0 | **0** | 99.434 % | 1.0000 | 0.9994 | pass |
| srallyc | 3x | 136116 | 136116 | **0** | 0 | **0** | 99.464 % | 1.0000 | 0.9993 | pass |
| vcop2 | 3x | 154196 | 154196 | **0** | 0 | **0** | 99.739 % | 1.0000 | 1.0000 | pass |
| vf2 | 3x | 107569 | 107567 | **2** | 0 | **0** | 99.574 % | 1.0000 | 0.9997 | pass |
| waverunr | 3x | 177764 | 177764 | **0** | 0 | **0** | 99.478 % | 1.0000 | 0.9999 | pass |

⚠️ **The `M2VK_FORCE_SOLID=2` table below is the ONLY one here that is still 2026-07-26 numbers**, and
it has not been re-run because it needs its own `MODE=` sweep. Its *purpose* survives regardless: with
flat shading the 1× and n× renders should stay coverage-identical with SSIM 1.0000, which is a claim
about the depth path and not about which frames the run covers.

### point resolve  —  M2VK_FORCE_SOLID=2

| game | scale | covered 1x | covered Nx | A only | B only | interior | same colour | agreement | ssim covered | exact |
|---|---|---|---|---|---|---|---|---|---|---|
| dynamcop | 3x | 187571 | 187571 | **0** | 0 | **0** | 99.999 % | 1.0000 | 1.0000 | pass |
| srallyc | 3x | 136116 | 136116 | **0** | 0 | **0** | 99.998 % | 1.0000 | 1.0000 | pass |
| vcop2 | 3x | 154203 | 154203 | **0** | 0 | **0** | 99.999 % | 1.0000 | 1.0000 | pass |


_Tables generated by `res-table.py`. `interior` counts real interior disagreements only._
