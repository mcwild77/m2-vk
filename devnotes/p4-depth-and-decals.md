# P4 — depth, decals and sort order

**Status: ✅ DONE — all three steps. The early-Z pipeline split (2026-07-26), the
resolution-invariance check (2026-07-27) and the docs pass (2026-07-27). All three exit criteria are
met and the upstream diff is still 30 lines.**

**The headline, and it is not what this phase was scoped to be: the problem P4 exists to solve does
not exist at this seam, and switching to interpolated z would be a regression rather than a fix.**
That is measured below, not argued. What survives of P4 is small, and the phase ordering wants
revisiting in the Polydiver plan as a result.

Plan of record for P4 as scoped in `../Polydiver/PDDocs/model2/model2_libretro_core.md` §3 and §4
("**P4 — Correctness: decals / z-fight / sort.** The hard part (§4). Gate before enhancements.").
Read [p3-hw-geometry.md](p3-hw-geometry.md) first — P3's settled decision that **depth is draw order,
not z** is the thing this phase was expected to undo, and the reason it should not is the whole of
§1 below. [ab-baselines.md](ab-baselines.md) is what any change here has to be measured against, and
[performance.md](performance.md) §4 assigns one item to this phase.

---

## What P4 was scoped as

The Polydiver plan, §4, written before P3 was built:

> Model 2 has **no depth buffer**; it draws in priority/painter's order. A real Vulkan z-buffer makes
> coplanar decals (shadows, road markings, cockpit overlays, VF2 stage decals) z-fight.

and the solution shape it proposed: interpolated z for hidden-surface removal, plus per-primitive
`VK_DYNAMIC_STATE_DEPTH_BIAS` keyed on display-list order to resolve the coplanar ties that creates.
`depthBiasClamp` was checked for on the target device at P2 and it is present
([vulkan-target.md](vulkan-target.md)); `D24_UNORM_S8_UINT` is not, hence `D32_SFLOAT`.

**Both halves of that were conditional on a premise: that the renderer would use a real z-buffer.**
P3 did not. The depth attachment holds the **draw-order key** `1 - n/65536` for the *n*th polygon in
seam order, tested `GREATER` with depth writes on against a buffer cleared to 0 — first writer wins
the pixel, which is `m_fillmap` in hardware. So there is no z-fighting to fix, because there is no z.

The question P4 has to answer is therefore not "how do we resolve the ties" but **"should the depth
key become real depth at all?"** Everything below is that question.

---

## 1. The measurement — draw order and real depth do not disagree

Six polytap dumps, one late frame each, taken 2026-07-26 from `retrohost` at HEAD `c38dbbefffe`:
`vf2` rendered frame 1200, `srallyc` 900, `vcop2` 1400, `sgt24h` 1600, `overrev` 1600, `desert` 700.
Per-vertex depth comes from the record's `rz`, **normalised first** — textured polygons carry `1/z`
and solid ones carry raw `z`, because `model2_v.cpp` only reciprocates for the textured case
(`m2vk_frame.h` documents this; forgetting it inverts half the comparisons).

### 1.1 Model 2 draws front-to-back, and the stream confirms it to the polygon

`render_polygons` walks `z` from `min_z` to `max_z` (`model2_v.cpp:727`), and `m_fillmap` makes the
first polygon to reach a pixel keep it. So the correct-order case is **"the polygon drawn later is
farther"**, which is the opposite of a painter's algorithm and is easy to check backwards.

Over all polygon pairs whose bounding boxes overlap (`i` drawn before `j`):

| | vf2 | srallyc | vcop2 | sgt24h | overrev | desert |
|---|---|---|---|---|---|---|
| bbox-overlapping pairs | 9694 | 42720 | 9479 | 3424 | 12135 | 4832 |
| `j` strictly farther — correct | 5691 | 33558 | 5936 | 2232 | 8957 | 2577 |
| **`j` strictly nearer — a sort error** | **0** | **6** | **23** | **4** | **7** | **2** |
| | 0.00 % | 0.01 % | 0.24 % | 0.12 % | 0.06 % | 0.04 % |

