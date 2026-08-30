# Worklog

Append-only. Newest at the bottom. Absolute dates. **One entry per real milestone, not per thought** —
no corrections-of-corrections, no "SUPERSEDED" archaeology. When a fact stops being true, edit it in
place or delete it.

Closed history lives in [worklog-archive.md](worklog-archive.md):
- **Model 2** (P0 → steering block), committed at `aabcd8d5cac`.
- **System 22 / Super System 22** (S0 → sprites + `system22_texture_filter`), committed at `6e62265dff6`.
- **System 21** (T0 → T5 Winning Run + savestates/pads/A-B), committed at `6e62265dff6`.

This file continues from the **shippable pass** — the work that remains to turn three complete renderers
into a public release. The scheduled queue is [shippable-plan.md](shippable-plan.md) (R0 triage → R1
input mapping → R2 savestates/compat → R3 options → R4 polish → R5 release). Renderer/geometry work is
done across all three families.

Upstream diff vs `mame0288`, measured 2026-08-25: **457 insertions / 16 deletions across 11 files**
(`git diff --shortstat mame0288 -- src/devices src/mame`). Quote the measurement or say nothing.

---

**2026-08-25 — R1 System 22 input mapping, all 18 port sets authored.** The padmap sweep/editor were
already family-aware (2026-08-22); this pass authored the 12 port sets that were still on the generic
hedge — `adillor`, `airco22b`, `alpiner` (Alpine Racer + Alpine Racer 2, one ipt block), `alpines`,
`aquajet`, `cybrcomm`, `cybrcycc`, `propcycl`, `raverace`, `timecris`, `tokyowar` — plus fixed `ridgeracf`
(was a meaningless numbered-button stub; re-derived from source since it has no local ROM,
`MACHINE_NOT_WORKING`) and folded `ridgerac3m` into `ridgera`'s row (it `PORT_INCLUDE`s `ridgera`
verbatim). 43 rows / 50 sets total. Two real findings from reading source/dumps rather than guessing:
`cybrcomm` is a single-cabinet twin-stick mech (`PORT_NAME` comment: "placed on both sticks"), same
shape as System 21's `cybsled` — reused `twin_ad_stick`. The Alpine Racer family's two independent
leg-lean axes are tagged P1/P2 exactly like `cybsled`'s treads, so the same flag puts the second leg on
the right stick. And `tokyowar` is a wheel-and-pedals tank sim, **not** a lightgun cabinet — the
shippable-plan bucketed it with `timecris` under "gun games", which the actual `INPUT_PORTS` block
(`flags.lightgun: false`) contradicts.

`padmap-gen.py --check` and `padmap-test.sh` both green (the "4 paddle port sets still on the generic
hedge" note is gone). `M2VK_HOST_DESCRIPTORS` read on all 12 rebuilt sets, output matches every label
authored. Build: `namcos22_libretro.dylib`. No upstream diff touched (input-only). Hand-check list is
the user's, next.

---

**2026-08-25 — R2 done: S22 savestates PASS, combined compat matrix built, SS22 vk crash found.**
Savestates: `state.sh` over the three representative S22 fixtures — `ridgerac` (racer), `raverace`
(fogged SS22), `timecris` (sprite-heavy SS22) — **all PASS** (`C==D`, `N!=D`, deterministic, `A==R`),
no `gen_fifo`-class regression; the framework is confirmed driver-agnostic. Compat: rewrote
`compatibility.md` as one Track-D matrix over all three families (50 rows, sorted by driver), Model 2
folded in per the user's call. `On GPU` measured for every row — Model 2's 11 from `ab-baselines`, the
other 18 Model 2 games + all S22/SS22 from a fresh `retrohost --vk` / `ppmdiff coverage` batch (bg
`M2VK_NO_3D`, sw `M2VK_SW_3D`, vk default). Model 2 all 0.9930–1.0000; S22/SS22 all 1.0000 except the
translucent-HUD-overlay residuals acedrive 0.8677 / alpinerd 0.9198 / victlap 0.9821 (screenshot-
confirmed geometry-identical, only the message backdrop composites differently). Model 2 play-test
notes preserved below the matrix and mapped into the Key-map/Notes columns.

🚨 Finding (R0 triage): the **Super System 22 Vulkan renderer hard-crashes on `cybrcycc`, `alpines`,
`alpinr2b`** — renders ~60 frames then dies with no error output before ~frame 200; the software path
(`M2VK_SW_3D`) completes all 3000. Reproducible standalone (not harness contention) and not
machine-class-specific (`aquajet` shares cybrcycc's machine and is 1.0000; `alpinerd` shares alpinr2b's
`alpine` machine and renders). Cause not isolated — a game-specific state reached shortly after boot is
the suspect. Marked `fails` in the matrix; the other 14 S22/SS22 parents render on GPU.

**2026-08-25 — R4.5 done: joystick gear-shift routed onto L1/R1 for the S22/S21 racers.** On these
cabinets the shift is `IPT_JOYSTICK_UP/DOWN`, not a numbered button, so the layout table (numbered
buttons only) could never reach it and it sat on the d-pad alone. `padmap-gen.py` now detects it from the
sweep dumps (`joy_shifter_portsets()`, mirroring `paddle_portsets()` — a P1 `IPT_JOYSTICK_UP`+`DOWN`
named "Shift …"), sets a new `joy_shifter` struct bool on every row naming that port set, and injects the
R/L descriptors from the dump field names — nothing hand-authored, `input_layouts.json` untouched. Seven
rows flagged (`acedrive`, `dirtdash` via its `dirtdashj` clone dump, `ridgera`, `ridgera2`, `victlap`,
`winrun`+`winrungp`, `raverace`). `configure()` binds `IPT_JOYSTICK_DOWN`→R1 (upshift, "Shift Up") and
`IPT_JOYSTICK_UP`→L1 (downshift) when the flag is set — additive, since `apply_device_defaults()` ORs a
device's assignments per type, so the d-pad keeps shifting and Ridge keeps its H-gate lane change on the
d-pad. New `BUTTON_L`/`BUTTON_R` fixed items (ITEM_ID_BUTTON13/14) carry L1/R1; inert on every other set.
Three cores build, `padmap-gen.py --check` green, daytona's descriptor dump unchanged (negative control:
no id=10/11, GEAR still on numbered buttons). No S22/S21 racer ROM is local, so the affected-set
`M2VK_HOST_DESCRIPTORS` could not run — `.ipp` emission verified statically (R="Shift Up", L="Shift Down"
on the seven rows). User hand-check pending.

