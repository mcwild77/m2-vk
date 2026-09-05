# m2-vk — Hardware-accelerated (Vulkan) libretro cores for Sega Model 2 (and now Namco System 22)

⚠️ **Renamed 2026-08-07: the GitHub repo is `mcwild77/m2-vk`** (was `mame-model2-vk`). `origin` is
updated. **The local working directory is still `~/Documents/GitHub/mame-model2-vk` and was
deliberately not renamed** — the `.app` launchers in `devnotes/shortcuts/`, `.vscode/settings.json`
and the installed-core symlink all hold that absolute path. Every filesystem path in this file is
correct as written. No committed file ever named the repo, so the rename is invisible to the public tree.

This repo is a **fork of `mamedev/mame`** turned into **libretro/RetroArch cores that render Sega
Model 2 3D in hardware via Vulkan** (replacing MAME's software rasterizer). Three families now render
through the same seam: **Sega Model 2**, **Namco (Super) System 22**, and **Namco System 21**. All three
renderers are complete; the remaining work is the shippable pass (input/compat/options/polish).

## Current state (2026-08-25)

**All three renderers are DONE and committed** at HEAD `5e06471a08e` (renderers landed at `6e62265dff6`;
the three later commits — control set up / more inputs / shift-to-L-R — are the R1/R4.5 input work). Tree is
clean. Upstream diff vs `mame0288`: **457 insertions / 16 deletions across 11 files**
(`git diff --shortstat mame0288 -- src/devices src/mame`).

- **Model 2** — DONE (`aabcd8d5cac` baseline). P0–P5, lightgun, twelve core options,
  per-game pads, the full steering block. Whole raster tail on the GPU; depth is **draw order, not z**.
  Phase detail in `devnotes/pN-*.md`.
- **System 22 / Super System 22** — renderer DONE ([devnotes/system22plan.md](devnotes/system22plan.md)).
  S0 boot → S1 seam → S2a–d GPU geometry (untextured → textured → 2D-over → shading/gamma) → sprites →
  per-quad scissor → `system22_texture_filter` + per-family option-visibility. Detail in
  `devnotes/s{0,1,2}-*.md`.
- **System 21** — DONE ([devnotes/system21plan.md](devnotes/system21plan.md)). T0 boot → T1 seam → T2 GPU
  geometry (real hardware z-buffer + layer-0 z-mix) → T3 routing/options → T4 Winning Run → T5
  pads/A-B/compat. Detail in `devnotes/t{0,1,2}-*.md`.

**Next: the shippable pass.** Renderer/geometry work is finished across all families; what remains to make
this a public release is scheduled in **[devnotes/shippable-plan.md](devnotes/shippable-plan.md)** —
R0 bug/option triage → R1 System 22 input mapping (authored + static-verified, all S22/S21 rows present in
`input_layouts.json`; only the user hand-check is open) → R2 combined compat matrix (done; the savestate
half of R2 is void — see the savestates section) →
R3 the S22 option set (open — texture-filter/depth-buffer exist, but the fog/no-lighting/no-textures/gamma
toggles are unbuilt and S22's menu still shows the three Model 2 debug options) → R4 release chores → R4.5
joystick-shifter (built, hand-check pending) → R5 optional S21 tails. Build:
`make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10` (the unified core; the three
per-family subtargets were retired 2026-08-27).

## 🚫 Savestates are DISABLED — do not regression-test against them (2026-09-04)
`retro_serialize_size()` returns **0** for every family, and `retro_serialize` / `retro_unserialize`
return **false**. RetroArch greys the save/load slots out for the session. This is deliberate and is
not a bug to fix.

**Why:** they were never uniformly trustworthy across the four families — `vcop2` never passed, the
Model 1 TGP-copro / `gen_fifo` gap was never verified (`model1update.md` §"the savestate gap"), and the
pipelined Android path dropped them outright — so every renderer change was being gated on a harness
only half the cores could satisfy. A state that loads a wrong future is worse than no state.

**What this means for how you work:**
- **`devnotes/state.sh` is RETIRED.** Do not run it, do not cite it, do not add a savestate row to any
  new plan's exit criteria. It will now fail everywhere for the trivial reason (size 0).
- A/B (`ab.sh`), resolution invariance (`res.sh`) and `perf.sh` are unaffected and remain the gates.
- `devnotes/savestates.md` is **history**, not a live spec. Same for the savestate steps inside the
  closed phase plans (`shippable-plan.md` R2, `system21plan.md` T5, `plan_system23.md` 23-7).

**What is still in the tree, deliberately unbuilt-out:** `m2vk_savestate.cpp` (including the
`gen_fifo` trailer), `m2vk_snd::state_*`, and `libretro_m2_osd_interface::state_{size,save,load}` all
still compile and still work — only the three ABI entry points in `retro_entry.cpp` were emptied, so
re-enabling is restoring three function bodies. ⚠️ **The startup spin loop in `retro_load_game` that
waits on `state_size() != 0` MUST STAY** — it no longer has anything to do with savestates, but it is
what fixes how many frames the machine runs before `retro_run` #1, and every recorded digest plus the
documented constant −1 host↔emulated frame offset was measured with it in place.

## ⚠️ Commit hygiene — READ FIRST
This repo must stay free of AI/Claude nomenclature (public repos that visibly use AI have been
targeted/harassed).
- NO `Co-Authored-By: Claude …` trailer on commits.
- NO "Generated with Claude Code" footer on PRs.
- NO "claude" / "AI" in branch names, file names, code comments, or commit/PR text.
- Author commits as the git user only; write plain, human commit messages.
- **CLAUDE.md / devnotes hiding is RETIRED** (2026-08-29, see the "Local-ignore hack — RETIRED" note
  below). Not a crisis if they land in the tree; the no-AI-nomenclature rules still apply to what you commit.
- **`devnotes/` is local-only too** — same treatment. Nothing in it ever ships.

## 🛑 DO NOT RUN AUTOMATED BUTTON-PRESS TESTING — READ FIRST (set 2026-07-30)
**The user does the in-game input verification by hand. Do not script it.**

Scripted press-and-compare sweeps through `retrohost` are **banned as a verification method**: a press
sweep costs a multi-thousand-frame run per arm, most arms are null, and every null then has to be
screenshotted and reasoned about. A 2026-07-30 session burned most of its budget on nine 4000-frame
daytona runs to establish two facts the user could confirm in under a minute with a pad.

When a change needs an in-game input check:
1. Build it, and verify everything that is *not* a button press — the A/B no-op guard, digests,
   `padmap-gen.py --check`, `padmap-test.sh`, `M2VK_HOST_DESCRIPTORS`, a static read of what the table
   emits. All cheap, none needs the game.
2. Then **stop and hand the user a short numbered list**: which game, which control, what they should
   see if it works, what they should see if it does not. Include the negative control.
3. Wait for their answer.

This does **not** ban `retrohost` generally — scripted runs for digests, rendering, resolution,
and boot-to-a-screen sweeps are fine. The ban is specifically on *pressing buttons to find
out what a button does*. `M2VK_INPUT_DUMP` and `M2VK_HOST_DESCRIPTORS` answer input questions
statically; reach for those first.

## Working docs — `devnotes/` (local-only, gitignored)
All documentation *we* write lives in `devnotes/`. Read `devnotes/README.md` first — it's the index.
(Not `docs/`, which is upstream MAME's own committed documentation.) Key files:

- `worklog.md` — running log; append a dated entry per milestone (one entry, not per thought).
  Closed-phase history is archived in `worklog-archive.md`.
- `system22plan.md` — the System 22 port plan (active work).
- `seam.md`, `roms.md`, `compatibility.md`, `feature-survey.md` — the tree as surveyed.
- `performance.md` — where the time goes, scoped to the Quest 3 target. §2a is the headline; §6.1 is the
  confirmed on-silicon per-device split.
- `ab-baselines.md` / `res-baselines.md` — what the renderers measure at. **Regenerate with the
  table scripts, never retype a digest** — the reason is at the top of each file.
- `legalstuff.md` — licence audit (release is clear; the open item is trademark, not copyright).
- Topic files for each shipped feature: `lightgun.md`, `steering-curve.md`,
  `blended-transparency.md`, `p5-internal-resolution.md`, `padmap-tool.md`, `user-options.md`, etc.

Add new docs here (one topic per file, kebab-case) and add a row to the README index. At a milestone
that changes the picture, drop a few `devnotes/screenshots/YYYY-MM-DD-<game>.png` — **from
`retrohost --vk`, never RetroArch** (RetroArch applies a P3 gamut conversion; the core writes
`(252,113,13)` and RetroArch presents `(252,131,43)`).

## Where the decisions live
Full plan + rationale is in the sibling research project **Polydiver** (proprietary; NOT part of this
repo). Launch with `--add-dir /Users/mcwildmacbookair/Documents/GitHub/Polydiver`.
- `…/Polydiver/PDDocs/model2/model2_libretro_core.md` — the Model 2 plan (P0–P6, architecture, A/B).
- `…/Polydiver/PDDocs/model2/model2_{lighting,shadows,culling,atlasing}.md` — decode/behavior specs.
- `…/Polydiver/PDTooling/model2/` — Python decoders = executable spec for texel/palette/luma/texparams.

## Key decisions
- **Model 2 seam:** tap `model2_renderer::model2_3d_render(polygon*, …)` at `model2_v.cpp:565`
  (mame0288). Per-poly texture/lighting params are already resolved into `m2_poly_extra_data`
  (`model2.h`). Model 2 is perspective-correct (`pz=1/z`) → no PS1 affine warp.
- **System 22 seam:** `poly3d_drawquad` + `render_sprite` + the three `render_scene` sites in
  `namcos22_v.cpp` (see `system22plan.md`).
- **Depth = draw order, not z** — reproduces `m_fillmap` exactly, cannot z-fight. Translucency is a
  cutout; `checker` is a stipple; `model2_transparency=blended` adds a deferred back-to-front pass.
- **Vulkan device ownership:** core creates its own device via HW-render context negotiation and
  renders into frontend-provided images. Links no Vulkan library — every entry point comes from the
  frontend's `vkGetInstanceProcAddr`. Committed SPIR-V is `--target-env=vulkan1.0`; the core is
  portable (proven on Adreno 740, see `devnotes/android.md`).
- **Mergeability golden rule:** all new logic in NEW files; the only edits to upstream files are
  a handful of guarded hook calls. 🚨 **The real upstream diff to mame0288, measured 2026-08-22, is
  135 insertions / 2 deletions across 5 files** (`scsp.cpp` +6, `scsp.h` +5/−2, `model2.flt` +1,
  `model2.cpp` +37, `model2_v.cpp` +86; everything in `src/osd/libretro_m2/` is a new file). Measure
  with `git diff --shortstat <newest merged release tag> -- src/devices src/mame`. 🚨 **That tag is
  `mame0289`, not `mame0288`** — 0289 has been merged, so measuring against 0288 reports the whole
  upstream release delta (4526 files) and is meaningless. Measured 2026-09-01 vs `mame0289`:
  **906 insertions / 18 deletions across 15 files**. **The old "still 30 lines" claim was
  false** — do not repeat any fixed diff-size number; quote the measurement or say nothing.

## Repo / upstream conventions
- Working branch **`main`**, anchored at release tag **`mame0288`** (NOT upstream `master`).
- Remotes: `origin` = the fork; `upstream` = `mamedev/mame`.
- **Sync = merge monthly release tags** (`mame0289`…) into `main`, rebuild, run A/B. Never track `master`.
- Own release tags use a prefix (`m2vk-v0.1`) to avoid clashing with upstream `mame*` tags.
- Never commit ROMs; `.inp` fixtures OK; golden PNGs via git-lfs.

### Local-ignore hack — RETIRED 2026-08-29
**Abandoned.** Modelizer is moving to its own repo (see `release_plan.md`), so the elaborate hiding of
`CLAUDE.md` + `devnotes/` from *this* fork's tree is no longer worth maintaining — don't re-do the
skip-worktree dance on the next upstream merge, and don't treat committing these as an emergency. The
commit-hygiene rules above (no AI/Claude nomenclature in commits/branches/code/PRs) still stand; only
the file-hiding mechanism is dropped. The existing skip-worktree flags can stay as-is or be cleared with
`git update-index --no-skip-worktree .gitignore` — either is fine. Original mechanism preserved below for
reference only.

`CLAUDE.md` and `devnotes/` are hidden from git by **`.gitignore` (working copy only) +
`git update-index --skip-worktree`**, with `.git/info/exclude` as a partial backstop. Both parts are
load-bearing. The **committed** `.gitignore` is pristine (a source tarball leaks nothing); the
**working** copy carries `/CLAUDE.md` + `/devnotes/` at lines 52–53 and is flagged skip-worktree so
the edit never enters the index.

`.git/info/exclude` **cannot** ignore `CLAUDE.md` on its own: upstream's `.gitignore` line 41
(`!/*.md`) un-ignores every root `.md`, and `.gitignore` beats `info/exclude`, so the pattern can only
win from *inside* `.gitignore` below line 41 — which is what lines 52–53 are. `devnotes/` is
double-covered by upstream's `/*/`, so a half-broken state hides `devnotes/` and pops only `CLAUDE.md`.
**Always check `CLAUDE.md` by name.**

Verify (all three, and `CLAUDE.md` by name):
```sh
git check-ignore -v CLAUDE.md          # must name .gitignore:52, NOT .git/info/exclude
git ls-files -v | grep -v '^H '        # must print: S .gitignore
git add -A --dry-run | grep -iE 'CLAUDE|devnotes'   # must print NOTHING
```

⚠️ **Upstream-merge dance:** `--no-skip-worktree` → merge → re-add the two lines → re-`--skip-worktree`.
The dangerous moment is the *recovery*: committing `.gitignore` with the two local lines still in it
publishes `/CLAUDE.md`. Run the three checks above before the first commit after any merge. A fresh
clone carries none of this — re-add the two lines and re-apply skip-worktree in any second working copy.

## Build + run
`REGENIE=1` after any script change:
```sh
make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10 # → ./modelizer_libretro.dylib
make SUBTARGET=modelizer OSD=sdl3 REGENIE=1 NOWERROR=1 -j10        # → ./mamemodelizer
./devnotes/build-retrohost.sh                                      # → ./devnotes/retrohost

# the core, without RetroArch: [--vk] <core> <rom> <frames> <out.ppm> [input script]
M2VK_POLYTAP=1 ./devnotes/retrohost ./modelizer_libretro.dylib devnotes/roms/vf2.zip 1100 /tmp/f.ppm

# retrohost as a Vulkan frontend — this is the A/B harness. Run both ways and compare the `digest:`
# line (covers every frame, not just the last):
M2OPT_model2_renderer=vulkan ./devnotes/retrohost --vk ./modelizer_libretro.dylib \
  devnotes/roms/vf2.zip 2500 /tmp/vk.ppm

# the standalone binary
M2VK_POLYTAP_EVERY=100 ./mamemodelizer vf2 \
  -rompath "devnotes/roms;/Users/mcwildmacbookair/Documents/GitHub/Polydiver/roms" \
  -video none -window -nomaximize -nothrottle -skip_gameinfo -str 65
```
The polygon tap is **off unless an `M2VK_POLYTAP*` variable is set**.

Android: `./devnotes/build-android.sh` → `model2_libretro_android.so`; `./devnotes/deploy-android.sh`.
The harness does NOT cross-compile — `retrohost`, `ab.sh`, all digests stay on the Mac. See
`devnotes/android.md`.

🎯 **Deploying core-only to the Quest 3 — set `M2VK_ANDROID_ROMDIR`:**
```sh
./devnotes/build-android.sh                                          # incremental (REGENIE=1 only after a scripts/ change)
M2VK_ANDROID_ROMDIR=/sdcard/Download ./devnotes/deploy-android.sh    # core only, no ROMs
```
`deploy-android.sh` resolves an SD-card ROM directory **unconditionally**, before it pushes the core,
by the fsLabel `RPFlip2` — which is the user's **Odin handheld** card, not the Quest. On the Quest that
lookup fails and the script exits (`no SD card labelled 'RPFlip2'`) **before the core is ever pushed**,
even though no ROM args were given. Setting `M2VK_ANDROID_ROMDIR` to any existing dir (`/sdcard/Download`)
satisfies the gate; with no ROM args nothing is written there but a harmless `mkdir -p`, and the core
still lands in RetroArch's `downloads/`. The Quest reports `video vulkan` and pkg `com.retroarch.aarch64`.
Then on the headset: **Load Core → Install or Restore a Core → `libmodelizer_libretro_android.so`**
(re-do this copy-into-place after every rebuild — RetroArch will not pick up the new `.so` otherwise).

### Run-invocation gotchas — each of these has already cost a session
1. **`-window` is mandatory.** Fullscreen is MAME's default and `-video none` still creates a window,
   so omitting `-window` blanks the user's display for the length of the run. Add `-nomaximize`.
2. 🚨 **`devnotes/roms` is the ONE ROM directory** (since 2026-07-31). The harness, the Desktop app,
   and the core's second rompath (`<system dir>/model2`, a symlink to it) all read it. No set is
   reachable only from outside it. Details in `roms.md`. ⚠️ The two LOOSE-FILE dirs `manxttc/` and
   `overrev/` each hold a BIOS the zip does not — part of the set. `vcop` needs `epr-17181.6` +
   `hd44780_a00.bin` (merged into its zip). Use `manxttc`/`von`/`hotd` (which are `manxttc`/`vonj`/`hotdo`
   sets), never `manxtt`/`vcop`-without-BIOS.
3. **Nothing renders for the first ~16 emulated seconds.** VF2's first 3D frame is ~16 s in. Use
   `-str 45`+ for anything real. No recording flag (`-aviwrite`/`-mngwrite`) is needed to force render.
4. **Screenshot before believing poly statistics.** A service/attract screen is itself hundreds of
   textured translucent quads, so it passes as plausible geometry. `devnotes/snap.lua` takes timed PNGs.
5. **RetroArch runs (cost most of a session at P2 step 4):**
   - Prefix with `caffeinate -dsu` — an idle macOS throttles the unfocused window and RetroArch stops
     calling `retro_run`, which reads exactly like the core hanging after a few hundred frames.
   - `pause_nonactive = "false"` in the `--appendconfig` file is mandatory — else the core advances ~3
     frames while the log looks healthy. **Read `Content ran for a total of` first**; if it's far
     shorter than `max-frames / 57.5`, nothing else in that run means anything.
   - `config_save_on_exit = "false"` in the same file — else `--appendconfig` values get written into
     the user's real `retroarch.cfg` on exit.
   - Core options are per-core in **`config/m2-vk/m2-vk.opt`** (⚠️ was `config/Model 2/Model 2.opt`
     before the 2026-08-07 rename; a stale script writing the old path pins nothing and the run inherits
     the menu's last choice). **Read `[model2] options:` before believing any result.**
   - There is no working way to screenshot RetroArch from a shell here (`screencapture` needs a
     permission; `GET_STATUS` segfaults RA 1.22.2). Use `M2VK_VK_DUMP=<prefix>` instead.
6. 🚨 **Host frame N is NOT emulated frame N** — every frame-indexed instrument counts `retro_run`
   calls and the emulated frame is `k + offset`, a **constant −1** (was a −6/−7 coin-flip before the
   2026-07-29 startup-frame fix). The read-out lags a script by a further ~4 frames.
7. **`retrohost --vk` specifics:**
   - `M2VK_POLYTAP_DUMP=N` counts *rendered* frames, not host frames (vf2 renders nothing for ~990).
   - Give every run its own `M2_SAVE_DIR` — two scripts sharing `./retrohost-save` cross each other's
     NVRAM and start from different emulated state (reads exactly like a renderer bug).
   - `env $e` and `for p in $pts` **do not word-split in zsh** — a run labelled `software` can silently
     run `vulkan`, and a sweep loop can silently script no input. Put sweeps in a `#!/usr/bin/env bash`
     script; read the core's own `[model2] options:` line.
   - Knobs (all optional): `M2VK_HOST_SYNC_MASK`, `M2VK_HOST_MASK_AT`, `M2VK_HOST_RESET_AT`,
     `M2VK_HOST_OPT_AT=<frame>:<key>=<value>` (a core option changing *while content runs* — the one
     frontend behaviour needed to test liveness; ⚠️ `M2OPT_<key>` in the env still wins, so don't pin
     the key you're scripting), `M2VK_HOST_SKIP_DESTROY=1`, `M2VK_HOST_RSS`, `M2VK_HOST_PERF=1`.

## Core options (12) — `DEFINITIONS[]` in `retro_options.cpp` is the authority
Every prose count in this file has been wrong at least once; check that array. Matching `M2VK_*`
switch **overrides its option**, never the reverse (the core logs a line when one is overriding). All
but the two marked apply **live**; `model2_renderer` and `model2_internal_res` need a content reload.
Full rationale in `devnotes/user-options.md`, `steering-curve.md`, `blended-transparency.md`,
`p5-internal-resolution.md`.

| Option | Default | Switch | Notes |
|---|---|---|---|
| `model2_renderer` | `vulkan` | — | `software` fallback. Reload. |
| `model2_diagnostic_input` | `None` | — | `None` / `L3 + R3` / `Hold Select`; the only way into a game's test mode (no MAME UI). Pared down 2026-08-27 from FBNeo's original 11-value list, which built most combos out of real gameplay buttons. |
| `model2_internal_res` | `496x384` | `M2VK_RES=<w>x<h>` | 9 absolute sizes to 2848×2136, Flycast-style. Reload. NOT supersampling — `M2VK_SS` is that and wins. |
| `model2_flat_shading` | `off` | `M2VK_FORCE_SOLID=2` | Both renderers. |
| `model2_flat_luma` | `off` | `M2VK_FLAT_LUMA=0\|1` | "No Lighting". Both renderers. |
| `model2_transparency` | `stipple` | `M2VK_BLEND=0\|1` | `blended` = real 50% + deferred pass. Vulkan only; accurate default. |
| `model2_steering_response` | `Slight` | `M2VK_STEER_GAMMA` | γ 1.0/1.3/1.7/2.2/3.0. |
| `model2_steering_deadzone` | `5%` | `M2VK_STEER_DEADZONE` | 0–20%. |
| `model2_steering_range` | `80%` | `M2VK_STEER_RANGE` | 100–60%. |
| `model2_steering_damp_drive` | `4` | `M2VK_STEER_DAMP_DRIVE` | frames-to-lock; input slew limit. |
| `model2_steering_damp_return` | `8` | `M2VK_STEER_DAMP_RETURN` | frames-to-recentre. |
| `model2_steering_display` | `off` | `M2VK_STEERBAR=0\|1` | Steering setup read-out bar; draws over the HUD. |

🚨 **The five steering options are the one group whose default is NOT the untouched path** — there is
no accuracy ground truth for a control the cabinet lacks. `M2VK_STEER_LINEAR=1` bypasses the whole
chain and is what a harness run pins itself with. They gate on the **detector** (the machine declaring
`IPT_PADDLE`, 30 of 90 GAME entries), not a table.

⚠️ **Liveness is checked with `M2VK_HOST_OPT_AT`, and the obvious version passes vacuously** — a live
change landing where the two static configs produce the same picture reads as "applied at load". Pick a
change point where the two static arms visibly differ afterwards.

## Harness / A/B
```sh
./devnotes/ab.sh <game> [frames] [outdir]        # four runs, background cmp, ppmdiff report
MODE="M2VK_FORCE_SOLID=2" ./devnotes/ab.sh vcop2 2500 /tmp/ab   # MODE applies a switch to BOTH renderers
./devnotes/ab-table.py /tmp/ab                   # regenerate ab-baselines.md — never retype a number

POINT=1 ./devnotes/res.sh vf2 2500 3 /tmp/res    # resolution-invariance: Vulkan vs itself at n×.
                                                 # POINT=1 + ODD scale carries a claim; a box resolve
                                                 # moves sample points and cannot.
./devnotes/perf.sh vf2 2500 1500 /tmp/perf       # 3 configs × 3 repeats; reports serial% and pipe%
```
Baselines for 12 fixtures in `ab-baselines.md` — read it before calling a number a regression.
`ppmdiff.py coverage` calls a pixel drawn when it differs from the `M2VK_NO_3D=1` reference (both
renderers produce that bit-identically — that's what makes the method valid). `ppmdiff.py exact` is
exit criterion 1.

⚠️ **`ppmdiff.py coverage` cannot tell "drew black" from "did not draw"** — an interior disagreement
over a `(0,0,0)` background where one side is `(2,0,0)` is a measurement artefact, not a bug. Print the
colours; cross-check with `M2VK_FORCE_SOLID=2`.

⚠️ A green `ab.sh` is evidence of nothing breaking, never of anything working — input changes no pixel.
For input, the working evidence is `M2VK_HOST_DESCRIPTORS`, digest sweeps, and the user's hand-check.

The key `M2VK_*` switches: `M2VK_SW_3D=1` puts MAME's rasteriser back in charge (bit-exact vs
`renderer=software`); `M2VK_NO_3D=1` background-only reference; `M2VK_OPAQUE_ONLY=1` opaque-path guard;
`M2VK_ONLY_POLY=<n>` one polygon; `M2VK_NO_EARLY_Z=1` the one *pure no-op* switch (must not move a
pixel); `M2VK_NO_RETICLE=1`, `M2VK_NO_SCISSOR=1`, `M2VK_SS=<n>`, `M2VK_RES=<w>x<h>`.
`M2VK_LAZY_BAUD=0` restores the stock 500 kHz i8251 `CLOCK` — the demand-gated baud clock ships as the
`model2_lazy_baud` core option ("Fast Sound-Link Timing", **default ON**, reload-gated, Model 2 + Model 1
menus only) and is worth 35–48 % of core ms/frame ([devnotes/lazy-baud.md](devnotes/lazy-baud.md)).
⚠️ `retrohost` pins `model2_self_throttle` off for itself — that option now ships ON and maps to MAME's
`-throttle`, which would cap every harness run at 1×.
**Full reference: [devnotes/switches.md](devnotes/switches.md)** — that file carries the count; do not
quote one here.

## 🎮 Just playing it? None of the harness commands. Use `~/Desktop/Model 2.app`
**Playing and measuring must not share a command line** — every invocation above pins options and
passes switches, which is wrong for a session where the options menu is meant to be in charge. The app
launches RetroArch with no flags and `env -u`'s every `M2VK_*`. The cores dir holds a **symlink** to
the repo build.

🚨 **The installed-core symlink keeps reverting to a plain copy** (recorded at least four times; a
RetroArch core-updater path is the suspect). The failure is silent — the copy is byte-identical when it
reverts, so it looks fine until the *next* rebuild, at which point you play a stale core while
`git status` and the build log both look healthy. **Check it before concluding a change "does not work"
in the app:**
```sh
ls -la ~/Library/Application\ Support/RetroArch/cores/model2_libretro.dylib   # must print '-> …/mame-model2-vk/…'
```
(Note: `~/Library/Application Support/RetroArch/cores/`, not `~/Documents/RetroArch/cores/`.)

## Quest 3 port — LIVE and hitting frame rate (updated 2026-09-05)
The 2026-07-27 "SHELVED" decision is **overtaken and no longer applies.** The arm64 core runs on the
Quest 3's Adreno 740 and hits the target: **57.5 fps locked outside the heaviest scenes, ~56 fps
worst-case** (worklog 2026-09-01). The two levers that got it there both shipped as core options —
`model2_lazy_baud` (−37 % to −57 % of core ms/frame, whole Model 2 family) and `model2_sound_thread`
(model2o only; 49.96 → 57.86 fps on the daytona heavy race). Performance work **is** live; the profiled
per-device split is real Quest silicon (`retroarch-quest-perf.md` §4.1, `performance.md` §6.1). Correctness
still gates on the host harness — the Quest is a performance instrument only, no per-frame digest.

## Shelved / do-not-re-propose
- **The fork README is the user's to write** — it stays the open item on public release
  (`legalstuff.md` §5.2, §9). Do not draft it unasked.
- Closed and not to be re-proposed: widescreen/FOV widening, wireframe, scanline/CRT filters,
  interpolated z, networked cabinets, the steering Rate mode (built and removed 2026-08-08).

## Releasing a public binary is CLEAR — the open item is trademark, not licence
`devnotes/legalstuff.md` is the audit; §9 is the release checklist. 585 of 606 linked `src/` objects
are BSD-3-Clause, **zero GPL-tagged**; two LGPL-2.1+ files are satisfied by the public repo; all
third-party libs permissive. Release as GPL-2.0-or-later, tag the building commit, ship
`COPYING` + `docs/legal/`. The repo rename to `m2-vk` closed the repository half of the trademark
question; `library_name = "m2-vk"` keeps MAME branding out of RetroArch. Ship the core only, not the
standalone (`3rdparty/astc-encoder` is Apache-2.0, live only if `mamemodelizer` is ever released).
