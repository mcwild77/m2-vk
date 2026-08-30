# System 22 plan — add Namco (Super) System 22 behind the Vulkan seam

High-level plan. Two tracks run in parallel: **A. the port** (new driver on the existing
architecture) and **B. the cleanup** (retire the monuments the Model 2 work left behind). Deliberately
lean — this document and the notes it spawns should stay short. If a section here grows past a screen,
that is the smell we are trying to avoid.

Anchor facts (not taken from prose):
- Upstream diff to mame0288 is **457 insertions / 16 deletions across 11 files** (measured 2026-08-25,
  after the S21 family landed; `git diff --shortstat mame0288 -- src/devices src/mame`). Every future
  diff-size statement quotes the real number or says nothing.
- OSD is ~26k lines across 27 new files; the mergeability discipline (all logic in new files, upstream
  touched only by guarded hooks) held and is what makes a second driver feasible.
- ROMs: `devnotes/roms/system22/` holds all 18 working parent sets (6 plain S22 + 12 SS22),
  audited complete 2026-08-22 — see `roms.md` "System 22 set" for the list and the offline audit
  (`devnotes/tools/audit-s22-roms.py`). SS22 dev targets `timecris` and `propcycl` (sprite-in-tree,
  §A.3) are now in hand.

---

## Guiding posture (read before adding anything)

- **One short worklog entry per real milestone**, not per thought. No corrections-of-corrections, no
  "SUPERSEDED" archaeology, no self-assessment adjectives.
- **Claims carry their measurement or don't get made.** No invented invariants ("costs nothing",
  "still N lines") that then go stale and get quoted as fact.
- **Notes are for the next session, not for the record's own sake.** When a fact stops being true,
  edit it in place or delete it — do not stack a warning on top.
- Model 2 is not touched by this work except where the two drivers can genuinely share a new file.

---

## Track A — the System 22 port

### What's the same as Model 2 (reuse directly)
- The libretro OSD shell, Vulkan device ownership, image ring, context lifecycle, savestate framework,
  A/B harness (`retrohost`, `ppmdiff.py`, `ab.sh`), and the padmap tooling.
- Perspective-correct 3D → no affine warp. Depth-as-draw-order maps cleanly: the driver already hands
  geometry back-to-front via its z-sort tree.

### What's different (the real work)
1. **Geometry arrives as a z-keyed radix tree, not a flat display list.** The tree walk in
   `namcos22_v.cpp` already emits primitives back-to-front — that ordering *is* the depth key, same
   role record-index played for Model 2.
2. **Two hardware variants in one driver** — System 22 and Super System 22 (different memory map, plus
   an extra 2D sprite layer and spot/fog on SS22). Effectively two shading paths
   (`renderscanline_poly` vs `renderscanline_poly_ss22`).
