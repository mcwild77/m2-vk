# T2 — untextured 3D on the GPU (System 21)

## T2b status — DONE 2026-08-24

The `pri1==4` z-mix: `namcos21_c67_state::mix_layer0_sprites` gates layer-0 C355 sprites per pixel
against the polygon z-buffer, which the CPU no longer has once T2a turns the software rasteriser off.
Moved the gate onto the GPU, which still has a real depth attachment:

- **Driver (`namcos21_c67.cpp`):** new `capture_mix_sprites`, the same band-0 draw as the existing
  `pri1∈{0,2}` capture (into `m_mix_layer0_bitmap`, filled with the 0 sentinel first), called only when
  `pri1==4`. Per pixel it replicates `mix_layer0_sprites`' own branch — `pen & 0x5000` → priority-bank
  gated, `pen < 0x1000` → unconditional show, anything else (including untouched/0) → never shown — and
  packs the verdict as a **tag** in place of the pri[]-vs-zbuf comparison, which the CPU can no longer do.
- **Seam (`s21_seam.h`):** `capture_mix` template packs tag (0/1+bank/255) in the top byte, RGB below,
  exactly like `capture_over`'s alpha convention repurposed. `mix_begin/end/pixels/forget` mirror the
  `over_*` shape.
- **GPU (`s21_geom.cpp`, `shaders/s21_mix.frag`):** a fullscreen pass (rides `fullscreen.vert`, no vertex
  buffer) reading the tag/colour buffer from a plain host-visible storage buffer (no image/sampler — the
  same no-staging pattern the palette CLUT already uses). Tag 0 discards; tag 255 writes `gl_FragDepth =
  1.0` (always passes); tag 1..16 recomputes `pri[bank]` in-shader (the `/1.24` recurrence, 15 divides
  worst case — cheaper than a push-constant array, which would blow the 128-byte guarantee under GLSL's
  std430 array padding) and writes its mapped depth. The pipeline tests `GREATER_OR_EQUAL` against the
  attachment `geom_draw` just wrote (LOAD, not cleared, for this draw), write disabled — mirrors
  `pri[bank] <= z[x]` exactly, and sprite-vs-sprite order is the CPU capture's draw order, not a depth
  comparison. Drawn between `geom_draw` and the OVER pass (the OVER band-3 sprites stay unconditionally
  topmost, as before). Independent of whether the frame has any 3D quads at all — tracked separately in
  `geom_upload`.
- **Bug found and fixed, not T2b-specific:** `capture_over_sprites`/(now) `capture_mix_sprites` fire
  whenever `!sw_owns_3d()`, which `M2VK_NO_3D`'s `set_no_3d()` also sets — so the overlays populated and
  drew even under the "neither draws" background reference, corrupting it for every coverage comparison.
  Fixed with a new `gpu_owns_3d()` accessor (true only for `set_gpu(true)`) that `over_begin`/`mix_begin`
  now gate on. Pre-existing since T2a; just never exercised because T2a's own verification never ran the
  NO_3D reference for S21.

**Verification (cybsled, `retrohost --vk`, software vs vulkan, several frame counts):** cybsled's
`model2_internal_res` default applies a real **0.8× vertical rescale**, not a crop — comparing the two
PPMs requires resizing the software capture first, not cropping it (cost some time to notice). Once
compared correctly: HUD boxes (SHIELD POWER, radar dial, ITEM, TIME/WIN/LOSE/DRAW), the pilot-portrait
briefing screen, and pure-terrain combat frames all matched pixel-for-pixel or within the same
edge-rounding residual T2a found for starblad (~1.5–9% by frame, no structural mismatch). One unrelated
gap found: cybsled's in-HUD radar/camera-preview boxes differ from software — **root-caused 2026-08-24,
see the next section. It is NOT a windowing bug** (the `s21.h` "does not window the 3D" assertion is
correct); it is C355 palette-shadow compositing.

## T2a sub-viewport gap — FIXED by option B (pen-space composite), 2026-08-24

The full pen-space composite is built and verified. The whole S21 frame is carried as palette pen
INDICES and resolved to RGB once at the end, so the C355 palette-shadow OVER sprites index the
polygon-blend banks (1/2) by the real pen beneath them. Pipeline (all in `s21_geom.cpp` + new shaders):

