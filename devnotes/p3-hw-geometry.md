# P3 — HW geometry, the meat

**Status: planned 2026-07-26; DONE the same day — all 8 steps, both exit criteria met.** Commits:
`d05bdda8f31`, `2f988821871`, `dee26204c72`, `f3aa8614856`, `b8fd2951634`, `3c8632ce4d3`,
`c38dbbefffe`. Steps 7 and 8's docs work changed no core code; step 8 also carries the stale-3D fix
that step 7 found.
**The GPU draws, as of step 3, and bit-exactness against the software renderer is over for the 3D
layer. As of step 4 the picture is the game; as of step 5 the whole raster tail is on the GPU.**
Step 1 is the compositing, step 2 the recording — both bit-exact against P2, which is what made them
checkable at all — step 3 is the untextured polygon pass with a real depth buffer, step 4 is texture
RAM and `draw_scanline_tex`, step 5 is its translucent specialisation, step 6 is scissor/windows/the
dupe path (which changed no pixel in 25 games, and establishing *why* was the work), step 7 is the
A/B harness, and step 8 is the empty-display-list fix plus these docs.

**Exit criterion 1 passes on all 12 fixtures; exit criterion 2 passes with room on the three it
names — vf2 0.9963, vcop2 0.9999, srallyc 0.9896 — and the residual is attributed** (float rounding
in `z = 1/ooz` and `int(uoz*z*256)` on Metal rather than x86-64, amplified by a LUT with a step per
index; isolated pixels, no spatial trend, symmetric signed difference, and flat-shaded A/B is
pixel-identical). The numbers live in [ab-baselines.md](ab-baselines.md) and are regenerated, never
retyped.
What replaced `cmp` is `devnotes/ppmdiff.py`: a coverage diff against an `M2VK_NO_3D=1` reference
that both renderers still produce bit-identically, plus the masked exact test that is exit criterion
1. See "Order of work" for what each step actually built.

Plan of record for P3 as scoped in `../Polydiver/PDDocs/model2/model2_libretro_core.md` §3. Read
[vulkan-target.md](vulkan-target.md) first — it is the measured device, and three of its facts
(Vulkan **1.1**, no `D24_UNORM_S8_UINT`, `depthBiasClamp` present) decide things below. Then
[seam.md](seam.md), which is what arrives at the tap and in what order; P3 changes nothing about the
seam's *shape*, only what happens to the stream after it.

[p2-vulkan-passthrough.md](p2-vulkan-passthrough.md) is the phase this builds on. P2's summary in one
line: we own a `VkDevice`, a queue, a ring of 3 images and a lifecycle, and we currently blit MAME's
finished software frame into them. **P3 is where the GPU starts drawing polygons instead.**

## What P3 delivers

MAME's software rasterizer stops drawing the 3D layer. The tapped polygon stream becomes real Vulkan
vertex buffers, drawn with a real depth buffer into the visible 496×384 picture (step 3 established
that the 512×512 target the plan assumed is not needed), textured from the two
1 MB texture-RAM sheets decoded in the fragment shader, and shaded by transliterating
`src/mame/sega/model2rd.ipp` into GLSL. The 2D tilemap layers stay MAME's and are composited around
the 3D layer on the GPU.

Rendered **native 1×, nearest, no filtering, no MSAA, no widescreen** — deliberately, so the output
is still directly comparable to the software renderer. Enhancements are P5 and they are the reason
P4 (decals, z-fight, sort) comes first.

**Explicit non-goals:** internal-res scaling, MSAA, bi/trilinear on the *output*, widescreen,
performance work, context negotiation, savestates, and the decal/z-fight problem — that last one is
P4 and this plan is written to avoid pre-empting it (see problem 2).

## Exit criterion

Two parts, and the first is exact:

1. **Every pixel the 3D layer does not touch stays bit-exact against the software renderer.** The
   tilemaps, the palette-0 background, the render-test framebuffer path and the crop are a `memcpy`
   in both renderers, so anything else is a bug in the compositing and is findable by `cmp`. This is
   checkable per frame: build the software frame and the Vulkan frame, mask off the pixels the poly
   stream covered, and the remainder must be identical.
2. **The 3D layer is measurably close, with the residual attributed.** SSIM ≥ 0.95 per fixture frame
   on vf2 / vcop2 / srallyc attract, and a written list of what the remaining difference *is* —
   rasterization fill rules, filtering rounding, and (expected) coplanar ordering, which is P4's
   whole job. A number without an explanation is not an exit; "0.97 and we don't know why" fails.

The P2 exit criterion — bit-exact — **is not available any more and will never be again.** A GPU
rasterizer and MAME's scanline loop do not agree on which pixels a triangle covers, and no amount of
care makes them. That is the phase transition P3 represents, and the A/B harness has to change shape
with it (problem 7).

## The seven problems

### 1. The frame is not just 3D — it is a sandwich, and the middle slice is the only part we own

