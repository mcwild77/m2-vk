# Modelizer — one core for all three families

**Goal.** Collapse the three subtarget cores (Model 2 / System 22 / System 21) into **one** libretro
core named **Modelizer** — MODEL (Sega) + POLYGONIZER (Namco's name for the System 2×'s geometry chip).
One dylib whose driver table holds all three families; each game boots and renders correctly through it;
each family's option menu still gates to the right subset; no renderer, seam, or shader changes.

This plan **supersedes and absorbs** shippable-plan.md's R6 (one combined core), and folds in the still-open
shippable items — R1/R4.5 input hand-checks, R3's deferred toggles, R4 branding/licence — because the
rename touches the same branding/option surface. After this lands, shippable-plan.md is closed history.

**Decisions locked with the user (2026-08-27):**
- **Rename reaches user-facing strings + option keys**, not internal code. `library_name`, dylib/soname
  basename, config dir, and the `model2_*` core-option keys become Modelizer; the internal `m2vk_*`
  filenames, `M2VK_*` env switches, and `s22_seam`/`s21_seam` symbols **stay** (churn not worth it).
- **One build replaces the three.** `SUBTARGET=modelizer` is the only build; `model2`/`namcos22`/
  `namcos21` subtargets are retired. Per-family isolated builds are given up deliberately.
- **Absorb the open shippable items** as phases here.
- **Trust the existing name-lookup detection** — verify set names don't collide across families; add
  disambiguation only if a real collision turns up (none expected).

**Posture (unchanged):** all new logic in NEW files, upstream touched only by guarded hooks; no scripted
button-press testing — build it, run static guards, hand a numbered hand-check list; one worklog entry per
milestone; no AI nomenclature anywhere that ships.

---

## M0 — family-detect refactor (the blocker; do first) 🔴

**Why first, and why it's the whole risk.** Family is detected today by *"is this family's flagship
compiled into my driver table?"* — `driver_list::find("ridgerac") >= 0` → System 22,
`driver_list::find("starblad") >= 0` → System 21, else Model 2. That is correct *only* because each
subtarget compiles in one family's drivers. **In a merged core all three flagships are always present, so
every game misidentifies as System 22.** This is the real work; the build merge is trivial next to it.

Sites (current `retro_entry.cpp`, by symbol not line — numbers drift): the load-time option-hiding block
(the `if find("ridgerac") … else if find("starblad") … else` cascade near the top of `retro_load_game`),
the seam-arming probes further down, and the `family_dir` selector. Grep the anchor:
```sh
grep -n 'driver_list::find("ridgerac")\|driver_list::find("starblad")' src/osd/libretro_m2/retro_entry.cpp
```

**Fix.** Derive family from the **loaded system**, which is already in hand at
`system_name_from_path(path)` (the `system` local in `retro_load_game`):
```
enum class family { model2, system22, system21 };
family family_of(const std::string &system);   // driver_list::find(system) → driver().type.source()
                                                //   "sega/model2.cpp"        → model2
                                                //   "namco/namcos22.cpp"     → system22
                                                //   "namcos21.cpp"/"..c67.."  → system21
```
One helper, ~7 call-site swaps from the `find(flagship)` idiom to `family_of(system)`. The seam-arming
sites that run before `system` is resolved get the family threaded in from the load path.

**Do M0 on the current three-core build first.** It's a behavior-preserving refactor there (each core has
one family compiled in, so `family_of` returns what the old probe did) and is independently worth having.
Verify by rebuilding all three today and confirming digests are byte-identical to their pre-M0 baselines
(one Model 2, one S22, one S21 fixture via `retrohost --vk`), plus each menu still shows the right options.

**Exit M0:** every runtime family decision keys off the running driver's source file, not the driver
table's contents; three-core digests unchanged; per-family menu gating unchanged.

---

## M1 — build merge: one subtarget, three families 🟢 ✅ DONE 2026-08-27

Cheap because the renderer/OSD/seam/shaders already compile into **every** build today and sit inert until
the running driver arms capture — there is no compile-time family `#ifdef` in the shared OSD.