**Bounding-box overlap is a superset of real pixel overlap, so that error row is an upper bound**, and
the bound is 23 pairs in the worst frame measured. The largest such overlap region anywhere is 2369
bbox px on srallyc and 605 on vcop2 — and those are bbox areas, not covered pixels. The 16-bit bucket
sort is right essentially always.

### 1.2 There is a large coplanar population, and float32 cannot separate it

The pairs whose depth ranges overlap to within 1e-4 of relative depth — decals, road markings, stage
overlays, the things §4 named:

| | vf2 | srallyc | vcop2 | sgt24h | overrev | desert |
|---|---|---|---|---|---|---|
| coplanar pairs | 124 | 366 | 104 | 43 | 113 | 47 |
| of those, **< 1 float32 ULP apart** | 85 | 336 | 34 | 30 | 62 | 38 |
| median separation | **0 ULP** | **0 ULP** | 211 ULP | **0 ULP** | **0 ULP** | **0 ULP** |

**The median coplanar pair is exactly zero float32 ULPs apart** — the two polygons have bit-identical
interpolated depth, not merely close depth. On five of six fixtures that is the median, so it is the
typical case and not a tail. `depthBiasClamp`, `VK_DYNAMIC_STATE_DEPTH_BIAS` and every other tie-break
mechanism the plan proposed exists to nudge values that are *nearly* equal. **There is no tie to
break here; there is no difference at all to amplify.** The bias would have to be applied on the
basis of draw order — which is to say, it would reconstruct the draw-order key by a longer and lossier
route.

### 1.3 The existing A/B result already proves the ordering is exact

This is the strongest evidence and it did not need a new run. **`M2VK_FORCE_SOLID=2` A/B is
pixel-identical, SSIM 1.0000, on vcop2 / srallyc / dynamcop** (P3 step 7, reproduced at step 8). Flat
shading removes the texture chain and leaves the rasterizer, the depth key, the scissor and the
composite. If the draw-order key mis-ordered *any* polygon against the software renderer, flat-shaded
output could not be pixel-identical — a mis-ordered polygon under flat shading is a solid patch of the
wrong colour, which is the single most visible failure available. It scores 1.0000.

So the ordering question is closed by a measurement that already exists in
[ab-baselines.md](ab-baselines.md). P4 does not need to re-derive it.

### 1.4 The depth key cannot saturate, and that is now checked rather than assumed

`vk_geom.cpp:957` clamps the index at `DEPTH_MAX_INDEX = 65535`. MAME's own hard limit is
`MAX_POLYGONS = 32768` (`model2.h:776`, a `fatalerror` above it), so **the clamp can never fire** —
the key has exactly 2× headroom over a limit the emulation enforces first. Keys land in (0.5, 1.0],
where a float32 ULP is ~6e-8 against a key step of 1.5e-5, i.e. every polygon is separated by ~256
ULP. The comment at `vk_geom.cpp:84` justifies the width with "ample for 1450 polygons"; the real
justification is the `MAX_POLYGONS` bound and it is stronger. Worth correcting in place.

---

## 2. What follows: keep the depth key

**Recommendation: P4 does not adopt interpolated z, and the draw-order key stays.** Three reasons, in
order of weight:

1. **The real hardware has no depth buffer.** The Polydiver plan's own §4 opens with that. MAME's
   bucket sort plus `m_fillmap` *is* the hardware's behaviour, so reproducing it is correctness.
   Interpolated z would make the renderer diverge from the machine it is emulating, in the direction
   of "looks like a modern renderer" rather than "looks like a Model 2".
2. **It would create the problem it was scoped to solve.** §1.2: hundreds of coplanar pairs per frame
   at zero ULP separation. Today they are ordered exactly, by construction, at zero cost. Under
   interpolated z every one of them becomes a tie needing a bias whose only correct value is derived
   from draw order.
3. **There is nothing measurable to gain.** §1.1 bounds the polygon-level sort error at ≤ 23 pairs per
   frame, and §1.3 shows we already match the software renderer exactly on ordering.