---

**2026-08-25 — R0: SS22 vk crash fixed (short "textile" region over-read).**
The Super System 22 Vulkan hard-crash on `cybrcycc`, `alpines`, `alpinr2b` was a buffer
over-**read**, not a Vulkan bug. `s22::upload_static` copied a hardcoded `TTDATA_BYTES` (0x1000000)
from `t.ttdata` (= gfx(1) "textile", RAW layout). Those three are the only S22/SS22 parents whose
`textile` ROM_REGION is short of 16MB — cybrcycc `0xe00000`, alpinr2b `0xc00000`, alpines
`0xa00000` — so the tile-data upload on the first 3D frame (`upload_static`, ~frame 60) walked
2–6MB off the end → EXC_BAD_ACCESS in `_platform_memmove`. The lldb backtrace put it in
`s22::geom_upload` → the inlined `upload_static` memcpy; the fault source was `ttdata + 0xe00000`,
exactly the short region's end. Read as "renders ~60 frames then dies"; the sw path never uploads, so it
was unaffected.

Fix (4 files): thread the region's real byte length across the seam (`texture_ram::ttdata_bytes`, set
from `memregion("textile")->bytes()` in `namcos22_v.cpp`); `upload_static` copies
`min(actual, TTDATA_BYTES)` and zero-fills the tail of the always-16MB GPU buffer so an out-of-range
tile index fetches pen 0. Verified: all three complete 3000 frames on `--vk` (cybrcycc renders the full
bike/track scene, screenshot `screenshots/2026-08-25-cybrcycc-vk-fixed.png`); full-region path
unchanged — ridgerac 1800f digest `000263dec4db0fa1` == baseline, aquajet clean. Upstream diff vs
`mame0288` now: ** 11 files changed, 461 insertions(+), 16 deletions(-)** (`git diff --shortstat mame0288 -- src/devices src/mame`).

---

**2026-08-25 — R3 System 22 option set + polygon counter.** Five new toggles, all live, each a
`DEFINITIONS[]` entry + `M2VK_*` switch. Detail in [shippable-plan.md](shippable-plan.md) R3. Renderer
side: three `s22.frag` `poly_flags` bits (`PFLAG_NO_FOG`/`NO_TEX`/`NO_LIGHT`) — no push-block size change,
regenerated `s22_frag_spv.h` only. `system22_fog` (default on) skips the fog blends; `system22_no_textures`
whitewashes the surface to white so shade renders it greyscale (S22 luma-only shading → true greyscale);
**`model2_flat_luma` now actually works on S22** (was a dead menu entry — the S22 shade tail never read it)
and Flat Shading is hidden from the S22 menu; `system22_2d_overlay` gates the HUD/text sandwich via
`over_pixels()`→null. The **polygon counter** (`model2_poly_counter`, all three families) is a new
`counter.frag` (3×5 bitmap font, scissored fullscreen triangle) + `s_pipeline_counter` built alongside the
steerbar, drawing the active family's per-frame primitive count top-right; count pulled via new
`geom_primitive_count()`/`geom_frame_polys()` getters selected by draw flag in `record_and_submit`.
Vulkan-only. All changes are in the OSD (new files: none in `src/mame`/`src/devices`; upstream diff
unchanged). Verified: three cores build; per-arm digests differ; screenshots cybrcycc (fog/notex/nolight/
hud-off), vf2 (counter=519), starblad (counter=617). Dropped from the original R3 list: gamma bypass
(user's call); `system22_depth_buffer` left as-is. **User hand-check pending.**

Also added **per-game menu hygiene**: the six steering options now hide themselves on any machine without a
wheel (gun/fighting games, all of System 21), via a new `m2opt::set_option_display()`
(`RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY`) called once in `retro_run` the first frame after the
`IPT_PADDLE` detector resolves (`m2vk::steer().resolved`), showing/hiding by `.active` so a reload between a
wheel and a non-wheel game settles both ways. The steering *effect* was already gated on that detector;
this hides the dead entries to match. Visibility only — a hidden option still reads its default, so no
harness pin moves. `hide_option()` (family-level, declare-time) is unchanged; this is the game-level layer
it could not do. Verified via the `[model2] steering options shown/hidden` log: ridgerac/daytona shown,
timecris/vf2/starblad hidden.

**2026-08-26 — Analog deadzone option for non-wheel games (`model2_analog_deadzone`).** MAME applies a
fixed 0.15 joystick deadzone + 0.85 saturation to every ABSOLUTE item on a `DEVICE_CLASS_JOYSTICK`
device (`inputdev.cpp:475` `adjust_absolute_value`), so every pad-stick analog axis — Star Blade's aim,
the flight/twin-stick sets, the racers' pedals — ate a 15% dead band. The wheel games already cancel it
via the steering pre-compensation; nothing did for the aim sticks, which read as squidgy off-centre.
New option: `model2_analog_deadzone` (0/5/10/15%, default 5%), shared across all three families. New
`m2vk::analog_shape()` in `m2vk_steer.h` pre-compensates MAME's 0.15/0.85 on the two thumbsticks so the
*effective* deadzone is the option value while **MAME's saturation knee is preserved** — only the dead
band moves. Applied to BOTH axes of each stick (an aim stick uses X and Y equally), unlike the wheel path
which is X-only. Gated on `!steer().active` (wheel games keep the steering path) and `steer().resolved`;
`M2VK_ANALOG_DEADZONE` switch overrides, same presence-wins rule as the wheel switches. Menu visibility is
the wheel options' complement: shown on non-wheel games, hidden on wheel games. Verified: three cores
build; option declares + logs `model2_analog_deadzone=5%`; standalone transfer-function check confirms 5%
responds off-centre, 0% continuous from centre, and **15% is byte-for-byte identical to the pre-option
path (max diff 0 over the full sweep)**. Zero digest impact — no fixture scripts an analog axis and a
centred stick returns a hard 0 at every setting. starblad.zip is empty in this sandbox, so the in-game
feel + the visibility log line are **user hand-check pending**.

**2026-08-26 — Analog saturation override (`model2_analog_saturation`).** Companion to the deadzone
option. MAME's fixed 0.85 joystick saturation rescales so the control tops out at 85% of host-stick
throw — an aim reticle reaches the screen edge on a partial push. New option `model2_analog_saturation`
(70/80/85/90/95/100%, default 85% = MAME, unchanged), shared across the three families, shown on the
non-wheel games beside Analog Deadzone. `analog_shape()` now takes the outer knee from `s.analog_sat`
instead of MAME's captured 0.85: `a = (mag - dz_opt)/(sat_opt - dz_opt)`, still pre-compensated so MAME's
own 0.15/0.85 reproduces "dead below dz_opt, linear, full at sat_opt". At (15%, 85%) the whole thing is
still the exact identity (max diff 0). `M2VK_ANALOG_SATURATION` overrides. Verified: three cores build;
both options log (`model2_analog_deadzone=5% model2_analog_saturation=85%`) and the visibility line fires
on vf2 ("steering options hidden, analog deadzone shown"); transfer-function check confirms sat=100% makes
full deflection require full throw (50% stick → 47%), sat=85% unchanged. Default left at 85% (a
taste/hardware call with no accuracy ground truth, unlike the deadzone defect) — user to pick the value by
hand-check. Background on the numbers: MAME's dz/sat are host-controller calibration (potentiometer/
hall-sensor rest-noise and inability to reach electrical extremes), NOT the arcade hardware; they predate
this core and apply to whatever pad is plugged in now. **User hand-check pending.**

