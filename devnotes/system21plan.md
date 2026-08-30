# System 21 plan — fold Namco System 21 into the System 22 core

Add Namco **System 21** (the 1991 "polygonizer": Star Blade, Air Combat, Cyber Sled, Winning Run) as a
supported family **inside the same core** that already runs System 22 and Super System 22 — one dylib,
routed to the right renderer at runtime. This is the sibling of [system22plan.md](system22plan.md);
read that first for the seam philosophy and the shared-OSD mechanics this reuses wholesale.

## Why this is a smaller lift than S22 was (the headline)

Three facts, all verified against the tree this session, make System 21 the *easy* second family:

1. **Untextured.** `namcos21_3d_device` (`src/mame/namco/namcos21_3d.cpp`) rasterises flat/Gouraud
   polygons with **no texture system at all** — no tile fetch, no palette banks, no fog/czram/gamma
   tail. The renderer is a fraction of `s22_geom.cpp`; the closest thing we already have is the S22
   *untextured* path.
2. **Genuinely z-buffered in hardware.** `renderscanline_flat` writes a real per-pixel depth buffer
   (`m_poly_framebuffer_z`) and tests `if (zsort < zbuf[x])` (namcos21_3d.cpp:86–101). So the
   `s22_depth_buffer` machinery we built 2026-08-23 is not an *enhancement* here — it is the
   **accurate** model. No painter's algorithm, no draw-order sort, no coplanar-decal tradeoff.
3. **One seam covers the whole family.** Every System 21 game — across all three driver files — draws
   through the *same* `namcos21_3d_device`. Tap it once and Star Blade, Cyber Sled, Air Combat and
   Winning Run are all covered. Widening from one game to all of them is build/boot plumbing, **not**
   renderer work.

## Scope — which games

The 3D device is shared; the driver *front-ends* differ (CPU/DSP wiring). Phase by front-end:

| Driver file | Games | Status flag | Phase |
|---|---|---|---|
| `namcos21_c67.cpp` | Star Blade, Air Combat, Cyber Sled | `IMPERFECT_GRAPHICS` (boots) | **1 — the C67 set is the whole renderer effort** |
| `namcos21.cpp` | Winning Run, Winning Run Suzuka GP, Winning Run '91 | `IMPERFECT_GRAPHICS` (boots) | **DONE (T4)** — seam covered geometry; the option-B present path was ported for its bitmap-layer 2D + partial updates |
| `namcos21_de.cpp` | Driver's Eyes | `MACHINE_NOT_WORKING` | **out of scope** — blocked on MAME's own driver, like System 23 |

Star Blade is the test case (`starblad` / `starbladj`, namcos21_c67.cpp:1296).

## Seam — located

- **Primitive:** `namcos21_3d_device::blit_single_quad(int sx[4], int sy[4], int zcode[4], u16 color)`
  (namcos21_3d.cpp:216) — the S21 analogue of `poly3d_drawquad`. Called from `draw_quads()`. Each vertex
  arrives as screen-space `sx/sy` + a `zcode` depth; `color` is a `u16` palette index (base `0x2000 |
  penmask`), with a depth-cue shift already folded in (`color += depth`, derived from mean zcode).
- **Frame bracket:** the device double-buffers (`m_poly_framebuffer_{z,pens}` + `…2`), clears/swaps per
  frame, and `copy_visible_poly_framebuffer()` blits the visible page to the screen bitmap. The
  begin/end hook goes around the clear and that blit — find the exact swap site at T1.
- **2D layer:** System 21 has a C355 sprite layer (`namco_c355spr`, already in the subtarget file list),
  separate from the 3D. Handle it the way the S22 seam handles the text/2D layers (the `capture_over`
  pattern) — the 3D is composited under/over it.

Everything crossing the seam is a plain snapshot carrying no MAME types, exactly as `s22_seam.h` does.

## "Part of the System 22 core" — the integration mechanism

The shared OSD already distinguishes families at runtime (`driver_list::find()`), already carries
per-family options with `hide_option()`, and already names the core `<subtarget>_libretro`. So folding
S21 in is not a new core — it is **widening the S22 subtarget's build and adding a third render path**:

