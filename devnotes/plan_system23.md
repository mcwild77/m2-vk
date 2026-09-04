# System 23 plan — fold Namco System 23 into Modelizer as a fifth family

Add **Namco System 23 / Super System 23** (Time Crisis II, Crisis Zone, and siblings) as a family
inside the same core that already runs Model 2, System 22/SS22, and System 21, routed at runtime by
`family_of()`. This is the sibling of [system22plan.md](system22plan.md) and
[system21plan.md](system21plan.md) — read those first for the seam philosophy and shared-OSD
mechanics this reuses. Unlike System 21 ("a smaller lift than S22"), **System 23 is the biggest lift
of the four ports done so far** — closer to a second System-22-sized effort than a quick add.
Verified against `src/mame/namco/namcos23.cpp` (392 KB, one driver for Gorgon/System 22.5, System 23,
Super System 23, and Evolution 2) this session. All 27 sets ship `MACHINE_NOT_WORKING`; Time Crisis II
is playable but glitchy, Crisis Zone additionally has input issues — no status doc written yet, this
plan carries the summary.

## Why this is the hard one (the headline)

Four facts push this past System 22's difficulty, not below System 21's:

1. **Depth is draw order + priority, same family as S22 — not S21's real z-buffer.** Every producer
   computes a 24-bit `zkey` (21-bit `zsort` OR'd with a 3-bit `absolute_priority` in the high bits),
   pushes a `namcos23_poly_entry` into `render.polys[]`, and `render_flush()`
   (`namcos23.cpp:4362`) `qsort()`s the whole frame by `zkey` before rasterizing back-to-front. So
   the depth model is the **painter's-algorithm** one we already ported for S22 — good news, no new
   depth architecture — but it inherits S22's coplanar-decal/Z-sort fragility, and the driver's own
   TODO list names it explicitly (`panicprk`'s mini-games break on Z-sort).
2. **The shading tail is roughly 2× System 22's.** `render_flush` builds a 6-bit `render_hash` from
   independent booleans — `stencil_enabled`, `shade_enabled`, `pfade_enabled` (poly-fade),
   `fadefactor != 0xff` (screen/color fade), `blend_enabled`, `alpha != 0xff` (poly-alpha) — and
   dispatches through **64 template instantiations** of `render_scanline<...>`
   (`RENDER_SCANLINE_ENTRY`, one `case` per combination). S22's shading tail (fog / z-fog / fade /
   poly-alpha / gamma) is a linear chain of maybe 5 toggles; this is a genuine combinatorial state
   space, plus a **stencil buffer** (`stencil_lookup`) and a **priority map write-back**
   (`primap`/`prioverchar`, feeding `screen.priority()` so the 2D layer can occlude correctly) that
   neither S22 nor S21 has in this shape.
3. **Four distinct primitive producers feed one shared render list**, not one:
   `render_model()` (indexed 3D models, textured, the main path), `render_direct_poly()` (explicit
   polys streamed straight over PIO/DMA — no model lookup), `render_immediate()` (small immediate-mode
   polys), and `render_sprite()`/`render_sprite_tile()` (row×col tiled 2D sprites with their own
   xflip/yflip/zcoord/alpha). Each independently builds a `poly_vertex pv[]`, calls
   `zclip_if_less<4>()`, computes its own `zkey`, and appends to `render.polys[]`. A GPU seam has to
   sit at all four sites (or unify them behind one recorder), where S21/S22 had exactly one producer
   (`blit_single_quad` / `poly3d_drawquad`).
4. **Five state subclasses override rendering per game family**, each potentially diverging:
   `namcos23_state` (base — downhill/motoxgo/panicprk/timecrs2), `namcoss23_state` (Super System 23 —
   500gp/aking, `timecrs2v4a`/`v5a`), `namcoss23_gmen_state` (adds GMEN comms — gunwars/raceon),
   `gorgon_state` (System 22.5 — rapidrvr/finfurl, its **own** `render_run`/`render_sprite` override,
   different sprite table layout — see the `gorgon_state::render_run` snippet at
   `namcos23.cpp:4478`), `finfurl2_state`, `crszone_state` (Evolution 2). `dispatch_render_entry` is
   virtual and overridden per class. The seam must be virtual-call-safe the way Model 2's per-game
   hooks already are, not assume one code path.

None of this is a reason not to do it — it's the reason to scope tightly (§ below) and expect a
longer T-series-equivalent arc than System 21's.

## What carries over unchanged (reuse, don't re-derive)

- **Never re-implement the CPU-side math.** Matrices are set up via C435 command-stream ops
  (`c435_matrix_matrix_mul`, `c435_matrix_vector_mul`, `c435_matrix_set`, `c435_vector_set`) and
  applied per-vertex in `render_apply_transform`/`render_apply_matrot`
  (`namcos23.cpp:3677,3688`), then `render_project` (`namcos23.cpp:3695`) does the perspective divide
  with a per-viewport FOV. Exactly like Model 2/S22/S21: let MAME's own C code do all of this: the
  seam taps the **already-transformed, already-projected** `poly_vertex pv[]` each producer builds,
  the same policy as `poly3d_drawquad`'s pre-projected quad and `blit_single_quad`'s screen-space
  `sx/sy`.
- **Texture atlasing is the S22 model, not new.** `tmlrom`/`tmhrom` (tile-ID → texrom address +
  attribute, decoded once into `m_tmrom_decoded`/`m_texattr_decoded` in the renderer constructor,
  `namcos23.cpp:2307`) plus `texrom`/`texram` is architecturally the same tile/microtexture split S22
  already has a GPU path for (`s2-gpu-geometry.md`). Reuse the atlas-upload approach, not reinvent it.
- **Custom float format, already a known quantity.** `f24_to_f32` (`namcos23.cpp:2338`) unpacks an
  8-bit-exponent/16-bit-mantissa custom float — decode once at the seam into IEEE `float`, same as
  any other cross-seam data normalization.
- **2D layer**: the text/tilemap layer is "identical to System 22 & Super System 22" per the driver's
  own header comment — the S22 `namco_c123tmap` compositing (`capture_under`/`capture_over` pattern)
  should transfer with only the priority-map interaction (point 2 above) as new work.
- **Shared-OSD mechanics** (family detection via `driver_list::find()`, per-family `hide_option()`,
  one dylib) are already built and need no new plumbing — see `system21plan.md`'s "part of the System
  22 core" section, same story here.

## Scope — which games, and why only these to start

ROMs on hand (`devnotes/roms/system23/`): `timecrs2.zip`, `timecrs2v4a.zip`, `crszone.zip`,
`namco_tssio.zip` (JVS I/O device ROM, required by all sets).

| Set | Driver class | Status flag | Phase |
|---|---|---|---|
| `timecrs2` (US, TSS3 Ver. B) | `namcos23_state` | `NOT_WORKING \| IMPERFECT_GRAPHICS` | **Primary test case** |
| `timecrs2v4a` (World, TSS4 Ver. A) | `namcoss23_state` | same | Confirms the Super System 23 path (`namcoss23_state` subclass) matches `namcos23_state` |
| `crszone` (World, CSZO4 Ver. B) | `crszone_state` | `NOT_WORKING \| IMPERFECT_GRAPHICS` | **Secondary test case** — Evolution 2, own subclass, driver notes separate "Input issues" (orthogonal to rendering — likely unrelated MCU/JVS issue, triage before blaming the renderer port) |

Everything else in the driver (`rapidrvr`/`finfurl` = Gorgon, `downhill`, `motoxgo`, `panicprk`,
`gunwars`, `raceon`, `500gp`, `aking`, `finfurl2`) is **out of scope until its ROM is acquired** —
don't widen scope speculatively; System 21 and System 22 both widened *after* the seam and shading
tail were proven on one game each.

## Seam — located, not yet tapped

- **Per-primitive tap points (four, not one):** `render_model()` (`namcos23.cpp:4026`),
  `render_direct_poly()` (`namcos23.cpp:3708`), `render_immediate()` (`namcos23.cpp:3915`), and
  `render_sprite()`/`render_sprite_tile()` (`namcos23.cpp:2033-2034`, subclass-overridable). Each
  computes `zclip_if_less<4>(ne, pv, p->pv, 0.0001f)` then fills a `namcos23_poly_entry` (`pv[16]`,
  `namcos23_render_data rd`, `zkey`, `vertex_count`) that gets appended to `render.polys[]`. Snapshot
  at that append point in all four functions — the plain-C-struct pattern every prior seam uses
  (`s22_seam.h`, `s21_seam.h`-equivalent).
- **Frame bracket:** `render_flush()` (`namcos23.cpp:4362`) is where the collected list is `qsort`'d
  by `render_poly_compare` and walked back-to-front through the 64-way shading dispatch. The
  begin/end hook goes around this — record the **sorted order** (not insertion order) since that
  sort *is* the depth result, matching S22's painter's-pass philosophy.
- **`namcos23_render_data`** (`namcos23.cpp` struct above line 1480) is the per-poly extra-data
  payload — texture id/mode (`cmode`, `tbase`), CZ (depth-cue) params (`cz_value`, `cz_type`,
  `fogfactor`), poly-fade/color-fade/alpha/blend factors, viewport size/offset/FOV, and the
  `sprite`/`direct`/`immediate`/`model` flags that say which producer built it. This is the field
  list the cross-seam snapshot struct needs to carry — it is the union of everything S22's shading
  tail carries, plus stencil and the two extra fade channels.

## Depth-model and shading-tail risk (read before committing to a design)

- **Confirm draw-order-vs-real-depth the way P4 did for Model 2**, not assume it from S22's
  precedent — the header's Z-sort complaints (`panicprk`) suggest this family's sort key is *less*
  reliable than S22's, not equally reliable. Measure mis-ordered-pair counts before trusting a
  painter's GPU pass to be sw-accurate here.
- **Do not attempt all 64 shading combinations at once.** Build the untextured/no-fade/no-blend
  corner first (`render_hash == 0`), get one frame of Time Crisis II on the GPU bit-plausible, then
  add stencil, then fade, then blend/alpha — mirroring S22's S2a→S2d incremental order
  (untextured → textured → 2D-over → shading tail), just with more steps because there are more
  independent toggles here.
- **The priority map (`primap`/`prioverchar`) is new territory** — it's a per-pixel byte plane the 3D
  writes that the 2D tilemap composite reads back to resolve occlusion. Neither S22 nor S21 needed a
  GPU-side priority buffer; this likely wants a second small render target, not a repurposed depth
  attachment (avoid the mistake `zfighting.md` documents — a borrowed attachment for an unrelated
  purpose corrupted unrelated state there).

## Phases (mirror S22's S0-S2 / S21's T0-T5 arc; each gets its own short file only when started)

- **23-0 — subtarget boot, software only.** Add `namco/namcos23.cpp` to the Modelizer `.flt`/`.lua`
  (already lists `sega/model2.cpp`, `namco/namcos22.cpp`, `namco/namcos21.cpp`,
  `namco/namcos21_c67.cpp` — this is the fourth driver file, not a new subtarget). Confirm `timecrs2`
  boots and renders 3D **in software** through the shared OSD; record the sw baseline digest with
  `retrohost`. Triage `crszone`'s separately-noted input issue here — if it's a JVS/MCU bug unrelated
  to rendering, document it and don't let it block 23-1/23-2.
- **23-1 — seam tap (record, draw nothing).** New `s23_seam.h/.cpp`; guarded hooks at all four
  producer append sites plus the `render_flush` frame bracket. Record the sorted poly stream, draw
  nothing, output **byte-identical** to the 23-0 baseline. Measure and record the real upstream diff
  — this driver's sheer size (392 KB) makes the diff-budget discipline (new files only, guarded hook
  calls in upstream) more important here than anywhere else so far.