`model2_state::screen_update` ([model2_v.cpp:2420](../src/mame/sega/model2_v.cpp#L2420)) builds one
frame in three passes:

1. fill with palette pen 0, draw System-24 tilemap layers 3→0 (the low half of each layer's
   priority pair) into `m_sys24_bitmap`, `copybitmap_trans` into the screen bitmap. **Opaque
   background.**
2. `render_polygons()` — or `draw_framebuffer()` in render-test mode — `copybitmap_trans`es
   `m_destmap` on top with `0x00000000` as the transparent key. **The 3D layer.**
3. `m_sys24_bitmap.fill(0)`, draw layers 3→0 again (the high half of the priority pair),
   `copybitmap_trans` with pen 0 transparent. **The foreground overlay.**

So the 3D is *between* two 2D layers, and neither of them is ours. There is no way to composite the
GPU's 3D on top of MAME's finished frame — the foreground has to go over it.

**The decision: capture the two 2D layers separately and composite all three on the GPU.** Two new
`#ifdef M2VK` hooks in `screen_update`:

- one immediately after the render-polygons/draw-framebuffer branch, handing over the screen
  `bitmap` — which in Vulkan mode contains background *and nothing else*, because the software
  rasterizer was skipped (below). In render-test mode it also contains `draw_framebuffer`'s output,
  which is exactly right: that path has no polygons and should pass straight through.

  **This placement is what makes step 1 verifiable, and it was the one real insight of the step.**
  While `m2vk::rasterize()` is still true the software 3D is *already inside* the captured under
  layer, so the GPU composite is under + over and must equal MAME's own frame bit for bit — the layer
  plumbing gets a `cmp`-able test before a polygon renderer exists. The same hook then serves the
  hardware path unchanged. Capturing before the branch would have needed a third capture of
  `m_destmap` purely to have something to test against.
- one immediately before the final `copybitmap_trans`, handing over `m_sys24_bitmap` — the
  foreground layer with pen 0 as its transparency, before it is flattened.

Plus one two-line addition inside the existing `model2_3d_render` hook block: after `m2vk::submit`,
`if (!m2vk::rasterize()) return;` — skipping the scanline dispatch when the Vulkan path owns the 3D.
That is also the phase's only performance win, and it is a large one: the software rasterizer is
where nearly all of MAME's Model 2 CPU time goes.

**Upstream diff after P3: 16 lines → about 26.** That is the budget. It should still be about 26 at
the end of P6. *(As built through step 3 it is **28**: the two extra lines recycle `poly_manager`'s
object-data arena, which is not optional once the scanline dispatch is skipped — see the gotcha.)*

Rejected: reading the GPU's 3D layer back into `m_destmap` so MAME composites as usual. It is
tempting — the entire P2 present path and the bit-exact harness would keep working unchanged — but
it cannot be done. The seam runs on the **emulation thread**, and
[vulkan-target.md](vulkan-target.md)'s standing rule is that Vulkan is only ever touched on the
frontend's thread. A readback would have to happen inside `screen_update`, on the wrong thread, and
one frame too early. It also dies at P5 anyway: a 4×-internal-res frame does not fit in a 512×512
`bitmap_rgb32`. Do the compositing on the GPU now, once.

### 2. What goes in the depth buffer — and it is not z

Model 2 has no depth buffer. The software renderer draws **front-to-back with an occlusion mask**
(`m_fillmap`): a pixel is written only if nothing has written it yet. Draw order is
window-descending, then sort-bucket-ascending, then bucket-list order (which is *reverse* submission
order, because the lists are built by prepend — [seam.md](seam.md)). **First writer wins the pixel,
and that is the entire hidden-surface algorithm.**

That is reproducible exactly with a depth buffer, and it does not need real z:

> Give polygon *n* in draw order the constant depth value `1 - n/65536`. Depth test `GREATER`, depth
> write on. The first polygon to reach a pixel has the largest depth value, wins the test, and every
> later polygon fails it. This *is* `m_fillmap`, implemented in hardware.

**P3 uses draw-order depth, flat per polygon.** Reasons, in order of weight:

- It reproduces the hardware's actual behaviour, which is a priority sort, not a depth sort.
- It makes P3's output comparable to the software renderer, which is the whole point of rendering at
  native 1×.
- It **cannot z-fight**. 86 % of polygons in a sampled VF2 frame share a sort bucket with an adjacent
  polygon ([seam.md](seam.md)), so interpolated-z would produce coplanar fighting on most of the
  frame, and diagnosing a new renderer through a fog of z-fighting is a bad trade. Interpolated depth
  and the decal problem are **P4**, together, on purpose.

Two consequences to keep in mind while building it:

- 1450 polys/frame worst case observed, so a 16-bit draw-order key is ample and `D32_SFLOAT` holds it
  with enormous margin. Use `D32_SFLOAT` — [vulkan-target.md](vulkan-target.md): the 24-bit combined
  format does not exist on Apple GPUs.
- Nothing blends. Model 2's "translucency" is a **cutout** (alpha < 50 % is discarded) and `checker`
  is a **stipple**; both are per-pixel discards, and every surviving fragment is opaque. There is no
  sorted transparent pass in P3 and there does not need to be one. (`model2_libretro_core.md` §4
  anticipates a back-to-front translucent pass; at this seam it is not needed, because the stream is
  already priority-ordered and the fragments are opaque.)

The vertex depth still has to be *carried* — `poly.v[i].rz` is `1/z` for textured polys and raw `z`
for untextured ones ([seam.md](seam.md)) — because the fragment shader needs `z` to compute mip level
and to undo the perspective divide on u,v. It just does not go in `gl_Position.z`.

### 3. Texture RAM is 2 MB in total. Do not build an atlas

The reflex here is a texture cache: 51 distinct pages in one VF2 frame, page decode on demand,
eviction, atlas packing. All of that is unnecessary.

Each sheet is a single memory share of **1 MB** (`textureram0`, `textureram1` —
[model2.cpp:1420](../src/mame/sega/model2.cpp#L1420)), which at 4 bpp is 1024×2048 texels. Both
sheets together are **2 MB**. Upload both as one storage buffer of raw `uint32` words, exactly as
they sit in RAM, and transliterate `model2_renderer::get_texel` into GLSL. No decode, no format
question, no atlas, no cache, no eviction. 2 MB per changed frame on unified memory is not a cost
worth engineering around at this stage.

The mip chain needs no generation either, and this is the part that is easy to get wrong:
`fetch_bilinear_texel` computes level *n* by **halving `texwidth`/`texheight` and shifting
`texx`/`texy`** — `tex_x = ((texx - 2048) >> level) & 2047` — and reading `texsheet[level & 1]`. The
mip levels are **already resident in texture RAM at other addresses, alternating between the two
sheets**, put there by the game. Level 0 and level 2 come from this polygon's sheet; level 1 and
level 3 come from the other one. So both sheets must be addressable from every draw, and the sheet
index is `poly.sheet ^ (level & 1)`.

Microtexture is level `-1`: a 128×128 window at `utexx`,`utexy` in **`texsheet[1]`** — the *other*
sheet — with `u`,`v` shifted left by `1 << utexminlod`.

### 4. Filtering has to be done by hand

The obvious move — a `VkSampler` with `LINEAR` and a mip chain — is wrong for three independent
reasons, and each one alone is fatal:

- **The filtering happens in index space, before the LUT.** MAME bilinearly interpolates the 4-bit
  texel values (shifted to 8-bit), and *then* the result indexes `lumaram` → `colorxlat` → gamma.
  Interpolating after the LUT is a different picture.
- **The translucent path's filter is not a filter.** `fetch_bilinear_texel<true>` packs an alpha bit
  into the high half of each texel, and a transparent texel **takes the luma of its neighbour** so
  that the interpolation does not drag colour out of the transparent region. There is no sampler
  state that does this.
- **The wrap/clamp behaviour is a fudge, not an addressing mode.** When smooth wrap is off and the
  filter straddles the edge, MAME rewrites `u0`,`u1` and *forces* `ufrac` to 0 or 0x100 depending on
  which side of the texel centre it fell — a hard snap, not `CLAMP_TO_EDGE`. Mirroring is
  `if (mirror && (u & (width << 8))) u = ~u`, on the 8.8 fixed-point coordinate.

So: sample the storage buffer with the transliterated `get_texel`, and port `fetch_bilinear_texel`,
the `LERP` macro (which operates on two 8-bit lanes packed into `0x00ff00ff`, luma in bits 0–7 and
the alpha flag at bit 23), `fast_log2` and the trilinear/microtexture blend from
`draw_scanline_tex`, one for one, into GLSL. GLSL has the integer support to do it literally; do it
literally. This is the single largest piece of code in P3 and it is transliteration, not design.

Getting u,v right per fragment is the one place to use a Vulkan feature rather than a
transliteration. MAME interpolates `ooz`, `uoz`, `voz` **linearly in screen space** and then computes
`z = 1/ooz`, `u = uoz * z * 256`. That is exactly what `noperspective` varyings give: declare `rz`,
`uz`, `vz` as `noperspective` (screen-linear), leave `gl_Position.w` at 1, and the fragment shader's
first two lines are MAME's inner loop verbatim. Do **not** reach for perspective-correct
interpolation with a real `w` — it computes the same thing by a different route and will not round
identically.

### 5. The shading tables have to cross the seam, and they are small

`m2vk::poly` carries texture RAM pointers but none of the colour chain. Four more things are needed,
all of them `model2_state` members:

| | size | changes | how it crosses |
|---|---|---|---|
| `m_palram` | 0x2000 × `u16` | per frame | **resolve at submit**: only `palram[colorbase + 0x1000]` is read, one `u16` per polygon. Put the resolved 15-bit colour in `m2vk::poly`. |
| `m_colorxlat` | 0x6000 × `u16` (three 32×256 ramps at u16 offsets 0, 0x2000, 0x4000) | per frame | storage buffer, re-uploaded when the frame's copy differs |
| `m_lumaram` | 0x8000 × `u8` | per frame | storage buffer, same |
| `m_gamma_table` | 256 × `u8` | **never** — computed once from a formula in `video_start` and not written anywhere else | fold into the `colorxlat` upload: bake `gamma[colorxlat[i] & 0xff]` once and the shader does one lookup instead of two |

Resolving `palram` on the CPU at submit time is exactly equivalent to what the software renderer
does, and it is worth being sure of the reasoning: the scanline callbacks read `m_palram` during
`render_polygons`, which runs on the emulation thread inside `screen_update`, so no CPU write can
land between the submit and the raster. Same argument covers `colorxlat` and `lumaram`, which is why
snapshotting them once per frame is safe.

These arrive through the existing `frame_begin` hook, which grows a `m2vk::frame_tables const&`
parameter. The hook count does not change.

### 6. The seam is on the emulation thread; Vulkan is on the frontend's

This is the standing rule from P2 and P3 is where it costs something: the polygon stream arrives on
the emulation thread, and not one Vulkan call may be made there.

So the frame is **recorded, not rendered**. `frame_begin` opens a record; `submit` appends a
fixed-size poly struct to a vector; `frame_end` closes it; the two layer hooks copy their bitmaps
into it. The record is handed across the same baton that already carries the finished framebuffer
(`libretro_m2_osd.cpp`, `update()` posts and parks), and `retro_run` on the frontend's thread turns
it into buffers, command buffers and a submit.

Two properties this must have:

- **Storage is reused, not reallocated.** Two records, ping-ponged; each keeps its vectors' capacity
  across frames. 1450 polys at ~200 bytes is 300 KB, and it should be allocated once per run.
- **The "no new frame" case is preserved.** `render_polygons` returns early, re-copying the previous
  `destmap`, when the geometrizer has not presented a new list (`m_render_done`). The Vulkan path has
  the same case and the same answer: keep last frame's 3D image, composite the new 2D layers around
  it. This falls out naturally as long as the 3D target is not cleared when no record arrives — but
  it has to be *deliberate*, because the failure mode is a flickering 3D layer at whatever rate the
  geometrizer runs behind the display.

### 7. The A/B harness stops being a `cmp`

`retrohost --vk` currently proves the two renderers agree by comparing PPMs byte for byte and by
digesting every frame of a run. After P3 that answer is always "different", and a harness whose only
output is a boolean that is now permanently false is worse than no harness.

What replaces it, in the order the numbers matter:

- **The masked bit-exact test** (exit criterion 1). The 3D layer's coverage is known — it is exactly
  the set of pixels the Vulkan pass wrote, which the shader can record into a stencil-shaped mask, or
  which can be obtained more cheaply by running the software renderer's `fillmap` alongside. Outside
  that mask, `cmp` still applies and must still pass. **This keeps a hard, exact test in the harness
  after the soft ones arrive**, and it is the one that catches compositing, crop and palette
  regressions.
- **SSIM plus a per-pixel heatmap** over the 3D region, per fixture frame, with a recorded per-fixture
  threshold rather than one global number.
- **A coverage-only diff**: for each pixel, drawn-or-not in each renderer, ignoring colour. This
  separates "the rasterizer disagrees about which pixels the triangle covers" from "the shading is
  wrong", and those two have completely different causes. Expect a thin disagreement along every
  polygon edge and nothing else; a *filled region* of coverage difference is a real bug.
- **Single-polygon A/B.** A debug mode that renders polygon *n* of frame *m* and nothing else, in both
  renderers. This is the tool that will actually find the shading bugs, and it is worth building
  early — before the first textured polygon, not after.

Contact sheets and the ROM matrix are P6. P3 needs the four measurements above and a baseline
recorded for three fixtures.

## Looking ahead: what this shape does and does not enable (asked at step 1, answered here)

Not P3's scope, but the answers are load-bearing for P3's design and were established while building
step 1. **Internal-res scaling is unblocked; widescreen is capped by game code, not by us.**

### Internal resolution — yes, and step 1 is what bought it

Vertices arrive in `m_destmap` screen space. Rendering at S× is `x,y *= S` into an S·512 target, and
every other decision in this plan is scale-invariant: draw-order depth does not care; `noperspective`
`rz`/`uz`/`vz` interpolate the same endpoint values over more pixels, which is exactly correct finer
sampling; per-poly scissor from `poly.clip[]` scales by the same factor. Nothing needs re-projecting,
because the perspective divide already happened and a uniform screen-space scale commutes with it.

**This is the payoff of the sandwich decision in problem 1.** The rejected readback-into-`m_destmap`
design would have been hard-capped at 512×512 by `bitmap_rgb32` — a 4× frame does not fit in it. GPU
compositing means the 3D layer's resolution is decoupled from MAME's bitmaps from the start; the 2D
layers just get nearest-upscaled by S.

Three things that do **not** come for free, and all three are shader-side:

⚠️ **Two of these three were resolved on 2026-07-28, one of them by deciding it was not a problem.**
The internal-resolution *option* shipped that day (`model2_internal_res`, [user-options.md](user-options.md)
§4); the text below is the original scoping and is kept because the third bullet is still the reason the
default is `1x`.

- ~~**Mip selection is resolution-blind.**~~ ✅ **Correct as it stands — do not add the bias.** It is
  resolution-blind because it keys on view depth, and view depth is what the *hardware* selected on. A
  supersampled frame therefore gets the mip an arcade board would have picked: edges smooth, texture
  detail unchanged. That is the accuracy-first answer, and it is what makes the 3× point runs score
  98.97–99.97 % bit-identical against 1× — if `mml` tracked the attachment they could not. **Revisit
  only as an explicit sharpening option**, never as a correctness fix. Original text: `mml = -texlod +
  fast_log2(z)` uses view depth, which does not change with S — so at 4× the shader picks the 1× mip
  and textures come out soft. If it is ever wanted, **derive the constant from the `fast_log2` fixed
  point** (it returns log2 in 8.8 and `level = mml >> 7`, i.e. the chain is log2(z²)); do not guess it,
  and do not apply it via `maxSamplerLodBias` — that limit is irrelevant here because the filtering is
  hand-written.
- ~~**`checker` stipple gets finer with S.**~~ ✅ **FIXED 2026-07-28, and the description above was
  wrong about the symptom.** It does not read as "a grey blend" — at an *even* scale a box resolve of a
  finer checkerboard covers **every** output pixel, so the polygon goes fully opaque: measured on one
  vcop2 quad, 78968 px at 1× and **157945 at 2×, the whole hull, 0.000 % of the overlap the same
  colour**. The dither-vs-real-alpha decision was therefore never live — a screen door that stops being
  a screen door is not a dither, it is a missing cutout. The fix is one integer divide in `poly.frag`:
  test the parity in **picture** pixels (`ivec2(gl_FragCoord.xy) / scale`), so all n² subpixels of a
  picture pixel share a parity. That quad now draws **78968 at 2× box, 100.000 % identical to 1×**.
- **Scaling must be a runtime option, not a build one.** The A/B harness only means anything at 1×.
  ✅ Satisfied: `M2VK_SS` was always a switch, and `model2_internal_res` defaults to `1x`.

### Widescreen — partial at best, and the ceiling is the i960

The seam itself is not the obstacle, which is the surprising part. Because Model 2 is
perspective-correct and every vertex carries z, **un-projecting is exact arithmetic**:
`x_view = (x_screen - cx) · z`, invert `model2_3d_project`, re-project at a wider FOV. No
approximation anywhere.

Two walls behind that, in increasing order of how badly they end it:

1. **The geometry is already clipped against four planes** built from the game's own `center` and
   `viewport` registers ([model2_v.cpp:893-912](../src/mame/sega/model2_v.cpp#L893)) — recomputed
   whenever the geo engine writes them. Widening `left_plane`/`right_plane` there is a genuinely small
   patch, but it is an upstream edit *in the geometry engine*, it changes what the software renderer
   sees, and it therefore costs the A/B reference an axis. Worth knowing: `check_culling`
   ([model2_v.cpp:245](../src/mame/sega/model2_v.cpp#L245)) does backface, linktype, `master_z_clip`
   and behind-camera rejection only — **no lateral culling**. Sideways rejection is purely a side
   effect of the four-plane clip, so widening those planes really does recover geometry.
2. **…but only geometry the game bothered to submit.** The culling that actually bites is
   object-level, on the i960, in game program ROM — the game does not emit draw commands for objects
   outside its own ~4:3 frustum at all (`../Polydiver/PDDocs/model2/model2_culling.md`, which is the
   authoritative treatment; it was written for the VR case and the analysis is identical). There is no
   register for it. Patching the i960 is per-game, fragile, and self-defeating: an object the game
   decided to cull **has no valid transform computed for it**, so it comes back as garbage rather than
   as the off-screen world.

So the realistic menu is: **stretch** (trivial, distorted), **vert-** (crop top and bottom, keep
horizontal FOV — needs nothing at all), or **modest hor+** that lives inside whatever margin each game
happens to submit past its own frustum, degrading to pop-in and empty gutters at the sides. Genuine
16:9 hor+ across the library is a research project against game code, not a renderer feature, and it
should not be sold as one in the core's options.

## Corrections to the plan of record

Two things in `../Polydiver/PDDocs/model2/model2_libretro_core.md` §4 do not survive contact with
this seam. Both are Model 1 / Unity conclusions that were carried across, and both are listed here
rather than silently dropped because the reasoning is still right *where it applies*.

- **"Shadows via matrix singularity — detect `det(3×3)≈0` and route to a dedicated pipeline" cannot
  be done here.** The det test is a *Model 1* technique operating on per-object model→view matrices
  in Unity's playback path (`model1_sortorder.md` §9.5). At this seam there are no matrices at all —
  polygons arrive already transformed, projected and clipped to screen space. There is nothing to
  take a determinant of.

  It is also unnecessary for Model 2. `model2_shadows.md` establishes that VF2's shadows are **real
  flattened silhouette geometry** — a 15-mesh, 32-poly per-body-part group rigidly attached to the
  fighter, proven camera-independent over a 168° orbit. They arrive as ordinary polygons in the
  stream. So Model 2 shadows need **no shadow pipeline in P3** and become, in P4, an instance of the
  general coplanar-ordering problem rather than a special case.
- **Most of `model2_lighting.md` is already done for us, and the part that isn't is the part that
  matters.** §1's *first* block (dot products, diffuse/ambient/specular, the Phong term) is the
  geometry engine's work, and MAME's copro emulation has already performed it by the time the tap
  fires — the result is the single `poly.luma` byte. Porting it to GLSL would be recomputing an
  answer we were handed. What P3 ports is §1's *second* block, the raster tail: `lumaram` →
  `colorxlat` → gamma. That is the `model2rd.ipp` transliteration in problem 4, and
  `model2_lighting.md` is the right cross-check for it.

## Where the code goes

As built through step 2. The one departure from the original map is that `m2vk::vertex` and
`m2vk::poly` live in `m2vk_frame.h`, not `m2vk_sink.h` — see step 2's note for why.

```
src/osd/libretro_m2/
├── m2vk_sink.h/.cpp             ← the seam only: frame_begin gains a frame_tables parameter, the
│                                   templated submit() resolves the palette colour, active() ORs in
│                                   capturing(), m2vk::rasterize() added
├── m2vk_frame.h/.cpp            ← NEW. What crosses: the poly and vertex structs, frame_tables, and
│                                   the record itself — polys, the baked colour chain, the two 2D
│                                   layers, and (step 4) the two texture-RAM pointers, which are held
│                                   live rather than copied. One record, capacity reused.
└── renderer_vk/
    ├── vk_geom.h/.cpp           ← NEW at step 3. Per-slot vertex/index/param/table/texture buffers,
    │                                the pipeline, and the frame's single indexed draw. No colour
    │                                target of its own: it draws into the ring image, between the two
    │                                2D draws, against a D32_SFLOAT depth attachment vk_present owns
    ├── vk_tables.h/.cpp         ← NOT BUILT. The LUT and texture-RAM buffers are four more
    │                                mapped_buffers in vk_geom.cpp, which owns the descriptor set they
    │                                are bound to; a second file would have owned nothing else
    ├── vk_present.cpp           ← grows the three-layer composite; the passthrough path stays for
    │                                renderer=software and as a fallback. M2VK_GEOM_LOG=1 reports the
    │                                record as the renderer sees it, one line per frame with new
    │                                geometry — which is what makes it line up with the polytap
    └── shaders/
        ├── poly.vert            ← screen-space position, noperspective rz/uz/vz, draw-order depth
        ├── poly.frag            ← the model2rd.ipp transliteration: get_texel,
        │                            fetch_bilinear_texel, trilinear + microtexture, the LUT chain,
        │                            the cutout and the checker stipple
        └── composite.frag       ← under / 3D / over, with the crop

src/mame/sega/model2_v.cpp       ← 16 lines → 26 at step 1. Two hooks in screen_update, two lines in the
                                    existing model2_3d_render block.
```

## Order of work

Each step ends in something observable. Nothing is done on the strength of it compiling.

1. ~~**The sandwich, with MAME's own 3D in the middle.** The two `screen_update` hooks, the frame
   record, and a GPU composite of under + `m_destmap` + over — with the software rasterizer still
   running and `m2vk::rasterize()` still returning true. No polygon is drawn by the GPU.
   *Verify:* **bit-exact against P2** over vf2/vcop2/srallyc through `retrohost --vk`.~~
   **Done 2026-07-26.** Built smaller than planned: there is **no third capture of `m_destmap`**,
   because the under-layer hook goes *after* the render-polygons branch rather than before it. While
   the software rasteriser owns the 3D it is already inside the captured under layer, so the composite
   is under + over and equals MAME's own frame exactly; when `m2vk::rasterize()` goes false at step 3
   the same hook yields background alone and the GPU's 3D drops into the gap. One hook serves both
   renderers.

   New: `m2vk_frame.{h,cpp}` (the record — two layers, cropped at capture, one buffer and no lock
   because the baton parks the writer for all of `retro_run`), `capture_layer()` and
   `rasterize()`/`set_rasterize()` in `m2vk_sink.h`, `shaders/overlay.frag`, a `layer_tex[2]` inside
   `frame_slot` with a doubled descriptor pool and a second pipeline built from the *same* structs as
   the first. **Upstream diff against mame0288: 16 → 26 lines**, three new `#ifdef M2VK` sites (six blocks in all).

   Verified by `cmp`, not by eye: software vs vulkan byte-identical PPM *and* whole-run digest over
   vf2 (1500), vcop2 (2500) and srallyc (2500); identical again across two context losses and two
   sync-mask changes mid-run, and across the abandon path; `M2VK_VK_DUMP`'s in-core read-back
   `src == vk` under both hosts; the committed `vf2-frame800-polytap.txt` fixture still matches;
   RetroArch 1.22.2 clean over 1400 frames. `M2VK_NO_SW_3D=1` (new) confirms the slices really are
   separate — sky and clouds intact, every polygon gone, `CREDIT 0/2` and `© SEGA 1994` still drawn
   *over the hole*. The 3D covers 19 % of that frame (37073 of 190464 pixels).
2. ~~**Record the geometry, draw nothing.** Buffer the poly stream and the tables into the record and
   hand them across the baton; the composite still uses `m_destmap`. *Verify:* the record's per-frame
   poly counts match the polytap's frame for frame over 2500 frames; output unchanged from step 1
   (still bit-exact against P2); RSS flat across the run.~~
   **Done 2026-07-26.** All four things in problem 5's table cross, plus the poly stream itself.
   `frame_begin` grew the `frame_tables` parameter as planned and it is passed braced at the seam, so
   **one existing line changed and none were added — the upstream diff is still 26.**

   Built differently from the file map in one respect: **`m2vk::vertex` and `m2vk::poly` moved out of
   `m2vk_sink.h` and into `m2vk_frame.h`.** The record holds a `std::vector<poly>` and the sink
   already includes the frame header, so leaving them in the sink meant a circular include, a third
   header, or an extra include in `model2_v.cpp`. Moving the data shapes to the file that owns the
   data costs nothing and the division reads better: **`m2vk_frame.h` is what crosses, `m2vk_sink.h`
   is how it is intercepted.** Two other things that were not in the plan: `active()` became
   `g_active || capturing()` (the polygon tap used to be the only thing that could turn the stream
   on, so the record would have been dead unless a diagnostic variable was set), and `g_want_layers`
   was renamed `g_capturing`, because it gates geometry as well as layers now and the old name lied.

   The record is deliberately **not** a consumer behind the sink: it is the renderer's half of the
   seam rather than something watching it, it is the only thing that needs the tables, and it must
   not depend on a consumer list an environment variable can empty.

   The tables carry their own serial, bumped **only when the bytes actually changed** — the compare
   rides along with the copy rather than being a separate `memcmp`. vf2 changes them six times in
   nine hundred frames, so step 4's upload can skip almost every frame.

   Verified by `cmp` and by count, not by eye: software vs vulkan byte-identical PPM *and* whole-run
   digest over vf2 (1500), vcop2 (2500) and srallyc (2500), all six equal to the values step 1
   recorded; **poly and submitted counts identical to the polytap frame for frame** (511/512 on vf2,
   1804/1805 on vcop2, 1142/1143 on srallyc — see the gotcha below for the off-by-one); identical
   again across two context losses, two mask changes and the abandon path; the committed
   `vf2-frame800-polytap.txt` fixture still matches; the standalone `mamemodel2` still builds and
   runs; RetroArch 1.22.2 clean over 1400 frames with the tables hashing to the same values as under
   `retrohost`. **RSS: see the note under "Gotchas" — the criterion as written is not achievable and
   the reason is not this step.**
3. ~~**Untextured polys, real depth buffer.** Vertex and index buffers (fan every poly to `n-2`
   triangles), `D32_SFLOAT`, draw-order depth, `GREATER` + depth write, the solid shading path
   (`draw_scanline_solid` — resolved palette colour → `colorxlat` → gamma, no `lumaram`) and the
   `checker` stipple. Untextured *translucent* must draw **nothing**, as in the software renderer.
   *Verify:* VF2 has one solid poly per frame, so verify with a debug switch that forces every
   polygon down the solid path in **both** renderers, and compare **coverage** — drawn-or-not, colour
   ignored. Disagreement should be a one-pixel rim on polygon edges and nothing else. Also: the
   single-polygon A/B mode from problem 7 gets built here, not later.~~
   **Done 2026-07-26. The GPU draws.** New: `renderer_vk/vk_geom.{h,cpp}`, `shaders/poly.vert`,
   `shaders/poly.frag`, `devnotes/ppmdiff.py`. The render pass gained a `D32_SFLOAT` depth
   attachment and every ring slot a depth image; the whole frame's geometry is **one indexed draw**
   between the under and over layer draws. `find_memory_type` moved into `vk_funcs` so the ring and
   the geometry buffers share one copy of it.

   **Three departures from the plan, all in the same direction — simpler.**

   - **There is no 512×512 render target and no crop.** The gotcha below said to render into 512×512
     because vertices are in `m_destmap`'s space, and then crop. It is unnecessary: the vertex shader
     divides by a push-constant *half-extent of the visible picture* and the viewport is 496×384, so
     `gl_FragCoord.xy` equals the software renderer's `x`/`scanline` directly, which is all the
     512×512 target was ever for. Geometry past x=495 is clipped by NDC, which is exactly the crop.
     It is scale-invariant too, so P5's internal-res scaling is a viewport change and nothing else.
   - **No separate 3D image, so no compositing pass.** The polygon pass draws straight into the ring
     image between the two 2D draws. One render pass, three draws, one depth attachment.
   - **The `m_render_done` dupe path costs nothing and is already handled** (it was step 6's).
     `render_polygons` takes its early return without touching the record, so what is still in there
     *is* last frame's list; the renderer re-uploads and redraws it. One behaviour rather than two,
     and no "keep last frame's 3D image" state to get wrong.

   Textured polygons are counted and skipped (steps 4 and 5); untextured translucent ones are dropped
   at upload rather than discarded in the shader, because that is visibly the same decision
   `draw_scanline_solid<true>` makes, in the same place. So the default `renderer=vulkan` picture is
   still mostly a hole — **`M2VK_FORCE_SOLID=2` is what makes a frame visible, and it is the harness.**

   **Upstream diff 26 → 28 lines, and the two extra ones are not the drawing.** Skipping the scanline
   dispatch leaks `poly_manager`'s object-data arena: `model2_3d_render` takes a slot for every
   polygon before the seam is reached, and `poly_manager` only recycles the arena in `wait()`, which
   early-outs when there are no work units outstanding. RSS went 175 → **398 MiB** over 2500 vf2
   frames. `if (!m2vk::sw_owns_3d()) m_renderer->object_data().reset();` in the existing `frame_end`
   block fixes it; the run now ends at 187 MiB, *below* step 2's 202.7.

   Verified by measurement: coverage agreement **1.0000** on vcop2, srallyc and dynamcop (zero
   disagreeing pixels of 154203 / 136116 / 187571) and 1 of 37449 on vf2, all of them isolated and on
   an edge; exit criterion 1 exact on all four; **single-polygon A/B pixel-perfect** on vf2 polygons
   0/100/300 and on a vcop2 `checker=1` quad; `M2VK_SW_3D=1` still byte-identical to
   `renderer=software`; ring rebuild, mask change, abandon path and the 2048→4096 growth path all
   clean; the committed polytap fixture still matches; RetroArch 1.22.2 clean at **104.55 % of full
   speed**, up from 72.88 %.
4. ~~**Texture RAM and the textured opaque path.** Both sheets as one storage buffer; `get_texel`,
   `fetch_bilinear_texel<false>`, `fast_log2`, the mip level and trilinear blend, microtexture, and
   the `lumaram` → `colorxlat` → gamma tail. *Verify:* single-polygon A/B on a textured poly first,
   then whole-frame SSIM on vf2 frame 800 against the committed P0 fixture's frame. This is the step
   where the picture first looks like the game.~~
   **Done 2026-07-26, committed `f3aa8614856`. The picture is the game.** Built as planned — the transliteration went in
   without any of the design being wrong — so what is worth writing down is the two things the plan
   did not say and the shape of the residual.

   **Texture RAM is not snapshotted.** The plan's table in problem 5 has the three colour tables
   crossing as copies; texture RAM is 2 MB and copying it on the emulation thread would be the
   largest single cost in the frame for no gain. The `frame_tables` struct grew
   `texram[2]` + `texram_words[2]` and the record holds the **live pointers**, which the frontend
   thread reads while the emulation thread is parked on the baton — the same guarantee the snapshots
   were resting on anyway. One `memcpy` per frame, into the slot's own 2 MB buffer, and only on a
   frame that actually drew a textured polygon. **`frame_begin`'s call site grew, so the upstream
   diff is still 28 lines.**

   **`M2VK_OPAQUE_ONLY=1` is new and it is why this step is measurable at all.** With the translucent
   cutout still a step away, the software renderer draws 72 polygons of a vf2 frame that the GPU does
   not, and the coverage diff reads that as a filled region of rasterizer disagreement — 1102 interior
   pixels on vf2, which is exactly the failure the diff is meant to be diagnostic of. The switch
   rewrites every translucent polygon to renderer class 1, **the one class neither renderer draws**
   (`draw_scanline_solid<true>` returns before writing a pixel; `vk_geom` drops it at upload), so it
   subtracts the identical set from both paths. With it, coverage agreement is 1.0000 on five of the
   six games tested and effectively on the sixth (dynamcop's 72 are the black-on-black artefact in the
   gotchas below).

   Other departures, all small: `max_level` is resolved on the CPU (`30 - clz(min(w,h))` is per
   polygon, not per fragment); `gpu_poly` went from 4 words to 16, which is where the texture
   parameters live rather than in the `reserved` word the plan earmarked; and `fast_log2`'s table is
   written out as a literal 128-entry array rather than packed, because the argument of the whole file
   is "transliterate literally" and a hand-packed table is a place to be silently wrong.

   Verified by measurement. **Coverage is exact** on vf2, vcop2, srallyc, dynamcop, desert and
   waverunr, with at most two disagreeing pixels and both of them on an edge. **Single-polygon A/B on
   textured polygons is pixel-perfect** on five of vf2's, and 99.54 % on a 35910-pixel floor quad with
   coverage still 1.0000. Microtexture is genuinely exercised: desert taps 71582 microtextured
   polygons over the run and comes out 97.3 %, waverunr covers 99.2 % of the picture at 90.8 %. See
   the worklog for the table and for the attribution of the residual, which is float rounding in the
   perspective divide amplified by a LUT with a step per index — isolated pixels, zero signed bias, no
   spatial trend, and concentrated exactly where the texture minifies hardest.
5. ~~**The translucent cutout path.** `fetch_bilinear_texel<true>`: the packed alpha lane, the
   transparent-texel-takes-neighbour-luma rule at all three interpolation stages, the < 50 % discard.
   *Verify:* vf2 frame 800 SSIM improves and the translucent polygons (81 of 552 in the sampled
   frame) stop being either invisible or opaque rectangles.~~
   **Done 2026-07-26. The faces have eyes.** Built exactly as the plan said and as small as it said —
   `poly.frag` gained the packed alpha lane, the neighbour rule and the discard; `vk_geom.cpp` lost
   the `cls == 3` skip. **No new files, and no upstream file touched: the diff is still 28 lines.**

   MAME reaches the specialisation by template parameter; the shader reaches it by an ordinary `bool`
   that is uniform across the polygon, because two copies of a hundred lines that must stay identical
   is the more expensive mistake. Texel index 15 is the transparent one, so the alpha flag is the
   *absence* of that index, parked at bit 23 of the `0x00ff00ff` packing `LERP` already had.

   **The property that had to be true, and is: a discarded fragment does not write depth.** No
   `EarlyFragmentTests` execution mode, so depth is written at late fragment tests, after the discard
   — which is what keeps the draw-order key equal to `m_fillmap`, where a skipped pixel likewise leaves
   `fill[x]` at zero for a later polygon to claim. The `checker` stipple has rested on this since step
   3; the cutout is the same mechanism over a much larger fraction of the frame.

   Verified by measurement, whole frame, **without** `M2VK_OPAQUE_ONLY` — which is the measurement
   this step exists to make possible. **The two heaviest translucency users in the 29-game survey are
   the result that matters:** `sgt24h` (1436 of 1742 polygons translucent) and `overrev` (1094 of
   1145) both come out at coverage agreement **1.0000 with zero disagreeing pixels** over 187983 and
   183505 pixels. The cutout is a hard binary decision per fragment, so agreeing about it that many
   times means the packing, the neighbour rule at three stages, the blend of two packed lanes and the
   threshold all produce MAME's alpha. Same colour: vcop2 99.54 %, overrev 98.07 %, dynamcop 97.33 %,
   desert 97.32 %, vf2 95.06 %, waverunr 90.10 %, srallyc 86.95 %, sgt24h 84.81 %; exit criterion 1
   passes on all eight; single-polygon A/B on a translucent quad is 96.08 % with one edge pixel.
   **Step 4's 1102 interior disagreements on vf2 are gone** — 0 interior, 1 edge — which was the
   specific thing this step was for. `M2VK_OPAQUE_ONLY=1` still reproduces step 4's numbers to the
   pixel, so the opaque path is untouched. The worklog has the full table.
6. ~~**Scissor, windows and the dupe path.** Per-polygon scissor from `poly.clip[]`, grouped into draws
   so the common case is one draw; window ordering (descending, later windows drawn first); and the
   `m_render_done` "keep last frame's 3D" case. *Verify:* srallyc and vcop2, which have viewport and
   window behaviour VF2's single constant viewport never exercises; plus a run where the geometrizer
   falls behind, checked for a stable rather than flickering 3D layer.~~ **DONE — and the picture does
   not change anywhere in a 25-game survey, which is the finding rather than a disappointment.** Only
   `vk_geom.{h,cpp}` changed; no shader, no new file, no upstream file. Two of the three items were
   already correct for structural reasons and this step's real work was establishing *that*.

   **The scissor is built and it never fires.** `geom_upload` groups consecutive polygons sharing a
   clipped viewport into a `draw_batch`; `geom_draw` records one `vkCmdSetScissor` + `vkCmdDrawIndexed`
   per batch and **restores the full extent before returning**, without which the OVER tilemap layer
   would be clipped to the last polygon's window. An empty `vp` drops the polygon, as
   `render_triangle` against an empty cliprect does. Across 25 games only **`schamp` (up to 8 batches)
   and `dynabb97` (up to 3)** ever have more than one; the other 23 are one full-screen rectangle every
   frame. Polygons actually cut by a viewport tighter than the screen: dynabb97 3421, schamp 35, **zero
   in all 23 others** — and the worst excess anywhere is **1/32768 px, one float ULP**. The reason is
   upstream of the seam: the geometry engine has already clipped every polygon against four frustum
   planes built from the same registers `clip[]` comes from ([model2_v.cpp:894](../src/mame/sega/model2_v.cpp#L894)),
   so the screen-space scissor is an exactly redundant second cut. Whole-run digests with the scissor
   on and with `M2VK_NO_SCISSOR=1` are equal on dynabb97, schamp, vcop2 and lastbrnx.

   ⚠️ **This step's fixtures are `schamp` and `dynabb97`, not the vcop2/srallyc named above.**
   vcop2's inset demo panel is a Model 2 *window*, not a viewport — 4 window runs, **1 scissor draw, 0
   polygons cut** — so its contents are built to fit and nothing was ever going to scissor them.
   srallyc has one window and one viewport for every frame of a run.

   **Window ordering was already correct and is now asserted.** The record is in seam order and the
   draw-order depth key is the record index, so the priority `for (window = cur_window; window >= 0;
   window--)` expresses is already in the key — nothing to sort, nothing to group. `geom_upload` checks
   it rather than assuming: **all 25 games descending, every frame**, up to 7 window runs (lastbrnx).

   **The dupe path is proven and exercised far harder than expected.** The counter now separates dupes
   from *drops* — a drop being a frame that had 3D and then did not, i.e. flicker. `vstriker` redraws
   last frame's list on **1165 of 2500 frames, 47 % of the run**, lastbrnx 529, vf2 259, schamp 197 —
   and **3D dropped from 0 frames on every game**. No code was needed for it.

   Regression: the eight-game step-5 table reproduces **to three decimals**, `M2VK_OPAQUE_ONLY=1`
   reproduces step 4's vf2 numbers to the pixel, and the whole-run vf2 digests are byte-equal to a
   build of HEAD without step 6. RetroArch **104.50 %**; RSS peak 195.20 MiB. The worklog has the full
   tables, the new `M2VK_NO_SCISSOR=1` switch, the corrected `M2VK_SW_3D` digest, and one anomalous
   non-reproducing schamp digest written down so it is not rediscovered as a scissor bug.
7. ~~**The A/B harness, rebuilt.** The masked bit-exact test, SSIM, the coverage diff and the
   heatmap in `retrohost`. *Verify:* baseline numbers recorded for vf2, vcop2 and srallyc, and exit
   criterion 1 passing exactly on all three.~~ **DONE — and it found a real bug on its first full
   run, which is the result rather than the SSIM number.** No core code changed; the whole step is
   `devnotes/`. `ppmdiff.py` rebuilt, `ab.sh` / `ab-table.py` / `ab-baselines.md` new.

   **Built in `ppmdiff.py`, not in `retrohost`.** The plan said `retrohost`; the coverage definition
   and the background reference both live in the Python tool, and SSIM needs the `M2VK_NO_3D=1`
   reference that a single `retrohost` run does not have. `retrohost` was not touched.

   `ssim` is per **RGB channel**, not luma — the `colorxlat` tail produces chroma-only differences a
   luma SSIM scores as perfect — and is reduced over the **covered region**, with three means printed
   (`whole frame` / `covered` / `interior`, the last over pixels whose whole 11×11 window is covered)
   so the gap is visible rather than asserted. Percentiles come with it: `sgt24h` is 0.9414 with a
   **p1 of 0.137**, and that spread is the fixture. Validated at 1.0 exactly on identity and against
   a brute-force 2D-window implementation to **1.9e-15**; the numpy rewrite of `coverage`/`exact` is
   byte-identical to the old pure-Python one on real data, exit status included.

   **`coverage`'s verdict is now trustworthy.** Interior disagreements are split by how far the two
   renderings actually differ — `<= 8` is the documented drew-black artefact and passes, more is a
   missing polygon and fails. dynamcop's three are `(0,0,0)` vs `(2,2,0)`/`(6,6,6)`, printed and
   checked, so it passes; vstriker's 190464 still fail.

   **Exit criterion 1 passes on all twelve fixtures. Exit criterion 2 passes with room on the three
   named: vf2 0.9963, vcop2 0.9999, srallyc 0.9896.** Guards: `M2VK_OPAQUE_ONLY=1` reproduces step
   4's vf2 numbers to the pixel, `M2VK_FORCE_SOLID=2` reproduces step 3's counts exactly **and scores
   SSIM 1.0000** on vcop2/srallyc/dynamcop — with flat shading the renderers are pixel-identical,
   which puts the whole residual in the shading chain and none of it in the rasterizer, depth key,
   scissor or composite.

   ⚠️ **Two findings, both in `ab-baselines.md`.** (1) **A stale-3D bug, present since step 3**: the
   GPU redraws the last polygon list after the game stops submitting geometry, because
   `render_polygons` bails at `poly_list_index == 0` (line 711) *before* `m2vk::frame_begin`, leaving
   the record as untouched as the dupe case at line 705 — and the two want opposite behaviour.
   **Step 6's `3D dropped from 0 frames` counter cannot see it**, since it reads the record. Not
   fixed; the fix is core. (2) **`lastbrnx` is bistable and dropped from the set** — two digests,
   11.26 % of the frame apart, both occurring under `renderer=software` with `M2VK_NO_3D=1` where
   none of this code runs. It is `draw_framebuffer`'s `frame_number() & 1` and it **explains step 6's
   unreproducible `schamp` digest**.
8. ~~**Docs.** This file rewritten as-built, a worklog entry, and CLAUDE.md's "Where we are" / "Next
   step". Anything measured about the device goes into [vulkan-target.md](vulkan-target.md), not into
   the worklog only.~~ **DONE — but the step grew a piece of code first, and that is the honest order
   of it.** Step 7 ended with the stale-3D bug open, so step 8 opens with the fix
   (`c38dbbefffe`, "Stop redrawing the 3D after the display list goes empty") and then the docs.
   Nothing measured about the device changed, so [vulkan-target.md](vulkan-target.md) is untouched.

   **The fix.** `m2vk::frame_begin` is hoisted above the `poly_list_index == 0` bail so the record
   hears about an empty display list — **upstream diff 28 → 30 lines**, the two being a comment
   explaining why the call sits above the bail, which is the kind of thing a future merge conflict
   resolver needs. `frame_begin(0, …)` routes to a new `m2vk::geometry_none()`, which marks the record
   valid with `poly_count = 0` and bumps the serial: *a new frame that is empty*, as against *no news*.
   The renderer draws nothing for the first and redraws for the second, which is what MAME does with
   its own `destmap` either side of the same two branches. Files: `model2_v.cpp`, `m2vk_frame.{h,cpp}`,
   `m2vk_sink.{h,cpp}`, `renderer_vk/vk_geom.cpp`. No new file, no shader change.

   **Two design points that should survive a rewrite.** The sink deliberately does **not** notify the
   consumers on an empty frame: "rendered frame" in the polytap means a frame *carrying polygons*,
   which is what `M2VK_POLYTAP_DUMP=N` counts and what the committed `vf2-frame800-polytap.txt`
   fixture is keyed on — vf2 alone queues nothing for its first 987 frames, so counting them would
   renumber every fixture in the tree. And step 6's single `dropped` counter became **three** —
   `dupes` / `empty` / `dropped`. `empty` is not a fault; it is an **inverted** check. Every game boots
   through empty display lists, so a run reporting zero of them means the core has stopped notifying
   the record and the bug is back with nothing else able to see it.

   *Verified:* **vstriker@2500 goes from 190464 real interior disagreements to 0** — 190464 being the
   entire 496×384 picture. All 12 fixtures regenerated: **every last-frame metric reproduces the
   pre-fix table to the digit** on the eleven whose last frame carries 3D, exit criterion 1 passes on
   all 12, and no interior disagreement anywhere. Whole-run digests move on exactly the six fixtures
   with empty frames after the 3D had been drawn and stand still on the four without — which is the
   fix working, since those frames were drawing a stale list.

   The one fixture that broke that pattern was chased rather than explained away: **`schamp` has 197
   such frames and its digest did not move.** `3c8632ce4d3` was rebuilt and both measured —
   `8f1abfef0c4f9bed` either side, against vf2's `7aa3c7c7bdfd2be6` → `55da761fecca5c01` — so schamp's
   stale redraws were landing on pixels indistinguishable from drawing nothing and it never visibly
   showed the bug. The same rebuild confirmed the counter accounting exactly: pre-fix `dupes` was
   **259 on vf2 and 197 on schamp**, the identical frames the new `empty after the 3D had been drawn`
   figure counts. One reclassification, no frame gained or lost.

   Also verified: the committed `vf2-frame800-polytap.txt` fixture is byte-identical under **both**
   renderers, which is the check that proves the not-notifying decision rather than assuming it;
   `renderer=software` untouched at `16af05bb8d02a9a5`; the standalone `mamemodel2` (`OSD=sdl3`, no
   `M2VK`) still builds and links.

## Gotchas known in advance

- ~~**Render at 512×512, not 496×384.**~~ **Superseded at step 3 — it is 496×384 and there is no
  crop.** The concern was right and the remedy was not needed. Vertex `x`,`y` are in `m_destmap`'s
  coordinate space — CRTC offsets already applied — and `m_destmap` is 512×512, while the visible
  area is x 0–495, y 0–383 ([model2.cpp:2538](../src/mame/sega/model2.cpp#L2538):
  `set_raw(..., 656, 0, 496, 424, 0, 384)`). What actually matters is that `gl_FragCoord.xy` equals
  the software renderer's `x`/`scanline`, because the `checker` stipple draws where `(x ^ y) & 1` is
  1 and a half-pixel or origin disagreement inverts the entire pattern. Dividing by the *visible*
  half-extent with a 496×384 viewport gives that directly, and clips the invisible columns as NDC
  clipping rather than as a later crop. Do not reintroduce the 512×512 target.
- **`rz` means two different things.** `1/z` for textured polygons, raw `z` for untextured ones,
  because `model2_3d_render` only runs the reciprocal loop in the textured branch. **The convention
  that won: the GPU vertex's `rz` is always `1/z`**, normalised in `vk_geom.cpp`'s upload
  (`normalise_rz`) by reproducing MAME's own `1.0f / (z + FLT_MIN)` verbatim, denormal guard and all,
  so a polygon taking the textured path later gets the identical float. The record still carries the
  raw two-meaning value, because that is what the seam handed over.
- **Skipping the scanline dispatch leaks `poly_manager`'s object-data arena, and it looks like a
  renderer leak.** `model2_3d_render` takes a slot from `object_data()` for every polygon *before*
  the seam is reached; `poly_manager` recycles the arena in `wait()`, which early-outs when
  `m_unit.count() == 0` ([poly.h:659](../src/devices/video/poly.h#L659)). With nothing rendering
  there are never any work units, so the arena spills into its overflow chain for the whole run —
  measured at 175 → 398 MiB over 2500 vf2 frames, and it reproduced with `M2VK_NO_3D=1`, which draws
  no geometry at all. That is what identified it. Fixed by resetting the arena by hand in the
  `frame_end` hook block. **If RSS ever climbs again, check this before the Vulkan side.**
- **`M2VK_VK_DUMP`'s `cmp -src -vk` stopped being an equality test at step 3.** `-src` is MAME's
  finished software frame, which now has a hole where the 3D is; `-vk` is the composite with the GPU's
  3D in it. They are supposed to differ. Use `ppmdiff.py` against `retrohost --vk`'s read-back, which
  was always the ground truth anyway.
- **Within a sort bucket, polygons draw in reverse submission order.** The bucket lists are built by
  prepend. The draw-order depth key must come from the order the seam actually sees them
  (`frame_begin`-relative sequence number), never from anything reconstructed from the display list.
- **Later windows draw first.** `for (window = cur_window; window >= 0; window--)` — descending, so
  higher-numbered windows end up on top. Reading it as "later = on top" gets it backwards.
  **Settled at step 6 and it costs nothing:** the record is in seam order, the draw-order depth key is
  the record index, so the priority the loop expresses is already in the key. `geom_upload` asserts the
  order rather than assuming it — 25 games, descending, every frame, up to 7 window runs. A run that
  prints `ASCENDING` has window priority inverted, and that is invisible in a single-window game.
- **`lumabase + (texel >> 1)` is exactly 15 bits.** `lumabase` is `(hdr1 & 0xff) << 7`, max 0x7f80;
  `texel >> 1` maxes at 0x7f; `m_lumaram` is 0x8000. It fits with nothing to spare, so an off-by-one
  in the shift reads out of bounds rather than wrapping harmlessly.
- **The texel index is 8-bit but `lumaram` is indexed by 7 bits.** `t >> 1` — the comment in
  `draw_scanline_tex` says why: the filtered texel has 8 bits of precision and the translator map has
  128 entries. Do not "fix" the shift.
- **Virtua Striker's `min(luma, 0x3f)` clamp is load-bearing.** It is not defensive coding; national
  flags on the bleachers set a luma of 0x40 and index past the ramp without it.
- **`vkCmdSetDepthBias` is available (`depthBiasClamp` is present) and must not be used in P3.** It is
  P4's tool. Reaching for it to fix something in P3 means the draw-order depth is being undermined
  rather than debugged.
- **Do not use `VK_KHR_dynamic_rendering` or `VK_KHR_synchronization2`.** Both would simplify the
  pass setup considerably, both are present on the device, and neither is reachable — we did not
  create the device and cannot know what the frontend enabled ([vulkan-target.md](vulkan-target.md)).
  Render passes and `VkFramebuffer`, core 1.1, until negotiation lands.
- **The queue belongs to the frontend.** Every `vkQueueSubmit` between `lock_queue`/`unlock_queue`,
  and `vkDeviceWaitIdle` counts as one. P2's rule, and P3 adds more submits to forget it on.
- **The ring is 3 images and can be rebuilt mid-run.** Anything P3 allocates per sync index has to
  survive the same `context_destroy`/`context_reset`/mask-change paths P2 exercised, and
  `retrohost --vk`'s `M2VK_HOST_*` knobs script all three without RetroArch.
- **Primitive restart cannot be disabled on this implementation.** MoltenVK logs
  `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart` once per pipeline
  creation, and has since P2. Harmless for the non-indexed fullscreen triangles — but **step 3 brings
  indexed draws**, and an index value of `0xffff` (or `0xffffffff` for 32-bit indices) will restart the
  primitive whether or not the pipeline asked for it.
- **The polytap always reports exactly one more rendered frame than the record delivers**, and it is
  not a dropped frame. It is the shutdown timeslice: `retro_unload_game` sets the exit flag,
  `schedule_exit()` only takes effect at the end of the timeslice, and the OSD's `update()` runs
  several more times on the way out without parking on the baton — one of which does a full
  `screen_update`. Confirmed by ordering in the log: the extra `[polytap] frame N` is emitted **after**
  `ring of N destroyed`, so it was rendered after the renderer had already been torn down. Compare the
  two sequences over the record's length, not the tap's.
- **RSS is not flat on any path, and it never was.** Measured at step 2 with `retrohost`'s new
  `M2VK_HOST_RSS=<n>`, over vf2 2500 frames: `renderer=software` 160.4 -> 178.4 MiB,
  `renderer=vulkan` at step 1 175.6 -> 205.3, at step 2 175.6 -> **202.7**. Step 2 ends *below* step 1,
  so the record costs nothing measurable — but all three columns share a ~25 MiB drift with the same
  shape and the same inflection at frame 1500, **including `renderer=software`, where none of this
  code runs.** It predates P3. Do not rediscover it at step 6 and blame the geometry path; if it is
  ever worth chasing, the check that isolates it is the software column.
- **`M2VK_POLYTAP_DUMP=N` counts rendered frames, not host frames** — vf2 renders nothing for ~990
  frames, so rendered frame 800 is around host frame 1790. Already cost a session once.
- **`retrohost` writes NVRAM to `./retrohost-save`.** `rm -rf` it before any fixture run or credits
  carry over and the run is not reproducible. **And two harness scripts running at once will fight
  over it** — found at step 4, where a background A/B and a background single-polygon sweep were in
  flight together and one run's `renderer=software` reference came out 17127 pixels different from
  every other run of the same command. It reads as a renderer bug and it is a shared file. **Give
  every run its own `M2_SAVE_DIR`** (retrohost has honoured it since P1) rather than relying on not
  overlapping.
- **`ppmdiff.py coverage` cannot tell "drew black" from "did not draw", and dynamcop is where it
  bites.** The caveat is in the tool's own help and step 4 is the first time it mattered: 72 of
  dynamcop's pixels report as coverage disagreements, three of them as *interior* — which the tool
  calls a real bug — and every one is a pixel over a `(0,0,0)` background where one renderer produced
  `(0,0,0)` and the other `(2,0,0)`. Both drew it. **Before believing an interior coverage
  disagreement, print the three colours**; if the background is black and one side matches it exactly,
  it is a two-level colour difference wearing a coverage difference's clothes. `M2VK_FORCE_SOLID=2`
  is the cross-check, because flat shading cannot land on the background by accident.
- **A fixture whose last frame has no 3D measures nothing, and the tool will not tell you.** `coverage
  by A = 0` with a plausible-looking SSIM is what that reads like. `lastbrnx` at 2500 frames and
  `vstriker` at 2500 are both on 2D-only screens; 2300 and 1500 respectively put them back on
  geometry. Check `covered px` before reading any other number in a row.
- **`M2VK_ONLY_POLY=<n>` names a polygon in the run's LAST rendered frame**, because that is the
  frame retrohost writes to the PPM — not in whatever frame a `M2VK_POLYTAP_DUMP` file happens to
  hold. Picking a big polygon out of the frame-800 dump and asking for it over a 1900-frame run
  selects a *different* polygon and usually a tiny one; the frame-800 dump's `seq=568` covers 52783
  pixels and the same seq at the last frame covered six. Dump the run's last rendered frame (the
  polytap summary's `frames=` minus one) and take the seq from there.

## Risks

- **The transliteration is long and every line of it is a place to be subtly wrong.**
  `fetch_bilinear_texel` alone has the mirror test, the half-texel bias, the fractional extraction,
  four wrapped fetches, the non-wrap edge fudge, the translucent neighbour rule at three stages, and
  two `LERP`s on packed lanes. This is why step 3 builds the single-polygon A/B mode: whole-frame
  SSIM will say "0.91" and nothing about which of those twelve things is broken.
- **Nearest-equivalent output is not achievable and the temptation is to chase it.** The exit
  criterion is deliberately split into an exact part (the 2D layers) and a soft part (the 3D). Time
  spent driving SSIM from 0.97 to 0.99 in P3 is time spent on differences that P4 will change again
  and P5 will make irrelevant.
- **Skipping the software rasterizer changes MAME's timing.** `renderer=vulkan` will now run the
  emulation thread substantially faster than `renderer=software`, and anything that was accidentally
  depending on frame pacing — audio buffering, the baton, the geometrizer's `m_render_done` race —
  gets a different shape of load for the first time. If something intermittent appears in P3, check
  whether it also reproduces with `M2VK` forcing the rasterizer back on; that isolates timing from
  rendering the same way "does it reproduce on the other renderer" isolates the environment.
- **The `m_render_done` dupe path is the most likely source of a wrong-looking but plausible frame.**
  A stale 3D layer composited under fresh 2D layers looks like a rendering bug and is a lifecycle bug.
- **Texture RAM upload volume is unmeasured.** 2 MB per changed frame is a guess at acceptable, not a
  measurement. If it hurts, the answer is a dirty-range check on the write handlers, not an atlas —
  but do not build the dirty tracking before measuring that it is needed.