- **Build:** the subtarget's `.flt` lists both drivers (`namco/namcos22.cpp` **and**
  `namco/namcos21_c67.cpp`), and the `.lua` unions their dependencies. The background agent already
  generated a correct standalone `scripts/target/mame/namcos21.lua` + `src/mame/namcos21.flt` (from
  `makedep.py`, including the shared-OSD `GEN_FIFO` quirk) — those are the raw material to **merge into**
  the S22 target, not a separate core to ship. Decide at T0 whether to keep a `namcos21` subtarget for
  bringup and merge later, or go straight to a combined target.
- **Naming:** a core that plays S21 + S22 + SS22 is a "Namco polygon" core, not a "System 22" core.
  `library_name` and the RetroArch display name want revisiting (keep MAME branding out — see
  legalstuff.md). Not urgent; decide before any public build.
- **Runtime routing:** family detection (`driver_list::find("starblad") >= 0`, mirroring the existing
  `find("ridgerac")` switch) routes the seam, the renderer, and the option set. A third family stays
  safe-by-default the way the S22 detection already does.

## Phases (mirror the S22 arc; each gets its own short file only when started)

- **T0 — combined subtarget + software boot.** Merge the S21 C67 driver into the core's build; confirm
  `starblad` renders 3D **in software** through the shared OSD; record the software A/B baseline digest.
  *(Assumed DONE entering the next session — Star Blade is the working test case.)*
- **T1 — seam tap (record, draw nothing).** New `s21_seam.h/.cpp`; `#ifdef` hooks at `blit_single_quad`
  and the frame bracket. Record the quad stream, draw nothing, output **byte-identical** to the T0
  baseline. Measure and record the real upstream diff. This is the diff-budget checkpoint.
- **T2 — untextured 3D on the GPU. DONE 2026-08-24 (T2a 2026-08-23, T2b 2026-08-24).** New
  `s21_geom.{h,cpp}` + `s21.vert/frag`: flat, CLUT-coloured quads, `zsort → gl_Position.z` into a real
  per-quad z-buffer (S21 genuinely z-buffers in hardware, unlike S22's painter's pass — this is the
  accurate model, not an enhancement). T2b added the `pri1==4` layer-0 C355 z-mix as a second GPU pass
  (`shaders/s21_mix.frag`) that hardware-depth-tests against the same attachment. Verified on starblad
  (T2a, 97.1% exact) and cybsled (T2b, HUD/portrait/combat frames match). Detail in
  [t2-untextured-gpu.md](t2-untextured-gpu.md).
- **T3 — family routing + options. DONE 2026-08-24.** Runtime detection (`driver_list::find("starblad")`)
  already routed the S21 seam/renderer/native-res; this phase settled the option *menu*. Five options are
  now hidden from the S21 menu: the two S22-only (`system22_texture_filter`, `system22_depth_buffer`) and
  three Model 2 render options the S21 path never reads (`model2_flat_shading` — S21 is always untextured;
  `model2_flat_luma` — no per-poly luma hook; `model2_transparency` — S21 hardcodes `blendEnable=VK_FALSE`).
  Steering (5 + display bar) kept, runtime-gated on `IPT_PADDLE` (Winning Run at T4). Naming left as-is:
  `library_name="m2-vk"` is family-neutral and S21 has no unique options, so no `[system21]` log line; the
  RetroArch display-name revisit stays the pre-public item. **Native-resolution bug fixed here:**
  `set_native_resolution("496x480")` was a silent no-op because `496x480` was not in the Internal
  Resolution value list, so S21 Vulkan defaulted to Model 2's `496x384` (the "0.8× vertical rescale" seen
  at T2b was this — 384/480 = 0.8). Added `496x480` to the shared list; S21 now defaults to true native,
  and a T2 A/B no longer needs the software side resized. All three families rebuilt and confirmed
  (namcos21 496x480, namcos22 640x480, model2 496x384).
- **T2a sub-viewport shadow gap — FIXED by option B (pen-space composite), 2026-08-24.** cybsled's
  radar/camera-preview HUD boxes are C355 **palette-shadow** sprites; the RGB present path lost the pen
  beneath them. Reworked the S21 present path to composite the whole frame in pen-index space (private
  R16_UINT pen pass: under + 3D + mix; then a finish pass applies the OVER band and resolves via the CLUT
  once). Camera panel pixel-exact, radar/terrain shadows exact; starblad/aircomb do not regress. One
  residual left — a small block at the radar top where vk's geometry is missing a far poly the shadow
  now reveals (a pre-existing geometry gap, not the shadow logic). Detail + evidence in
  [t2-untextured-gpu.md](t2-untextured-gpu.md).
