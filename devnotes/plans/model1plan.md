# Model 1 plan — fold Sega Model 1 into Modelizer as a fourth family

Add **Sega Model 1** (Virtua Fighter, Virtua Racing, Star Wars Arcade, Wing War, NetMerc,
Virtua Formula) as a fourth family **inside the Modelizer core**, alongside Model 2, System 22 and
System 21 — one dylib, routed to the right renderer at runtime by `family_of()`. This is the sibling
of [system21plan.md](../plan_finished/system21plan.md); read that first for the seam philosophy and shared-OSD mechanics
this reuses wholesale. The two headline features asked for are **high internal resolution** and a
**No Lighting** option, both of which fall out cleanly from where Model 1 resolves its geometry.

> **Prior research exists and is load-bearing.** The sibling project **Polydiver**
> (`~/Documents/GitHub/Polydiver`, launch with `--add-dir`) contains a deep Model 1 study built for the
> extraction/Unity-reconstruction workflow. It is *not* a renderer, but its hardware findings are
> ground truth for this port. Cited inline below; the crown jewels are
> `PDDocs/model1/model1_lighting.md`, `…/model1_sortorder.md`, `…/model1_2d.md`, and the executable
> color-LUT spec `PDTooling/model1/model1_colorxlat.py` + the captured `model1_vf_colorxlat.bin`.

## Why this is a small lift (and where the one real surprise is)

Model 1's primitive stream is the **simplest of the four families** — closer to System 21 than to
anything textured, and even simpler on depth. Verified against our mame0288 tree this session:

1. **No textures at all.** `push_object` (`model1_v.cpp:758`) walks the polygon-ROM strip and resolves
   each face to **one flat pen** (`quad_t::col`, `model1.h:79`). No tile fetch, no palette banks, no
   fog/gamma tail. The GPU consumer is the smallest of the four — flat quads plus a moiré stipple for
   translucency.
2. **Depth is draw order, not a z-buffer — the *Model 2* model, not S21's.** `sort_quads`
   (`model1_v.cpp:452`) does one global `qsort` over every quad in the frame by view-space z, then
   `draw_quads` (`model1_v.cpp:472`) paints back-to-front. Per-poly z-key mode is `(flags>>10)&3`;
   the comparator (`model1_v.cpp:437`) breaks ties by **submission order** (later = on top). So the GPU
   pass reuses Model 2's draw-order pipeline with **no depth attachment**.
3. **One seam covers the family.** Every Model 1 game draws through the same `draw_quads`/`fill_quad`.
   Tap it once; widening from `vf` to the whole set is build/boot plumbing, not renderer work.

**The one surprise — 2D is not just a HUD.** Model 1's 2D layer is the Sega System 24 tile chip
(`segas24_tile_device`), and the **stage backgrounds — sky, mountains, skyline — are opaque 2D tile
layers drawn *under* the 3D**, not geometry (`model1_2d.md`). `screen_update_model1`
(`model1_v.cpp:1653-1663`) interleaves them around the 3D:

```
layers 6,4,2,0  → TILEMAP_DRAW_OPAQUE   (backgrounds, UNDER)
tgp_render()                             (the 3D scene)
layers 7,5,3,1                           (HUD, OVER)
```

So Modelizer must composite **2D-under → 3D → 2D-over**, exactly the shape of the System 21 option-B
under/over machinery already in `s21_seam.cpp` + `renderer_vk/s21_geom.cpp`. That machinery is the
template to lift, not new invention — but it *is* the part that makes Model 1 more than a one-file S21
clone. (There is no sprite chip; the `/* OBJ */` slot is unmapped.)

## Scope — which games

Source file is `sega/model1.cpp`; its token does not collide with the three families already detected in
`family_of()` (`retro_entry.cpp:347`). ROMs live in `devnotes/roms/` per the one-ROM-dir rule.

| Game | id | Type | Notes |
|---|---|---|---|
| Virtua Fighter | `vf` | fighter, digital | The test case; all Polydiver findings are VF-derived |
| Virtua Racing | `vr` | racer, analog wheel/pedals | Exercises the steering detector + camera-relative track |
| Virtua Formula | `vformula` | racer, analog | data-only after `vr` |
| Star Wars Arcade | `swa` | rail shooter, analog stick | recent upstream control fixes (see risk 1) |
| Wing War | `wingwar` | flight, analog | shares the TGP-ROM 2-bit-corruption fix |
| NetMerc | `netmerc` | — | shares that fix; lowest priority |

## Seam — located (our mame0288 tree)

