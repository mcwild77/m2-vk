# The interception seam

**Status: verified at mame0288 (2026-07-25). The seam works; P0 is done.**

## The decision (from `../Polydiver/PDDocs/model2/model2_libretro_core.md`)
Tap `model2_renderer::model2_3d_render(polygon*, …)` in `src/mame/sega/model2_v.cpp`. By that
point each polygon's texture and lighting parameters are already resolved into
`m2_poly_extra_data` (`src/mame/sega/model2.h`), and Model 2 is perspective-correct (`pz = 1/z`),
so there is no PS1-style affine warp to undo. This is the seam — **not** the OSD blit.

## Location at mame0288

| Thing | Where |
| --- | --- |
| `model2_renderer::model2_3d_render(polygon *poly, const rectangle &cliprect)` | `src/mame/sega/model2_v.cpp:565` (was ~611 in the older reference tree) |
| Software rasterizer dispatch (`render_triangle` / `render_polygon`) | `model2_v.cpp:619-624` — the tap goes immediately above this `switch` |
| Frame loop that calls it | `model2_state::render_polygons()` — `model2_v.cpp:697` |
| Called from | `model2_state::screen_update()` — `model2_v.cpp:2421` |
| `m2_poly_extra_data` | `model2.h:635` |
| `model2_state::polygon` | `model2.h:734` |
| `class model2_renderer : public poly_manager<float, m2_poly_extra_data, 4>` | `model2.h:659` |
| Scanline shaders (the shading model to port to GLSL) | `src/mame/sega/model2rd.ipp` — `draw_scanline_solid`, `draw_scanline_tex`, `fetch_bilinear_texel` |

Note the signature takes `polygon *` (mutable) — `model2_3d_render` itself rewrites `v[i].pz/pu/pv`
in place for textured polys (see below), so the tap must run *after* that loop to see what the
rasterizer sees.

## Data shape at the call site

### Vertices — `poly_vertex v[8]`
`poly_vertex` is `poly_manager<float, m2_poly_extra_data, 4>::vertex_t` = `{ float x, y;
std::array<float,4> p; }`. `model2_v.cpp:102-104` aliases the params:

```
#define pz  p[0]
#define pu  p[1]
#define pv  p[2]
```

`p[3]` is unused by Model 2 (the `4` is just the `MaxParams` the template was instantiated with;
the rasterizer is dispatched with `ParamCount = 3`).

Vertex state on entry to the tap:

- `x`, `y` — **screen space already**. `model2_3d_project()` (`model2_v.cpp:662`) ran immediately
  before the call from `render_polygons`, applying `x = crtc_xoffset + center[0] + x/z` and
  `y = ((384 - center[1]) + crtc_yoffset) - y/z`. So the perspective divide is *done*; the
  Vulkan path receives 2D screen positions, not clip-space coords.
- `p[0]` — for **textured** polys (`renderer & 2`) it has been replaced by `1/(z+ε)`, and
  `p[1] p[2]` by `u·(1/z)/8`, `v·(1/z)/8` (`model2_v.cpp:605-610`). For **untextured** polys the
  loop is skipped, so `p[0]` is still raw view-space `z` and `p[1] p[2]` are meaningless.
  ⚠️ Consequence for the Vulkan port: depth for solid polys has to come from `p[0]` directly (or
  from `poly->z`, the sort bucket), not from a uniform "always 1/z" assumption.
- The verts are **already frustum-clipped** against `raster->clip_plane[center_sel][0..3]`
  (`model2_v.cpp:468-473`) — hence up to 8 vertices from an input tri/quad. They arrive as a
  convex fan; `render_polygon<N,3>` fans them from `v[0]`.

### Per-polygon — `model2_state::polygon`
`next` (bucket list link), `v[8]`, `num_vertices` (3…8), `z` (16-bit sort bucket),
`texheader[4]`, `luma`, `texlod`, `viewport[4]`, `center[2]`, `window`.

### Resolved parameters — `m2_poly_extra_data`
Filled in at the top of `model2_3d_render` (`model2_v.cpp:576-603`) — decode we do **not** have to
redo:

- always: `state`, `checker` (hdr0 b15 — the 50%-stipple "translucency"), `lumabase`
  (`(hdr1 & 0xff) << 7`), `colorbase` (`(hdr3 >> 6) & 0x3ff`), `luma`, `texlod`