**2026-08-26 — Analog Deadzone + Reach BUILT (`m2vk_analog.h`), per analog-deadzone-reach-plan.md.**
⚠️ Reconciles the two earlier 2026-08-26 worklog entries above (`model2_analog_deadzone` +
`model2_analog_saturation`): that prior attempt's code was **not in the tree** — HEAD `5e06471a08e` was
clean, `m2vk_analog.h` did not exist, `retro_options.cpp` had no analog options — so those entries
describe work that had been reverted/lost. The plan doc (newer, "Analog Reach chosen over …Saturation on
2026-08-26") is authoritative; this build follows it. Same transform as the saturation version
(`a=(mag−dz)/(knee−dz)`, pre-compensated against MAME's 0.15/0.85), renamed knee **reach** with values
{100,95,90,85,80,75}% **default 100%** (removes MAME's 85% early-saturation; the saturation entry kept
85%). Deadzone {0,2,5,10,15}% default 5%.
New file `m2vk_analog.h` mirrors `m2vk_steer.h` (no gamma/damping/read-out); detector = any
`IPT_AD_STICK_X/Y/Z`; shaper on the four stick axes in `libretro_m2_pad_device::shape_analog()`, live via
`set_option_analog()`, switches `M2VK_ANALOG_DEADZONE/REACH/LINEAR` (fractions), per-game visibility block
in `retro_run` gated on `analog().active`, hidden from wheel/gun/fighter menus.
🚨 **desert (Desert Tank) breaks the plan's mutual-exclusivity premise** — declares BOTH `P1_PADDLE`
(left X) AND `P1_AD_STICK_Y` (left Y), so both detectors fire. `shape_analog()` skips left X when
`steer().active` so the paddle isn't double-shaped; desert's AD_STICK_Y (throttle, rests near min) is
still shaped (harmless, noted). Three cores build clean. Static: pre-comp identity holds (full stick →
MAME full lock; first non-zero at 0.15 floor), monotonic/symmetric, diagonals preserved (x==y⇒out_x==out_y).
Visibility confirmed: shown skytargt/gunblade/waverunr/desert AND the real S21/S22 targets
**starblad, cybsled, cybrcomm, propcycl** (ROMs are in `devnotes/roms/system22/` — I had wrongly said
starblad was absent; run starblad/cybsled with the namcos21 core, the S22 sets with namcos22). Dumps:
starblad AD_STICK_X/Y (centred aim), cybsled/cybrcomm P1+P2 X/Y (twin-stick), propcycl X/Y + Z "Cycle
Pedal". Hidden daytona/vf2/von/motoraid/powsled. `ab.sh vf2` criterion 1 still exact → **in-game feel is
the only open item, user hand-check pending.**

**2026-08-27 — Modelizer M0: runtime family detection keyed off driver source file.** Pre-empts the
merged-core misdetection bug (all three flagships present → every game reads as System 22). Added
`family_of(const std::string&)` in `retro_entry.cpp`: `driver_list::find(system)` → `driver().type.source()`
→ substring-match `namcos22`/`namcos21`(covers `namcos21_c67.cpp`)/else `model2`. Robust to any build path
prefix; MAME's own info_xml_creator matches the same way. Collision check first: no set name shared across
`sega/model2.cpp` (90 rows), `namco/namcos22.cpp` (42), `namcos21.cpp` (3), `namcos21_c67.cpp` (7) — name
lookup is unambiguous, no disambiguation needed. Swapped the four runtime (load/run) family decisions from
the `driver_list::find(<flagship>)>=0` idiom to `family_of(system)`: the S22 live-options push, the S22 seam
arm, the S21 seam arm, and the `family_dir` rompath selector. The retro_run live-options handler has no
`system` in scope, so family is cached in a file-scope `s_family` at load and reset on unload beside the
steering/analog display flags. **The one family decision left on table-content detection is the
`retro_set_environment` menu cascade** (per-family option-hide + native-res default) — it runs at declare()
time before any set loads, so it has no running driver to key off; deferred to M1 by decision, where the
merged menu is built and the gating moves to load time via the SET_CORE_OPTIONS_DISPLAY pattern already used
for the steering/analog detectors. That site changes no rendered pixel (option values and native-res read
from the live `DEFINITIONS[]` at load, not from what the menu was told), so it is safe to leave. Verified:
all three subtargets rebuild clean; `retrohost --vk` 2500-frame digests byte-identical to pre-M0 baselines —
vf2 `de94f44a06151f71`, ridgerac `7bb336b41e2e2747`, starblad `7080003fbe32aef1`. Menu gating unchanged by
construction (set_environment cascade + retro_options.cpp untouched). Only `retro_entry.cpp` changed; no
upstream file touched. **M0 exit met; M1 (build merge) unblocked.**

**2026-08-27 — Modelizer M1: unified core (one dylib, three families) + launcher wired.** Additive, not
destructive — the three per-family subtargets still build unchanged; `modelizer` is added alongside.
New `src/mame/modelizer.flt` (union of the four driver cpps) drives the driver list (142 drivers =
90+42+3+7, the collision-check total); new `scripts/target/mame/modelizer.lua` unions the three per-family
target scripts, keeping a separate driver project + scoping define (M2VK/S22VK/S21VK) per family so the
_v.cpp seam hooks stay scoped. One de-dup vs a literal concat: `namco_dsp.cpp` is in both Namco file lists
→ compiled once (mame_namcos22), namcos21's refs resolve at final link (else duplicate device
registration). `libretro_m2.lua` Android soname now tracks `_subtarget` (was hardcoded model2). Build:
`make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10` → `modelizer_libretro.dylib`.

🚨 **The M0-deferred set_environment cascade had to be finished here — the merge surfaced it exactly as
predicted.** First unified run: ridgerac matched baseline but vf2/starblad differed. Cause: the cascade
keyed on `driver_list::find("ridgerac")>=0`, now true for every game → S22's 640x480 native forced on all
families. Moved the cascade (native size + per-family option hiding) out of retro_set_environment into
retro_load_game, keyed on `family_of(system)`. Second attempt (declare at set_environment + redeclare at
load) STILL broke ridgerac/starblad in BOTH three-core and unified: a frontend caches the FIRST
declaration's option values and ignores the redeclare, so the renderer read the stale 496x384 (confirmed
in the `[model2] options:` line). Fix: **declare exactly once, at load, after the family is known** — no
set_environment declare. Any frontend then either reads the load-time declaration or falls back to
`default_value()` (DEFINITIONS, patched by set_native_resolution at load); both give the family native.
Added `m2opt::clear_hidden()` + `redeclare()` (rebuilds the cached def table) for the multi-load reset.
Verified: all three subtargets AND modelizer rebuild clean; `retrohost --vk` 2500-frame digests
byte-identical to baselines in BOTH the three-core builds (no regression) and the unified core —
vf2 `de94f44a06151f71`, ridgerac `7bb336b41e2e2747`, starblad `7080003fbe32aef1`.

