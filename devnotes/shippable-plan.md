# Shippable plan — from three working renderers to a public release

The renderer/geometry work is **done** across Model 2, System 22/SS22 and System 21 (HEAD `6e62265dff6`).
This plan schedules everything between here and a binary someone else can play. It is the single live
queue; the phase files (`p*`, `s*`, `t*`) and the two port plans are closed history.

**Posture (unchanged):** all new logic in NEW files, upstream touched only by guarded hooks; measure the
vk/sw brightness ratio before blaming coverage; **no scripted button-press testing** — build it, run the
static guards, hand a numbered hand-check list; one short worklog entry per milestone; no AI nomenclature.

## Release-blocker vs quality vs optional

- **Blocker** (can't hand someone the binary without it): R1 input mapping, R4 branding/licence chores.
- **Quality** (expected of a RetroArch core): R2 savestates + compat matrix, R3 the S22 option set.
- **Optional** (post-ship candidates): R5 the S21 tails.

---

## R1 — System 22 input mapping (Track C) ✅ authored, static-verified; user hand-check pending

All 18 S22 port sets now have a real row (43 rows / 50 sets total, `input_layouts.json`) — the six
racers done earlier (`acedrive`, `dirtdash`, `ridgerac`+clones incl. `ridgerac3m`, `victlap`) plus twelve
authored 2026-08-25: `adillor`, `airco22b`, `alpiner` (Alpine Racer + Alpine Racer 2, one shared `ipt`
block), `alpines`, `aquajet`, `cybrcomm`, `cybrcycc`, `propcycl`, `raverace`, `timecris`, `tokyowar`. Also
fixed `ridgeracf`, which had shipped as a meaningless numbered-button stub — re-derived from source since
it has no local ROM (`MACHINE_NOT_WORKING`).

1. ✅ Padmap sweep/editor were already S22-aware (2026-08-22, family tab + driver tag).
2. ✅ Layouts authored, reusing what exists — with two corrections found by reading source/dumps instead
   of guessing from the bucket list below:
   - **Racers** (ridgerac, raverace, acedrive, cybrcomm, victlap, dirtdash, aquajet) — wheel + pedals +
     shifter; `IPT_PADDLE` racers inherit the steering-curve block automatically. **`cybrcomm` is not a
     wheel racer** — it's a single-cabinet twin-stick mech (source: buttons "placed on both sticks"),
     same shape as System 21's `cybsled`; mapped with the same `twin_ad_stick` flag instead.
   - **Gun games** (timecris, tokyowar) — reuse the Model 2 lightgun path. **`tokyowar` is not a gun
     game** — it's a wheel-and-pedals tank sim (`flags.lightgun: false` in the actual `INPUT_PORTS`
     block); mapped like a driving cabinet instead, with twin cannon triggers on X/R.
   - **Exotic** (propcycl pedals, cybrcycc handlebar, alpine/alpinr2b ski, adillor) — Alpine Racer's two
     independent leg-lean axes are tagged P1/P2 exactly like `cybsled`'s treads, so `twin_ad_stick` puts
     the second leg on the right stick's X axis. `adillor` is a trackball, not a paddle. `aquajet` has an
     unnamed second axis in source (no `PORT_NAME`) — labelled "Lean" as a best guess pending hand-check.
3. ✅ `padmap-gen.py --check` and `padmap-test.sh` green; `M2VK_HOST_DESCRIPTORS=1` read on all 12 newly
   built sets, matches every label authored.
4. 🚧 Hand-check list handed to the user 2026-08-25 — **waiting on the pad-in-hand result.**

