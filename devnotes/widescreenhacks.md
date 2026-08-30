# Widescreen hacks (16:9) — System 22 / Model 2

Notes on what a 16:9 widescreen mode (e.g. Rave Racer) would take, and a recommendation.
Widescreen/FOV-widening is on the Model 2 **do-not-re-propose** list; this file records *why*
so the reasoning doesn't get re-derived, and notes the one thing that differs for S22.

## Where projection happens (S22)

At the seam, `poly3d_drawquad` ([namcos22_v.cpp:321-330](../src/mame/namco/namcos22_v.cpp#L321-L330))
projects with a bare pinhole divide:

```
screen_x = cx + Xview * (1/z)      cx = 320 + vx
screen_y = cy - Yview * (1/z)      cy = 240 + vy
```

- There is **no separate FOV/scale term** — the focal length is baked into `Xview`, which the
  emulated geometry engine (`simulate_slavedsp`, the slave-DSP sim) has already produced in eye space.
- For the **non-direct** case the **eye-space vertex (x/y/z before the divide) is available at the
  seam**. So we own everything needed to re-project — no reaching into the DSP required. We already
  own the vertex shader (`s22.vert`) and `s22::submit_quad`.

## The easy part — Hor+ projection

To reveal more at the sides while keeping vertical framing, compress x anamorphically:

```
screen_x = W/2 + Xview*(1/z) * (640 / W_wide)
```

A few lines in our own submit/shader path. (A plain anamorphic *stretch* of 4:3 → 16:9 is trivial and
looks wrong — wrong aspect on everything. The Hor+ factor above is the correct version and is barely
harder.)

## The three things that make it actually hard

1. **Missing side geometry / pop-in — the real blocker.** The game frustum-culls **upstream**, in the
   emulated geometry engine, for a 4:3 field. Objects entirely off the sides were never emitted as
   scene nodes at all. Widening the projection reveals empty space and pop-in at the new edges, and
   there is **nothing at the seam to un-cull** — the cull already happened in hardware we're faithfully
   simulating. Driving games (Rave Racer: distant roadside geometry, cars entering at the edges) show
   this worst. **Not fixable at our seam.** This is exactly why widescreen was shelved for Model 2.

2. **2D vs 3D separation.** The `direct` path ([namcos22_v.cpp:334](../src/mame/namco/namcos22_v.cpp#L334))
   and `render_sprite` are already-projected screen-space quads — HUD, some backgrounds, billboards.
   These must be **pinned/centered, not widened**, or the dashboard and HUD stretch. Tractable: the
   direct / non-direct / sprite distinction already exists in the code, so widen only true 3D.

3. **Per-quad scene-clip rectangles.** Every quad carries `vl/vr/vu/vd` + `vx/vy` (the bb0003 viewport)
   in native 640-space, and `m_cliprect` is clamped to `screen.visible_area()`
   ([namcos22_v.cpp:301](../src/mame/namco/namcos22_v.cpp#L301)). Left unscaled they re-crop the widened
   geometry back to 4:3. Each must be rescaled — and split/mirror viewports (dashboard, rear-view)
   each carry their own, so it's per-viewport bookkeeping.

## Difficulty tiers

| Tier | Result | Effort | Verdict |
|---|---|---|---|
| Anamorphic stretch | 4:3 content stretched to fill 16:9 | an afternoon | Wrong aspect, looks bad. Not worth shipping. |
| True Hor+ | Projection factor + clip-rect rescale + HUD/sprite pinning + per-viewport handling | ~a few days | Works, but **permanent pop-in / empty edges** at the sides — can't un-cull. |
| Clean widescreen | Widen the game's *own* frustum in the DSP sim so it emits side geometry, and defeat the game CPU's object-level culling | per-game research project | Brittle, partial at best. Effectively RE per title. |

The projection is genuinely easy (we have eye-space verts and own the shader). The difficulty is
entirely the culling: the sides are empty because the geometry was never submitted, and that decision
lives upstream of anything we can touch. Same wall for Model 2 and S22.

## Recommendation

- **Don't ship tier 1 or tier 3.** Tier 1 is just a wrong-aspect stretch; tier 3 is unbounded per-game
  RE with brittle results.
- **Tier 2 is the only interesting option, and only worth it if the edge pop-in is acceptable for a
  given game** — which is an empirical question, not a design one. It varies wildly by title (a tunnel
  or a game with a low horizon hides the void; open desert road shows it).
- **Next step if pursued:** prototype the tier-2 Hor+ path on `raverace` behind a debug switch
  (e.g. `M2VK_WIDE=<w>`), widening only the non-direct 3D path, and screenshot the new edges. That's
  the cheapest way to judge whether the pop-in is tolerable before investing in clip-rect rescale and
  HUD pinning. If the edges look bad on a road game, the feature stays shelved with evidence.
- Keep this behind a per-game/opt-in flag if it ever ships — it is a "hack", not accuracy, and the
  default must stay the native 4:3 the games were designed for.