**This voids `../Polydiver/PDDocs/model2/model2_libretro_core.md` §4's first two bullets at this
seam** — submission-order depth bias and the z-fight problem it solves — in the same way P3 voided its
`det(3x3)` shadow bullet. The third bullet, translucency ordering via a sorted back-to-front pass, was
already voided at P3: nothing blends anywhere, translucency is a cutout. **§4's fourth bullet,
precision, is the one that survived, and it survived intact** — the residual in
[ab-baselines.md](ab-baselines.md) is exactly the interpolation rounding it predicted.

⚠️ **Do not read this as "P4 was wrong".** §4 was written when the renderer's shape was unknown and it
correctly identified the hazard of the obvious design. P3 took a different design *because* §4 named
the hazard. The phase is being closed by the plan having worked, not by the plan having been wrong.

### The one thing genuinely left open, and it is out of scope

§1.1's error bound is about **our renderer versus MAME**. It says nothing about **MAME versus a real
cabinet**. Pairs whose depth ranges interleave over a substantial shared area, in different sort
buckets and far apart in draw order — i.e. plausible interpenetration, where a per-object sort cannot
be right at every pixel — run 446 (desert) to 2065 (srallyc) per frame. Those are ordered by MAME's
bucket sort and we reproduce that order exactly.

Whether the arcade hardware ordered them the same way is **not answerable with the ground truth this
project has**, because the ground truth *is* MAME. It would need a real cabinet or reference capture.
Recorded as an open question, assigned to nothing, and explicitly **not** a reason to build
interpolated z — a z-buffer would not reproduce the hardware here either, it would just be wrong
differently.

---

## 3. What P4 actually contains

Three items. This is a much smaller phase than "the hard part".

### ~~Step 1 — split the pipeline so opaque non-checker polygons carry no `discard`~~ — **DONE**

**Built 2026-07-26. No pixel moves on any of the 12 fixtures; upstream diff still 30 lines.** As
built, with the three places it differs from the text below:

- **A specialisation constant cannot do it.** `EarlyFragmentTests` is an execution mode on the entry
  point, not a value, so it takes a second *module*. `poly.frag` compiles twice from one source
  (`-DEARLY_Z=1`), which is better than the two-pipelines-one-module shape this step assumed: the two
  `discard` sites are gated on exactly the two flag bits `vk_geom.cpp` tests to pick the pipeline, so
  the predicate is written once and cannot drift. The discards are removed *textually* under
  `EARLY_Z`, because an unreachable discard is still a discard in the module. Checked in the SPIR-V:
  general 2 `OpKill`, early **0** plus the `EarlyFragmentTests` mode. `poly_frag_spv.h` came out
  byte-identical to the committed blob, so the general path is provably untouched.
- **⚠️ The cost is draw calls and it is far larger than this step expected.** A batch breaks on the
  pipeline as well as the viewport, and the pipeline alternates with the polygon stream where the
  viewport essentially never does: **92–831 draws a frame against 1–8 before**. Early-Z share runs
  13.4 % (`overrev`, all-translucent, pays the batching for nothing) to 93.1 % (`schamp`). The
  per-fixture table is in the worklog.
- **[measured] The GPU win is real and bigger than predicted; the wall clock does not move.** `gpuwait`
  on `waverunr` 0.892 → 0.759 ms (**−15 %**), `desert` 0.714 → 0.699 (−2 %); `pipe%` unchanged inside
  the spread on both, because `core` is 4.3 ms against a `gpuwait` of 0.8. The difference between the
  two fixtures is **overdraw** — front-to-back with first-writer-wins means occluded fragments run the
  whole 0.388 ms filtering chain under late tests and are then thrown away, and waverunr has far more
  of that than desert. This step predicted no movement; that is right about the wall clock and wrong
  about the GPU.

**`M2VK_NO_EARLY_Z=1`** is the new switch and it is a *pure no-op* switch, unlike `M2VK_NO_SCISSOR`:
it must not move a pixel on either renderer, so "digests equal on and off" is the whole verification.
On vf2 it gives the same digest and the same last-frame PPM from 1 draw a frame instead of 205.