- **Private R16_UINT pen render pass** (`pen_pass`, off to one side of the shared present pass, per slot):
  `s21_pen_under.frag` lays the 2D-under pens (fullscreen, captured by the new `s21::capture_under` in
  `namcos21_c67.cpp`), `s21_pen_geom.frag` draws the 3D quads depth-tested (the real z-buffer), and
  `s21_pen_mix.frag` does the layer-0 z-mix (gl_FragDepth thresholded). Leaves the composited pen in the
  attachment.
- **Finish pass** (`finish_draw`, one fullscreen draw INSIDE the shared present pass, in place of the
  UNDER draw): `s21_finish.frag` samples the pen attachment, applies the OVER band — opaque pens carried
  through, and the shadow banks as `bank_k | (composite_pen & 0x1fff)` — then resolves the final pen
  through the CLUT. `s21_seam.h`'s `capture_over` now packs a per-pixel tag (0 transparent / 1 shadow
  bank1 / 2 shadow bank2 / 3 opaque+pen) instead of a resolved colour; `capture_mix` packs tag+pen.

**Verification (cybsled `pri1==4` gameplay, retrohost --vk, sw vs vk, 6000f into the arena):** the
camera-preview panel is **pixel-exact**; the radar disc's shadowed lower region, the terrain shadows and
the opaque HUD are exact (opaque HUD diff 0.00–1.4%). The reported defect — the flat ~½-bright wrong
tint on the radar and panel — is gone. starblad (0.80% diff>24) and aircomb (1.92%) do **not** regress.

**One residual, NOT the shadow logic:** a uniform ~56×28 block at the radar's upper-centre reads
`(100,100,100)` in software and `(0,0,0)` in vk. A debug build (finish shader with the shadow disabled)
showed vk composites the **sky** (`30,96,158`) there while software has a nonzero poly pen (its shadow
`0x6000|0 = (100,100,100)`). `copy_visible_poly_framebuffer` copies poly pens with `if (pen)`, so
software genuinely has a far polygon at that spot that vk's geometry does not draw — a pre-existing
T2a-class geometry gap that the now-correct shadow makes visible. Left for a geometry pass, not a shadow
one. Needs the user's hand-check to confirm it is acceptable / was already present.

## T2a sub-viewport gap — root cause (superseded by the fix above, 2026-08-24)

The cybsled radar disc and camera-preview panel render at ~½ brightness vs software (radar
`(100,100,100)`→`(50,56,56)`; measured by navigating into gameplay, `pri1==4`). Isolation (geometry-only,
mix-only, over-only GPU captures) proved:

- The boxes are **C355 sprites in the OVER band (band-3)**, not 3D geometry and not windowing.
- They are **palette-shadow sprites**: `namcos21_c67_state::sprite_mix_callback` (c67 ~line 420), for
  sprite low-byte `0x00`/`0x01`, does not write its own colour — it ORs a bank select onto the
  **underlying** pen: `dest = 0x4000 | (dest & 0x1fff)` (or `0x6000`). The palette's banks 1/2
  (`0x4000..0x5fff`, `0x6000..0x7fff`, "polygon palette for sprite blending", c67 ~line 254) resolve
  that to a blended colour **of whatever is beneath**.
- `capture_over_sprites` draws these into a scratch bitmap **pre-filled with the transparent sentinel 0**,
  so the shadow lands on pen 0 → a constant `pens[0x4000]`, not the real backdrop. Software shadows the
  real scene pen. Hence the flat, wrong tint.
- Banks 1/2 are **independent gradient palettes**, NOT a scale of the base (measured: bank0 near-black
  `(1,1,1)` while bank1 ramps `(12,0,5)→(82,58,62)`; per-channel ratio stdev > mean). So an RGB
  multiply/alpha approximation is wrong — the fix **must** index banks 1/2 by the underlying pen.

The boxes sit over the 2D **sky** (an UNDER-layer pen) and 3D **terrain**, so the underlying pen can come
from any S21 layer. The present path composites in **RGB**, which throws that pen away.