`devnotes/shortcuts/Game Launcher.command` rewired: every pick (and the bare "open RetroArch" choice) now
installs+launches `modelizer_libretro.dylib` via `launch_secondary_core`, family detected at runtime; only
the ROM dir differs per family. Deferred (not done): retiring the three old subtargets (M1 step 4 — large
blast radius: CLAUDE.md, ab.sh/res.sh/perf.sh, the other .command shortcuts, Android); the Modelizer rename
(M2 — library_name/option keys/.info, the Desktop `Model 2.app` and installed symlink still point at
model2_libretro). **Open: user hand-check that RetroArch loads the unified core and the per-family option
menus look right (retrohost can't exercise the RA menu).**

---

## 2026-08-27 — Retired the three per-family subtargets (modelizer M1 step 4)

`SUBTARGET=modelizer` is now the only build. Deleted `src/mame/{model2,namcos22,namcos21}.flt` and
`scripts/target/mame/{model2,namcos22,namcos21}.lua`; `SUBTARGET=model2|namcos22|namcos21` no longer
resolves a target. In `scripts/src/osd/libretro_m2.lua` the core name is now the constant
`modelizer_libretro` (dylib + Android soname), replacing `_subtarget .. "_libretro"`. Regenie clean;
full build links `modelizer_libretro.dylib` (exit 0).

Repointed the retrohost harness off the old basename — `ab.sh`/`res.sh`/`perf.sh`/`state.sh` `CORE`
default → `modelizer_libretro.dylib` — and `build-android.sh` → `SUBTARGET=modelizer`,
`OUT=modelizer_libretro_android.so`. CLAUDE.md build block and `vk_funcs.h` comment updated.

**Deliberately left on the old name (M2 branding scope — coupled to the not-yet-created
`modelizer_libretro.info`):** the installed-core symlink, `~/Desktop/Model 2.app`, `.vscode/settings.json`,
and the RetroArch-launching shortcuts `retroarch.sh` / `Model 2 Steering.command` /
`System 22 Fog A-B.command` (their `SUBTARGET=model2|namcos22` build lines are now dead until M2 repoints
them). The Game Launcher already installs `modelizer_libretro.dylib` via `launch_secondary_core`.

---

## 2026-08-29 — M1-0: Sega Model 1 boots in the modelizer core (software)

Added Model 1 as the fourth family's build plumbing — **no seam yet, no `model1_v.cpp` edits**.
`sega/model1.cpp` added to `src/mame/modelizer.flt`; a `mame_model1` project block added to
`scripts/target/mame/modelizer.lua` with an `M1VK` scoping define (in place for M1-1; no hooks
consume it yet). Deps from `makedep.py sourcesproject`: union gained CPUS `V60`+`I386`, MACHINES
`AM9517A`+`MB89374`; sounds/videos/buses all already present. **Dedup like namco_dsp:** Model 1
shares six sources with Model 2 (`segaic24`, `segam1audio`, `dsbz80`, `model1io`, `model1io2`,
`315_5338a`) — compiled once in `mame_model2`; `mame_model1` compiles only its four unique sources
(`model1`, `model1_m`, `model1_v`, `m1comm`) and resolves the shared devices at link.

Build clean (`REGENIE=1`, 152 drivers). ROMs copied Polydiver→`devnotes/roms/model1/` (vf, vr, swa).
Software boot via retrohost, 1800 frames each: **vf** `6ed76ed4b9319aa5` (99.2% coverage — fighter +
sky tile + floor, correct), **vr** `36a579ace7200017` (96.0% — track/mountains/cars, correct),
**swa** `b055c11a6680738a` (32.2% — menu frame). VF/VR screenshots match stock MAME software output.

`family_of()` untouched: `model1.cpp`'s source token matches neither `namcos22` nor `namcos21`, so
Model 1 falls through to `family::model2` (the `[model2]` log line confirms) — as planned until M1-1
adds the branch. The three existing families route unchanged. Tree otherwise clean.

**Next: M1-1 (the seam)** — `m1_seam.{h,cpp}`, the two `#ifdef M1VK` sites in `model1_v.cpp`
(`draw_quads` tap + frame bracket), observation-only, output byte-identical.

---

## 2026-08-29 — M1-1: Sega Model 1 seam (observation-only)

The merge firewall for Model 1. New `src/osd/libretro_m2/m1_seam.{h,cpp}` on the s21_seam template,
listed in the shared OSD (`scripts/src/osd/libretro_m2.lua`) so `m1::` symbols resolve in every build,
inert until a Model 1 driver arms them. Three `#ifdef M1VK` hooks in `model1_v.cpp` (+40 insertions, 0
deletions — the whole Model 1 upstream footprint):
  * include `libretro_m2/m1_seam.h`;
  * in `draw_quads()`'s per-quad loop: `m1::submit_quad(s.x/s.y, z, col)` in sorted paint order, then a
    `!m1::sw_owns_3d() → continue` guard that suppresses the software `fill_quad` when the GPU owns the 3D
    (always false at M1-1, so it never trips);
  * `m1::frame_begin()/frame_end()` bracketing the 2D-under/3D/2D-over sequence in
    `screen_update_model1`. The bracket lives in screen_update, NOT draw_quads, because draw_quads fires
    more than once per frame (draw_objects' sorted quads, then draw_direct's direct quads).

Seam data shape: 4 int16 screen corners (`spoint s.x/s.y`), the float sort z, and `col` — the value
fill_quad writes to the bitmap: `0x00RRGGBB | (MOIRE 0x01000000)`. No CLUT crosses the seam (unlike
S21) — the colour is already resolved RGB; the MOIRE bit is the translucency stipple flag M1-2 will
reproduce like Model 2's `checker`. Two captures are deliberately deferred: the pre-luma albedo for "No
Lighting" (needs a quad_t field; built at M1-5) and the float sub-pixel pixel for hi-res (two projection
paths compute it differently; designed at M1-3). M1-1 keeps to guarded hook calls with no struct edits.