3. ~~**On SS22, sprites are z-interleaved with polygons in the same tree.**~~ **Done 2026-08-22.**
   Sprites join the polygon GPU stream in tree-walk order (painter's), so the interleave is exact. The
   coupled text-vs-3D question is solved with **draw order, not the priority buffer**: `capture_over`
   grabs all SS22 text as the OVER overlay (text over normal 3D), then a second GPU pass redraws the
   **prioverchar** primitives (poly `cmode&7==1`, sprite `cz==0xfe`) over the text (MAME's priority-7
   case). timecris/propcycl match software frame-aligned; see S2 below and `s2-gpu-geometry.md`. (The
   tokyowar letterbox was a *separate* missing per-quad scissor, fixed earlier.)
4. **Fuzzier accuracy ground truth.** The driver's own TODO calls several effects "not understood well"
   (spot, alpha layering, u/v 1px offset). MAME's rasterizer is still the A/B reference, expect a looser
   SSIM target than Model 2 on SS22. **But don't over-apply this** — the plain-S22 shading turned out to
   be ~bit-exact once the final gamma LUT was added (S2d); a real gamma bug had hidden behind a "coverage
   noise" hand-wave for two rounds. Measure the vk/sw brightness ratio (want 1.000) before blaming
   coverage.

### Seam (already located — see the audit answer in-session)
- Per-primitive tap: `poly3d_drawquad` (params resolved into `namcos22_object_data`) and
  `render_sprite`, both in `namcos22_v.cpp`.
- Frame brackets: inside `render_scene` (one function covers both `screen_update` variants) plus a
  `capture_over` hook at the end of each `screen_update`.
- Upstream footprint, **measured 2026-08-22 (letterbox fix)**: `namcos22_v.cpp` +100/−0; total vs
  `mame0288` 236/2 across 7 files (`git diff --shortstat mame0288 -- src/devices src/mame`). Everything
  else is new files.

### Phases (high level — detail each in its own short file only when started)
- ~~**S0 — Software core boots.**~~ **DONE 2026-08-22** (see worklog-archive).
  `ridgerac` renders 3D in software through the **shared** `libretro_m2` OSD (no sibling OSD needed).
  New subtarget: `namcos22.flt` + `namcos22.lua`; core renamed `<subtarget>_libretro` to avoid clobber.
  Software baseline digest `2678231fae7f3aa6` / 1800 frames; first 3D at ~frame 200.
- ~~**S1 — Seam + passthrough.**~~ **DONE 2026-08-22** (see worklog-archive).
  Three guarded `#ifdef S22VK` hook sites in `namcos22_v.cpp` (quad / sprite / frame brackets inside
  `render_scene`) tap the stream, record it, draw nothing; `ridgerac`'s digest is unmoved with the tap
  on. New `s22_seam.{h,cpp}`. **Upstream diff: namcos22_v.cpp +24/-0; total vs `mame0288` 159/2 across
  6 files.** Sprite hook wired but unexercised — all three ROMs are plain-S22 racers with no sprites.
- **S2 — GPU geometry, incrementally.** All detail in [s2-gpu-geometry.md](s2-gpu-geometry.md);
  `namcos22_v.cpp` +99/−0, total 235/2. Dev targets: `raverace`/`acedrive` (plain S22, heavy fog) and
  `tokyowar` (SS22, z-fog + mixer gamma). Steps:
  - ~~**S2a** — untextured.~~ **DONE.** Flat Gouraud-shaded quads, painter's order (tree back-to-front,
    depth test off). Seam moved to the shared OSD; Model 2 unaffected.
  - ~~**S2b** — texture tail.~~ **DONE.** Per-fragment `renderscanline_poly` fetch; static tile system
    uploaded once, palette re-uploaded per frame.
  - ~~**S2c** — 2D-over compositing (UNDER 2D → GPU 3D → OVER 2D).~~ **DONE for plain S22** (prival 2);
    the SS22 (prival 6) path was wired but untested — see the open item below.
  - ~~**S2d** — shading tail: poly fog, SS22 z-fog, poly-fade, screen-fade, poly-alpha, and the final
    **gamma LUT** (plain = PROM, SS22 = per-frame mixer RAM).~~ **DONE.** Direct fog, z-fog and gamma
    verified against software (raverace ~bit-exact; tokyowar ratio 1.000). Also fixed a latent GPU-path
    stack-overflow (`object_data` never reclaimed → SIGBUS ~frame 4000). Spot is a mixer-stage effect,
    not the poly tail — out of scope. Screen-fade/poly-alpha are wired and stable but only lightly
    exercised in attract → in-game hand-check (the user's).
  - ~~**SS22 2D compositing** (Tokyo Wars letterbox / sky bleed).~~ **Done 2026-08-22.** It was not the
    UNDER/OVER text sandwich — it was a **missing per-quad scissor** in the 3D pass. SS22 games window
    the 3D into a letterbox (`m_cliprect` = the scene viewport, `T112 B367` in tokyowar); the GPU pass
    ignored it so the sky bled into the black bars. Fixed by carrying the clip window across the seam and
    setting a per-run scissor scaled to the attachment, exactly as `vk_geom` does — the identical rect
    MAME clips to, so it can only match software better. tokyowar 70→**99.25 % exact**, ratio 1.000;
    plain S22 provably inert (`M2VK_NO_SCISSOR` == default on ridgerac). See `s2-gpu-geometry.md`
    "Per-quad scissor". The prival-6 UNDER/OVER split is a separate, still-open sprite question (§A.3).
  - ~~**Sprites** (`render_sprite`).~~ **Done 2026-08-22.** Per-tile hook at `poly3d_drawsprite`
    (`s22::submit_drawsprite`); sprite tiles join the polygon GPU stream in tree order, reusing the
    pipeline/vertex/scissor machinery with an `ATTR_SPRITE` fetch in `s22.frag` and one new static buffer
    (the `"sprite"` gfx region, binding 7). SS22 text-vs-3D rebuilt from draw order: capture all text
    (prival 4) as OVER, then `geom_draw_over` replays prioverchar primitives over it. timecris/propcycl
    match `M2VK_SW_3D=1` frame-aligned (0.11–0.71 % of pixels, ratio 1.000); plain S22 inert. See
    `s2-gpu-geometry.md` "Sprites (GPU)".
- **S3 — Input + per-game mapping.** Via the extended padmap tooling (Track C).
- **S4 — Options, savestates, polish.** Only the options that make sense for these games; reuse the
  savestate framework.
  - **Per-family option set (started 2026-08-23).** The core options table (`DEFINITIONS[]` in
    `retro_options.cpp`) is shared across both subtargets. The visibility mechanism is now built:
    `m2opt::hide_option(key)` marks an entry hidden and all three declare forms skip it; `retro_entry`
    positive-detects the family (`driver_list::find("ridgerac")`) at `retro_set_environment` time and
    hides the wrong family's options before `declare()` — the same runtime-not-compile-time gate
    `set_native_resolution()` uses (the OSD object files are shared, so a `#define` cannot tell the
    subtargets apart).
    - **DONE: `system22_texture_filter` (Off/On, S22-only).** Bilinear on the textured 3D poly tail —
      a 4-tap blend in RGB space *after* the palette lookup (the pens are indices; neighbours must
      resolve to colour before they can be averaged), gated on a `tex_filter` push-constant bit in
      `s22.frag`. The alpha-pen test stays on the point-sampled centre pen, so cutout/alpha shape is
      unchanged. Off is bit-identical to the pre-change core (ridgerac 1800f digest `000263dec4db0fa1`
      == HEAD-shader build); on differs (`74b1a2b0d88545e9`). `M2VK_S22_FILTER=0|1` overrides. Hidden
      from the Model 2 menu (verified: Model 2 declares 12 options, not this one). Sprites stay
      point-sampled (2D, screen-aligned). No mip chain — System 22 stores one resolution, so trilinear
      / distance-flicker mips are a separate (bigger) job, deferred.
    - **Still wanted:** hide the Model 2-specific debug graphics options (`model2_flat_shading`,
      `model2_flat_luma`, `model2_transparency`) from the S22 menu — its renderer does not honour them —
      and give S22 its own debug toggles matched to the S2d shading tail (fog on/off, no-lighting,
      no-textures, gamma bypass). Keep the genuinely shared ones (steering block + `model2_steering_display`,
      resolution). The `hide_option` mechanism above is what does the hiding.

Do not pre-commit to a phase count beyond this. Each phase closes when its output is measured, not when
a document says it should.

---

## Track B — retire the monuments (do this alongside S0/S1, not "later")

1. **CLAUDE.md diet.** Cut it to a working brief: current state, next step, the load-bearing gotchas,
   the build/run commands. Remove self-congratulation, superseded blocks, and corrections-of-corrections.
   Fix the "30 lines" claim everywhere it appears. Target: a fraction of its current length.
2. **worklog.md** (7,283 lines) — archive the closed-phase history to a `worklog-archive.md` and keep
   the live worklog short. Going forward, one entry per milestone.
3. **Switch audit.** 56 distinct `M2VK_*` env switches exist. Keep the harness-essential ones, delete
   dead/one-off diagnostics, and write a single short reference listing what survives. New System 22
   switches must justify themselves against that list.
4. **Reconcile stale numbers** (digests, diff size, "costs nothing" perf claims) — correct in place or
   delete; do not annotate.

Cleanup is scoped to *documentation and dead switches*. It does not touch shipping code paths or the
committed renderer.

---

## Track C — padmap web app, extended to System 22

Keep doing the thing that works: `input_layouts.json` as the single source, `padmap-gen.py` compiling
`input_layouts.ipp`, labels derived from assignments (never a second table), `--check` guarding drift,
and the browser editor with Save/Rebuild/Play.

For System 22:
- Add the S22 driver's port sets to the sweep (`M2VK_INPUT_DUMP` equivalent for the new OSD path) so
  the editor sees each machine's own `PORT_NAME`s and paddle/pedal/gun fields.
- Author layouts for the racing wheel + pedals + shifter cabinets (Ridge Racer, Rave Racer, Ace Driver),
  the gun games (Time Crisis, Tokyo Wars), and the exotic controls (Prop Cycle pedals, Alpine ski,
  Cyber Cycles handlebar).
- Reuse the steering-curve pipeline for the `IPT_PADDLE` racers once the steering block is committed.

---

## Track D — compatibility list: expand and formalise

`devnotes/compatibility.md` is currently a 29-row Model 2 play-test table with useful hand-notes but no
consistent status vocabulary and no key-mapping column. Rework it into one matrix covering **both
drivers**, with columns:

| Game | romset | Driver | Boots | Renders (SW) | On GPU | Input mapped | Key-map verified | Savestate | Notes |

- **Status vocabulary** (fixed set, no prose in the status cells): `—` / `partial` / `ok` / `n/a`.
- **Key-map verified** is the column the user maintains by hand — did the pad/wheel/gun actually do the
  right thing in-game. Preserve the existing play observations ("Shifters dont work", "kasumi broken")
  in Notes, don't discard them.
- Seed the System 22 rows from the working-set list (17 playable parents; `ridgeracf` and `ridgerac3m`
  marked not-working). Fill as ROMs arrive.
- One table, both drivers, sorted by driver then name. This replaces the informal list, not supplements
  it.

---

## Order of work (suggested, not binding)

Done: ~~steering commit~~ → ~~Track B CLAUDE.md diet + worklog archive~~ → ~~S0 software boot~~ →
~~S1 seam~~ → ~~S2a–S2d GPU geometry (untextured → textured → 2D-over → shading tail + gamma)~~ →
~~sprites on the GPU (incl. SS22 sprite-in-tree z + prival OVER/text compositing)~~ →
~~`system22_texture_filter` + per-family option-visibility mechanism~~. All committed at `6e62265dff6`.

**The S22/SS22 renderer is complete; what remains is the shippable pass** (still open — this is what
the shippable plan will schedule):
1. **S3 — input + per-game mapping** (Track C padmap tooling). `input_layouts.json` currently has **no
   S22 rows** — the wheels/pedals/shifter racers (ridge/rave/ace), the gun games (timecris, tokyowar),
   and the exotic controls (propcycl pedals, cybrcycc handlebar, alpine ski) are all unauthored.
2. **Track D — the combined compatibility matrix.** [compatibility.md](compatibility.md) has S21 rows
   only; S22/SS22 has **zero** rows. Seed from the 17-parent working-set list.
3. **S22 savestates** — the framework is driver-agnostic and applied unchanged to S21, but **no S22 set
   has been verified**. Run `state.sh` over a few S22 fixtures.
4. **S4 — per-family options.** Hide the Model 2-specific debug options from the S22 menu
   (`model2_flat_shading`/`flat_luma`/`transparency`) and add S22 debug toggles matched to the S2d
   shading tail (fog on/off, no-lighting, no-textures, gamma bypass); settle `system22_depth_buffer`.
   `hide_option` is already built.

The full sequenced queue lives in [shippable-plan.md](shippable-plan.md).