**Decision (2026-08-24): fix by option B — full pen-space S21 composite.** Carry the whole S21 frame
(2D-under + 3D + mix) as **pen indices**, do all compositing including the shadow in pen space
(`dest = bank_k | (pen & 0x1fff)`), and resolve to RGB once at the end via the CLUT. This is a rework of
the committed T2a/T2b RGB-overlay present path, not a blend tweak. (Option A — a targeted shadow
pen-buffer leaving opaque over/mix as RGB — was the contained alternative; B was chosen for a cleaner,
uniformly-correct pipeline since S21 is fully palette-indexed anyway.)

## T2a status — DONE 2026-08-23

Star Blade's untextured 3D renders on the GPU with a real per-quad z-buffer, in place of the software
rasteriser, and matches software on the attract screen. Built in two increments:

- **Increment 1 — geometry pipeline.** New `renderer_vk/s21_geom.{h,cpp}` + `s21.vert`/`s21.frag`
  (16-byte vertex `{float x,y,z; uint pen}`, palette CLUT, depth `z = 1 − zsort/32768`, clear 0.0 /
  `COMPARE_GREATER` / write — reproducing `zsort < zbuf`). Seam grown with `set_gpu`/`sw_owns_3d`/
  `record_*`/`set_palette`; `namcos21_3d.cpp` skips `rendertri` when the GPU owns the 3D. First light
  showed correct geometry/perspective/CLUT, with the C355 **high-pri sprites hidden behind the 3D**
  (they were in the UNDER layer).
  - ⚠️ **Bug found and fixed:** the record must be **double-buffered**. The seam fires
    `frame_end()+frame_begin()` back-to-back at the single swap site, so a single-buffer record was
    zeroed by `record_begin` the instant `record_end` validated it — the GPU captured nothing. The
    record now swaps a work buffer into a visible buffer at `record_end`, mirroring the hardware's
    double-buffered framebuffer.
- **Increment 2 — the OVER-sprite sandwich.** Seam gains `over_begin/end/pixels/forget`;
  `namcos21_c67.cpp::capture_over_sprites` draws the C355 bands that sit over the 3D (high-priority,
  and — for `pri1∈{0,2}` — the flat layer-0) into a transparent overlay; `vk_present.cpp` composites it
  after the GPU 3D (an `s21_sandwich` parallel to `s22_sandwich`). The "GAME OVER / PLEASE INSERT COIN"
  overlay now sits correctly over the polygons.

**Verification (starblad attract, 4500f, retrohost --vk):** GPU vs software (same present path) —
**97.1% of pixels exact**, mean per-channel diff 0.34, the 5470 differing pixels confined to `y 140–273`
(polygon edges, the GPU/scanline coverage residual Model 2 also has). No structural mismatch. Model 2 /
namcos22 builds link and render unchanged (every `s21::` path is inert there, gated on `find("starblad")`
or an empty record).

**Deferred to T2b (now done, see above):** the `pri1==4` z-mixed layer-0 (`mix_layer0_sprites`) — those
sprites were omitted from the overlay until the z-mix moved onto the GPU depth buffer.

---

## Plan

**Started 2026-08-23.** Get Star Blade's flat-shaded, pre-projected polygons rendering on the GPU with
a real per-quad z-buffer, in place of the software rasteriser — A/B against software until the picture
matches. Depends on the T1 seam (`s21_seam.{h,cpp}`, two guarded hooks in `namcos21_3d.cpp`); mirrors the S22 GPU pass
([s22_geom.cpp](../src/osd/libretro_m2/renderer_vk/s22_geom.cpp)) stripped of everything textured.

## The headline difference from S22: a real z-buffer

S22's GPU pass is a painter's algorithm (depth test off) because the S22 tree is pre-sorted. **S21 is
genuinely z-buffered in hardware** and that is the accurate model, not an enhancement:
`renderscanline_flat` does `if (zsort < zbuf[x])` with `zbuf` cleared to `0x8000`, per-quad `zsort`. So:

- `gl_Position.z = zsort / 32768.0` — **flat** per quad (constant across the four corners; no interp).
- depth clear **1.0**, `VK_COMPARE_OP_LESS`, depth **write on**.
- strict `<` ⇒ first writer wins a tie ⇒ `LESS` (not `LESS_OR_EQUAL`). No `depth_scale`/`bias`
  dual-mode like s22.vert — this is the only mode.

## Vertex format — tiny

