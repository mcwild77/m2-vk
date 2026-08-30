# S2 — System 22 GPU geometry (untextured first)

**Step S2a done 2026-08-22.** `ridgerac`, `raverace`, `acedrive` render their 3D on the GPU through
the shared Vulkan path, untextured: every quad is a flat Gouraud-shaded triangle in its base palette
colour. Screenshots in `devnotes/screenshots/2026-08-22-*-vk-s2a-*.png` (the ridgerac attract road/wall
and the f1600 waving flag are the clearest).

**Step S2b done 2026-08-22 — the texture tail.** The quads now sample the tile texture per fragment
(`s22.frag` transliterates `renderscanline_poly`'s texel fetch) instead of the flat `pens[0]`. The
ridgerac attract road, buildings, guard rail and sky match the software reference closely; the Rave
Racer title logo (flame + metal + stone) reads correctly. Screenshots
`devnotes/screenshots/2026-08-22-{ridgerac,raverace}-vk-s2b-textured.png`. See the "Texture tail (S2b)"
section below.

**Step S2d done 2026-08-22 — the shading tail.** The polygon fragment now applies the rest of
`renderscanline_poly` / `renderscanline_poly_ss22`: **poly fog** (both variants), and for Super System
22 **per-z fog** (the czram table), **poly fade**, **screen fade** and **per-pixel poly alpha**. Rave
Racer's fogged tunnel now matches software (`devnotes/screenshots/2026-08-22-raverace-{vk,sw}-s2-fog.png`);
before this step vk drew the tunnel bright and untinted. Also fixed a **pre-existing GPU-path crash**
(stack overflow after ~4000 frames — `object_data` was never reclaimed; see below). See the "Shading
tail (S2d)" section.

**SS22 letterbox fixed 2026-08-22 — the per-quad clip window (scissor).** Tokyo Wars (SS22) windows
its 3D into a letterbox: each scene quad carries a clip window (`m_cliprect`, the viewport
`vl/vr/vu/vd` intersected with the visible area) that `render_triangle_fan` clips every scanline to. In
gameplay this is `T112 B367` — a black bar top and bottom, the sky being 3D geometry clipped to the
window. The GPU pass ignored it, so the sky bled to the top edge. Now the pass groups consecutive quads
by clip window into runs and sets a per-run scissor (scaled to the attachment exactly as `vk_geom`
does), the same rectangle MAME's rasteriser already clips to — so it can only match software better.
tokyowar frame 2850 went from 70.08 % → **99.25 % exact / mean 0.079**, brightness ratio 1.000; plain
S22 is provably inert (ridgerac's digest is bit-identical with the scissor and with `M2VK_NO_SCISSOR`).
This was the "SS22 2D-compositing" open item — it turned out to be a missing scissor in the 3D pass, not
the UNDER/OVER text sandwich (which is correct). Screenshots
`devnotes/screenshots/2026-08-22-tokyowar-{vk-s2-letterbox-fixed,sw-letterbox}.png`. See the "Per-quad
scissor" section below.

**Step S2c done 2026-08-22 — 2D-over compositing.** System 22 now gets Model 2's UNDER/OVER sandwich:
the finished 2D frame is the UNDER background, the GPU 3D draws over it, and the text/HUD layer is drawn
again on top. ridgerac's "TODAY'S BEST" banner (+ subtext) and "CREDIT 0/2" now sit above the road/wall
exactly as software instead of being painted out. Screenshot
`devnotes/screenshots/2026-08-22-ridgerac-vk-s2c-2d-over.png`; matches the `M2VK_SW_3D=1` reference at
frame 1000. See the "2D-over sandwich (S2c)" section below. Still to do: sprites, SS22 fog/fade/alpha.

**Sprites on the GPU 2026-08-22 — the last unexercised seam.** Super System 22 sprites now render on the
GPU, interleaved in z with the polygons in the same tree walk, and the SS22 text-vs-3D compositing that
was coupled to it is resolved with draw order instead of the priority buffer. timecris and propcycl match
software frame-aligned (0.11–0.71 % of pixels differ, brightness ratio 1.000); plain S22 provably inert
(ridgerac digest unmoved, `62db03fd8ee89035`). See the "Sprites (GPU)" section below. Screenshots
`devnotes/screenshots/2026-08-22-{timecris-vk-sprites,propcycl-{vk,sw}-sprites}.png`.

## What it draws

Each quad crosses the seam (`s22::submit_quad`) with, added at S2:
- `basecolor` — `extra.pens[0]` as `0x00RRGGBB`, the colour `renderscanline_poly` reads with `pen == 0`
  (the untextured value). One per quad, resolved on the emulation thread.
- `bri[6]` — the per-vertex brightness `(bri+0.5)*ooz` the rasteriser interpolates.

On the frontend thread the record becomes vertex + index buffers and one indexed draw. Per vertex the
colour is `basecolor` scaled by the hardware shade, `(bri/ooz)/64` (mirrors `scale_imm_and_clamp(shade
<< 2)`), interpolated across the polygon. Untextured polygons come out exactly right; textured ones are
the right shape and shading with a flat fill — a checkered flag reads as a shaded grey quilt, a road as
flat grey.

## Texture tail (S2b)

The fetch is `renderscanline_poly`'s, moved to the GPU. What crosses the seam per quad gained `uoz`/`voz`
(`clipv.p[1]`/`p[2]` = `(u+0.5)*ooz`, `(v+0.5)*ooz`), `texturebank` and `shade_enabled`. The vertex
carries `x,y`, the three screen-linear params `uoz/voz/ooz` and the shade param `iw` (interpolated
**noperspective**, exactly as `render_triangle_fan` walks them), plus three flat per-quad words
(`attr` = flags|color|cmode, `bn`, untextured `basecolor`) replicated into every vertex.

`s22.frag` divides per fragment (`ooz = 1/z`, `tx = int(uoz*ooz)&0xfff`, …), walks
`ttmap`/`ttattr`/`ayx`/`ttdata` to a pen, applies the `cmode` pen mask/shift and the palette, then the
per-pixel shade (`scale_imm_and_clamp(shade<<2)`). An untextured quad (driver disabled textures) uses
`basecolor` directly. Fog, fade and poly-alpha are applied after this — see "Shading tail (S2d)".

The tile system (`ttmap` 2 MB, `ttattr` 1 MB, `ttdata` 16 MB, `ayx` 4 KB) is ROM-derived and **static**,
so it is uploaded ONCE into buffers shared by every slot; only the palette (128 KB) is re-uploaded each
frame, per slot. The driver hands the pointers over `s22::set_texture_ram()` from the frame bracket. On
little-endian hardware each packed `uint` buffer is byte-identical to the driver array, so every upload
is a `memcpy` and the unpack lives in the shader. Indices are masked to their buffer size (the shader
stays in bounds where the software renderer would read past a large `texturebank`).

Pipeline gained a descriptor set (5 storage buffers: the 4 static arrays + the per-slot palette). The
S2a untextured pipeline is subsumed — one pipeline now, textured and untextured switch on the flag.

## Shading tail (S2d)

The rest of `renderscanline_poly` / `renderscanline_poly_ss22`, transliterated into `s22.frag` as
integer math (`rgbaint_t::blend` is `(a*f + b*(256-f)) >> 8`; `scale_*_and_clamp` is
`clamp((c*s)>>8, 0, 255)` — reproduced exactly). The two variants differ in **order**: plain System 22
fogs **before** it shades; Super System 22 shades first, then fogs, then poly-fade, then screen-fade,
then a per-pixel alpha blend.

**Per-quad vs per-frame.** Working through the driver's `poly3d_drawquad`, only fog is genuinely
per-quad: `fogfactor`, `fogcolor` (per-cztype on plain S22, the global fog colour on SS22),
`zfog_enabled` + cz bank + `cz_sdelta`, and `alpha_enabled` (`(color&0x7f) != m_poly_alpha_color`).
Everything else the SS22 tail reads — screen fade (`m_screen_fade_*`), poly fade (`m_poly_fade_*`),
the alpha factor (`m_poly_alpha_factor`) and the alpha pen (`m_poly_alpha_pen`, which the scanline
loop reads straight off `m_state`) — is the same for every quad in the frame. So the per-quad part
rides two new flat vertex words (`sf0`/`sf1`), the per-frame globals ride the push constant
(`set_shading_state()` from the frame bracket → `shading_globals`), and the four `recalc_czram` z-fog
tables ride a sixth per-slot storage buffer (32 KB, re-uploaded each frame like the palette; a plain
System 22 game has no czram, so a null bank is zero-filled and never read).

**Final gamma (the big one — S2d fix 2026-08-22).** Both mixers run every output pixel through a gamma
LUT as the very last step, and the GPU 3D was skipping it, so it read **~half as bright** (ratio
0.4–0.5 in shadows, ~0.8 in highlights — a gamma curve, not a scale). This is separate from the palette
(the pens are *not* pre-gamma'd) and is applied after all shading/fog/fade. The two families source it
differently:
- **Plain System 22** (`namcos22_mix_text_layer`): a static ROM PROM, `m_gamma_proms`
  (rlut|glut|blut, 0x100 each), indexed directly.
- **Super System 22** (`screen_update_namcos22s`'s post-pass): mixer RAM at byte 0x100
  (`m_mixer[0x100/4]`), which **changes per frame** and is stored as u32 words, so the byte index is
  swapped — `rlut[NATIVE_ENDIAN_VALUE_LE_BE(3,0) ^ c]`, i.e. `^3` on little-endian.

The driver hands the active LUT over each frame in the frame bracket (`set_texture_ram`'s new `gamma`
pointer — `m_gamma_proms` for plain, `memshare("video_mixer")+0x100` for SS22; both members are
protected, so they are fetched through the public `memregion`/`memshare`). It rides a per-slot 768-byte
storage buffer (re-uploaded each frame like the palette). `s22.frag` applies it as the final op on both
paths, with the `^3` swap gated on the `ss22` flag. For an opaque pixel this is exact; an alpha pixel's
fixed-function blend then happens in gamma space instead of before gamma — a small residual, accepted
for the SS22 tail. This fix is why plain-S22 output went from ~mean-41/255 off to **bit-exact**.

**Spot is not here.** `m_spot_factor` is a compositing-stage effect (the mixer, `namcos22_v.cpp`
~2070), not part of `renderscanline_poly_ss22` — it never touches the polygon shading, so it is out of
scope for this shader.

**Alpha is the one fixed-function step.** The final `rgb.blend(dest, ...)` blends against the
framebuffer, which a fragment cannot read; instead the fragment emits alpha `(0xff-poly_alpha)/255`
for a pixel that alpha-blends (`alpha_enabled || pen == alpha_pen`) and `1.0` for an opaque one, and
the pipeline's `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` blend does the mix. Opaque pixels (every plain-S22
pixel, every non-alpha SS22 pixel) emit `1.0` and pass through bit-exact. An actual alpha pixel carries
a small residual (software is integer `>>8`, GPU blend is float UNORM `/255`) — expected for the SS22
tail, whose accuracy ground truth is looser than Model 2's (§A.4 of the plan).

**Verification.** `ridgerac` turned out to be a poor fog test — its attract loop never programs poly
fog (the S1 tap, extended here to count fog quads, reports `max fog 0` across 4400 frames). A full sweep
of the 18 sets (`devnotes/tools/…` one-offs; the shortcut `System 22 Fog A-B.command` wraps it) found
who fogs in attract:
- **direct poly fog:** `raverace` and `acedrive` (max 255 / 196, ~2000+ of ~2800 scenes), `cybrcomm`.
- **SS22 z-fog** (the czram table): `tokyowar`, `propcycl`, `airco22b`, `dirtdash`, `adillor` all
  render it heavily in attract (2000–2900 scenes) — the earlier "SS22 barely renders in attract" was a
  too-short frame count, not the truth.
- **no fog in attract:** `ridgerac`, `ridgera2`, `timecris`, `victlap`.

Once the gamma fix landed, exact-pixel diff *is* usable and the match is far tighter than first
thought: with both runs frame-aligned from a pristine `M2_SAVE_DIR` (bit-deterministic vk-vs-vk), Rave
Racer's fogged city highway is **98.5 % exact / mean 0.14** over the 3D, and every region's vk/sw
brightness ratio is exactly 1.000. (An earlier note here claimed the ~mean-41/255 gap was
"GPU-vs-scanline coverage disagreement"; that was wrong — it was almost entirely the missing gamma.
The genuine coverage residual is tiny.) Tokyo Wars (SS22, mixer gamma) went from mean 58 to **mean 23 /
88 % exact** with the gamma fix, ratio 1.000 everywhere; its remaining residual is edge coverage on a
busy scene plus the not-yet-drawn sprites and the alpha-in-gamma-space approximation — the looser SS22
target. **Direct fog, z-fog and gamma are verified.** The remaining SS22 pieces — **screen fade** and
**poly-alpha** — are wired and stable but appear only lightly in attract, so their accuracy is the
in-game hand-check the user still owns; `System 22 Fog A-B.command` is the compare tool.

The Tokyo Wars "blue cast" first read as a 2D-compositing bug turned out to be this same missing gamma
(the SS22 mixer LUT is per-channel); with gamma applied the 3D colour matches. What *remains* a genuine
2D-compositing issue on SS22 is the missing top letterbox bar / wrong sky layer — see §A.3, still open.

**Pre-existing GPU-path crash, fixed here.** `poly3d_drawquad` allocates one `object_data` per quad
but, when the GPU owns the 3D, never enqueues a render unit (`render_triangle_fan` is skipped below the
seam). `poly_manager::wait()` early-outs with no units outstanding and so never resets its poly arrays,
so the `object_data` arena grows every frame; after ~4000 frames `poly_array::next()` chains deeply
enough to overflow the stack (`ridgerac` SIGBUS'd at frame ~4200). This has been latent since S2a —
every earlier test ran ≤1800 frames. Fixed with one guarded line in the frame bracket:
`if (!s22::sw_owns_3d()) object_data().reset();`. The software path is untouched.

## Per-quad scissor (SS22 letterbox)

`poly3d_drawquad` clips every quad to `m_cliprect` — the scene viewport (`vx/vy` centre, `vl/vr/vu/vd`
half-extents) intersected with the visible area. For the games shipped so far it is full-screen almost
always, but SS22 titles window the 3D: Tokyo Wars runs at `L0 R639 T112 B367` in gameplay, a 256-px-tall
viewport with black letterbox bars, and the sky is a `direct=0` quad clipped to it (measured with a
temporary dump at the seam, since removed). The rect crosses the seam per quad (`quad.clip_*`, added to
`s22_seam.h`), and `geom_upload` groups consecutive quads that share it into runs (`draw_batch`), the
same structure `vk_geom` uses. `geom_draw` sets one scissor per run, scaling the inclusive bitmap-pixel
rect to the attachment (`floor` the top-left, `ceil` the bottom-right — rounds OUTWARD, so a fractional
internal-res scale never shaves a boundary column), then restores the full attachment scissor before
returning so the OVER overlay is not clipped. The scissor rect is *identical* to the one MAME's
`render_triangle_fan` clips against, so it can only bring the GPU closer to software. `M2VK_NO_SCISSOR=1`
collapses every window to full-screen (the pre-fix behaviour) — the attribution switch `vk_geom` also
exposes; on ridgerac it changes nothing, on tokyowar it is the whole letterbox.

## Sprites (GPU)

Super System 22 sprites (`render_sprite` → `poly3d_drawsprite`, the sibling of the poly seam; sprites are
SS22-only — `draw_sprites` runs only in `screen_update_namcos22s`, and gfx(2)/the `"sprite"` region exist
only in `gfx_super`). Each sprite tile is a screen-aligned affine textured quad: `renderscanline_sprite`'s
fetch, transliterated into `s22.frag` under an `ATTR_SPRITE` flag.

**One stream, tree order.** The killer constraint is z: quads and sprites interleave in one back-to-front
tree, so they can't be drawn as two separate batches. They go into the SAME record in walk order
(`s_order` — a `(kind,index)` list over `s_quads`/`s_sprites`), and the painter's pass draws them in that
sequence. Sprites reuse the polygon pipeline and vertex layout: affine u/v ride the perspective slots with
`ooz = 1` (noperspective interpolation is exact for a screen-aligned quad), the pens base is `(color&0x7f)
<<8` — the same word the poly path's `pens_base` uses — the tile byte offset rides `bn`, the fade colour
rides `base`, and the fog/fade/alpha tail rides `sf0`/`sf1` in a sprite-only layout. So no second pipeline
and the existing scissor/`draw_batch` machinery already preserves order. One new static storage buffer
(binding 7) holds the `"sprite"` ROM region (8bpp 32×32 tiles, `char_modulo` 1024, `line_modulo` 32),
uploaded once like `ttdata`; plain S22 binds a 16-byte placeholder and never samples it. The per-tile
seam (`s22::submit_drawsprite`) carries the four screen verts, the tile offset, palette bank, one-texel
flip shift (the mirroring itself is already in the vertex positions), fog/fade/alpha and the clip window;
the software `render_polygon` is guarded off with `sw_owns_3d()`.

**Text-vs-3D, by draw order not priority.** MAME's mixer decides text over/under each 3D primitive from
the priority buffer: a normal primitive loses to text (the tilemap draws text at priority 4, a normal
primitive over it makes 6, and `namcos22s_mix_text_layer(6)` re-draws the text on top), but a
**prioverchar** primitive (poly `cmode&7==1`, sprite `cz==0xfe`) covers the text (priority 7, not
re-mixed). With the GPU owning the 3D nothing writes priority, so that decision is gone. It is rebuilt
with draw order, from the prioverchar flag the seam already carries:

- `capture_over` now snapshots ALL SS22 text (priority 4 — the only priority present once the GPU owns the
  3D) as the OVER overlay, drawn above the GPU 3D. That puts text over every *normal* primitive.
- a second GPU pass, `geom_draw_over` (wired after the OVER overlay in `vk_present`'s `record_and_submit`),
  redraws just the prioverchar-flagged primitives — recorded as an index-range list into the same buffer
  during `geom_upload` — so they sit *above* the text, reproducing MAME's priority-7 case. A prioverchar
  primitive is thus drawn twice: once in the main pass (for z vs other 3D) and once over the text.

Net layering matches native SS22: normal-3D < text < prioverchar-3D, sprites/polys z-interleaved. The
accepted residuals are the existing family — alpha/spot text blends against the UNDER background rather
than the live 3D, and an alpha prioverchar primitive double-blends slightly.

**Verified** frame-aligned vk vs `M2VK_SW_3D=1` from pristine `M2_SAVE_DIR`s, brightness **ratio 1.000**
in every region: timecris 0.11–0.29 % of pixels differ (mad 0.014–0.070), propcycl 0.33–0.71 % (mad
0.075–0.107) — edge coverage + the alpha-in-gamma-space approximation. With the `M2VK_NO_3D` fix (below),
the bg-based tools also work: `ppmdiff.py coverage`/`exact` on timecris frame 2200 reports **coverage
agreement 1.0000** (GPU covers the exact same pixels as software, sprites included), 99.79 % same colour
in the covered region, and exit-criterion-1 (the 2D-only pixels bit-identical) holds.

**`M2VK_NO_3D` fixed for System 22 (2026-08-22).** It had never suppressed the S22 software 3D:
`set_gpu(false)` couples "GPU off" to `sw_owns_3d() == true`, so the rasteriser kept drawing and the
"background" contained the whole 3D scene (coverage read ~0.3 %). A new `s22::set_no_3d()` hands
`sw_owns_3d()` back **false** as well, so neither path draws and the picture is just the 2D layers — the
real background reference the coverage/exact harness needs. `retro_entry` calls it when `M2VK_NO_3D` is
set. (Model 2's `M2VK_NO_3D` path — `m2vk::set_rasterize` + `vk_geom`'s `s_no_3d` — is untouched.)

## Depth = painter's, no depth buffer

The System 22 tree is walked **back-to-front** (opposite of Model 2's front-to-back stream), so ordering
is a plain painter's algorithm: draw in record order, last writer wins. The pipeline runs with the depth
test **off** and never touches the ring's depth attachment (Model 2's). Confirmed correct by eye — no
depth inversion in any of the three games.

## 2D-over sandwich (S2c)

System 22 now uses the same three-draws-in-one-pass sandwich as Model 2 (`record_and_submit`): UNDER
2D → GPU 3D → OVER 2D. The UNDER layer is the passthrough `pixels` — the finished 2D frame the seam has
stripped of software 3D (`sw_owns_3d()` false), i.e. background + text baked in. The GPU 3D draws over
it (covering the baked text where polygons land). The OVER layer redraws just the text/HUD on top, so it
always wins.

The OVER layer is captured by `s22::capture_over(bitmap, screen.priority(), prival, cliprect)`, a new
guarded hook at the end of each `screen_update`. It walks the priority buffer: a pixel whose priority ==
`prival` (2 for plain System 22 text, 6 for the topmost SS22 text) becomes `0xff000000 | bitmap[x]` —
the already-mixed, gamma-corrected text pen — and every other pixel is 0 (transparent). The overlay
fragment shader (`overlay.frag`) discards an all-zero texel, which is why the high byte is forced to
0xff (a pure-black text pixel must not read as transparent). The buffer lives in `s22_seam.cpp`
(file-scope, links inert in the tap/Model 2 builds); `present_frame` reads it via `s22::over_pixels()`,
copies it into the existing `slot.layers[LAYER_OVER]` staging (allocated in every build for Model 2, so
no new Vulkan resource), and passes `draw_over=true`.

Faithful for opaque HUD text (ridgerac). On **plain S22** one known limit remains: alpha/shadow text
(shadow pens 0xfc–0xfe; ridgerac uses none) blends against the UNDER background in the software mix
rather than against the GPU 3D. On **SS22** the "text behind a prioverchar primitive" case is now
handled — see the "Sprites (GPU)" section: `capture_over` grabs all text (prival 4) and `geom_draw_over`
replays the prioverchar primitives over it, so text-vs-3D ordering matches software without the priority
buffer. (The earlier note here — that behind-poly text still showed on top and sprite-in-tree z was
unaddressed — is superseded by that work.)

## Architecture / where it lives

- `src/osd/libretro_m2/s22_seam.{h,cpp}` — the seam (S1) plus S2 controls: `set_gpu(bool)`,
  `sw_owns_3d()`, and forwarding to the record consumer. **Moved from the driver project into the
  shared OSD** so `retro_entry.cpp` and `vk_present.cpp` (both in every build) resolve the `s22::`
  symbols in the Model 2 build too, where the code is inert.
- `src/osd/libretro_m2/renderer_vk/s22_geom.{h,cpp}` — the record + GPU pipeline. A fraction of
  `vk_geom`: one pipeline, descriptors for the tile system (8 storage buffers, incl. the sprite gfx at
  binding 7), no depth; a per-quad clip-window scissor (grouped into `draw_batch` runs) but no depth
  buffer. Quads and sprite tiles share the pipeline and vertex layout and interleave in one draw order
  (`s_order`); `geom_draw_over` replays the prioverchar subset after the OVER overlay. Pipeline and
  per-slot buffers build lazily on the first captured frame, so the Model 2 build pays nothing.
- Shaders `renderer_vk/shaders/s22.{vert,frag}` (+ committed `s22_{vert,frag}_spv.h`).
- Wiring in `vk_present.cpp`: `geom_build`/`destroy`/`forget`/`end_run` beside the Model 2 ones,
  `geom_upload` in `present_frame`, `geom_draw` after the background draw in `record_and_submit`, and
  `geom_draw_over` (the prioverchar over-pass) right after the OVER text overlay is drawn.
- Trigger in `retro_entry.cpp`: `s22::set_gpu(s_hw_render && !M2VK_NO_3D && !M2VK_SW_3D)`.

## Build + run

`make SUBTARGET=namcos22 OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10` (REGENIE for the lua move).
Model 2 build unchanged and still links/presents (verified: vf2 vk 1200 frames, no S22 capture line).

```sh
M2OPT_model2_renderer=vulkan ./devnotes/retrohost --vk ./namcos22_libretro.dylib \
  devnotes/roms/system22/ridgerac.zip 1000 /tmp/f.ppm      # logs "s22: first GPU geometry"
```

`M2VK_SW_3D=1` puts the software rasteriser back in charge (no GPU 3D) — the S2 isolation switch.

## Upstream diff

`namcos22_v.cpp` **+121/−0** (the sprite work added the per-tile `submit_drawsprite` hook + `sw_owns_3d`
guard in `poly3d_drawsprite`, the `prioverchar` arg on the `submit_quad` call, the `set_sprite_ram` call
in the frame bracket, and changed the SS22 `capture_over` prival 6→4; on top of S2d's per-quad shading
builder / `set_shading_state` / gamma source / crash fix and the letterbox `m_cliprect` args). Total vs
`mame0288`: **257 insertions / 2 deletions across 7 files** (measured
`git diff --shortstat mame0288 -- src/devices src/mame`; the other files are the scsp pair, the two
`.flt`s and the Model 2 pair). Everything else is new files.

## Internal resolution / supersampling (verified 2026-08-22)

`model2_internal_res`, `M2VK_RES=<w>x<h>` and `M2VK_SS=<n>` all reach and scale the System 22 3D pass
with **no S22-specific code** — the option is in the shared `DEFINITIONS[]` (no driver gating), and
`s22.vert` builds NDC from the visible half-extent push constant, so `build_ring`/`s_ss`/`draw_width`/
`draw_height` scale the S22 draw exactly as they do Model 2's. Verified on `ridgerac`, 1000 frames,
`retrohost --vk`:

- 3D sharpens cleanly at every size: `M2VK_RES=640x480 / 1280x960 / 2560x1920` (present at that size)
  and `M2VK_SS=2 / 3` (draw big, box-resolve back to native 640x480). Road edges, wall seams and
  guardrail chevrons go from stair-stepped to crisp. Screenshots
  `2026-08-22-ridgerac-vk-s22-res-2560.png` (direct upscale) and `-ss3.png` (supersampled).
- **UNDER/OVER 2D overlay lines up 1:1 and matches software.** The apparent "double" on the
  "TODAY'S BEST" banner at high res is the game's own HUD **drop shadow** — baked into the UNDER
  passthrough at a non-text priority, correctly *not* re-captured by `capture_over` (prival 2) — not a
  compositing offset. `M2VK_SW_3D=1` at 640x480 shows the identical sharp-text-over-offset-shadow, so
  the GPU sandwich reproduces it faithfully. The SS box resolve does **not** smear the overlay (ss3
  overlay is clean).

**Native default is now driver-correct (fixed 2026-08-22).** The shared Internal Resolution list keeps
one "(Native)" entry, but which one — and the option's default — now follows the driver family:
**496x384 for Model 2, 640x480 for System 22** (S22's real hardware picture). The object files are
shared across subtargets, so this is decided at *runtime*, not compile time: `retro_entry`'s
`retro_set_environment` probes `driver_list::find("ridgerac")` and calls
`m2opt::set_native_resolution("640x480")` for the S22 family before `declare()`, which retargets the
default and moves the "(Native)" label. Model 2 is untouched (`driver_list::find` misses in that
build). Verified: ridgerac defaults to `640x480` and presents at native with no downscale; vf2 still
defaults to `496x384`. Explicit option/`M2VK_RES` values still override on both. A user can still pick
`496x384` on S22 (it downscales, `0.775x by 0.800x`) or any larger size / `M2VK_SS`.

Per-resolution digests (ridgerac, 1000 frames — differ by pixel count, as expected; recorded as the
baseline, not a cross-resolution invariant; captured before the default flip, so "default 496x384" is
the old default — the new default matches the `M2VK_RES=640x480` row):

| Config | digest |
|---|---|
| old default 496x384 | `2d964f032429a371` |
| **new default = 640x480** | `4353105bd1984b08` |
| M2VK_RES=1280x960 | `a8ce5074857c7eaf` |
| M2VK_SS=2 | `c8628f1543f31738` |
| M2VK_SS=3 | `b582dd2c88dd0c2d` |

## Open (next S2 steps)

- ~~SS22 shading path (`renderscanline_poly_ss22` tail): fog, fade, poly-alpha.~~ **Done S2d
  2026-08-22** (spot is a mixer-stage effect, not the poly tail — out of scope). Direct poly fog
  (verified on `raverace`/`acedrive`) and SS22 z-fog (verified on `tokyowar`) match software. Screen
  fade and poly-alpha are wired and stable but show only lightly in attract, so they remain an in-game
  hand-check (`System 22 Fog A-B.command`).
- ~~SS22 2D compositing (Tokyo Wars letterbox / sky bleed).~~ **Done 2026-08-22** — it was a missing
  per-quad scissor, not the UNDER/OVER text sandwich; see "Per-quad scissor" above.
- ~~Sprites (`render_sprite`) and the SS22 sprite/text z (§A.3).~~ **Done 2026-08-22** — see the
  "Sprites (GPU)" section above. Sprites join the polygon stream in tree order; SS22 text-vs-3D is
  rebuilt from draw order (capture all text as the OVER overlay, then a `geom_draw_over` pass redraws
  prioverchar primitives over it) instead of the now-empty priority buffer. timecris/propcycl match
  software frame-aligned (0.11–0.71 %, ratio 1.000); plain S22 inert. The SS22 `capture_over` prival is
  now 4 (all text), not 6.