Verify (retrohost software, 1800 frames): tap OFF byte-identical to the M1-0 baselines — vf
`6ed76ed4b9319aa5`, vr `36a579ace7200017`, swa `b055c11a6680738a`. Tap ON (`M2VK_M1TAP_EVERY=300`)
fires: vf ~2300–2600 quads/frame, x 0..495, y in-range, positive z, moiré counted — and the digest is
STILL `6ed76ed4b9319aa5`, so the tap changes no pixel. `set_gpu`/`set_no_3d`/`gpu_owns_3d` are declared
(the gating surface) but nothing calls them yet; `submit` drives only the tap. `m1_geom` record consumer
lands at M1-2.

**Next: M1-2 (GPU geometry)** — `renderer_vk/m1_geom.{cpp,h}`, flat quads two-tris-each in submission
order, NO z attachment (draw order is the accurate model; a z-buffer would z-fight coplanar decals),
moiré→stipple, wire `m1::set_gpu/set_no_3d` into the OSD dispatch, A/B vs `M2VK_SW_3D`.

---

## 2026-08-29 — M1-2: Sega Model 1 3D on the GPU (untextured, painter's)

The 3D now renders on the GPU. New `renderer_vk/m1_geom.{h,cpp}` + `shaders/m1.{vert,frag}` — a stripped
s22_geom: no textures, no palette/CLUT, no descriptor sets (the flat 0x00RRGGBB rides the vertex), no
per-quad clip windows, depth test OFF. Model 1's stream is already sort_quads' back-to-front, so it is a
plain painter's pass: record order, last writer wins. MOIRE translucency (col bit 24) → a `(x^y)&1`
stipple discard in the fragment, quantised by a stipple divisor so it stays one bitmap pixel at raised
internal res. **Zero new `model1_v.cpp` edits** — the M1-1 seam already carried the stream; upstream
driver diff still +40/0. All M1-2 work is OSD-side.

Wiring: `m1::set_gpu/set_no_3d` gated on `family::model1` in retro_entry (enum + `family_of()` now detect
the `model1` source token, and the rompath leaf is `model1`). vk_present threads a `draw_3d_m1` flag
through `record_and_submit` — geom_build/upload/draw/destroy/forget/end_run + the poly counter — mirroring
S22. Model 1 rides the passthrough background (`pixels`, sw-3D stripped by the seam); the 2D-over HUD
re-composite is deferred to M1-4. Registered m1_geom.cpp + the two spv headers in libretro_m2.lua.

A/B (retrohost --vk, vf, 1800f, native 496x384; bg=M2VK_NO_3D, sw=M2VK_SW_3D):
- vk renders vf and vr correctly (screenshots): fighter/floor/sky, track/mountains/cars.
- `ppmdiff coverage` agreement **0.9147**; 88.5% of the overlap pixel-identical. Full pixel diff vk-vs-sw =
  12983 px (6.8%), classified: **5021 = the 2D-over HUD** vk's 3D covers (the planned M1-4 gap — yellow
  text where sw shows it on top, vk shows 3D); **806 = floor not covering the bottom/right screen edge**
  (the whole y=383 row — integer-coord fill-rule, ~1px); **7156 = polygon-edge different-colour** (fill-rule
  at silhouettes; VF is finely tessellated). No moiré phase bug (diff parity ~even, not a checkerboard).
  Geometry itself is correct; the residual is the deferred 2D + the integer-seam edge rounding M1-3 addresses.

**Next: M1-3 (high resolution)** — capture the float projected pixel at the seam (two projection paths:
project_point with zoom, project_point_direct without), scale by the internal-res factor, reuse the P5
present path; this also fixes the bottom-edge fill-rule row. Then M1-4 (2D-over HUD composite).

---

## 2026-08-29 — M1-3: Sega Model 1 high internal resolution (float seam capture)

The seam now carries the **float projected pixel** instead of the rounded `spoint s.x/s.y`, so the GPU
rasterises sub-pixel and a raised internal resolution genuinely supersamples. `m1::quad` corners are
`float x[4]/y[4]`; `submit_quad` takes floats; `m1_geom`'s vertex drops its int→float cast.