- **23-2 — untextured GPU pass, `render_hash == 0` only.** New `s23_geom.{h,cpp}` + shaders: flat/
  textured-off quads, draw-order depth via the recorded sort (no z-buffer, no shading tail). Prove
  Time Crisis II's basic geometry (title/attract, in-game corridor) matches software before adding
  anything.
- **23-3 — textures.** Wire the tile atlas (`tmlrom`/`tmhrom`/`texrom`/`texram` decode, reusing the
  S22 atlas-upload code path) onto the 23-2 geometry.
- **23-4 — shading tail, incrementally.** Add stencil, then poly-fade, then screen/color-fade, then
  blend, then poly-alpha — one flag at a time against the corresponding `render_hash` bit, each with
  its own A/B checkpoint. This phase is where most of the session budget will go; do not batch it.
- **23-5 — priority map + 2D compositing.** Land the primap/prioverchar plane and the text-tilemap
  `capture_under`/`capture_over` composite (reusing S22's approach per "What carries over" above).
- **23-6 — sprites.** `render_sprite`/`render_sprite_tile`'s row×col tiled path, including the
  `gorgon_state` sprite-table variant if Gorgon ever comes into scope (not in the initial two-game
  set — Time Crisis II and Crisis Zone are both `namcos23_state`-family, not Gorgon).
- **23-7 — widen + polish.** `timecrs2v4a` (confirms the `namcoss23_state` subclass path), options
  menu (hide S22/S21-only toggles the way T3 did), ~~savestates~~ (🚫 void — disabled core-wide
  2026-09-04), per-game pad layout (both are
  light-gun games — reuse `lightgun.md`'s reticle/reload/offscreen-reload mechanics directly, they
  don't need re-deriving), compat-matrix rows.

## Posture (same as every prior family)

- All new logic in NEW files; the only edits to `namcos23.cpp` are guarded hook calls. Measure the
  diff, never quote a fixed number — this file alone is bigger than the other three drivers combined,
  so "the diff is still N lines" will be wrong fast if repeated without measuring.
- No scripted button-press testing — build it, run the static guards
  (`M2VK_HOST_DESCRIPTORS`/digests/`ppmdiff`), then hand the user a hand-check list. Both games here
  are light-gun games, so the hand-check will look like `lightgun.md`'s, not a pad-mapping one.
- Commit hygiene: no AI nomenclature anywhere. `devnotes/` and `CLAUDE.md` stay local-only.

## Status — 23-0 DONE (2026-08-29)

`namco/namcos23.cpp` (+ its three private device files `md8412b_s23`/`namco_settings`/`vpx3220a`) is
wired into the Modelizer build as a fifth driver project `mame_namcos23` (`S23VK` scoping define, no
hooks yet) in `scripts/target/mame/modelizer.lua`, and added to `src/mame/modelizer.flt`. No edits to
`namcos23.cpp` itself yet — this phase is pure build plumbing.

Getting it to **link** needed five extra global feature flags beyond namcos23's own direct
dependencies, all because `CPUS["MIPS3"]` and `BUSES["JVS"]` unconditionally pull in code for
hardware this driver doesn't use (PS2 vector-unit glue, a JVS LED-sign peripheral) that the stock MAME
build always has satisfied elsewhere: `VIDEOS["PS2GIF"]`, `VIDEOS["PS2GS"]`, `MACHINES["PS2INTC"]`,
`MACHINES["INTELFLASH"]`. Namcos23's own real deps: `CPUS` H8/MIPS3/SH/F2MC16 (the last for the
TSS-I/O board's mb90570), `MACHINES` RTC4543/I2CHLE, `BUSES` JVS, `SOUNDS` C352 (shared with S22).



**Software boot confirmed for all three in-scope sets**, `retrohost` (no `--vk`), 3000 host frames:

| Set | Boots | Digest (3000 frames) | Notes |
|---|---|---|---|
| `timecrs2` | ✅ | `beb6f072f9a745c8` | Full textured 3D attract intro by frame 3000. |
| `timecrs2v4a` | ✅ | `65c47f69957b0af2` | Confirms the `namcoss23_state` path boots too. |
| `crszone` | ✅ | `b48f3a0f55a7d824` | Needed a second JVS I/O ROM, see below. |

`crszone` initially failed with `csz1prg0a.8f NOT FOUND` — it uses a **light-gun-specific** JVS I/O
board (`namco_csz1`, `namcoio.cpp:1290`), not the generic `namco_tssio.zip` board `timecrs2` uses.
Needed `devnotes/roms/system23/namco_csz1.zip` (`csz1prg0a.8f`) alongside `namco_tssio.zip` — both
boards are now on hand. The separately-noted "input issues" for `crszone` are still untriaged (were
out of scope for 23-0, which only proves the boot).

These are `retrohost`-without-`--vk` digests (MAME's own software rasterizer through the shared OSD),
not yet a Vulkan A/B baseline — there's no GPU pass to compare against until 23-2.

## Status — 23-1 DONE (2026-08-29)

Seam tapped and byte-identical. New `s23_seam.h/.cpp` (added to `libretro_m2.lua`); one guarded
`#ifdef S23VK` bracket in `namcos23.cpp` `render_flush()` — `frame_begin` at the top of the sorted
walk, `submit(poly)` per entry in **sorted (draw) order**, `frame_end` before the `poly_count` reset.
Diff: **44 insertions / 0 deletions** to `namcos23.cpp`, all guarded and all new (measure with
`git diff --shortstat mame0288 -- src/mame/namco/namcos23.cpp`).

**Design note — one tap, not four.** The plan floated hooks at all four producer append sites. Not
needed: every producer funnels into one `namcos23_poly_entry` (it already carries `pv[16]` + the
producer flags `direct`/`immediate`/`sprite` in `rd`, "model" = none of those set), and `render_flush`
walks the qsorted list. Tapping that single walk is the plan's own "or unify them behind one recorder"
option — it yields the **sorted** stream (the depth result) for free and needs one bracket instead of
five. `s23::poly` snapshots num_verts, screen `x/y`, `pv.p[0]`, `zkey`, the four producer flags, the
six `render_hash` booleans, `cmode`/`tbase`/`model_id`.

Tap is `M2VK_S23TAP=1` / `M2VK_S23TAP_EVERY=N` (inert otherwise — `g_active` stays false, output
byte-identical). Verified vs the 23-0 baselines (3000 host frames, tap on):

| Set | Digest (tap on) | Matches 23-0 | Tap observations |
|---|---|---|---|
| `timecrs2` | `beb6f072f9a745c8` | ✅ | attract all `model` polys, ~1k–2.3k/scene; `shade`+`pfade` on every poly, some `blend`; no direct/imm/sprite |
| `crszone` | `b48f3a0f55a7d824` | ✅ | ~4k–6k model polys/scene; **stencil path live** (st 511), exercising a `render_hash` bit timecrs2 doesn't |

(`x/y` in the tap lines are pre-scissor projected coords — off-screen extents run to ±billions before
the rasterizer's `(0,639,0,479)` scissor clips; a diagnostic bbox only, not a bug.) No sprite/direct/
immediate primitives appeared in either game's tested screens — those producers await gameplay/later
screens; the snapshot carries their flags regardless.

## Status — 23-2 DONE (2026-08-29)

Untextured GPU pass live and geometry-plausible. New `renderer_vk/s23_geom.{h,cpp}` +
`shaders/s23.{vert,frag}` (a stripped `s22_geom`: no texture system, no palette, **no descriptor sets** —
the shade rides the vertex, the only uniform is the visible half-extent). Painter's order (depth test
off, last-writer-wins over the qsorted stream), one indexed draw per viewport-window scissor run. The
seam grew `set_gpu`/`set_no_3d`/`gpu_owns_3d` + the `record_begin/poly/end` consumer, and the `poly`
snapshot now carries `ish[]` (param[3], the shade term) and the four `vp_size/offset` fields; the driver
hook fills them and one guarded `if (s23::sw_owns_3d())` skips the 64-way software dispatch when the GPU
owns the 3D. Wired into `vk_present.cpp` (build/upload/draw/end_run + `draw_3d_s23` + poly counter) and
`retro_entry.cpp` (`family::system23`: `family_of` on the `namcos23` token, an `apply_family_cascade`
branch at 640×480 hiding every family-specific option, the `set_gpu` gate, `family_dir` "system23").

**The untextured stand-in is the greyscale "lit geometry" view** — System 23 has *no* untextured path
(every `render_scanline` pixel is a texel fetch), so 23-2 replaces the texel with white and applies only
the per-pixel SHADE step (`shade = clamp(ish/ooz, 0, 63)`, `luma = shade/64`; full white where a poly has
shade disabled). Same idea as System 22's `system22_no_textures`. `render_hash` is otherwise ignored —
stencil/fade/blend/alpha are all drawn as the hash-0 corner, the accepted inaccuracy until 23-4.

Verified (`retrohost --vk`, timecrs2, 3000 frames): `s23: first GPU geometry` fires, ~2.0 M polys over
the run (max 2352/scene), GPU digest `5f0b885c8117240b`. The rendered frame is a clean **silhouette match**
to the software reference — same corridor, same figure pose/position, same painter's depth order; the only
differences are the missing textures (23-3) and the 2D-over HUD text bands (23-5), both out of scope.
`M2VK_SW_3D=1` returns the software rasteriser **bit-exact** (`beb6f072f9a745c8` == the 23-0 baseline), so
the `sw_owns_3d` gate is clean both ways. Screenshot: `devnotes/screenshots/2026-08-29-timecrs2-23-2-untextured.png`.

Driver diff to `namcos23.cpp`: **58 insertions / 0 deletions**, all guarded `#ifdef S23VK` and all new
(measure with `git diff --shortstat mame0288 -- src/mame/namco/namcos23.cpp`). Everything else is new files.

Tap knobs unchanged (`M2VK_S23TAP*`); the geometry pass adds `M2VK_NO_SCISSOR=1` (collapse every viewport
window to full-screen) and honours `M2VK_SW_3D`/`M2VK_NO_3D` like every other family.

## Status — 23-3 DONE (2026-08-29)

Textures live and a texel-for-texel match to software. `namcos23_renderer::texture_lookup` +
render_scanline's per-pixel texel/shade steps are transliterated into `s23.frag`; the tile system is
uploaded as storage buffers exactly as it sits in the driver's arrays, one indirection shorter than S22
(the tileid→address lookup is already resolved into `m_tmrom_decoded`, and there is no `ayx` table).

**Design note — decoded arrays, not the raw tilemap ROMs.** The plan named `tmlrom`/`tmhrom`, but the
renderer decodes those once in its constructor into `m_tmrom_decoded` (tileid → texrom base, already
`<<8`) and `m_texattr_decoded` (tileid → 3-bit orientation). Uploading the *decoded* arrays means the
shader skips the tmlrom/tmhrom/attr unpack the constructor already did — three static buffers
(`tmrom_decoded` u32, `texattr_decoded` u8, `texrom` u8) plus the per-frame palette, bound through one
4-binding descriptor set. `texram` (the C412 sram) is stencil-only and stays out until 23-4.

Seam grew `uoz[]`/`voz[]` (param[1]/[2]) and `pens_base` on `s23::poly`, and a `texture_rom` struct +
`set_texture_rom`/`get_texture_rom` (the S23 analogue of S22's `texture_ram`). The driver hook fills them
and hands over the renderer's decoded arrays + texrom + live palette; one guarded `friend class
namcos23_renderer;` lets `render_flush` reach the protected `m_palette`. The geometry pass grew the
descriptor set/pool, `upload_static` (ROM-derived buffers, once) and a per-frame palette copy; `s23.frag`
does the fetch, the cmode pen resolve, and the shade. Four push-constant masks carry the game's fixed
array sizes. The untextured 23-2 stand-in (flat white + shade) is gone — every pixel is a real texel now.

Verified (`retrohost --vk`, timecrs2, 3000 frames): GPU digest `452549c208678955` (was `5f0b885c8117240b`
untextured); ~2.0 M polys, max 2352/scene, unchanged from 23-2 (same geometry, textured). The rendered
frame is a **texel-for-texel match** to the software reference — same corridor, girders, wall/floor
panels and character, now with their texture detail instead of the greyscale silhouette; the only
differences are the 2D-over HUD text bands (the priority-map sandwich, 23-5, still out of scope).
`M2VK_SW_3D=1` returns the software rasteriser **bit-exact** (`beb6f072f9a745c8` == the 23-0 baseline),
so the `sw_owns_3d` gate is clean both ways. Screenshot:
`devnotes/screenshots/2026-08-29-timecrs2-23-3-textured.png`.

Driver diff to `namcos23.cpp`: **82 insertions / 0 deletions**, all guarded `#ifdef S23VK` and all new
(measure with `git diff --shortstat mame0288 -- src/mame/namco/namcos23.cpp`). Everything else is new
files (the two decoded-array uploads, the descriptor set, the textured shaders).

**Next: 23-4** — the shading tail. Add stencil (the `texram` cutout), then poly-fade, then screen/color
fade, then blend, then poly-alpha — one `render_hash` bit at a time, each with its own A/B checkpoint.

## Status — 23-4 stencil DONE (2026-08-29)

First of the five 23-4 increments: **stencil** (`render_hash` bit 5, the `texram` alpha-cutout). A
`stencil_enabled` poly discards any pixel the C412 sram masks off — `namcos23_renderer::stencil_lookup`
transliterated into `s23.frag` (`stencil_cut`), run on the **pre-tbase** texel coords exactly where the
software `if ((!Stencil || !stencil_lookup(tx, ty)) && …)` runs it, then `discard`. No dst read, so it
drops straight into the painter's pass.

Plumbing: the C412 sram (`m_texram`, 0x20000 u16 = 256 KB) is live RAM the game writes, so — like the
palette — it re-uploads every frame. New per-slot `texram` buffer, a **5th** descriptor binding, a
`FLAG_STENCIL` vertex bit; the seam's `texture_rom` grew `texram`/`texram_count` and the driver hook fills
them. Driver diff to `namcos23.cpp`: **84 insertions / 0 deletions**, all guarded `#ifdef S23VK` (was 82 at
23-3, +2 for the two `trom.texram*` lines). Everything else is new-file/shader edits.

Verified (`retrohost --vk`, 3000 frames):

| Run | Digest | Meaning |
|---|---|---|
| `crszone` SW gate (`M2VK_SW_3D=1`) | `b48f3a0f55a7d824` | == 23-0 baseline — the `sw_owns_3d` path is untouched |
| `timecrs2` GPU | `452549c208678955` | **bit-identical to 23-3** — stencil path inert on no-stencil content (no regression) |
| `crszone` GPU | `2670651b9c323aff` | new — the stencil path fires (crszone is the set the 23-1 tap saw at "st 511") |

Frame-matched visual A/B (crszone intro, the URDA commander): the GPU render is a texel match to
software — same ceiling/doors/"E SPORTS" sign, and the uniform's medal ribbons and collar insignia (the
alpha-cutout stencil detail) match. The only difference is the missing 2D-over HUD text bands (the
priority-map sandwich, 23-5, out of scope). The `ppmdiff coverage` "100 % B / 2651 interior" is that
missing text plus full-frame GPU-vs-SW float shading, not a stencil bug — confirmed by eye, means match
within ~4/channel. Screenshot: `devnotes/screenshots/2026-08-29-crszone-23-4-stencil.png`.

**Next in 23-4:** poly-fade (`polycolor_r/g/b`, bit 3), then screen/color fade (`fadefactor`/`fadecolor`,
bit 2) — both pure per-pixel colour math, no dst read — then blend (bit 1) and poly-alpha (bit 0), which
do need a destination read (fixed-function blend or a rework of the painter's pass).

## Status — 23-4 poly-fade + colour-fade DONE (2026-08-29)

The two pure-per-pixel-colour steps, both no destination read, so both drop straight into the painter's
pass beside SHADE and stencil. Transliterated from `render_scanline` (`namcos23.cpp:3639-3651`) in the
exact order and integer arithmetic the software rasteriser uses, applied after SHADE:

- **poly-fade** (`render_hash` bit 3, `PolyFade`): `c = (c * polycolor) >> 8`, a per-poly colour
  multiply (`polycolor_r/g/b`, u8).
- **colour-fade** (bit 2, `ColorFade`): `c = (c*fadefactor + fadecolor*(0x100-fadefactor)) >> 8`, a lerp
  toward `fadecolor` by `fadefactor`. The driver keeps `fadefactor_inv = 0x100 - fadefactor`; the shader
  recovers it, so only `fadefactor` + `fadecolor_r/g/b` cross the seam. Both `>>8` sums are bounded to
  0..255 (proper complementary lerp), matching software's un-clamped `<<16` store with no divergence.

Plumbing: all source values are u8, so the seam's `poly` grew seven bytes (`polycolor_r/g/b`,
`fadefactor`, `fadecolor_r/g/b`); the geometry pass packs them into **two new flat u32 vertex attributes**
(`pfade` = polycolor RGB, `cfade` = fadefactor|fadecolor RGB) and two new flag bits (`FLAG_PFADE`,
`FLAG_COLORFADE`) — no new descriptor binding, no dst read, no pipeline blend state. Vertex stride 36 → 44.
Driver diff to `namcos23.cpp`: **91 insertions / 0 deletions**, all guarded `#ifdef S23VK` (was 84 at
stencil, +7 for the fade fill). Everything else is new-file/shader edits.

Verified (`retrohost --vk`, 3000 frames):

| Run | Digest | Meaning |
|---|---|---|
| `timecrs2` SW gate (`M2VK_SW_3D=1`) | `beb6f072f9a745c8` | == 23-0 baseline — `sw_owns_3d` path untouched |
| `crszone` SW gate (`M2VK_SW_3D=1`)  | `b48f3a0f55a7d824` | == 23-0 baseline |
| `timecrs2` GPU | `7d6a0beb1b3a46a1` | was `452549c208678955` — poly-fade fires (the 23-1 tap saw `pfade` on every attract poly) |
| `crszone` GPU  | `797c296be2c862b3` | was `2670651b9c323aff` — fade fires |

Poly counts unchanged (timecrs2 2352/scene, crszone 6268/scene) — fade is per-pixel colour, not geometry.
Frame-matched A/B (timecrs2 attract): the GPU 3D is a match to the software reference — same corridor,
girders and character, same fade colouring; the only difference is the 2D-over HUD text bands (the
priority-map sandwich, 23-5, out of scope). Screenshots:
`devnotes/screenshots/2026-08-29-timecrs2-23-4-polyfade-colorfade.png`,
`…-crszone-23-4-polyfade-colorfade.png`.

**Next in 23-4:** blend (bit 1, fixed 50%) and poly-alpha (bit 0, `alpha`/`alpha_inv` over the dest pen)
— the two that read the framebuffer. These cannot ride the painter's pass unchanged: either a
fixed-function blend attachment (loses the exact `(r*a + dr*inv)>>8` integer form and the `pen ==
alpha_pen` per-texel gate) or a rework so the pass can read dst (a self-referencing input attachment or a
back-to-front deferred pass like Model 2's `model2_transparency=blended`). Decide before building.

## Status — 23-4 blend + poly-alpha DONE (2026-08-29) → 23-4 COMPLETE

The last two shading-tail steps, the ones that read the framebuffer. **Design decision (fixed-function
blend, chosen for low-spec):** the painter's pass is already back-to-front (the seam records in
`render_flush`'s qsorted order), so a fixed-function over-blend reproduces `render_scanline`'s dst read
for free — no self-referencing input attachment, no deferred pass, and blend runs in tile memory on the
Adreno target (zero framebuffer round-trip). The rejected alternatives (input-attachment / framebuffer
fetch, or a Model-2-style deferred back-to-front pass) preserve the exact integer form but cost barriers
or an extra pass for a sub-1-LSB gain. Rationale is the "which one for low-spec" note near the top of the
plan and in `s23_geom.cpp`'s `blend_attachment` comment.

**One always-on blend pipeline, not a per-poly switch.** The fragment emits a per-pixel SRC weight `a` in
`out_color.a` and the blend unit does `src*a + dst*(1-a)` (`SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA`). An opaque
fragment sets `a = 1.0`, which yields `src` exactly, so the same pipeline serves opaque and translucent
alike — no batching by blend mode, no second pipeline. The shader reproduces `render_scanline`'s exact
priority: poly-alpha (bit 0) wins over blend (bit 1) where its **per-texel gate** (`alpha_enabled || raw
texel pen == alpha_pen`) passes → `a = alpha/256`; else blend → `a = 0x80/256` (fixed 50%); else opaque.
`a = alpha/256` keeps software's `>>8` (÷256) basis on **both** blend terms, so the only divergence is
unorm rounding (<1 LSB). The 3D over-pass writes **RGB only** (alpha masked out) so it never clobbers the
target's alpha with a fractional weight.

Plumbing: the seam's `poly` grew `alpha`/`alpha_pen`/`alpha_enabled` (the driver hook fills them from
`rd.alpha`/`poly_alpha_pen`/`alpha_enabled`); the geometry pass packs them into **one new flat u32 vertex
attribute** (`ablend` = alpha | alpha_pen<<8 | alpha_enabled<<16) and two new flag bits (`FLAG_BLEND`,
`FLAG_POLYALPHA`), stride 44 → 48, and flips the pipeline's `blend_attachment` to blend-enabled/RGB-mask.
No new descriptor binding. Driver diff to `namcos23.cpp`: **94 insertions / 0 deletions**, all guarded
`#ifdef S23VK` (was 91 at colour-fade, +3 for the alpha fill). Everything else is new-file/shader edits.

Verified (`retrohost --vk`, 3000 frames):

| Run | Digest | Meaning |
|---|---|---|
| `timecrs2` SW gate (`M2VK_SW_3D=1`) | `beb6f072f9a745c8` | == 23-0 baseline — `sw_owns_3d` path untouched |
| `crszone` SW gate (`M2VK_SW_3D=1`)  | `b48f3a0f55a7d824` | == 23-0 baseline |
| `timecrs2` GPU | `a05060b3a153dd54` | was `7d6a0beb1b3a46a1` — blend fires (the 23-1 tap saw `blend` on some attract polys) |
| `crszone` GPU  | `706d1f8da138d80d` | was `797c296be2c862b3` — blend/alpha fire |

Poly counts unchanged (timecrs2 2352/scene, crszone 6268/scene) — blend/alpha are per-pixel, not
geometry. Frame-matched A/B (both attract scenes): the GPU 3D matches the software reference — corridor/
girders/character for timecrs2, the URDA commander (ceiling, "E SPORTS" sign, medal-ribbon stencil
detail) for crszone; the only difference is the 2D-over HUD text bands (the priority-map sandwich, 23-5,
out of scope). Screenshots: `devnotes/screenshots/2026-08-29-timecrs2-23-4-blend-alpha.png`,
`…-crszone-23-4-blend-alpha.png`.

**23-4 is complete** — all six `render_hash` bits (SHADE + stencil + poly-fade + colour-fade + blend +
poly-alpha) now render on the GPU. **Next: 23-5** — the priority map (`primap`/`prioverchar`) + the
text-tilemap `capture_under`/`capture_over` composite, which lands the missing 2D-over HUD.

## Status — 23-5 2D-over composite DONE (2026-08-29)

The missing HUD/text bands now sit over the GPU 3D — the last visible gap. It is the **plain System 22
sandwich, reused verbatim**, not the priority-map/second-render-target design the plan hedged for.

**Design decision — no priority buffer, no over-pass.** The plan flagged `primap`/`prioverchar` as new
territory (a per-pixel byte plane the 3D writes, read back to resolve occlusion) and floated a second
render target. It isn't needed for the in-scope games: `namcos23_renderer::render_flush`'s software loop
forces `extra.prioverchar = 2` on **every** primitive (`namcos23.cpp:4473`), so `primap[x] = (primap[x]
& ~1) | prioverchar` turns every text pixel the 3D covers into priority 6 → the `mix_text_layer(…, 6)`
second pass redraws it. Net effect: **the text layer is always entirely over the 3D**. So there is no
"primitive over text" (priority 7) case to reproduce — unlike Super System 22, System 23 needs **no
prioverchar over-pass** (`geom_draw_over`), just the text overlay. With the GPU owning the 3D nothing
writes `primap`, so the only priority in the buffer is the text tilemap's own 4 — capture priority 4 =
the whole text layer.

Plumbing (all mirroring S22): the seam grew `over_begin/over_end/over_pixels/over_forget` + a
`capture_over(bitmap, priority, prival, cliprect)` template (identical to `s22_seam.h`'s) and an
`M2VK_S23_HUD` env override / `set_option_hud` hook (the core option itself is deferred to 23-7).
`frame_begin` clears the overlay. One guarded `#ifdef S23VK` call — `s23::capture_over(bitmap,
screen.priority(), 4, cliprect)` — after the two text-mix passes in `namcos23_state::screen_update`
(covers all three in-scope sets; only `gorgon_state` overrides screen_update and it is out of scope).
`vk_present.cpp` grew the `s23_over`/`s23_sandwich` detect, the `LAYER_OVER` memcpy, and `|| s23_sandwich`
in `draw_over` — the shared OVER pass (already drawn after `draw_3d_s23`) does the composite. No new
descriptor, no over-pass. Driver diff to `namcos23.cpp`: **105 insertions / 0 deletions**, all guarded
`#ifdef S23VK` (was 94 at 23-4, +11 for the capture hook). Everything else is new-file edits.

Verified (`retrohost --vk`, 3000 frames):

| Run | Digest | Meaning |
|---|---|---|
| `timecrs2` SW gate (`M2VK_SW_3D=1`) | `beb6f072f9a745c8` | == 23-0 baseline — `sw_owns_3d` path untouched |
| `crszone` SW gate (`M2VK_SW_3D=1`)  | `b48f3a0f55a7d824` | == 23-0 baseline |
| `timecrs2` GPU | `aac10d47de0cbfa2` | was `a05060b3a153dd54` — the HUD overlay lands |
| `crszone` GPU  | `9bfb201a4847109f` | was `706d1f8da138d80d` — HUD lands |
| `timecrs2` GPU `M2VK_S23_HUD=0` | `a05060b3a153dd54` | **exactly the 23-4 GPU digest** — the overlay is cleanly additive, and the toggle collapses the whole OVER draw |

Frame-3000 A/B vs the software reference: **timecrs2 the top HUD band ("1P PLAY IS ALSO") matches with
zero pixels differing**; whole frame only 403/307200 pixels differ by >12 (the documented GPU-vs-SW
float-shading rounding on the 3D, not the overlay). crszone 993/307200. Frame-matched by eye: timecrs2's
"STARLINE NETWORK" / "CREDIT 0/4" and crszone's "…terrorist group called the URDA" / "INSERT 2 COINS"
sit over the 3D exactly as software draws them. Screenshots:
`devnotes/screenshots/2026-08-29-timecrs2-23-5-hud.png` (+ `-nohud` for the before),
`…-crszone-23-5-hud.png`.

**Next: 23-6** — sprites (`render_sprite`/`render_sprite_tile`'s row×col tiled path). No sprite/direct/
immediate primitives appeared in either game's tested attract screens (the 23-1 tap saw only `model`
polys), so the sprite path awaits gameplay/later screens.

## Status — 23-6 sprites: N/A for current scope, DEFERRED to Gorgon (2026-08-30)

**Finding (static, from the driver source — no build needed): System 23 proper has no sprites, so
23-6 is a structural no-op for every in-scope game.** The driver's own header says it
(`namcos23.cpp:953-954`: Gorgon "has sprites also, whereas System 23 is a full 3D system and doesn't
have sprites"). The code proves it three ways:

- `re->type = SPRITE` is assigned at **exactly one site** — `namcos23.cpp:4581`, inside
  `gorgon_state::render_run`. No other producer ever creates a `SPRITE` entry.
- `SPRITE` is dispatched at **exactly one site** — `gorgon_state::dispatch_render_entry`
  (`namcos23.cpp:4654`). The base `namcos23_state::dispatch_render_entry` (`:4667`) has no `SPRITE`
  case (`MODEL`/`DIRECT`/`IMMEDIATE` only).
- `render_sprite`/`render_sprite_tile` are declared **only in `gorgon_state`** (`:2043-2044`), and
  only `gorgon_state::render_sprite` is defined (`:3833`).

All three in-scope sets inherit `render_run`/`dispatch_render_entry` from `namcos23_state` and override
neither: `timecrs2` = `namcos23_state`, `timecrs2v4a` = `namcoss23_state` (`:2167`, no override),
`crszone` = `crszone_state` (`:2265` → `namcoss23_state`, no override). So none of them can ever emit
a `SPRITE` primitive — confirming the 23-1/23-5 tap observation ("no sprite/direct/immediate
primitives appeared") as a structural certainty, not an artefact of which screens were sampled.
Sprites exist only on **Gorgon / System 22.5** (`rapidrvr`, `finfurl`), whose ROMs are **not on hand**
(§Scope: out of scope until acquired).

**Decision — do not build a blind GPU sprite path.** There is no in-scope game that emits a sprite, so
a GPU sprite path could not be A/B'd against software — building it would violate the project rule that
every phase carries a software A/B checkpoint, and the plan's own "don't widen scope speculatively."
The 23-6 plan line already hedged exactly this ("including the `gorgon_state` sprite-table variant **if
Gorgon ever comes into scope**"). 23-6 is therefore **deferred**, blocked only on a Gorgon ROM.

**When Gorgon is acquired, 23-6 is a real (and separate) texture path, not a reuse of 23-3.** Sprites
do *not* read the `tmrom`/`texrom` tile atlas — `render_sprite_scanline` (`:3502`) blits straight from
a `gfxdecode` element: `rd.sprite_source` (a raw `gfx->get_data()` pointer), `sprite_line_modulo`
(`gfx->rowbytes()`), and `sprite_xflip`/`sprite_yflip` (`:3888-3898`). A GPU port needs to upload that
gfx element region as its own texture and sample it in a sprite-specific shader, plus the `gorgon_state`
sprite-table layout in `gorgon_state::render_run` (`:4565-4649`). The seam already carries the `sprite`
flag and the four viewport fields, so the recorder needs no change; the geometry pass needs a second
texture source and a sprite branch. Scope it as its own increment when the ROM lands.

**No code written this phase** — the finding is a source read; the tree is unchanged, driver diff still
**105 insertions / 0 deletions** (`git diff --shortstat mame0288 -- src/mame/namco/namcos23.cpp`).

**Next: 23-7** — widen (`timecrs2v4a`), the S23 option set (hide S22/S21-only toggles),
per-game light-gun pad layout (reuse `lightgun.md`), compat rows. 23-7 *is* buildable and verifiable
with the current ROMs; 23-6 stays parked until a Gorgon ROM is on hand.

## Status — 23-7 widen + polish DONE (2026-08-30) — hand-check pending

All four buildable-and-verifiable 23-7 items are landed and statically verified; the only open item is
the user's in-game light-gun hand-check (the project rule — no scripted button-press testing). **No
driver edit** — the diff to `namcos23.cpp` is unchanged at **105 insertions / 0 deletions**. Changes are
in the OSD input table (`input_layouts.json`/`.ipp`, regenerated) and the two padmap dev tools.

- **Light-gun pad layout — the two in-scope games now have rows.** `input_layouts.json` gained
  `timecrs2` and `crszone` rows (lightgun cabinets), authored from the driver's own `PORT_NAME`s at
  `namcos23.cpp:7425-7453` (a static read, not a press-sweep): **BUTTON1 = Gun Trigger** (pad `B`),
  **BUTTON2 = Foot Pedal** (pad `A`), **BUTTON3 = User Enter** (pad `Y`, the operator user-service-menu
  nav button); aim on the left stick; `SELECT`=Coin, `START`=Start, `L3`=Service Coin. `crszone`
  `PORT_INCLUDE`s `timecrs2`, so it carries the same three (its BUTTON5 is a driver-`UNUSED` motor-test
  line, not a control). The padmap generator/sweep learned the fifth driver: `namcos23.cpp` added to
  `padmap-gen.py`'s `DRIVER_PATHS` (else the set-existence check refuses the rows) and to
  `padmap-sweep.sh` (`system23` family, `driver_system23`, `roms/system23`), keeping the two in step per
  the generator's own comment.
- **Verified statically** (all cheap, none needs the game): `padmap-gen.py --check` → *ok: 60 rows
  naming 66 sets, .ipp matches .json*; `padmap-test.sh` → *all checks passed*; core rebuild links
  (`libretro_m2_input.cpp` recompiles the new `.ipp`). `M2VK_HOST_DESCRIPTORS=1` on `timecrs2`,
  `crszone`, **and** `timecrs2v4a` all emit the three per-game labels (Gun Trigger id0 / User Enter id1 /
  Foot Pedal id8) + Aim X/Y on the light-gun device — and each logs *"has its own control layout"*.
- **Widen — `timecrs2v4a` (`namcoss23_state`) confirmed.** Its descriptor dump resolves the `timecrs2`
  row **by parent** (v4a is a `timecrs2` clone in the driver), so the Super System 23 subclass needs no
  own row. Boots and renders through the GPU seam like `timecrs2` (23-0/23-2 already had it booting).
- **Option set — already complete at 23-2.** `apply_family_cascade(family::system23)`
  (`retro_entry.cpp:443`) hides every S22/S21-only toggle, both Smooth Shadings and the three Model 2
  render options, leaving Internal Resolution + the detector-gated steering/analog block (inert on a
  light-gun game). No S23-specific live options exist yet (the fog/no-lighting/gamma set is deferred, as
  R3 notes), so `retro_run` needs no S23 branch.
- **Savestates — work, family-neutral, verified non-vacuously.** State is 33,201,431 bytes; a
  cross-process save-at-1600 → load-at-1600 (separate runs) gives **bit-identical** post-load digests
  (`7279dc062069bc11`). The non-vacuous check (savestates.md §3 step 3): loading the frame-1600 state at
  frame **1640** makes the load run reproduce the *save's* future hash sequence, not its own pre-load
  future — proving `unserialize` genuinely replaces state (a no-op read would keep the 1640-history).
  Only the single first post-load frame differs (the family-neutral §9.3 display-cache transient); every
  frame after is identical.
- **Compat.** Render status is already recorded in the 23-2..23-5 status blocks (both sets a texel/HUD
  match to software); the combined matrix is the user-maintained spreadsheet
  (`game-compat-sweep.xlsx`), whose play-quality cells the hand-check below fills. `crszone`'s
  driver-noted "input issues" remain untriaged (out of scope here, flagged since 23-0) — likely
  JVS/MCU, orthogonal to the renderer.

**Hand-check handed to the user** (in the session, per the no-scripted-input rule): load `timecrs2`
and `crszone` with a light-gun (or a pad), confirm trigger fires, the foot-pedal reload/cover works,
aim tracks, and coin/start work; then the `verified` field on the two rows can be filled.

---

Original 23-4 plan follows.