- **Primitive tap:** `model1_state::draw_quads(bitmap, cliprect)` (`model1_v.cpp:472`). It already holds
  the sorted `m_quadind[]` of `quad_t*`; emit one `submit_quad()` per quad in that order, and skip the
  software `fill_quad` when `!sw_owns_3d()`. `quad_t` (`model1.h:79`) carries `point_t *p[4]`, `float z`,
  `int col`.
- **Hi-res note — capture the float projected coords, not the integer pixels.** `point_t`
  (`model1.h:72`) holds `float xx,yy` set by `project_point` *before* the integer `s.x/s.y` round. Cross
  the seam with `xx,yy`; then the GPU rasterises flat quads at any internal size cleanly. (S21 crosses
  with integer `s.x/s.y` and is res-limited; Model 1 should not repeat that.)
- **Frame bracket:** start of `draw_quads` / end of `screen_update_model1` (`model1_v.cpp:1591`) →
  `frame_begin()`/`frame_end()`, the attach-decision point (same as S21's `g_active`).
- **2D capture:** `screen_update_model1` (`model1_v.cpp:1653-1663`) — capture the opaque under-band
  (6/4/2/0) and the over-band (7/5/3/1) into the s21-style under/over overlays when the GPU owns the 3D.

New files, mirroring the S21 set: `m1_seam.{h,cpp}`, `renderer_vk/m1_geom.{cpp,h}`. Upstream edits stay
a handful of guarded `#ifdef M1VK` hook calls in `model1_v.cpp` (the mergeability golden rule).

## No Lighting — the option, grounded in the hardware (`model1_lighting.md`)

Model 1 lighting is **baked into `col` at geometry time**, so "No Lighting" means emitting the raw
palette albedo *before* the luma stage. The hardware equation (`push_object`):

```
dif = dot(faceNormal, light)          // single directional, view space; specular is #if 0'd → accurate to omit
ln  = ambient + diffuse * max(0, dif) // per-poly lightmode picks ambient/diffuse via (flags>>17)&15
```

Then — and this is risk 2 from the original plan, now resolved — **luma is a LUT, not a multiply.**
`lumval = clamp((255·ln)>>2, 0, 63)`, and each R/G/B channel is run separately through the runtime
`color_xlat` RAM: `chan = m_color_xlat[(chan<<8) | lumval | bank] >> 3` with banks `0x0/0x2000/0x4000`
(`model1_v.cpp:960-962`, and again at `:1125` on the poly-RAM path). `color_xlat` lives at
`0x910000–0x91bfff` (3 banks × 0x2000 u16), written once at boot — **capture it live like the palette**,
it is not in ROM. VF finding: all three banks identical, curve ≈ `out5 = clamp(chan5·lumval/32, 0, 31)`,
saturating above `lumval=32`. Executable spec: `PDTooling/model1/model1_colorxlat.py` +
`model1_vf_colorxlat.bin`.

Note there is a **second, older shading branch** using `scale_color()` (`model1_v.cpp:505`, a plain
5:5:5 multiply on `machine().pens[]`) selected by a different `(flags>>10)` case — the seam/consumer
must reproduce whichever branch each quad took, or colours shift. Decode-spec both before writing the
shader (same discipline as the Model 2 luma work).

**Implementation:** cross the seam with the **pre-luma albedo pen + `lumval` + bank** (not just the final
`col`), upload `color_xlat` as a small LUT texture, and apply it in `m1_geom` — exactly what makes the
toggle live and GPU-side. Reuse the existing **`model2_flat_luma`** ("No Lighting") key (matches how S22
reuses it): set → skip the LUT, output the raw albedo.

## Phases (following the s0–s2 / t0–t5 convention → **M1-0 … M1-6**)

- **M1-0 — Boot.** Add `sega/model1.cpp` to [modelizer.flt](../../src/mame/modelizer.flt); a `mame_model1`
  project block in [modelizer.lua](../../scripts/target/mame/modelizer.lua) with an `M1VK` scoping define +
  its CPUs/sounds (V60, MB86233 TGP, `segaic24`, the sound block) deduped against the shared Sega
  devices already pulled in by `mame_model2`. Boot `vf`/`vr`/`swa` in **software** — byte-identical to
  stock MAME. No seam yet.
- **M1-1 — Seam (observation-only).** `m1_seam.{h,cpp}` on the `s21_seam.h` template. The two
  `#ifdef M1VK` sites above. Capture float `xx,yy` + sort index + resolved `col` + (for No Lighting) the
  pre-luma albedo/`lumval`/bank. `active()`/`sw_owns_3d()`/`gpu_owns_3d()` gating identical to S21. Still
  draws in software → output unchanged; this is the merge firewall.
- **M1-2 — GPU geometry.** `renderer_vk/m1_geom.{cpp,h}`. Record quads, two triangles each, draw in
  **submission/sort order** (painter's) into the frontend image — **no z attachment**. Wire
  `m1::set_gpu/set_no_3d` into the OSD dispatch at `retro_entry.cpp:864-890`. Moiré translucency → a
  stipple like Model 2's `checker`. A/B: GPU coverage matches the `M2VK_SW_3D` reference.
  ⚠️ **Do not add a depth buffer** — coplanar decals (ring floor `0x000004`, vertical gap 0.0) resolve
  by submission order *only*; a z-buffer z-fights them (`model1_sortorder.md` §7-8). This validates the
  draw-order choice hard.
- **M1-3 — High resolution.** Because M1-1 captured float `xx,yy`, hi-res is a vertex scale by the
  internal-res factor, reusing the entire P5 present machinery ([renderer_vk/vk_present.cpp]) and the
  existing resolution option. Native **496×384** (`model1.cpp:1736`, same visible area as Model 2 — the
  9-size list already fits; only the "(Native)" label needs pointing). Resolution-invariance via
  `res.sh`.
- **M1-4 — 2D compositing (the one non-trivial piece).** Port the S21 option-B under/over overlays for
  the `segaic24` layers: opaque 6/4/2/0 as the 2D-under background, 7/5/3/1 as the 2D-over HUD, with the
  3D drawn between at internal res. `color_xlat` is shared 2D/3D. Without this the stage backgrounds
  vanish (they are not geometry).
- **M1-5 — No Lighting + options/routing.** Wire the `color_xlat` LUT + `model2_flat_luma` toggle
  (above). Add `family::model1` to `apply_family_cascade` (`retro_entry.cpp:372`): **show** No Lighting +
  Internal Resolution + shared steering (vr/vformula/swa are analog — the `IPT_PADDLE`/`IPT_AD_STICK`
  detectors already cover them, no table); **hide** the S22-only options, Transparency, and Flat Shading
  (Model 1 is always flat — no textured-vs-flat distinction to toggle). Add M1 rows to
  `input_layouts.json`. Savestates: verify the TGP copro / `gen_fifo` state is covered by the existing
  `m2vk_savestate` trailer.
- **M1-6 (optional) — compat tail.** Wireframe-only modes (the driver still uses them), moiré edge
  cases, `vr`/`wingwar` specifics, NetMerc.

## Risks

1. **Upstream churn — SETTLED: tap the mame0288 baseline now.** The 2026 Model 1 rework in
   `upstream/master` (not in our tree): `#15642` "Improved video and timer emulation", `#15738` moiré
   direct polygons, `#15712` wireframe clip, `#15649` the TGP-ROM 2-bit fix (Wing War / SWA / NetMerc),
   `#15597` HUD/blink/wireframe — several touch the exact `draw_quads`/`fill_quad`/`screen_update` region
   we tap. **Decision: tap mame0288 as-is** and do M1-0…M1-4 against it (none care about the driver
   bugs), then fold the fixes in as a normal `mame0289+` sync **before M1-5/M1-6**, where SWA/Wing War
   correctness starts to matter. The seam is 2-3 hooks in new files, cheap to re-apply after that merge
   (the whole point of the mergeability rule). **Corollary: test against `vf` first** — the baseline
   renders it correctly; SWA/Wing War are the games the baseline gets wrong, so they are *not* early
   targets.
2. **Dual color space — resolved but must be honoured.** The two shading branches (`scale_color` 5:5:5
   multiply vs the `color_xlat` 64-step LUT) must both be reproduced per-quad or colours shift. Fully
   decode-specced in `model1_lighting.md` + `model1_colorxlat.py`; no open unknown, just care.

## Settled decisions (2026-08-29)

- **Reuse everything reusable.** No Lighting reuses the existing **`model2_flat_luma`** key; hi-res
  reuses the P5 resolution option + present machinery; 2D compositing reuses the S21 option-B
  under/over overlays; the OSD dispatch, `family_of()`/`apply_family_cascade`, savestate trailer and
  steering/analog detectors are all extended, not reinvented. New code is confined to `m1_seam.*`,
  `renderer_vk/m1_geom.*`, and the `color_xlat` LUT upload.
- **Tap the mame0288 baseline** (risk 1). Sync upstream Model 1 fixes before the M1-5/M1-6 tail.
- **First test target is `vf`** — the one game the baseline renders correctly.