**Recompute mechanism (no struct edit to `point_t`).** The two projection paths store `p->xx/yy` but map
them to a pixel differently — `project_point` applies zoom+view (`s = xc/yc ± (xx*zoom + view)`),
`project_point_direct` does not (`s = xc/yc ± xx`) — and `draw_quads` doesn't know which produced a point.
But each `draw_quads()` call is **homogeneous**: `draw_objects` flushes only `project_point` quads,
`draw_direct` only `project_point_direct` quads. So the guarded `#ifdef M1VK` hook recomputes **both**
candidate float pixels from the live `view_t` and takes the one whose truncation reproduces the stored
integer `s` (degenerate z≤0 direct case, `s` forced to 0 / `xx` stale, matches neither → integer
fallback, i.e. no worse than the M1-2 integer capture). Driver footprint unchanged in hook count — the
recompute grew the one existing quad-tap block; still the only edited upstream driver file.

**Hi-res is free from there.** `vk_present` already passes the visible half-extent (`s_width/s_height`)
as the vertex→NDC divisor and the raised `draw_width/draw_height` as the viewport, so the same NDC scales
to any internal size. `model2_internal_res` already flows to `m1::geom_draw`; only the "(Native)" label
was Model-2-worded. Added a `family::model1` branch to `apply_family_cascade` (native 496×384 with a new
`RES_496x384_NATIVE_M1` = "496x384 (Native Model 1)" label; `set_native_resolution` gained an optional
496 label override) — otherwise matches the Model 2 else-branch (full M1 option gating stays M1-5).

**Results (vf, retrohost --vk, 1800f, native; bg=M2VK_NO_3D, sw=M2VK_SW_3D):**
- Full pixel diff vk-vs-sw **12983 (M1-2) → 10485 (−19%)**. Digest `1925260e2fcc4c0d`.
- **Resolution invariance PASSES** (`POINT=1 ROMS=devnotes/roms/model1 res.sh vf 2500 3`): 1x vs 3x
  (1488×1152, centre-subpixel resolve) coverage agreement **1.0000**, **0 interior disagreements**,
  exit-criterion-1 identical, SSIM covered 0.9996. The float geometry is genuinely resolution-independent.
- Firewall intact: pure-software digest still `6ed76ed4b9319aa5` (recompute only runs under `m1::active()`).
- vr renders (no regression, digest `22f9bf8b88e9d747`). Screenshot: `2026-08-29-vf-m1-3-native.png`.

**Open residual (deferred, NOT a regression).** The bottom pixel row (y=383) and right column (x=495)
remain background where the software draws the floor edge — **496 / 129 px**. Root cause found: the TGP
frustum clip clamps off-screen edges to **exactly** the integer viewport bottom/right (`view->y2`/`x2`),
and `fill_quad` — flooring to `spoint`, filling integer rows/cols inclusive — paints that last row/col,
whereas Vulkan's pixel-**centre** rule leaves an edge sitting on the integer boundary uncovered. This is a
pre-existing M1-2 issue that **float capture cannot fix** (the geometry is integer-clamped, not sub-pixel).
Two blanket fixes were measured and rejected: a +0.5 sample offset (10485→14239, shifts every edge the
wrong way vs the software's floor) and a boundary snap (`s ≥ y2/x2 → +1`, fixes the edge lines but distorts
the slopes of quads sharing a clamped vertex, spreading ~+3200 px through the lower scene). Correct fix is
a dedicated clip-edge fill or a driver clip at the framebuffer edge (384/496, not 383/495) — parked for the
M1-6 compat tail.

**Next: M1-4 (2D-over HUD composite)** — port the S21 option-B under/over overlays so the `segaic24`
layers 7/5/3/1 land back on top of the 3D (the ~5000 px HUD residual), 3D drawn between at internal res.

---

## 2026-08-29 — M1-4: Sega Model 1 2D-over HUD composite

The HUD now sits on top of the GPU 3D. The one non-trivial 2D piece turned out to be the *simple* S22
sandwich, NOT the S21 option-B pen-space machinery: Model 1's 2D layers are the System 24 tile chip
drawing **resolved 0xffRRGGBB pens straight into the bitmap_rgb32** (segaic24.cpp draw_rect), so there is
no CLUT / palette-shadow to preserve — the S21 pen-index composite exists only for C355's shadow banks,
which Model 1 has none of. So M1-4 is the plain RGB UNDER/OVER sandwich S22 uses, not a port of the S21
under/over/mix overlays.

Insight that shrank the job: the **2D-UNDER band (layers 6/4/2/0) needs no capture** — it is drawn into
`bitmap` before tgp_render and is already the passthrough background the 3D draws over. Only the
**2D-OVER band (7/5/3/1)** had to be lifted back on top. New seam surface (`m1_seam.{h,cpp}`):
`over_begin/end/pixels/forget` + a `capture_over` template, `over_forget()` in frame_begin — mirroring
s22_seam. The driver hook (inside the existing `#ifdef M1VK` block in screen_update_model1, +19 lines):
when `m1::gpu_owns_3d()`, redraw 7/5/3/1 into a **sentinel-filled scratch** (fill 0 → high byte 0; a tile
pen is 0xffRRGGBB, so a set high byte marks a touched pixel), snapshot the touched pixels as an opaque
overlay (alpha forced 0xff, untouched = transparent 0). vk_present detects `m1::over_pixels()` → an
`m1_sandwich` exactly like `s22_sandwich`, stuffs it into LAYER_OVER, and adds it to `draw_over` so the
existing `s_pipeline_over` pass redraws it after the M1 3D. No new pipeline/shader; reuses the Model 2
OVER pass wholesale. model1_v.cpp footprint still all-guarded (72 insertions / 0 deletions vs mame0288).

Verify (retrohost --vk, vf, 1800f, native 496×384; sw=M2VK_SW_3D, bg=M2VK_NO_3D):
- **vk-vs-sw exact pixel diff 10485 (M1-3) → 5555 (−47%)** — the ~5000 px 2D-over HUD residual collapsed,
  exactly as predicted. Classified: 625 = the known bottom-row(496)/right-col(129) integer clip-edge
  fill-rule (parked for M1-6); ~4930 = polygon-silhouette color-shift at VF's fine tessellation
  (fill-rule, also M1-6). **Zero HUD residual left** — the diff is now edges only.