- textured only: `texsheet[2]` (pointers into `m_textureram0/1`, selected by hdr2 b12),
  `texwidth`/`texheight` (`32 << hdr0[2:0]` / `32 << hdr0[5:3]`), `texx`/`texy`,
  `texwrapx/y`, `texmirrorx/y` (mirroring disables smooth wrap), `utex`, `utexminlod`,
  `utexx`, `utexy`

Renderer class is `(texheader[0] >> 13) & 3` → bit0 = translucent, bit1 = textured, indexing
`m_render_callbacks[4]` = {solid, solid+translucent, tex, tex+translucent}. Note
`draw_scanline_solid<true>` returns immediately — **untextured translucent polys draw nothing** at
all in the SW renderer. The Vulkan path must reproduce that, or it will paint geometry the real
hardware never shows.

### Viewport / clip
`vp` is computed in the function as
`rectangle(viewport[0]+m_xoffs, viewport[2]+m_xoffs, (384-viewport[3])+m_yoffs, (384-viewport[1])+m_yoffs)`
intersected with `cliprect`. `m_xoffs`/`m_yoffs` come from the CRTC (`horizontal_sync_w` /
`vertical_sync_w`, defaults 90 / -8). Per-poly scissor in Vulkan terms; the tap records both the
raw `viewport[4]` and the final clipped rect.

## Draw order — what the Vulkan depth-bias key must be

`render_polygons` (`model2_v.cpp:697`) walks:

1. `for window = raster->cur_window down to 0`
2. `for z = raster->min_z up to raster->max_z` (bucket index ascending)
3. the bucket's linked list, head first

and the SW renderer draws **front-to-back with an occlusion mask** (`m_fillmap`: a pixel is
written only if `fill[x] == 0`, then marked). So earlier submission in this walk = wins the pixel.
Two consequences:

- Bucket lists are built by **prepend** (`poly_sorted_list[z] = poly; poly->next = old head`,
  `model2_v.cpp:521-522`) → **within a bucket, polys draw in reverse submission order**. Do not
  assume display-list order equals draw order; the tap's `seq` field is the real draw order and is
  the value to key depth bias on.
- Later windows are drawn *first* so they end up on top (matches the header comment at
  `model2_v.cpp:45`) — inverted relative to what "later = on top" suggests.

Sort bucket `z` is `float_to_zval(zvalue, raster->z_adjust)` where `zvalue` is per-poly min z, max
z, or *the previous polygon's* z (attr bits 11:10 — `model2_v.cpp:438-455`). That "reuse previous
z" mode is exactly the decal/coplanar case: the game is explicitly saying "same depth as the last
poly, resolve by draw order". Strong hint for P4: **when consecutive polys share a bucket, bias
rather than depth-test.**

## The shading model (to port to GLSL in P3)
`model2rd.ipp` is short and complete; it is the reference:

- texel: 4-bit value from `texsheet[miplevel & 1]`, bilinear-filtered, `<<4` → 8-bit "luma index";
  mip level from `-texlod + fast_log2(z)`, trilinear between two levels; microtexture is a
  128×128 sheet blended in when `mml < 0`
- luma: `lumaram[lumabase + (texel >> 1)] * poly.luma / 256`, clamped to `0x3f`
- colour: `colorbase → m_palram[colorbase + 0x1000]` → 5:5:5 → three 256-entry slices of
  `m_colorxlat` indexed by the 6-bit luma → `m_gamma_table`
- translucent textured: alpha comes from the texel (`0xf` = transparent), `< 50%` is discarded —
  i.e. a **cutout**, not a blend

This matches `../Polydiver/PDDocs/model2/model2_lighting.md` (luma → colorxlat LUT).

## The instrumentation

- `src/osd/libretro_m2/m2vk_sink.h` — the sink: the polygon snapshot type (`m2vk::poly`), the
  `consumer` interface, and the conversion from `model2_state::polygon` + `m2_poly_extra_data` into
  the snapshot. Everything the seam sees.
- `src/osd/libretro_m2/m2vk_sink.cpp` — the sink object and the dispatch. One object with static
  storage duration, so it exists whether or not the game renders anything.
- `src/osd/libretro_m2/m2vk_polytap.h` — the diagnostic tap, now one consumer behind the sink.
  Reports per-frame poly counts by renderer class, vertex histogram, screen-space / `1/z` / bucket
  ranges, window count, submitting-thread stability, a run-level feature survey; optionally dumps
  every polygon of one frame as text.