1. **`src/mame/modelizer.flt`** — union of the four driver cpps:
   `sega/model2.cpp`, `namco/namcos22.cpp`, `namco/namcos21.cpp`, `namco/namcos21_c67.cpp`.
2. **`scripts/target/mame/modelizer.lua`** — union of the three existing target luas
   (`model2.lua` + `namcos22.lua` + `namcos21.lua`):
   - Union `CPUS`/`MACHINES`/`SOUNDS`; keep the deliberate cross-pull (namcos22 → `GEN_FIFO` for the shared
     savestate symbol).
   - **Keep three separate driver projects**, each with its own define (`M2VK` / `S22VK` / `S21VK`) so each
     family's `_v.cpp` seam hooks stay scoped. Merging into one project would force all three defines onto
     every driver source.
   - Watch for **duplicate device-registration** warnings when the dep sets union.
3. **Constant target name** in `scripts/src/osd/libretro_m2.lua`: `targetname("modelizer_libretro")` (was
   `_subtarget .. "_libretro"`); Android soname likewise constant `modelizer_libretro_android.so`.
4. **Retire** `model2.flt`/`namcos22.flt`/`namcos21.flt` and the three `scripts/target/mame/{model2,
   namcos22,namcos21}.lua`. Update the harness/build docs (below) to `SUBTARGET=modelizer`.

Build: `make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10` → `modelizer_libretro.dylib`.
Standalone dev binary: `make SUBTARGET=modelizer OSD=sdl3 …` → `mamemodelizer` (all three families).

**Exit M1:** one dylib; a Model 2, an S22 and an S21 zip each boot and render; no renderer/seam diff.

**Landed 2026-08-27** (the retirement half; M0 + the modelizer add landed the previous commit at
`e099e16523c`, which deliberately kept the three per-family subtargets buildable):
- Deleted `src/mame/{model2,namcos22,namcos21}.flt` and `scripts/target/mame/{model2,namcos22,
  namcos21}.lua`. `SUBTARGET=model2|namcos22|namcos21` no longer resolves a target — modelizer is
  the only build.
- `scripts/src/osd/libretro_m2.lua`: core name is now the constant `modelizer_libretro`
  (dylib + Android soname), replacing `_subtarget .. "_libretro"`. Regenie + full build verified —
  links `modelizer_libretro.dylib`, exit 0.
- Repointed local harness/build plumbing off the old basename: `ab.sh`/`res.sh`/`perf.sh` `CORE`
  default → `./modelizer_libretro.dylib`; `build-android.sh` → `SUBTARGET=modelizer`,
  `OUT=modelizer_libretro_android.so`. Comment in `vk_funcs.h` updated. CLAUDE.md build block updated.
- **Still on the old name (M2 launch-plumbing, deliberately not touched here):** the installed-core
  symlink `~/Library/Application Support/RetroArch/cores/model2_libretro.dylib`, `~/Desktop/Model 2.app`,
  `.vscode/settings.json`, and `devnotes/shortcuts/`. The play-app therefore launches a stale/absent
  dylib until M2 repoints them — flagged, not fixed, because renaming them is M2's branding scope.

---

## M2 — the Modelizer rename 🟡

1. **`library_name`** in `retro_entry.cpp`: `"m2-vk"` → `"Modelizer"`. This alone moves RetroArch's config
   dir to `config/Modelizer/Modelizer.opt` and sets the display name.
2. **Core-option keys** `model2_*` → `modelizer_*` in `retro_options.h` (the `KEY_*` string literals):
   renderer, diagnostic_input, internal_res, flat_shading, flat_luma, transparency, poly_counter, the five
   `steering_*`, the two `steering_damp_*`, steering_display, analog_deadzone, analog_reach.
   - **Leave `system22_*` keys as-is** — they are accurately family-scoped and only ever appear on the S22
     menu; renaming them to `modelizer_system22_*` buys nothing. (Flag for the user; reverse if they'd
     rather everything carry the `modelizer_` prefix.)
   - `KEY_*` constants are referenced by symbol everywhere (hide_option, DEFINITIONS, env parsing), so only
     the string *values* change — no logic touched.
   - **Pre-release, so this is a one-time local reset:** old keys in the user's `.opt` orphan, new keys take
     defaults. No external users exist yet.