- Screenshot `2026-08-29-vf-m1-4-vk.png`: INSERT COIN(S) / CREDIT 0 / © SEGA 1993 sit ON the 3D; sky /
  clouds / ocean / islands (2D-under) sit beneath. `2026-08-29-vr-m1-4.png`: VR's mountains/sky/ocean
  2D-under backgrounds render beneath the track/cars/palms, full ranking HUD on top — confirms both
  bands composite correctly on a stage game. Digests: vf vk `538d4dd8cbfa01e1`, vr `22433f4f0f2301bf`.
- **Firewall intact**: pure-software (no --vk) digest still `6ed76ed4b9319aa5`; M2VK_SW_3D arm exact vs the
  software baseline (`6ed76ed4b9319aa5`), so the OVER capture is inert unless the GPU owns the 3D.

NOTE for M1-5: `color_xlat` is shared 2D/3D (the tile pens and the 3D luma both index it). Relevant when
the No Lighting LUT lands — do not treat the 2D and 3D colour paths as independent.

**Next: M1-5 (No Lighting + options/routing)** — the color_xlat LUT + model2_flat_luma toggle, the
family::model1 option gating (show No Lighting + Internal Resolution + steering, hide the S22/Transparency/
Flat Shading), M1 input rows, savestate check. Then M1-6 the fill-rule/compat tail (the 5555 edge residual).

---

## 2026-08-29 — M1-5: Sega Model 1 No Lighting + option routing + input rows

**No Lighting — carried, not recomputed (a simpler build than the plan's LUT-on-GPU, chosen after a
correction).** The plan (risk 2) warned of *two* live shading branches — `scale_color()` 5:5:5 multiply
vs the `color_xlat` 64-step LUT, "selected by `(flags>>10)`". Re-read of our mame0288 tree: **that is a
misread.** `scale_color()` is DEAD — its only call sites (`model1_v.cpp:982-983`) are `#if 0`'d and `:1171`
is commented; `(flags>>10)&3` is the **z-key mode** (`:961-976`), not a colour selector. There is exactly
ONE live shading branch, the color_xlat LUT, and it runs on **both** the push_object main path and the
draw_direct path. Since the driver already computes the lit colour on the CPU every frame (it is the `col`
already crossing the seam), No Lighting needs no LUT on the GPU: the seam carries the lit `col` AND the
pre-luma `albedo`, and `m1.frag` picks by a `flat_luma` push-constant bit. Simpler (no LUT texture, no
descriptor set — m1_geom still has none, no live color_xlat capture) and strictly MORE faithful for the
lit/default case (it carries the driver's exact integer LUT bytes rather than re-deriving them). Toggle is
live: a uniform flip, no re-capture. **Decision put to the user, who chose this over the plan's LUT path.**

Implementation: `quad_t` gained a guarded `#ifdef M1VK uint32_t albedo`, captured in push_object at BOTH
LUT sites (before the LUT overwrites the 5-bit channels) as `pal5bit(r/g/b)`. Seam `quad` + `submit_quad`
carry it; `gpu_vertex` gained a 3rd attribute (16-byte vertex), the push block a `flat_luma` word;
`geom_draw` reads `m1::no_lighting()` live (frontend thread) — no vk_present signature change. `m1.frag`
emits `flat_luma ? albedo : col`; the MOIRE stipple stays keyed on `col`'s bit 24 (translucency is a
polygon property, not a lighting one). `set_option_no_lighting` wired into both retro_entry option-apply
paths (load + live), gated on `family::model1`.

**Option routing.** `apply_family_cascade(family::model1)` now HIDES Flat Shading + Transparency (Model 1
is always flat-shaded and untextured, and m1_geom hardcodes the stipple — both are dead entries), on top of
the four S22-only hides. No Lighting, Internal Resolution and the shared steering/analog block stay visible;
vr/vformula (IPT_PADDLE) and swa/wingwar/netmerc (IPT_AD_STICK) light up the steering / analog options via
the existing detectors, no table.

**Input rows.** Added 5 rows to `input_layouts.json` (vf, vr+vformula, swa+swaj, wingwar×3, netmerc), all
authored from the driver's own `PORT_NAME`s. `padmap-gen.py` DRIVER_PATHS gained `sega/model1.cpp` (it was
not scanned, so it could neither validate M1 sets nor detect vr's paddle). `--check` passes: 58 rows / 69
sets, the .ipp matches, and the paddle↔"Steering" consistency gate accepts vr. **The button *semantics* are
the user's hand-check** (per the no-scripted-input rule) — especially swa's second/third button, unverified
(MACHINE_IMPERFECT_CONTROLS).

⚠️ **Bug caught by the user's hand-check, then fixed.** The first cut of these rows had **all-`NONE`
`buttons[]`** — an authoring-script helper (`none9(*pairs)`) collected its mapping dict into a tuple, so every
button source silently dropped to NONE. The `labels` dict was still correct, so `M2VK_HOST_DESCRIPTORS` showed
the right names and I wrongly read that as "verified" — but descriptors are built from the label array, while
the pad is READ from `sources[]`, and NONE sources mean no control drives any MAME button. `--check` did not
catch it because all-NONE buttons is *legal* (winrun is analog-only, labels-only). VF's buttons went dead;
the user caught it in game. Fix: populate `buttons[]` (source+label) so `read_source` drives IPT_BUTTON1-3,
which also arms the generator's label-consistency gate. Lesson: for a BUTTON game, the static proof is the
`sources[]` in the .ipp (B/A/Y → BUTTON1/2/3), not the descriptor labels.