- **T4 — widen to all System 21. DONE 2026-08-24.** Added `namco/namcos21.cpp` (Winning Run set) to the
  `namcos21` subtarget (`.flt` + regenerated `.lua`; new `MB87077` sound dep). The plan's "seam already
  covers them" held only for **geometry capture** — the option-B **present path** (`set_palette` +
  `capture_under`/`capture_over`) lives only in the C67 driver, so it had to be ported to
  `namcos21.cpp`'s different 2D model. Two things that were not in C67: (1) Winning Run does raster-split
  **`update_partial`** screen updates, so the capture must accumulate all bands and snapshot the whole
  frame on the last band (a per-band capture kept only the last strip); (2) its GPU bitmap draws pen
  0x00/0x01 **opaque over the backdrop sentinel, shadow elsewhere** — a `shadow_enable`/sentinel push in
  `s21_finish.frag` resolves that per pixel (C67 passes `shadow_enable=0`, unchanged). Result: `winrun`
  0.9998 exact (18 edge px), `winrungp` **1.0000**; `starblad`/`cybsled` unregressed (0.9934 / 0.9995).
  `winrun91` ROM not on hand — untested. Driver diff `namcos21.cpp` +76/-0, all S21VK-guarded. Not
  committed.
- **T5 — polish. IN PROGRESS 2026-08-24.**
  - **Savestates: all 4 fixtures PASS with zero code changes** (`winrun`, `winrungp`, `cybsled`,
    `starblad`); the module is driver-agnostic (MAME registry + generic FIFO trailer) and needed nothing
    new. `state.sh` gained a `CORE=` override and a `roms/system22/` fallback. ⚠️ `starblad` FAILed twice
    first, and **both FAILs were test artefacts** (input held past the save point; then CPU contention) —
    cleared by a contamination-free single-history diff. The chase also surfaced a real upstream MAME
    quirk (`device_rom_interface::m_bank_count` is saved uninitialised) and added two diagnostics
    (`M2VK_HOST_SAVE_AT2`, `M2VK_SAVE_DIFF_HEX`). Full write-up in [savestates.md](savestates.md) §10.
  - **Per-game RetroPad layouts: authored** for `winrun`/`winrungp` (wheel, gas/brake, d-pad gearshift),
    `starblad` (AD-stick aim + fire), `cybsled` (twin-stick tank, face buttons for missile/gun/view).
    `input_layouts.json` rows added, `.ipp` regenerated, `padmap-gen.py --check` green. The tooling is
    now S21-aware: `padmap-gen.py` reads the three S21 driver files, `padmap-sweep.sh` has a `system21`
    family. ⚠️ **cybsled's second AD stick (right tread) binding is unverified** — hand-check.
  - **A/B baselines, compat rows:** in progress.

## Posture (same as S22)

- All new logic in NEW files; the only edits to upstream `namcos21_*` files are a handful of guarded
  hook calls. Measure the diff, never quote a fixed number.
- No scripted button-press testing — build it, run the static guards, hand the user a hand-check list.
- Commit hygiene: no AI nomenclature anywhere. `devnotes/` and `CLAUDE.md` stay local-only.

## Status — COMPLETE (T0–T5, all committed at `6e62265dff6`, 2026-08-25)

The System 21 family is done: `starblad`, `cybsled`, `winrun`, `winrungp` render sw-accurate on the GPU,
save/load, and carry per-game pad layouts. Verified sw-vs-vk (`retrohost --vk`, `ppmdiff coverage`, fresh
`M2VK_NO_3D` background, S21 native 496×480): winrun 0.9998, winrungp 1.0000, starblad 0.9934,
cybsled 0.9995. Build: `make SUBTARGET=namcos21 OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10`.

### Residual open items (feed the shippable plan, not the renderer)
- **cybsled twin-stick feel re-check** — the right tread now binds to pad 1's right stick
  (`m2vk_twinstick.h`); correct-axis/not-inverted is the owed hand-check.
- **cybsled radar-top far-poly gap** — a small block where vk is missing a far poly the shadow reveals; a
  pre-existing *geometry* hole, not shadow logic ([t2-untextured-gpu.md](t2-untextured-gpu.md)).
- **Not brought up:** `aircomb`, `solvalou` (namcos21_c67), `driveyes` (namcos21_de, `MACHINE_NOT_WORKING`,
  out of scope), `winrun91` (no local ROM).