- `src/mame/sega/model2_v.cpp` — **four `#ifdef M2VK` blocks only**: the include, a `frame_begin()`
  at the top of the `render_polygons` window loop, a `submit()` above the rasterizer `switch`, and a
  `frame_end()` after `wait("End of frame")`. 16 lines; that is the whole upstream diff.

Build and run:

```sh
make SUBTARGET=model2 OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10   # the core
make SUBTARGET=model2 OSD=sdl3 REGENIE=1 NOWERROR=1 -j10          # the standalone binary

M2VK_POLYTAP_EVERY=100 M2VK_POLYTAP_DUMP=500 \
  ./mamemodel2 vf2 -rompath devnotes/roms -video none -window -nomaximize -nothrottle -skip_gameinfo -str 30
```

The three points where this has changed since P0, all on 2026-07-25:

- the guard is `M2VK`, set by `scripts/target/mame/model2.lua` rather than by `ARCHOPTS_CXX`, and
  the standalone binary is `./mamemodel2` — see [p1-libretro-core.md](p1-libretro-core.md) for why,
  and for the `REGENIE=1` trap;
- the files moved out of `src/mame/sega/` (step 6 of P1) and the tap now sees `m2vk::poly` rather
  than the driver's own structures;
- **the tap is off unless an `M2VK_POLYTAP*` variable is set.** It used to be on in any `M2VK` build.
  `M2VK_POLYTAP=1` is enough; setting `_EVERY`, `_DUMP`, `_DUMP_FILE`, `_SUMMARY` or `_TAG` also
  attaches it, so the sweep scripts are unaffected. `M2VK_POLYTAP=0` forces it off.

## Runtime — VF2 attract mode, measured 2026-07-25

Ten sampled frames from a 30-second run (`vf2`, attract mode, mame0288 + tap):

```
frame 100  polys  853/ 853 sent  tex 675/177 trans  solid 1/0  verts 3:210 4:639 5:4  1/z 0.033..4.76   bucket 4137..16193  windows 8
frame 300  polys  554/ 554 sent  tex 504/ 49 trans  solid 1/0  verts 3: 99 4:448 5:7  1/z 0.038..2.07   bucket 1926..15151  windows 8
frame 400  polys 1447/1447 sent  tex 1255/192 trans solid 0/0  verts 3:266 4:1179 5:2 1/z 0.038..0.55   bucket 4426..14907  windows 8
frame 500  polys  552/ 552 sent  tex 470/ 81 trans  solid 1/0  verts 3: 90 4:452 5:10 1/z 0.035..2.24   bucket 1920..15443  windows 8
frame 600  polys  836/ 836 sent  tex 665/168 trans  solid 3/0  verts 3:201 4:632 5:3  1/z 0.032..30.24  bucket 3893..16418  windows 8
```

Answers to the questions this spike existed to settle:

1. **The tap sees the whole stream.** Tapped count == `raster->poly_list_index` on every frame, with
   no exceptions across thousands of frames. Nothing is culled, skipped or re-ordered between the
   geometry engine's list and the seam.
2. **Submission is single-threaded.** The thread-identity check never tripped. `poly_manager` farms
   *scanlines* out to workers, but `model2_3d_render` is only ever called from the emulation thread,
   so the Vulkan path can build command buffers at the seam without synchronisation.
3. **Budget: 550–1450 polys/frame** in VF2 attract mode at 30 Hz geometry. Trivial for Vulkan —
   the work is correctness, not throughput.
4. **Geometry is quads.** ~80 % 4-vertex, ~20 % 3-vertex, a handful of 5-vertex (clipped); nothing
   above 5 in this scene. Index-buffer strategy: fan every poly to `n-2` triangles.
5. **Almost everything is textured.** 470/552 textured opaque, 81 textured translucent, exactly
   **1** untextured poly. The `solid` path barely matters for fidelity, and untextured *translucent*
   (which the SW renderer discards entirely) never appeared.
6. **Runs are bit-repeatable.** Independent runs produced identical poly counts and identical `1/z`
   / bucket ranges frame-for-frame. The A/B harness can rely on frame numbers as fixtures.
7. **Headless works** — plainly, with no recording flag (see below).