```c
struct s21_vert { float x, y, z; uint32_t pen; };   // 16 bytes
```
4 verts + 6 indices (`0,1,2, 2,3,0`) per quad — the exact `rendertri(v0,v1,v2)`/`(v2,v3,v0)` split.
Backface cull already done at the seam (culled quads never recorded) → **cull disabled**. No UV, no
brightness, no perspective. `s21.vert`: pixel→NDC (`in_xy / half_size − 1.0`), pass `z`, flat-out `pen`.

## Palette as a GPU CLUT

Unlike S22 (which resolves `basecolor` to RGB at the seam), the S21 device only knows pen indices — the
palette lives in the driver (`m_palette`). So: seam setter `s21::set_palette(pens, count)`, called from
the driver frame bracket with `m_palette->pens()`, re-read per frame (game writes palette RAM live),
uploaded as a per-slot storage buffer. `s21.frag`: `out = vec4(unpack(clut[pen]), 1.0)`.

## The hard part: C355 compositing → split T2a / T2b

[namcos21_c67 screen_update](../src/mame/namco/namcos21_c67.cpp) is a 3-band sandwich:
1. low-pri sprites (`draw(…,2)`) — **UNDER** the 3D
2. the 3D visible page
3. **`pri1`-dependent** middle: either layer-0 drawn flat over (`pri1∈{0,2}`), or `mix_layer0_sprites`
   — a **per-pixel z-mix** testing sprite priority against the polygon z-buffer (`pri[bank] <= z[x]`)
4. high-pri sprites (`draw(…,3)`) — **OVER**

Bands 1 & 4 map onto the existing UNDER/OVER capture (the `capture_over` pattern, m2vk frame layers).
Band 3's z-mix does not — and suppressing software 3D (so the GPU owns it) empties the CPU z-buffer the
mix reads. So:

- **T2a (this step): geometry on GPU, flat compositing.** Gate the software 3D off — skip `rendertri`
  when `!sw_owns_3d()`, and skip `copy_visible_poly_framebuffer` + `mix_layer0_sprites` in
  screen_update. GPU draws the polys (real z-buffer) between UNDER and OVER; treat layer-0 as plain OVER
  for now. **Exact A/B on `pri1∈{0,2}` screens** (service mode, some non-gameplay) — the
  geometry-correctness checkpoint. Gameplay (`pri1==4`) differs where layer-0 interleaves in depth;
  expected, deferred to T2b.
- **T2b: z-mix on the GPU depth buffer.** S21 uniquely *has* a GPU depth buffer after T2a — capture
  C355 layer-0 pixels + their `pri[]`-derived z-threshold, draw as a screen layer testing against the
  3D depth. This is what makes gameplay A/B match; the analogue of S22's `geom_draw_over`, but
  depth-tested rather than priority-flagged.

## Files (mirror S22, s/s22/s21/, s/ridgerac/starblad/)

- `renderer_vk/s21_geom.{h,cpp}` — record consumer + per-slot vertex/index/palette buffers + `geom_draw`.
- `renderer_vk/shaders/s21.vert` / `s21.frag` (+ committed `*_spv.h` via `build_shaders.sh`).
- `s21_seam.{h,cpp}` grows the T1-deferred scaffolding: `set_gpu`/`set_no_3d`/`sw_owns_3d`,
  `record_begin/quad/end`, `set_palette`/`get_palette`, the C355 capture hooks (T2b).
- `retro_entry.cpp`: family branch `driver_list::find("starblad") >= 0`; `s21::set_gpu(...)`;
  `family_dir = "system21"`.
- `vk_present.cpp`: `if (draw_3d_s21) s21::geom_draw(...)` between the UNDER blit and the OVER pass.
- Build: `s21_geom.cpp` moves to the **shared OSD** lua (both dylibs reference `s21::` symbols), the same
  S1→S2 move S22 made; `s21_seam.cpp` follows or stays per the link check.
- `namcos21_c67.cpp`: guarded screen_update hooks to capture the C355 UNDER/OVER layers and suppress the
  software 3D copy/mix when the GPU owns the 3D.

## Settled with the user (2026-08-23)

1. T2a/T2b split as above.
2. CLUT re-read per frame (matches how S22 re-reads its palette pointer).
3. `m_depth_reverse` is carried in the pen already (affects the colour cue, not the z test), so z
   handling is uniform across games; Winning Run's reverse-depth is a T4 concern, not T2.
