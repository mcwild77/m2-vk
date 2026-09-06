# zfighting.md — the System 22 depth-buffer experiment (SHELVED, did not work)

**Status: removed before release. Option gone from the menu, renderer code left dormant.**
Date: 2026-08-25.

## What it was trying to do

System 22 has **no depth buffer**. The hardware is a sorting rasteriser: it assigns one depth per
polygon, sorts the list, and paints back to front (the painter's algorithm). Our S22 renderer
reproduces that exactly — it draws in the hardware's list order, last writer wins. The visible cost
is **z-fighting** where two surfaces genuinely cross through each other (the classic case is the road
surface in Ridge Racer / Rave Racer), because the single per-polygon depth cannot decide the crossing
and the two polys flicker over which is in front.

The experiment added an optional **real per-pixel depth buffer** (`system22_depth_buffer` / "Depth
Buffer (3D)", switch `M2VK_S22_DEPTH`): a `GREATER_OR_EQUAL` depth test, depth writes on, and the
frame's 1/z (`ooz`) linearly remapped onto `[0,1]` (nearest → 1.0). Framed as an enhancement in the
same class as blended transparency — not accuracy.

## Why it does not work

**A per-pixel depth buffer is fundamentally incompatible with System 22's draw order.** The hardware's
list order is *not* monotonic in z — the game deliberately draws some far things after near things
(overlays, insets, direct 2D primitives). A depth buffer assumes draw-order independence, which is
false here, so `GREATER_OR_EQUAL` silently reorders content the game layered on purpose.

Concrete failures seen in Rave Racer (in-race, after coin + start):

- **Ground texture / UV corruption.** The full-frame `ooz` range spans a huge dynamic range
  (measured `~5e-7 .. ~100000` in one frame — a real near polygon plus 2D sentinel values). Remapping
  that whole range onto `[0,1]` collapses the entire visible world into a razor-thin z band near 0, so
  the depth test degenerates into near-universal ties and the road/ground surfaces resolve
  inconsistently — the ground shows texture and UV breakup rather than the clean painter's result.
- **The UI / insets go black.** The rear-view mirror and the player-car / damage inset are drawn
  **after** the full-screen world, which has already written near-z into those pixels; the inset
  content (farther) then fails the `GREATER` test and only black shows. With `depth on` the mirror and
  the player's car became solid black rectangles.
- **Direct 2D primitives blocked / mis-ordered.** System 22 `direct` quads (pre-projected 2D — HUD
  backings, mirror frames) carry a sentinel 1/z and are meant to be composited purely by list order.
  Depth-testing them makes a backing quad drawn early reject the 3D behind it, or a 2D element land in
  the wrong layer.

## What was tried to rescue it (and why each failed)

1. **Per-batch depth clear + per-batch `ooz` remap** — treat each scissor window (inset) as its own
   painter's layer. Fixed nothing on its own: the black rectangles remained, because the inset content
   itself has non-monotonic order (a near backing quad drawn before its farther content).
2. **A second, no-depth pipeline for `direct` quads and sprites** (draw_batch split on a `nodepth`
   flag; those primitives neither test nor write depth, compositing by order). This fixed the *insets*
   in isolation — but combined with the per-batch clear it **blacked out the entire world**, because a
   full-screen backdrop quad that must stay *behind* the world (kept there by the shared depth buffer)
   was defeated the moment its rectangle's depth was cleared.
3. **Global remap, no clear, + the no-depth split.** The closest it got: one screen (the Rave Racer
   race view captured in retrohost) rendered correctly and depth-off stayed byte-identical. But it
   relied on the world/backdrop/inset z ranges happening to line up, and it did **not** hold up in the
   app — the ground corruption and UI breakage persisted in real play. It is not shippable, and the
   whole "different cameras sharing one depth space" model is unsound.

The root problem is not any one of these details — it is that the depth buffer is the wrong tool for a
sorting rasteriser whose draw order encodes layering the z values do not.

## What shipped instead

- **The menu option is removed** (`retro_options.cpp` — the `KEY_S22_DEPTH_BUFFER` entry is deleted;
  a note stands in its place). It is not shown by any family's menu.
- **The renderer code is dormant, not deleted.** `s22::depth_enabled()` (in `s22_geom.cpp`) is
  **forced to return `false`**, so `build_pipeline()` never builds the depth pipeline, the no-depth
  companion (`s_pipeline_nodepth`) is never built, and the draw path is the untouched painter's
  algorithm. The dormant pieces that remain in the file, should anyone revisit this:
  `s_pipeline_nodepth`, the `draw_batch::nodepth` split in `geom_build`, the `ooz` remap in the push
  block, and the `M2VK_S22_BATCHDUMP` diagnostic. The old `M2VK_S22_DEPTH` switch is now inert.
- **`retro_entry.cpp`** no longer parks or logs the option, and no longer hides it per family (nothing
  to hide).

Verified: Rave Racer renders **byte-identical to the pre-experiment painter's path**
(`digest 97960b0e41e4db82` over 2400 frames), with the old `M2VK_S22_DEPTH=1` switch set or unset —
depth is fully off and cannot be turned on.

## If this is ever revisited

Do not re-enable the menu entry as-is. A workable approach would have to respect the hardware's
layering rather than override it — e.g. depth used **only** to break ties within a single coherent
sorted sub-list, with 2D/direct/inset layers kept strictly on the painter's path and never sharing a
z space with the main scene. That is a real design problem, not a flag flip. Related context:
[s2-gpu-geometry.md](s2-gpu-geometry.md) (S22 as-built, painter's depth) and
[p4-depth-and-decals.md](p4-depth-and-decals.md) (the Model 2 finding that draw order never disagrees
with real depth there — the opposite conclusion, for a different reason).