### Headless: `-video none` works; give it enough emulated seconds, and pass `-window`
Two traps here, and an earlier version of this note got the first one wrong.

- **`-video none` renders normally.** An initial claim that it suppressed `screen_update` (and that
  `-aviwrite` was needed to force rendering) was **wrong** — it came from two runs that were merely
  too short, with the duration and the recording flag changed at the same time. Measured properly,
  VF2 does not render its first 3D frame until **~16 emulated seconds** (`-str 8` and `-str 12`
  produce zero rendered frames; `-str 16` produces one; `-str 20` produces 164). Any fixture shorter
  than that sees nothing, whatever the video option.
- **Pass `-window`.** `window` defaults to `0` — "otherwise, full screen mode is assumed"
  (`src/osd/modules/lib/osdobj_common.cpp:74`) — and `-video none` still creates a window, because
  `renderer_none` is a renderer *for* a window (`src/osd/modules/render/drawnone.cpp:25`). So
  `-video none` on its own blanks the whole display for the duration of the run. Stock MAME's SDL OSD
  has no true windowless mode; that is one of the things P1's custom OSD removes.

So the headless recipe needs no recording at all, and runs at 420–460 % speed with zero disk writes:

```sh
./mamemodel2 vf2 -rompath devnotes/roms -video none -window -nomaximize -nothrottle -skip_gameinfo -str 30
```

(`-aviwrite` writes ~860 MB for 26 s and `-mngwrite` 70–177 MB for 40 s. Neither is needed. Note
`-mngwrite /dev/null` fails with "Operation not permitted" yet rendering continues regardless —
further evidence recording has nothing to do with it.)

### One frame, analysed — `devnotes/fixtures/vf2-frame500-polytap.txt`
552 polys, and the numbers that matter for P4:

- **86 % of polygons (476/552) share a sort bucket with an adjacent polygon** in draw order; run
  lengths reach 11. Only 213 distinct buckets for 552 polys. Coplanar ties are not an edge case in
  Model 2 — they are the *common* case, so the depth strategy must resolve ties by draw order by
  construction, not by z-precision luck.
- Buckets are non-decreasing within a window (0 violations) → the stream arrives pre-sorted
  front-to-back; the Vulkan path gets its sorted order for free.
- Draw order visited window **7 then window 1** (descending, as expected); other windows were empty.
- 51 distinct texture pages (`texpos`/`texsize`/sheet) in one frame → the texture cache needs ~51
  live pages per frame for this scene.
- `lumabase` took only 4 distinct values (0, 128, 256, 384); 21 distinct `colorbase`; `luma`
  63…255. Small LUT working set.
- No microtextures and no `checker` polys in this frame — both deferrable for a first VF2 target,
  but they will show up in other games/scenes.
- Single viewport / centre for the whole frame (`vp=0,127,496,511`, `center=248,319`,
  clip `0,0,495,383`) — per-poly scissor is needed for generality but is constant here.

### Relationship to Polydiver's TCP geometry stream
Worth being precise: they are **different levels of the pipeline**, so this is not a like-for-like
diff. `model2_stream.lua` streams ~107 *object draws* per frame (`oba` + a 12-float matrix) and
Unity does the transform itself; the tap emits the post-transform, post-clip, screen-space polygons
those objects expand into (552 here). Consistent in magnitude (~5 polys/object in a light attract
frame), and the per-poly texture/luma/colour fields are the same fields the Unity decoders derive —
but the tap's stream is strictly *later* and more resolved. That is the point of choosing this seam:
the transform and clip work is already done for us.

## Open questions this leaves for P1+
- Untextured polys keep raw `z` in `p[0]` (not `1/z`) — pick one depth convention in the vertex
  format and convert at submit time. Rare in practice (1 poly in 552) but it will bite.
- `checker` (50 % stipple) is a screen-door transparency the hardware does per-pixel; at enhanced
  internal resolution it will look wrong. Needs a decision (dither vs. real alpha) in P5.
- Vertices arrive already projected to screen space, so a Vulkan pass wanting real depth has to
  reconstruct it from `p[0]` — fine for a depth buffer, but it means the port cannot re-project at
  a different aspect ratio or FOV. Widescreen (P5) will have to intervene earlier than this seam.
- Fixtures must start late enough to contain 3D. VF2's first rendered frame is ~16 emulated seconds
  in; other games will differ, so each fixture needs its own measured floor.