**Batches follow submission order and are never regrouped**, though the depth key makes the picture
order-independent (the winner is the lowest record index covering the pixel that does not discard,
whatever order draws arrive in — monotone key, `GREATER` test). Sweeping each class into one draw would
cut 831 back to a handful. Recorded in `vk_geom.cpp`'s header as the escape hatch rather than taken,
because §2a says the whole optimisation list is bidding for 4.5 % of a frame here. **⚠️ That lever is
for the Quest 3 port to pull if it needs to**: on an Adreno 740 both halves of the trade grow — the GPU
win is worth more and 831 draws a frame costs more — and neither half is measurable from a Mac.

The step as originally scoped:

[performance.md](performance.md) §4 item 1, assigned to P4 so the depth path is verified once rather
than twice. **Its speed argument is dead** (§2a: the whole optimisation list bids for ~4.5 % of the
frame on this machine, and rasterisation is 0.056 ms of it); **its risk argument survives**, and with
§2's recommendation the depth path is no longer being opened for anything else, so this becomes the
only reason to touch it in this phase.

A `discard` anywhere in the shader module forces depth writes to late fragment tests
(performance.md §3.2), and one pipeline currently serves every polygon, so opaque polygons pay for the
translucent cutout and the `checker` stipple. A specialisation constant plus a second pipeline lets
the opaque non-checker majority declare `EarlyFragmentTests`. MAME reaches the same specialisations by
template parameter, so this mirrors the reference implementation rather than diverging from it.

⚠️ **The correctness argument for late fragment tests must survive the split intact.** A discarded
fragment leaving the draw-order key unclaimed is what reproduces `m_fillmap`, and the `checker`
stipple has rested on it since P3 step 3, the translucent cutout since step 5. The split is only legal
for polygons that **cannot** discard: `renderer & 1` clear *and* `checker` clear. Getting the
predicate wrong is invisible on most frames and catastrophic on the ones with a stipple over a decal.

Verification: `M2VK_OPAQUE_ONLY=1` must reproduce P3 step 4's vf2 numbers to the pixel (105009
covered, 95.022 %, 2 edge, SSIM 0.9964), and the full 12-fixture table must reproduce
[ab-baselines.md](ab-baselines.md). `perf.sh` on `waverunr` and `desert` for whether it moved
anything; expect it not to, on this machine.

### ~~Step 2 — verify the depth path is resolution-invariant, which is what P5 depends on~~ — **DONE**