**Exit R1:** every playable S22 parent has a layout row with labels *derived* from assignments — done;
static checks green — done; user has hand-checked the controls (feeds R2's Key-map column) — **open**.

## R2 — S22 savestates + combined compatibility matrix (Track D) ✅ done 2026-08-25

1. ✅ **Savestates.** `state.sh` run over the three representative S22 fixtures — `ridgerac` (racer),
   `raverace` (fogged SS22), `timecris` (sprite-heavy SS22). **All three PASS** (`C==D`, `N!=D`,
   deterministic, `A==R`); no `gen_fifo`-class regression. Framework confirmed driver-agnostic.
2. ✅ **Compat matrix.** [compatibility.md](compatibility.md) rewritten as one Track-D matrix across all
   three families (50 rows), sorted by driver, with the Model 2 play-test notes preserved below it.
   `On GPU` cites `ppmdiff coverage` measured this session for every row; Model 2's from ab-baselines +
   a fresh batch for the rest. **Key-map verified** still `—` for S22 (R1 hand-check open); Model 2
   filled from the user's play-test notes.

✅ **R0 done 2026-08-25: the SS22 Vulkan-path crash is fixed.** `cybrcycc`, `alpines`, `alpinr2b` were a
buffer over-**read** in `s22::upload_static`, not a Vulkan bug: it copied a fixed `TTDATA_BYTES`
(0x1000000) from the `textile` region (gfx(1), RAW layout), but these three are the only S22/SS22 parents
whose region is short of 16MB (0xe/0xc/0xa00000), so the first-3D-frame tile upload walked 2–6MB off the
end → EXC_BAD_ACCESS. Fixed by threading the region's real size across the seam
(`texture_ram::ttdata_bytes` ← `memregion("textile")->bytes()`) and copying `min(actual, TTDATA_BYTES)`,
zero-filling the tail of the always-16MB GPU buffer. All three now complete 3000 frames on `--vk`;
full-region path unchanged (ridgerac 1800f digest `000263dec4db0fa1` == baseline). 4 files
(s22_seam.h/.cpp, s22_geom.cpp, namcos22_v.cpp); not yet committed. Matrix rows are now `ok`. All S22/SS22
parents render on GPU (0.87–1.0000).

## R3 — System 22 option set (S4) ✅ built, static-verified; user hand-check pending

**Done 2026-08-25.** Five new toggles, each a `DEFINITIONS[]` entry + `M2VK_*` switch + push-constant
bit, mirroring `system22_texture_filter`. All apply live.

- **`system22_fog`** (on/off, default on — the accurate path, so it tests "off"). `PFLAG_NO_FOG` skips
  every fog/z-fog/sprite-fog blend in `s22.frag`. `M2VK_S22_FOG`. Digest differs on cybrcycc (its SS22
  z-fog); a no-op in attract scenes that carry no active fog (ridgerac/alpinerd/timecris first frames).
- **`system22_no_textures`** (off/on) — `PFLAG_NO_TEX` whitewashes the surface to `ivec3(255)` after the
  fetch, so the per-pixel shade renders it greyscale (S22 shading is luma-only → true greyscale). Fetch
  still runs, so the alpha-pen cutout is unchanged. Polygon-only; sprites/2D left alone. `M2VK_S22_NOTEX`.
- **No Lighting now works on S22.** `model2_flat_luma` was dead on the S22 path (menu entry did nothing);
  `PFLAG_NO_LIGHT` now skips the shade step. Shares `M2VK_FLAT_LUMA` with the Model 2 sink. And
  `model2_flat_shading` (Flat Shading) is now **hidden** from the S22 menu — the S22 untextured look is
  its own greyscale option, so Flat Shading (base-colour draw) would be a dead entry.
- **`system22_2d_overlay`** (on/off, default on) — gates the 2D-over HUD/text sandwich. `over_pixels()`
  returns null when off → `s22_sandwich` false → the OVER draw falls away, leaving the 3D above the 2D
  background. `M2VK_S22_HUD`. (Scope: HUD/text only, per decision — not the 2D background.)
- **`model2_poly_counter`** (off/on) — **all three families**, a green digit read-out top-right of the
  primitive count the active family submitted this frame. New `counter.frag` (3×5 bitmap font, scissored
  fullscreen triangle) + `s_pipeline_counter`, built alongside the steerbar; `draw_counter()` after
  `draw_steerbar`. Count via `geom_primitive_count()`/`geom_frame_polys()` getters, selected in
  `record_and_submit` by the active draw flag. Vulkan-only (counts GPU primitives). `M2VK_POLYCOUNT`.

Verified: three cores build; digests differ per arm; screenshots on cybrcycc (fog/notex/nolight/hud),
vf2 (counter=519), starblad (counter=617).

**Not done / deferred:** the **gamma bypass** toggle (user dropped it). `system22_depth_buffer` left as-is
(still an enhancement toggle, not settled/removed). Both are open if wanted.

## R4 — public release

- **Branding.** `library_name = "m2-vk"` is already family-neutral (keeps MAME branding out of RetroArch).
  Revisit the RetroArch **display name** for a core that plays three families — decide before the build.
- **Licence chores** ([legalstuff.md](legalstuff.md) §9, do not touch that file otherwise): tag the
  building commit, ship `COPYING` + `docs/legal/`, declare GPL-2.0-or-later, ship the **core only** (not
  the standalone). Re-run the audit after any upstream merge.
- **README** — the fork README is **the user's to write** (legalstuff §5.2/§9); flagged, not drafted.

**Exit R4:** legalstuff §9 checklist complete except the README; a tagged commit builds the three cores.

## R4.5 — joystick-shifter onto the shoulders (System 22 / System 21 racers) ✅ built, static-verified; user hand-check pending

**Done 2026-08-25.** Detection in `padmap-gen.py` (`joy_shifter_portsets()` mirrors `paddle_portsets()`):
a port set whose sweep dump shows a P1 `IPT_JOYSTICK_UP`+`DOWN` named "Shift …" sets a new `joy_shifter`
bool on every row naming it, and the generator injects the R/L descriptors from the dump field names — so
`input_layouts.json` stays free of them. Seven rows flagged: `acedrive`, `dirtdash` (via its `dirtdashj`
clone dump — the parent has no dump), `ridgera` (ridgerac+3m), `ridgera2`, `victlap`, `winrun`(+winrungp),
`raverace`. The struct carries the flag; `configure()` binds `IPT_JOYSTICK_DOWN`→R1 and `IPT_JOYSTICK_UP`→L1
when set — **additive**, `apply_device_defaults()` ORs them onto the d-pad/left-stick bindings
add_directional_assignments already made, so the d-pad still shifts (and still lane-changes on Ridge). R1 is
the upshift ("Shift Up", which the driver PORT_NAMEs onto `IPT_JOYSTICK_DOWN` — the game's own inverted
names, followed not corrected); L1 the downshift. New `BUTTON_L`/`BUTTON_R` fixed items (ITEM_ID_BUTTON13/14)
carry L1/R1; inert on every non-shifter set. Three cores build; `padmap-gen.py --check` green; daytona
(Model 2) descriptor dump unchanged (no id=10/11, shift still on GEAR numbered buttons — negative control).
**No S22/S21 racer ROM is local, so a live `M2VK_HOST_DESCRIPTORS` on the affected sets could not run** —
`.ipp` emission verified statically instead (R="Shift Up", L="Shift Down" on the seven rows). User
hand-check pending.

### Original plan below

## R4.5 — joystick-shifter onto the shoulders (System 22 / System 21 racers)

**Why it's its own item:** on every S22/S21 racer the gear shift is not a numbered button — it is
`IPT_JOYSTICK_UP/DOWN` (and `LEFT/RIGHT` on the Ridge family), which `add_directional_assignments()`
routes to the d-pad + left stick. The layout table only maps *numbered* buttons, so there is no way to
put shift on R1/L1 from `input_layouts.json` alone; it needs a core routing change. The Model 2 racers
are unaffected — their shift already *is* `IPT_BUTTON1/2` and sits on R/L today.

Affected sets: `acedrive`, `dirtdash`, `victlap`, `winrun`, `winrungp` (2-way up/down) and `ridgerac`,
`ridgera2`, `raverace` (4-way H-gate: up/down **and** left/right).

Plan:
1. **Detect, don't author.** The sweep already labels these fields `"Shift Up"`/`"Shift Down"` on an
   `IPT_JOYSTICK_*` type. Have `padmap-gen.py` set a new `game_layout` bool (`joy_shifter`) when a set's
   dump shows a shifter on the joystick, and emit it into the `.ipp` struct (mirrors `lightgun` /
   `twin_ad_stick`). No new editor UI needed; no hand-authoring.
2. **Route in `configure()`** (`libretro_m2_input.cpp`, after `add_directional_assignments`): when
   `joy_shifter`, add `IPT_JOYSTICK_UP → buttonitems[BUTTON_R]` (R1) and `IPT_JOYSTICK_DOWN →
   buttonitems[BUTTON_L]` (L1), and give R/L the `"Shift Up"`/`"Shift Down"` descriptors.
3. **Decisions to settle before coding:**
   - *Additive vs exclusive:* keep the d-pad binding too (both work, harmless) or suppress it so shift is
     shoulders-only. Additive is less code and safe; exclusive is cleaner in the remap UI. Lean additive.
   - *Ridge 4-way:* the user's rule covers up/down only. `LEFT/RIGHT` (the H-gate lane change) stays on
     the d-pad, so Ridge's shifter ends up split shoulders (up/down) + d-pad (left/right). Acceptable, but
     call it out — a purely-sequential feel would lose lane changes.
4. **Verify statically** (per the no-scripted-input rule): `M2VK_HOST_DESCRIPTORS` shows R1/L1 now labelled
   Shift Up/Down on the affected sets and unchanged elsewhere; `padmap-gen.py --check` still clean; then a
   user hand-check on one 2-way (`ridgera2`) and confirm the d-pad still lane-changes on a 4-way.

**Exit:** the eight S22/S21 racers shift on R1/L1; Model 2 racers unchanged; descriptors static-verified.

## R5 — S21 tails (optional, post-ship candidates)

- cybsled twin-stick feel re-check (right tread now binds pad 1's right stick — correct-axis hand-check).
- cybsled radar-top far-poly geometry gap (a pre-existing hole, not shadow logic).
- `aircomb`/`solvalou` bring-up if wanted (`driveyes` out of scope; `winrun91` needs a ROM).

## R6 — one combined core (optional; packaging, ships three-in-one)

**Why it's cheap:** the renderer/OSD/seam layer is *already* a single shared unit — `s22_seam`,
`s21_seam`, `s22_geom`, `s21_geom` and all three shader sets compile into **every** build today
([libretro_m2.lua](../scripts/src/osd/libretro_m2.lua) lines ~308–340), inert until the running driver
turns capture on. The shared OSD carries **no compile-time family `#ifdef`** ([libretro_m2_cfg.lua](../scripts/src/osd/libretro_m2_cfg.lua)
defines only `OSD_LIBRETRO_M2`); the `M2VK`/`S22VK`/`S21VK` defines gate only the seam hooks in each
family's `_v.cpp`. So what makes them three cores is only (1) the driver filter and (2) seven runtime
probes that conflate "compiled-in" with "running." No renderer, seam, or shader work.

### A — build side: merge the three subtargets into one

1. **One merged `.flt`** listing all four driver cpps: `sega/model2.cpp`, `namco/namcos22.cpp`,
   `namco/namcos21.cpp`, `namco/namcos21_c67.cpp` (today [model2.flt](../src/mame/model2.flt) /
   [namcos22.flt](../src/mame/namcos22.flt) / [namcos21.flt](../src/mame/namcos21.flt) each name one).
2. **One merged `scripts/target/mame/<name>.lua`** = union of [model2.lua](../scripts/target/mame/model2.lua) +
   [namcos22.lua](../scripts/target/mame/namcos22.lua) + [namcos21.lua](../scripts/target/mame/namcos21.lua):
   - Union the device deps (`CPUS`/`MACHINES`/`SOUNDS`). Keep the deliberate cross-pulls already in place —
     e.g. namcos22 pulls `GEN_FIFO` to satisfy the shared savestate symbol.
   - Keep **three separate driver projects**, each carrying its own define (`M2VK` / `S22VK` / `S21VK`) so
     each family's `_v.cpp` seam hooks stay correctly scoped. Merging into one project would force all
     three defines onto every driver source; separate projects matches the current per-project arrangement.
3. **Fixed `targetname`** — [libretro_m2.lua:148](../scripts/src/osd/libretro_m2.lua) becomes a constant
   (`m2vk_libretro`) instead of `_subtarget .. "_libretro"`; likewise the Android soname at line ~167/174.
   `library_name` is already the family-neutral `"m2-vk"` ([retro_entry.cpp:393](../src/osd/libretro_m2/retro_entry.cpp)).

### B — runtime side: the one real fix (seven sites, all in [retro_entry.cpp](../src/osd/libretro_m2/retro_entry.cpp))

Family is currently detected by *"is this family's flagship compiled into my driver table?"* —
`driver_list::find("ridgerac") >= 0` → System 22, `driver_list::find("starblad") >= 0` → System 21,
else Model 2. Lines **342, 344, 719, 764, 776, 777, 1028**. In a combined core all three flagships are
always present, so every game would misidentify as System 22 — **this is the blocker, not the build
merge.** Fix: derive family from the **loaded system** (already in hand at
[retro_entry.cpp:578](../src/osd/libretro_m2/retro_entry.cpp), `system_name_from_path(path)`) via
`driver_list::find(system)` → `driver().type.source()` mapped to a family. One helper, seven call-site
swaps. This also fixes the load-time option-hiding block at
[retro_entry.cpp:342](../src/osd/libretro_m2/retro_entry.cpp) (native-resolution + `hide_option` per
family) — which is one of the seven — so R3's per-family menu gating rides along correctly once B lands.

Everything else is already family-dispatched at runtime (seam capture and per-family option visibility
both key off the running driver), so it works unchanged in the combined binary.

### Not in scope / caveats

- **Input mapping is done** — S22/S21 rows are present ([input_layouts.json](../src/osd/libretro_m2/input_layouts.json));
  a combined core neither creates nor removes that work.
- **SS22 vk crash** (cybrcycc/alpines/alpinr2b) — ✅ fixed 2026-08-25 (short `textile`-region over-read in
  `upload_static`); no longer a packaging concern.
- Watch for **duplicate device-registration** warnings when the three dep sets union.
- **RetroArch display name** for a three-family core is the same open question as R4's branding item.

**Exit R6:** one dylib whose driver table holds all three families; a Model 2, a Namco S22 and a Namco
S21 zip each boot and render correctly through it; per-family option menus still gate right; no renderer
or seam changes in the diff. Net cost: ~2 new build files + a `targetname`/soname change + one runtime
helper replacing 7 probe sites.

---

## Suggested order

**R1 (the blocker, largest)** → R2 (needs R1's hand-checks for the matrix) → R3 (independent, can
interleave) → R4 (release gate) → R4.5 (input polish, independent) → R5 (optional) → R6 (optional
packaging; do it *after* B's family-detect refactor is worth having anyway, or fold R6's step B into R3
since both touch the same [retro_entry.cpp](../src/osd/libretro_m2/retro_entry.cpp) family-detect block).