**Savestates — a real gap, pre-existing, NOT M1-5's.** `state.sh vf` FAILS: C≠D (loaded dirty state does
not reproduce the dirty future), while D==E (deterministic) and N≠D (script bites) pass. Save→reserialize
round-trips clean (`M2VK_SAVE_VERIFY` names no differing entry), so the registry is self-consistent — the
failure is **unregistered** state: Model 1's MB86233 TGP copro / gen_fifo is not in the save registry on the
mame0288 baseline, so it is neither saved nor restored. This is a driver-level gap independent of M1-5 (the
test runs on the software path; the only driver edit is a transient per-frame render field). The shared
savestate path is intact — `state.sh vf2` (Model 2) still PASSES C==D. The fix belongs with the mame0289+
Model 1 sync (risk 1's "Improved video and timer emulation" #15642) at M1-6, not the guarded seam.

Verify (retrohost, vf, 1800f): firewall pure-software digest STILL `6ed76ed4b9319aa5`; --vk lit digest
`538d4dd8cbfa01e1` = **exactly the M1-4 baseline** (albedo plumbing does not perturb the lit path, so the
5555-px edge residual is unchanged by construction); --vk No Lighting `c4b9d3eb03e49f10` (31.9% of pixels
change = the 3D region; 2D bands untouched). Albedo reads darker overall (mean 109 vs 135, ratio 0.80) —
correct: the color_xlat LUT *brightens* well-lit faces above the raw palette base (×~1.47, saturating), so
stripping it lands on the un-boosted albedo, the documented "raw unlit palette albedo" ground truth.
Screenshots `2026-08-29-vf-m1-5-lit.png` / `-nolight.png`: fighter's directional gradient and the
LUT-brightened floor are gone under No Lighting; HUD/2D-under composite unchanged in both. Driver footprint
still all-guarded (`#ifdef M1VK`, 89 insertions / 0 deletions vs mame0288, up from 72 at M1-4).

**Open (hand to the user):** the input-button hand-check list (below/handed off). **Next: M1-6** — the
fill-rule/compat tail (the 5555 edge residual) and the mame0289+ Model 1 sync that also closes the savestate
gap.

## 2026-08-30 — 23-7: System 23 widen + polish (light-gun rows, savestates, v4a)

The buildable half of 23-7's four items, all statically verified; only the user's in-game light-gun
hand-check stays open. **No driver edit** — `namcos23.cpp` unchanged at 105 insertions / 0 deletions.
The work is in the OSD input table plus the two padmap tools.

- **Per-game light-gun pad layout.** Added `timecrs2` and `crszone` rows to `input_layouts.json`,
  authored from the driver's `PORT_NAME`s (`namcos23.cpp:7425-7453`, a static read — no press-sweep):
  BUTTON1 Gun Trigger → `B`, BUTTON2 Foot Pedal → `A`, BUTTON3 User Enter → `Y`; aim on the left stick.
  `crszone` `PORT_INCLUDE`s `timecrs2` so it shares the three. Taught the padmap generator/sweep the
  fifth driver: `namcos23.cpp` into `padmap-gen.py` `DRIVER_PATHS` and a `system23` family in
  `padmap-sweep.sh`.
- **Verified static:** `padmap-gen.py --check` ok (60 rows / 66 sets, .ipp==.json); `padmap-test.sh`
  all checks passed; core relinks. `M2VK_HOST_DESCRIPTORS` on `timecrs2` / `crszone` / `timecrs2v4a`
  emits the three labels + Aim X/Y on the gun device, each logging "has its own control layout".
- **Widen `timecrs2v4a` (`namcoss23_state`):** resolves the `timecrs2` row by parent — no own row
  needed; boots/renders through the GPU seam.
- **Savestates work (family-neutral):** 33,201,431-byte state; cross-process save/load bit-identical
  post-load digest `7279dc062069bc11`. Non-vacuous (savestates.md §3 step 3): loading the 1600-state at
  frame 1640 reproduces the save's future hash sequence, not the load run's own — proving `unserialize`
  replaces state; only the single first post-load frame differs (§9.3 display-cache transient).
- **Options:** the S23 cascade (hide S22/S21/M2 toggles) was already done at 23-2; no S23-specific live
  option exists yet, so `retro_run` needs no branch.

**Open (handed to the user):** the light-gun in-game hand-check for `timecrs2` / `crszone`; the
`verified` field on the two rows fills after. `crszone`'s driver-noted input issue stays untriaged
(likely JVS/MCU, orthogonal). **23-6 (sprites) remains parked** on a Gorgon ROM. With 23-7 done, the
in-scope System 23 renderer is feature-complete.

## 2026-08-30 — mame0289 sync (M1-6 tail) + cosmetic edge-diff re-measure
Merged mame0289 into main (HEAD `7d309272736`). 4 conflict hunks: 3 trivial include-orderings
(kept both the guarded seam include and 0289's new corefloat/endianness/glm/mcs headers), 1 real —
#15597's above-HUD (0x41) rework in `screen_update_model1`. Resolved so the software path runs
upstream's `build_overlay_mask`/`tgp_render(RENDER_ABOVE_HUD)`/`apply_overlay_stencil` verbatim and
the GPU path also submits the 0x41 quads while keeping the M1-4 over-capture. The other 6 M1VK hooks
(and all M2/S22/S21/S23/scsp hooks) auto-merged; audited each — the pre-LUT albedo captures and the
`old_z` Smooth-Shading site sit correctly against the reworked code. Build fix: 0289's new rs232
`s97801` terminal is compiled unconditionally by `BUSES["RS232"]`; enabled its deps (S97801, SCN_PCI,
SCN2674, MCS48, MCS51) in modelizer.lua, same dead-weight pattern as IE15/SWTPC8212.

Payoff (#15649 TGP copro ROM repair) verified: **swa** (spaceship 3D), **wingwar** (aircraft 3D),
**netmerc** (title screen) all render — the three games the mame0288 baseline got wrong. Four-family
render smoke clean (daytona/vf2/ridgerac/starblad) — no endianness/C++20 regression. `state.sh vf`
PASS (the savestate gap the plan flagged as unverified is settled — working).

M1-6 cosmetic residual re-measured (vf, --vk, 1800f, native 496×384; sw=M2VK_SW_3D, bg=M2VK_NO_3D):
total vk-vs-sw differing 6576 px, of which clip-edge (bottom row 496 + right col 129 − corner) = 624,
**identical to the M1-5 baseline's 625**; the rest is polygon-silhouette. Absolute total is NOT
comparable to the 5555 baseline — 0289's #15642/#15715 video/timer changes land VF's attract on a
different pose at frame 1800 (digest da4688d2d89ffa5e vs the old 538d4dd8cbfa01e1). Character
unchanged: exit-criterion-1 clean (0 outside-coverage diff), parity 3316/3260 (no moiré checker),
91% of diffs are edges (575 solid-blob px), zero 2D-under/HUD residual. #15738/#15712 (moiré/wireframe)
don't touch VF — it has neither. Residual stays the inherent GPU-vs-scanline silhouette fill-rule,
the known M1-6 item. Heatmap: `devnotes/screenshots/2026-08-30-vf-edge-diff-heatmap.png`.