**Built and run 2026-07-27, committed as `83491ca0fa3` ("Draw the frame at an internal scale and
resolve it back down"). The depth path is resolution-invariant, measured rather than argued, and the
`checker` stipple is measurably not — one polygon's worth of proof, handed to P5.** As built:

- **`M2VK_SS=<n>`** draws the whole frame — both 2D layers and the polygon pass — into an n×
  oversized colour and depth attachment and resolves it back into the image the frontend is handed,
  so `ppmdiff.py`, `ab.sh` and everything else measure a supersampled run without knowing it is one.
  New: `renderer_vk/shaders/downsample.frag`, `devnotes/res.sh`, `devnotes/res-table.py`. Changed:
  `vk_present.cpp` (the oversized pair, a second render pass, the resolve pipeline), `vk_geom.cpp`
  (a `scale` on `geom_draw`, applied to the scissor rectangles and nothing else). **No upstream file,
  so the diff is still 30 lines**, and `M2VK_SS` unset is a proven no-op: vf2 at 2500 frames is
  `55da761fecca5c01`, the documented baseline, to the digest.
- **`M2VK_SS_POINT=1`** resolves by the centre subpixel instead of the box mean, and is **refused on
  an even scale** — only an odd n has a subpixel whose centre coincides with the 1× pixel's
  (`n*x + (n-1)/2` has centre `x + 0.5`). With one, the fragment shader runs at the *same screen
  positions* as the 1× render, so the two pictures are comparable pixel for pixel rather than in
  aggregate. That is the strong form of the check and it is where the result comes from; the box
  filter is what the step as scoped asked for and is what says something about coverage.
- **The plumbing checks itself before any picture is compared.** `res.sh` requires the
  `M2VK_NO_3D=1` background reference to come back **bit-identical at every scale**, last frame and
  whole-run digest. The 2D layers are uploaded at 1× and magnified by a NEAREST sampler, so every
  subpixel of a pixel holds the same texel and any exact resolve returns it; if the resolve, the
  viewport or the upscale were wrong the 2D would move and every 3D number would be measuring that
  instead. It also refuses a run whose log carries no `supersample:` line, because a mistyped
  `M2VK_SS` is otherwise a silent 1× run that agrees with 1× perfectly.

⚠️ **The first thing the check found was a bug in itself, and the shape of it is worth keeping.**
`poly.vert`'s half-extent was scaled along with the viewport on the first attempt. It takes m_destmap
pixels and produces NDC — and **NDC is the resolution-independent quantity**, so the visible
half-extent is correct at every scale and scaling it put the entire frame in a 1/n corner of the
attachment. The symptom was a 4× run with **zero** coverage overlap against 1×, which reads as a
catastrophic ordering failure rather than as a wrong constant. `poly.vert`'s header said "nothing here
changes" and was right; the misreading was in this step. The scale belongs to the viewport and the
scissor, and `geom_draw` now takes it as its own argument so the two cannot be confused again.

### The result

**10 fixtures, 2500 frames each, at 2× and 4× box and at 3× point.** Generate the tables with
`res-table.py`; never retype a number, for the reason written at the top of
[ab-baselines.md](ab-baselines.md). The full tables are in [res-baselines.md](res-baselines.md).

**The claim rests on the 3× point runs, and it has to, because a box resolve moves the sample
points.** This was the second thing the step got wrong before measuring it. The tempting statement —
"supersampling can only *add* partially-covered fringe pixels, so an `A only` pixel is a polygon that
stopped winning" — is **false for an even scale**: the 2× subpixel centres are at ±0.25, and the 1×
centre at +0.5 is *not among them*, so a sliver narrower than half a pixel that happens to contain the
1× sample point can miss every 2× sample point and vanish. That is exactly what desert does — **7
`A only` pixels at 2×, falling to 1 at 4×** as the sampling gets finer, all of them a one-pixel-wide
grey mast against the sky, and **0 at 3× point**. Gaining and losing sub-sample-width geometry is what
point-sampled supersampling does; it says nothing about depth.

An **odd** scale resolved by the centre subpixel keeps the 1× sample point as one of its samples, so
its coverage is comparable directly. That run is the measurement:

| | 3× point |
|---|---|
| `A only` = 0 | **8 of 10 fixtures** (dynamcop 12, schamp 2) |
| coverage agreement | **1.0000 on 8 of 10**, 0.9999 on the other two |
| covered pixels bit-identical in colour | **98.97 % – 99.97 %** |
| SSIM over the covered region | **0.9994 – 1.0000** |
| interior coverage disagreements | **0 on all 10** |

The 14 `A only` pixels across dynamcop and schamp are each **on a silhouette edge with 2–7
both-covered neighbours** — `ppmdiff` classifies all of them as edge, none as interior — and 10 of the
14 sit on a `(0,0,0)` background at values like `(2,0,0)` and `(0,0,1)`, which is the drew-black
artefact CLAUDE.md already documents. The rest are fill-rule ties at a polygon's outline.

**The decisive measurement is that the residual has no shape.** A depth-ordering change takes a
*region* — a polygon wins or loses an area. Connected-component analysis of the 3× point residual:

| | vf2 | vcop2 | srallyc | sgt24h | desert | waverunr | overrev | dynabb97 | schamp | dynamcop |
|---|---|---|---|---|---|---|---|---|---|---|
| differing px | 774 | 402 | 705 | 1089 | 1426 | 1000 | 270 | 983 | 19 | 252 |
| clusters | 688 | 247 | 645 | 1010 | 1259 | 873 | 261 | 885 | 19 | 248 |
| **largest cluster** | **5** | 129 | **6** | **4** | **5** | **7** | **2** | **7** | **1** | **2** |

Roughly one pixel per cluster, and the largest connected run is **1–7 pixels on nine of ten
fixtures**. vcop2's 129 was chased rather than waved through: it is a **one-pixel-wide vertical line
at x=485** whose colour moves from `(125,125,125)` to `(133,133,133)` — a single step along the
`colorxlat` ramp on a sliver seen edge-on, i.e. the float-rounding-amplified-by-a-LUT residual §1 of
[ab-baselines.md](ab-baselines.md) already attributes. It is shading, not depth, and the flat-shaded
run below has **one** differing pixel in the whole vcop2 frame.

**The strongest form, and the one that isolates ordering outright: flat-shaded 3× point**
(`MODE=M2VK_FORCE_SOLID=2`), which removes the texture chain and leaves the rasterizer, the depth key,
the scissor and the composite. A mis-ordered polygon there is a solid patch of the wrong colour.

| | vcop2 | srallyc | dynamcop |
|---|---|---|---|
| covered, 1× and 3× | 154203 / 154203 | 136116 / 136116 | 187571 / 187571 |
| `A only` / `B only` | **0 / 0** | **0 / 0** | **0 / 0** |
| pixels differing in colour | **1** | **3** | **1** |
| SSIM covered | 0.999999 | 0.999972 | 0.999980 |

Coverage identical **to the pixel**, and each of the five differing pixels sits on a boundary and
takes the colour of the polygon immediately next to it — a rasterisation edge tie. **The depth path is
resolution-invariant.**

⚠️ **`ppmdiff.py`'s "real interior disagreements … a bug" verdict does not transfer to a box resolve,
and `res.sh` does not inherit it.** The tool was built for two renderers sampling at the same points;
under a box filter an isolated fringe pixel is expected in *both* directions, as desert's mast shows.
`res.sh` therefore judges a box run on the background reference and exit criterion 1, prints
ppmdiff's verdict as information, and reserves the strict reading for the point resolve — where the
sample points are shared and the verdict is meaningful. **Quote a point run when claiming invariance;
quote a box run when asking what supersampling looks like.**

### What this hands to P5, measured on one polygon

**The `checker` stipple is not resolution-invariant and it is not a small effect.** One vcop2
checkered quad (`M2VK_ONLY_POLY=114` at frame 1804), drawn alone:

| | pixels drawn | same colour as 1× |
|---|---|---|
| 1× | 78968 — half the hull, the screen door | — |
| **2× box** | **157945 — exactly twice, the entire hull** | **0.000 %** |
| 3× point | 78968 | 100.000 % |

At 2× the 50 % screen door has become a uniform 50 % *blend* covering the whole polygon. This is a
**shading** problem, not a depth one, and it is P5's: [p3-hw-geometry.md](p3-hw-geometry.md) already
lists it beside the resolution-blind mip selection under "three things that do not come for free",
and [seam.md](seam.md) has it as a dither-vs-real-alpha decision. S× is what forces the decision.

⚠️ **The 3× point row is not evidence the stipple is invariant, and it would be easy to read as such.**
The stipple is `(x ^ y) & 1`, and the centre subpixel of an odd scale has
`(n*x + (n-1)/2) + (n*y + (n-1)/2) ≡ x + y (mod 2)` for odd n — the parity survives *by accident of
the odd scale*. What is actually on the oversized attachment before the resolve is a stipple n times
finer than the hardware's. **Use a box resolve to see the stipple problem; the point resolve hides
it**, which is precisely why both modes exist.

The step as originally scoped:

`poly.vert`'s header asserts scale invariance: "rendering at S x is a viewport of S*width by S*height
and nothing here changes, because the perspective divide has already happened and a uniform
screen-space scale commutes with it." **That is an argument, not a measurement**, and P5 is built on
top of it. P4 is the depth phase, so it is where the depth half gets checked.

The depth key is per-polygon and carries no screen-space term at all, so it is invariant by
construction — the check is cheap and the point is to have run it. Render a fixture at 2× and 4× into
an oversized attachment, downsample by exact box filter, and confirm the *ordering* is unchanged
(coverage agreement against the 1× run, not colour — filtering and the LOD chain will legitimately
differ, and conflating those with an ordering change is the trap).

⚠️ **The `checker` stipple is NOT resolution-invariant and this step must not pretend otherwise.** It
is `(x ^ y) & 1` on `gl_FragCoord`, so at 2× it becomes a half-density dither of a different pattern.
That is a real P5 problem, it is a *shading* problem rather than a depth one, and step 2's job is to
name it and hand it to P5, not to solve it.

### ~~Step 3 — the docs, and the plan correction~~ — **DONE**

**2026-07-27, committed as `eaa1355f451` ("Correct the comments on the depth key's width and the
bucket lists"). Two code comments, four docs in this tree, and the Polydiver plan.** No behaviour
changed; the core was rebuilt to confirm the comment edits compile and nothing else was touched.

**The two comment corrections, both made in place and both expanded past what this step asked for**,
because the *reason* is the part that gets trusted in a refactor and neither comment had it:

- **`vk_geom.cpp`'s depth-key width.** It justified 16 bits with "ample for 1450 polygons" — an
  observation about one VF2 frame, which is exactly the kind of justification that quietly stops
  holding. Replaced with the bound that cannot stop holding: **MAME `fatalerror`s above
  `MAX_POLYGONS = 32768`** (`model2.h`), so the key has 2× headroom over a limit the emulation
  enforces before the stream reaches us and `DEPTH_MAX_INDEX`'s clamp is unreachable. The ULP
  arithmetic from §1.4 went in with it — keys in (0.5, 1.0], ~256 ULP between neighbours — because
  "D32_SFLOAT represents these exactly" is the *other* half of the claim and was already there
  without its numbers.
- **`m2vk_frame.h`'s `bucket` field.** It said "within one bucket, draw order is submission order",
  and the lists are built by **prepend** (`model2_v.cpp:520-522`) so `render_polygons` walks the
  **newest first** — *reverse* submission order. Corrected, with the reason it has never mattered
  stated in the same breath: the record is taken at the seam, i.e. in traversal order, so the
  reversal is already baked into the stream and into the draw-order key, and nothing downstream has
  to undo it. A future refactor that rebuilt the record from the bucket lists instead of the seam is
  the case this comment exists to catch.

**The Polydiver plan correction** (`../Polydiver/PDDocs/model2/model2_libretro_core.md`), raised there
rather than forked here:

- **§4 gets a correction box** stating that three of its four bullets do not apply at this seam and
  why — submission-order depth bias (voided by P4: the draw-order key *is* submission-order priority,
  applied exactly and for free), `det(3×3)` shadow detection (voided at P3), translucency ordering
  (voided at P3: nothing blends). **The fourth, precision, survived intact and is the one that came
  true.** The original text is kept below the box, not deleted — the reasoning is why P3 chose the
  design it did.
- **§3 gets the as-built phase statuses** (P1–P4 done, with the settled decisions that should not be
  re-derived) and **a box on the phase order**: P4 does not gate P5, the desktop has no remaining
  performance question, and the Quest 3 port has no phase. Recorded as a suggestion — *take the port
  before P5* — and explicitly not decided here.
- Smaller corrections while in there, each of which was a stale statement rather than a new finding:
  the seam's line number (611 → **565 at mame0288**), §5's A/B harness as built (four forced
  departures — no savestates, `retrohost` rather than RetroArch, coverage before SSIM, no CI), and §7's
  open questions, of which **all four are now closed** (threading, microtexture, variants, licence).

⚠️ **What was deliberately *not* written into the Polydiver plan: the numbers.** It carries the
findings and the decisions; the measurements stay here, where the harness that produced them lives and
where they can be regenerated. A number copied into a second document is a number that will disagree
with the first one within two steps — the same reason [ab-baselines.md](ab-baselines.md) says never to
retype one.

Also done in this step: [worklog.md](worklog.md), CLAUDE.md's "Where we are" and "Next step",
[next-session-prompt.md](next-session-prompt.md), and this file.

---

## Exit criteria

1. **No pixel moves that is not accounted for.** All 12 fixtures reproduce
   [ab-baselines.md](ab-baselines.md); both P3 regression guards hold (`M2VK_OPAQUE_ONLY=1` to step
   4's vf2 numbers, `M2VK_FORCE_SOLID=2` to SSIM 1.0000 on vcop2/srallyc/dynamcop);
   `M2VK_SW_3D=1` still byte-identical to `renderer=software`.
2. **The ordering decision is written down with its measurement**, so it is not re-litigated a third
   time. §1 and §2 are that; they need to survive into CLAUDE.md and the Polydiver plan.
3. **The upstream diff does not grow.** It is 30 lines against mame0288 and nothing in §3 needs a
   line of it — all three steps are renderer-side or documentation.

## What this phase does NOT do

Interpolated z, depth bias, a sorted transparent pass, shadow-volume detection, internal-res scaling
(P5), and any optimisation from performance.md §4 other than item 1. **The depth key is not to be
touched** — that was P3's settled decision and §1 is now the second, independent reason for it.

## ⚠️ The consequence for the phase order

P4 was the gate before P5 "because enhanced res + z-buffer exposes decal cases the SW renderer never
worried about". **There is no z-buffer and there will not be one, so that gate is not load-bearing.**
The remaining P4 content is one free optimisation, one invariance check and a docs pass, none of which
P5 depends on except step 2, which is an afternoon.

That, together with [performance.md](performance.md) §2a promoting the Quest 3 port from sequel to
blocker, is an argument for **taking the Quest 3 port before P5** rather than after P6 — it is where
every remaining performance question can be answered, and it is the only phase whose risk is unbounded
(the open question there is what two interpreted i960s cost on an XR2 Gen 2, which no renderer work
moves). **A decision for the Polydiver plan, not one to take here.**

---

## Order of work

1. ~~Split the opaque pipeline (§3 step 1) — the one code change in the phase.~~ **DONE** — the
   early-Z pipeline split; no pixel moves on 12 fixtures, GPU time −15 % on waverunr, and it costs
   92–831 draws a frame where the frame used to be one. See §3 step 1 as-built.
2. ~~Resolution-invariance check on the depth path (§3 step 2), and hand the `checker` stipple to P5.~~
   **DONE** — `M2VK_SS=n` + `M2VK_SS_POINT=1`, `res.sh`, `res-table.py`,
   [res-baselines.md](res-baselines.md). The depth path is resolution-invariant on 10 fixtures at 2×,
   3× and 4×; the residual has no shape (largest connected cluster 1–7 px on nine of ten); flat-shaded
   3× point is coverage-identical to the pixel with 1–3 pixels of colour difference. The `checker`
   stipple is measurably *not* invariant and is handed to P5 with one polygon's worth of proof. See
   §3 step 2 as-built — **including the two things this step got wrong before measuring them**, which
   are the parts worth reading.
3. ~~Docs: this file as-built, the two comment corrections, CLAUDE.md, and the Polydiver plan
   correction (§3 step 3).~~ **DONE** — both comments corrected in place with their *reasons* rather
   than their conclusions; Polydiver §4 carries a correction box, §3 the as-built phases and the
   phase-order question, and §7's four open questions are all closed. The numbers stayed here on
   purpose. See §3 step 3 as-built.

**P4 is closed.** Exit criterion 1: no pixel moved in the phase — step 1 reproduced
[ab-baselines.md](ab-baselines.md) cell-for-cell, step 2 reproduced it again *and* all three whole-run
digests per fixture, and step 3 changed no executable code. Exit criterion 2: §1 and §2 are written
down with their measurement and have reached CLAUDE.md and the Polydiver plan. Exit criterion 3: the
upstream diff is **30 lines**, unchanged since P3 step 8.