3. **`.info` file** — RetroArch matches a core to `modelizer_libretro.info` by basename. Create it (or
     rename the existing Model 2 one); set `display_name`, `corename`, `systemname`, `manufacturer`.
4. **Local launch plumbing** (all hold absolute paths / the old basename — none ship, all break silently):
   - `~/Desktop/Model 2.app` launcher → point at `modelizer_libretro.dylib`; consider renaming to
     `Modelizer.app` (keeps the same env-scrub behavior).
   - The installed-core **symlink** `~/Library/Application Support/RetroArch/cores/model2_libretro.dylib`
     → `modelizer_libretro.dylib` (and re-verify it's a symlink, not the reverting copy).
   - `.vscode/settings.json`, `devnotes/shortcuts/`, and any harness script pinning
     `M2OPT_model2_renderer=…` / `SUBTARGET=model2` / the `model2_libretro.dylib` path.
5. **`M2VK_*` env switches stay** (internal, per decision). Note the intentional split in a one-line
   comment so a future reader doesn't "fix" the inconsistency.

**Exit M2:** the core presents as **Modelizer** in RetroArch; option keys are `modelizer_*` (+ untouched
`system22_*`); the Desktop app and installed symlink launch the renamed dylib.

---

## M3 — absorb the open shippable items 🔵

These were open in shippable-plan.md; close them under Modelizer branding.

- **R1 / R4.5 input hand-checks (open).** Layouts + joystick-shifter are authored and static-verified;
  what's pending is the **user's pad-in-hand pass** on the S22/S21 sets that have no local ROM was only
  statically verified. Re-hand the numbered hand-check list under the new build. No code.
- **R3 deferred toggles.** `system22_fog`/`no_textures`/`no_lighting`/`2d_overlay`/`poly_counter` shipped;
  the **gamma bypass** and the settle/removal of `system22_depth_buffer` are still open — decide keep or
  drop, then build or delete. (User dropped gamma once; confirm before re-adding.)
- **R4 release chores.** Branding is now settled by M2 (display name = Modelizer). Remaining: licence
  chores per legalstuff.md §9 (tag the building commit, ship `COPYING` + `docs/legal/`, GPL-2.0-or-later,
  **core only** — the `astc-encoder` Apache dep is live only in the retired standalone), re-run the audit
  after any upstream merge, and the **fork README** (the user's to write — flagged, not drafted).

**Exit M3:** input hand-checks returned green; R3 toggles resolved; legalstuff §9 complete except README.

---

## Verification (spans M0–M2)

The A/B invariance is the safety net: **the merged Modelizer core must produce byte-identical `digest:`
lines to the three separate cores** for one fixture per family.
- Model 2: `vf2` (or `daytona`) — `retrohost --vk`, compare digest to its pre-merge baseline.
- System 22: `ridgerac` — digest vs baseline (SS22 crash already fixed).
- System 21: `starblad` — digest vs baseline.
- Per-family menu gating: load one game of each, confirm the option list matches today's per-subtarget menu
  (M0 is what makes this hold in the merged binary).
- ~~Savestates: one `state.sh` per family.~~ 🚫 **Void — savestates are disabled core-wide as of
  2026-09-04** (`retro_serialize_size` returns 0; see the savestates section of `CLAUDE.md`). Drop this
  criterion; `state.sh` is retired and refuses to run.

Green A/B + matching menus across all three families = the merge changed packaging only, which is the
whole claim.

---

## Suggested order

**M0 (blocker, do on the current three-core build and prove digest-identical)** → M1 (build merge) →
M2 (rename) → M3 (absorbed items, independent, can interleave). M0 before M1 is load-bearing: skip it and
every game in the merged core boots as System 22.

## Not in scope

No renderer/seam/shader changes. No new game support. No per-family isolated builds after M1 (given up by
decision). No `M2VK_*`→`MODELIZER_*` switch rename, no `m2vk_*` file rename (internal, kept).
