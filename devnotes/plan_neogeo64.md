# plan_neogeo64 — SNK Hyper NeoGeo 64 as a Modelizer family

**STATUS: PARKED, do not start.** Blocked on *upstream*, not on us. HNG64's 3D geometry is still
being reverse-engineered; all seven games are `MACHINE_IMPERFECT_GRAPHICS`. Accelerating it now means
GPU-reproducing a pipeline that upstream is actively rewriting — the A/B ground truth moves under us
every monthly sync. **Revisit only when the driver's 3D is understood well enough that upstream drops
`IMPERFECT_GRAPHICS` (or the geometry/DSP notes stop churning).** This file is the standing scope so a
future session doesn't re-survey from scratch.

## Why it's a genuine candidate (when the time comes)
Architecturally the *cleanest* fit after the three shipped families — same MAME rasterizer primitive,
same seam shape as Model 2 / S21.

- **Hardware:** SNK 1997. NEC VR4300 (MIPS R4300, the N64 CPU) main, KL5C80 I/O, **TMS320C52 DSP** for
  transform/geometry, feeding a custom **z-buffered** polygon rasterizer + a 2D tilemap/sprite layer.
- **Games (7):** `sams64`, `sams64_2`, `fatfurwa`, `buriki` (`hng64_fight`); `roadedge`, `xrally`
  (`hng64_drive`); `bbust2` (`hng64_shoot`). BIOS root `hng64`. Driver:
  [src/mame/snk/hng64.cpp](../src/mame/snk/hng64.cpp), 3D in
  [hng64_3d.ipp](../src/mame/snk/hng64_3d.ipp), video in [hng64_v.cpp](../src/mame/snk/hng64_v.cpp).
  Licence **LGPL-2.1+** (Haywood/Salese/ElSemi/Gardner/Zaferakis) — same class as the LGPL files already
  cleared in `legalstuff.md`.

- **The seam:** [`hng64_poly_renderer::drawShaded(polygon *p)`](../src/mame/snk/hng64_3d.ipp#L1445) —
  the direct analogue of `model2_3d_render(polygon*)`. Everything below is already resolved:
  - `polygon` / `hng64_poly_data` ([hng64.h:39](../src/mame/snk/hng64.h#L39),
    [:84](../src/mame/snk/hng64.h#L84)) carry `texIndex`, `palOffset`, `tex4bpp`, `texPageSmall`,
    `blend`, `texscroll{x,y}`, `tex_mask_{x,y}` — the `m2_poly_extra_data` role.
  - `polyVert` gives homogeneous `clipCoords[4]` (XYZW), OpenGL-style `texCoords`, `normal[4]`, and a
    per-vertex scalar `light`. GPU-ready; no PS1 affine warp to unpick.
  - Built on `poly_manager<float, hng64_poly_data, 7>` — the primitive we've replaced three times.
  - **Real float z-buffer** (`m_depthBuffer3d`, 512×512) → maps to Vulkan depth test directly, like the
    **System 21** path, unlike Model 2 / S22 draw-order.

## The two things that make it unlike the shipped ports
1. **The 3D layer is a *mix input*, not the final frame.** The rasterizer writes a `u16` scanline
   `colorBuffer3d` that the final-mix in [hng64_v.cpp](../src/mame/snk/hng64_v.cpp#L899) composites
   against the 2D layers ("top/bottom" 2-way mix, tcram-controlled). So the seam **must resolve the GPU
   result back into a CPU-readable `colorBuffer3d`** (or lift the whole final-mix onto the GPU too —
   bigger job). This is the S21/S22 "2D-over compositing" problem but mandatory, not optional. Interlace
   means the 3D buffer can be half screen height.
2. **Perspective-correctness is unconfirmed.** `drawShaded` even flags it:
   *"very good chance the HNG64 hardware does not do perspective-correct texture-mapping — explore."*
   The `1/w` divide is applied speculatively. Whatever the seam does here has to match the software path
   bit-for-bit under `M2VK_SW_3D` — and that path itself may change upstream. Do **not** silently "fix"
   it to perspective-correct.

## Sketch of the port (T-style, only after unpark)
- **N0 boot** — get a family into Modelizer's runtime detect (driver_list, per
  [shared-osd-objdir-across-subtargets]); the four `hng64_*` machine configs vs the BIOS root.
- **N1 seam** — hook `drawShaded`, stream `polygon`+`hng64_poly_data` across, keep MAME's rasterizer as
  the `M2VK_SW_3D` reference. Nothing on the GPU yet.
- **N2 geometry** — untextured first (flat-shade path is a separate `render_flat_scanline`; do it
  first, it's simpler), real z-buffer, then textured with the HNG64 texture-page / 4bpp / palOffset
  decode (needs a Polydiver-style executable decoder — the actual work, as always).
- **N3 composite** — resolve GPU color back into `colorBuffer3d` so the existing final-mix is untouched
  (the low-risk route). Only consider a GPU final-mix if the readback is a measured bottleneck.
- **N4 routing/options/savestates/pads** — reuse `model2_flat_luma` ("No Lighting"), the hi-res option,
  per-game pads; A/B the whole set.

## Decision rule
Check at each monthly sync: `git log upstream --oneline -- src/mame/snk/hng64*` since last look, and
whether any HNG64 game has shed `MACHINE_IMPERFECT_GRAPHICS`. Churn in `recoverPolygonBlock` / the DSP
program = still too early. Quiet + flag dropped = green-light N0.
