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
input mapping → R2 compat → R3 options → R4 polish → R5 release; the savestate half of R2 is void as of
2026-09-04 — see the last entry). Renderer/geometry work is
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

---

**2026-09-01 — Sound-thread Stage 2 VALIDATED on Quest 3, and the `model2_sound_thread` core option wired.**
The sound-68000 worker thread (Stage 1, built 2026-08-31) proved out on device: daytona, heavy full-grid
race, clock pinned, RetroArch/Vulkan — thread **OFF 49.96 present-fps** (below the 57.5 Hz target, one core
maxed at 103%), thread **ON 57.86 fps** (at target, 81% main + 22% worker, cores idle). ~15% recovered =
it goes from missing the frame budget to hitting it, matching the profile's 12% sound-CPU share. The user's
ear-test agreed decisively ("way freaking worse" thread-OFF). Worker engagement triple-confirmed: a second
busy emulation thread in `top -H`, the savestate growing 463 B, and the fps delta itself.

Enabling it on device forced a fix: **getenv is null on Android** (`am start` doesn't propagate env), so the
`M2VK_SOUND_THREAD` env gate is dead there and `set_option_enabled()` had no caller. Wired the
**`model2_sound_thread`** core option (Model-2-family menu only, hidden on s21/22/23/model1), seeded in
`retro_load_game` before the machine builds so the model2.cpp config hook reads it; env still wins when set.
Host-verified: option off/on both `48bb93c7814cd3f4` (bit-identical video), savestate PASS via the option
path (C==D, N!=D). Retires shippable R3 #3 (the menu toggle).

Measurement method for the next device run (no RetroArch HUD FPS counter in this build; the plain build's
`[model2]` log is silent to logcat — only PROFILER=1's `m2prof` writes there): **present-fps via `dumpsys
SurfaceFlinger --latency <layer>`**, the app-uid `…RetroActivityFuture#<n>` row (its `#<n>` changes each
relaunch). AudioFlinger underruns are NOT the tell (both arms read 2 — `audio_rate_control` resamples,
trading underruns for pitch-warble). ⚠️ RetroArch **Restart** = `retro_reset`, does NOT rebuild the machine,
so toggling this reload-gated option + Restart ANRs — switch arms by a full relaunch. Residual 90 Hz-panel
judder on a 57.5 Hz core (vrr_runloop + rate-control) is a RA config artifact, not the core. Detail in
[m1audio-thread-plan.md](m1audio-thread-plan.md) §Stage 2. Next lever: drive-board Z80 park / interpreter
hot-path, ordered by a fresh `m2prof` ranking now that sound is off the main thread.

**2026-09-01 — On-screen emulated frame-rate read-out (`model2_fps_display`, default ON).**
(An earlier attempt at this feature was reverted for breaking the core; this is the clean re-do — see
the safety proof below.) A HUD frame-rate counter, top-LEFT, colour-coded against the machine's own
refresh rate — green while the emulated game holds its target (Model 2 ≈ 57.5 Hz, read from
`s_osd->refresh_rate()` via `set_target_fps`, so no hardcoded number and it tracks each family/set),
red once it drops more than **1 fps** below (the user asked for a tight band). This is the **emulated
game** rate: timed in `tick_fps()` at the top of `present_frame` (one call = one emulated frame handed
across), EMA-smoothed (α 0.2), a >0.25 s gap reseeds so a pause/reload doesn't print a fictitious rate.
Shown to **three decimals** (e.g. `57.795`), formatted fixed-point from integer milli-fps. Reuses the
poly-counter pipeline; the drawing was factored into `draw_glyph_box(glyphs,n,left,fg)` (glyph 0..9 =
digit, 10 = decimal point) with `draw_counter` (integer) and `draw_fps` (`NN.NNN`) both feeding it. The
only shader change is one added font entry — a bottom-centre dot at `FONT[10]` (bit 13) — so counter.frag
was recompiled (`build_shaders.sh` / glslc); the digit glyphs are byte-for-byte the old ones.
Option all families, Vulkan only; `M2VK_FPS` overrides. **Determinism guard:** the digits are wall-clock,
so an always-on overlay would poison every harness digest — `retrohost` now `setenv("M2VK_FPS","0",0)`
so all A/B/res/perf/hand digests default it OFF (players on RetroArch, a separate binary, keep default
ON); explicit `M2VK_FPS=1` still overrides for eyeballing. **Safety-verified (vf2, 1400 frames):**
FPS-off is byte-identical across runs (`63c764f5aec4c5a2` twice) and a default `retrohost --vk` run now
equals that baseline; FPS-on differs from FPS-off in exactly **231 px, all inside the top-left box, 0 px
elsewhere**; no crash/assert; only 6 OSD files + `retrohost.c` touched, zero upstream device/driver
files. Green/red-under-load still wants a user hand-check on the Quest.

**2026-09-01 — Quest 3 frame-rate session: 50 → ~56 worst-case, 57.5 locked outside the heaviest scenes.**
Live-debugged on the tethered headset with the user driving. Chain of findings, each measured before fixed:
1. **The leftover `set-fixed-performance-mode` pin was NOT the gap** (Quest OS holds big cores at 1.92 GHz
   under load either way; 2.36 GHz exists in the freq table but the OS never grants it — there really is
   no clock headroom). Pin removed; back to stock defaults.
2. **Thread placement**: this chip is 2 little (cpu0-1 @1.38 GHz) + 4 big (cpu2-5 @1.92). New
   `m2vk_affinity.h`: emu thread, sound worker, and the frontend's retro_run thread pin themselves to the
   big cluster, re-asserted every 128 frames because **Android silently wipes thread affinity on app-state
   transitions** (observed live). Worth ~3-4 fps.
3. **RetroArch on the Quest cannot pace this core.** Its cfg claimed a 60 Hz display (real panel 90 Hz),
   and core 57.52 vs "60" is inside the default 5% `audio_max_timing_skew` → RA time-warped the game to
   video timing, quantized by FIFO vsync to ~45-54 fps. Worse: opensl audio writes never block here
   (free-run test hit 90+ fps with `audio_sync=true`), so audio-clock pacing doesn't exist either, and the
   vrr_runloop timer undershoots ~6%. Config now: `video_threaded=true`, `video_refresh_rate=90`,
   `audio_max_timing_skew=0.01`, `vrr_runloop_enable=false`, `video_vsync=false`.
4. **`model2_self_throttle` core option** (new; default enabled on Android only): drops `-nothrottle` so
   MAME's sleep+spin throttle paces the core itself. With every RA limiter off this gives exact 57.5
   pacing (attract/select hold 57.5 flat). Host default stays `-nothrottle` (digest determinism).
5. **`m2vk_stallmeter.h`** (new, Android-only): logcat tag `m2stall`, splits each emu-thread frame into
   cpu / park (baton wait on the frontend round-trip) / other. Heavy-race worst before pipelining:
   `19.20 ms = cpu 15.44 + park 2.44 + other 1.33` vs the 17.38 budget.
6. **Frame pipelining** (Android-only, `M2VK_PIPELINE` overrides on host): retro_run now presents, copies
   audio, polls input, releases the emu thread, THEN pushes audio and returns — emulation overlaps the
   frontend tail. +1 frame display latency. Host keeps the legacy order: default digest verified
   byte-identical to baseline (`63c764f5aec4c5a2`), pipelined path deterministic across runs.
   **Savestates are DROPPED in pipelined mode** (state.sh fails C!=D under it; user call: no savestates on
   Quest rather than wrong ones) — `retro_serialize_size` returns 0 there; host savestates unaffected.
7. **`model2_drive_board` core option** (new, live both ways, wheel-set menus only, default enabled):
   parks the FFB drive-board Z80 via `SUSPEND_REASON_DISABLE` from the OSD (`m2vk_driveboard.h`, zero
   upstream edits). daytona 2500-frame attract digest bit-identical parked vs running; engagement log
   line verified. Live on-device A/B was scene-confounded (~1 ms, inside noise).

**Remaining gap** (heaviest full-grid moments only): `~18-19.7 ms = cpu 14.5-15.8 + park ~2 + other ~1.4`.
The park is now almost pure present_frame cost and CANNOT be moved after the release without renderer
surgery: `frame_record.texram` is live pointers into the running machine (m2vk_frame.h documents the
parked-thread guarantee). Next levers, in order: (a) double-buffer the record + snapshot/gate texram so
present runs off the critical path (~2 ms); (b) emu-thread priority bump (other ~1.4 ms is mostly
runqueue wait); (c) interpreter hot-path on the i960/TGP (performance.md §4). All three need the
manual install-or-restore cycle on the headset to test.

## 2026-09-01 (later) — scheduler-quantum experiment: run, and it redirected the roadmap

Ran `devnotes/plan_model2_quantum.md` end to end on the desktop. The quantum lever is **dead**; the
lever it was standing next to is worth 34–57 % of emulation-thread compute. Full write-up and every
number in that file; instrumentation saved as `devnotes/qprobe.patch` (throwaway, env-gated, reverted
from the tree — the upstream diff is back to what it was).

1. **The quantum cannot be coarsened.** model2o sets none, so `rebuild_execute_list` gives it MAME's
   default `attotime::from_hz(60)` = 16.67 ms — already the ceiling, and `compute_perfect_interleave`
   only ever raises the floor. The plan's sweep (18000→1200 Hz = 55→833 µs) was *finer* than the
   status quo on every arm; running it would have measured slowdowns and concluded the wrong thing.
2. **The quantum is masked anyway.** `timeslice()` targets `min(basetime + quantum, next timer)`.
   fvipers (model2b) has the driver's 18000 Hz quantum and still takes 1.006 M slices/s — so
   `model2.cpp:2915` has no observable effect. Don't cite it as precedent.
3. **The real cost is a 500 kHz baud clock.** `clock_device::clock_tick` is **99.87 %** of all timer
   callbacks. `model2.cpp:2586`, `:2654` and `shared/segam1audio.cpp:80` each instantiate a 500 kHz
   `CLOCK` into `i8251::write_txc`/`write_rxc`; both edges fire, so 1 M callbacks/s per device
   (daytona has two, coincident; 2A/2B have one). With `m_br_factor = 16`, 15 of every 16 edges only
   increment a counter — but each is still a full scheduler break: 33 k rounds and 67 k device
   `run()` dispatches per frame.
4. **Prize, `perf: core ms/frame`, baud clocks silenced** (`M2VK_QPROBE_NOUART=all`; deliberately
   breaks the sound link, so it's an upper bound): daytona 3.797→2.018, vf2 3.650→2.047, srallyc
   3.685→1.570, vcop2 3.538→1.968, fvipers 4.501→2.985.
5. **The control that makes it a claim.** A `CLOCK` device nothing listens to changes no emulated
   state, only scheduler granularity. Daytona digest `570fa675693d242f` held across none / 250 kHz /
   500 kHz / 1 MHz, while core went 3.817 / 3.959 / 3.998 / **5.300** — cost tracks *new* break points
   (the coincident rates are nearly free), pricing one at **~85 ns** here. +1 M/s = +1.48 ms predicted
   vs −1.78 ms measured for removal. It is scheduler overhead, not the game doing less.
6. **The sound thread didn't fix this, it moved half of it.** `m2vk_snd::enabled()` takes m1audio's
   clock to the worker machine; the main machine keeps 1 M edges/s. Consistent with the Quest 50→57.5.

**Next:** a demand-gated / bit-boundary baud clock (bit-exact for TX by construction; RX must wake on
`write_rxd` with phase computed on the true grid — naive batching quantises start-bit detection to a
whole bit and is not safe). Then re-measure before committing to the MB86233 DRC: a recompiler makes
each dispatch cheaper without making them fewer, and ~40 % of the time is the switching.
⚠️ The video digest cannot see sound — vf2/srallyc/vcop2 held their digests with the link fully dead.
Any candidate needs a listening check, not just `ab.sh`.
**Not run:** the Quest arm. Desktop cores aren't Adreno's; a break point there likely costs more,
not less.

---

## 2026-09-01 — the demand-gated baud clock, built ([lazy-baud.md](lazy-baud.md))

The lever the entry above found, taken. `src/osd/libretro_m2/m2vk_baud.{h,cpp}` replaces the 500 kHz
`CLOCK` feeding `i8251::write_txc`/`write_rxc` with a generator that delivers **the same edges on the
same grid at the same emulated instants** but only arms a timer for one the UART can act on. Default
**ON**; `M2VK_LAZY_BAUD=0` is the A/B arm. All the logic is in the new file — guarded hooks only, and
this change is **116 insertions / 1 deletion across 4 upstream files** (`git diff --shortstat` vs
`mame0289`, which is the merged baseline now, not `mame0288`; whole-fork total 906/18 over 15 files).

**Measured** (`retrohost --vk`, 2500 frames, second boot, own `M2_SAVE_DIR`): daytona 3.644→**2.322**,
vf2 3.824→**2.357**, srallyc 3.962→**2.066**, vcop2 3.432→**2.040**, fvipers 4.728→**2.949**. That is
−36 % to −48 %, essentially at the `NOUART` upper bound the previous entry measured. System 22/21 are
unaffected (the no-op control). Model 1 gains only 2–6 % because `model1.cpp`'s own `m1uart_clock` is
still a stock `CLOCK` — the obvious follow-up, left alone because that file scopes on `M1VK`.

1. **Three skips, each exact, none of them a batch.** TX bulk-adds the counter and schedules the bit
   boundary; RX bulk-subtracts and schedules the sample point; an RX waiting for a start bit sleeps
   outright and wakes on `write_rxd`. Start-bit detection still runs at the full 16× rate, so it never
   quantises to a whole bit. Two details carry it: on waking, up to 16 real idle edges are replayed so
   the 16-bit shift register ends bit-identical (**without at least one, a start bit after power-on is
   missed** — the register starts at 0 while the line idles at 1, so no 1→0 transition is ever seen);
   and every register write goes through the generator, because a mode byte rewrites `m_br_factor` and
   zeroes both dividers.
2. **The plan's "idle TX needs no timer" is wrong and was not built.** An idle bit boundary still
   reaches `sound_ready_w`, which re-asserts IRQ bit 10 whenever TxRDY is high — observable if the CPU
   cleared `m_intreq` in between. TX keeps its 31.25 kHz boundary; that is under 4 % of the break
   points removed, and the numbers land at the floor regardless.
3. **What it does change is scheduler interleaving, and the control proves it is only that.** Three of
   five picture digests hold bit-exactly; vf2 and vcop2 move. Running the previous entry's dummy-clock
   control *in reverse* — lazy clock **on** plus a `CLOCK` that restores the break points and drives
   nothing — returns vf2 to `3fe1c65ec124e202`, vcop2 to `959289e28ea8f11e`, and daytona's **audio** to
   `d0d75dbf470ad402` rms 3258.5, all bit-identical to stock. `M2VK_LAZY_BAUD=2` (eager: this device,
   a timer per edge) does the same. The residual is a sub-pixel geometry phase — 0–0.06 % of pixels on
   the frames sampled, vf2's final frame bit-identical.
4. 🚨 **The previous entry's warning was right, and it cost a real bug.** `retrohost` now prints an
   `audio:` line (FNV digest + RMS over every sample) beside the picture digest. It immediately caught
   **Model 1 going silent** under a bit-perfect video digest: `model1.cpp` feeds the board's RxD
   through `segam1audio_device::write_txd`, which was not routed through the generator. Fixed inside
   the board device, so model1.cpp, manxttdx and the sound-thread worker are all covered by one path.
   vf/swa/vr now match on video **and** audio; srallyc and fvipers match on both without any control.
5. **`subdevice<T>()` is a `downcast`, not a `dynamic_cast`.** With `M2VK_LAZY_BAUD=0` it hands back
   the stock `clock_device` reinterpreted as a generator and the next call segfaults. Cost: one crash
   in the sound-thread bridge. Use `dynamic_cast` on the untyped `subdevice()`.
6. **Verification.** `ab.sh` across 14 fixtures — all pass exit criterion 1, `white = 0`, zero interior
   disagreement. `dynabb97`/`waverunr` sit off `ab-baselines.md` at LB=1 only because frame 2500 lands
   on a different moment; **LB=0 reproduces the baselines exactly** (92.119 / 91.334). Savestates
   **8/8 PASS**. Sound thread: same video digest, no crash (its audio ring reads rms 0.0 under
   `retrohost` in *both* arms — pre-existing).

### 2026-09-01 (later) — the first cut was broken, and only a hand-check found it

The user played it. Daytona: engine stuck at one cadence, **no SFX, no crashes**, music and voice fine.
Same with the sound thread. Cause was a real bug in `sync_tx()`, not interleaving.

1. **The false invariant.** The first cut bulk-added every elapsed TX edge on a wake, arguing "the
   boundary edge is always the one we scheduled, so a wake can never step over it." MAME lets a CPU
   overshoot a pending timer by up to one instruction inside a timeslice. Measured on daytona: the game
   wrote to the UART at 55.929,940,040 s, **40 ns after** a boundary due at .940,000 whose callback had
   not run. The bulk add walked `m_txc_count` onto exactly `m_br_factor` with `transmit_clock()` never
   running; it only resets on *equality*, so the next tick made it factor+1 and **TX was dead for the
   rest of the run**. Fix: a bulk advance stops one short of a boundary and leaves it to the timer
   (which arms at zero delay); delivery accepts an edge whose time has passed; `arm()` clamps a past
   target. RX's sample point had the same hazard and the same fix. `arm_tx()` now permanently guards
   the divider overtaking the factor.
2. 🚨 **Attract mode is not the emulator, and this is the lesson worth keeping.** 14 `ab.sh` fixtures,
   8 savestate fixtures and a new audio digest were all green with the transmitter dead, because every
   one of them ran attract mode, where daytona moves 48 bytes in 43 s. The stall first fires **56 s into
   a race**. The check that finds it is a scripted-gameplay byte-stream diff: log `data_w` /
   `receive_character` with timestamps on both UARTs and compare per-UART sequences. daytona is the
   fixture because its **video digest is identical in both arms** under an input script, so the i960
   provably ran the same code and any byte difference is the link. After the fix: **3246 bytes vs 3246**,
   each UART's sequence byte-for-byte identical (1623 each way); before it, 3246 vs 1196. SCSP games
   diverge under script, so compare lazy+dummy-clock against stock: vf2 402/402, fvipers 492/492,
   sequences identical.
3. **Two things I called wrong and should not be repeated.** "The dummy-clock control passes, so the
   difference is benign interleaving" — the control was passing *while TX was dead*; it isolates the
   UART from interleaving but says nothing about a path the run never reaches. And "audio RMS within
   2 % means the sound is fine" — it read that for total TX death.
4. **Dead end, do not re-propose: `set_maximum_quantum`.** With the baud clock gone it finally binds, so
   it looks like a way to buy interleave back. A 2 µs quantum restores daytona's stock audio digest
   (core 2.970 vs stock 3.611) but **breaks srallyc's**, which is bit-identical under plain lazy, and
   gives vcop2 a third video digest belonging to neither arm. It is just a different arbitrary
   interleave.
5. **Still open:** the user also reports vf2 "sound volume very soft, music normal". Not explained and
   not shown to be a regression — its RMS is ~2062 in every arm including stock and its byte stream is
   exact given matched interleave. Needs an `M2VK_LAZY_BAUD=0` A/B by ear before chasing it.

### 2026-09-01 (later still) — the hand-check's real culprit was the SOUND THREAD, not the baud clock

The user's daytona report survived the transmitter-stall fix. Cause: their live
`config/m2-vk/m2-vk.opt` carries `model2_sound_thread = "enabled"` from the Quest validation, and the
Game Launcher uses whatever is in that config. **With the sound thread on, the serial bridge stops
delivering ~11 s in — the board receives 96 of 1622 bytes.** Identical at `M2VK_LAZY_BAUD=0` and `=1`,
so it is a pre-existing fault in `m2vk_snd`, not this work. Written up as an open bug in
[m1audio-thread-plan.md](m1audio-thread-plan.md). Disabling the option restored the game's sound.

1. **Check which options the user's config actually has before interpreting a hand-check.** Two rounds
   of ear-testing were spent attributing a sound-thread fault to the baud clock. Their `.opt` file is
   one `cat` away and would have said so immediately. The "same problem with threaded audio" line in
   the first report was the tell.
2. **The baud clock is now DEFAULT OFF**, opt-in via `M2VK_LAZY_BAUD=1`. Verified that the no-env build
   is bit-identical to stock (daytona video `b90f8192848a723b`, audio `d0d75dbf470ad402` rms 3258.5).
3. **The Quest "Stage 2 VALIDATED" note is narrower than it reads** — it measured frame rate
   (daytona ~50→57.5), not that the sound board was still being fed. The video digest cannot see the
   serial link, so nothing in that validation would have caught an 11-second cliff.
4. **Frame rate question, answered and not a bug.** The core reports **57.5242 fps**, exactly
   16 MHz/(656×424) from `model2.cpp`'s `set_raw`. The core's own `model2_fps_display` reads *wall-clock*
   `retro_run` rate, so a steady 60 means the frontend is pacing to a 60 Hz display rather than to the
   content — the game then runs ~4.3 % fast. RetroArch's "Sync to Exact Content Framerate" or the core's
   own `model2_self_throttle` ("Self-Paced Timing", currently `disabled` in the user's config) each fix
   it. Pre-existing, unrelated to any of today's work.

**Next:** the Quest arm, still not run — `build-android.sh` → `deploy-android.sh` → the manual
Install-or-Restore → `adb logcat -s m2stall:V m2prof:V` on a heavy daytona race. A break point there
likely costs more than the 85 ns measured here, not less. Then re-measure the per-device profile
before committing to the MB86233 DRC. ⚠️ **The listening hand-check is open** — see `lazy-baud.md`.

## 2026-09-01 (evening) — vcop "laggy" on the Quest: config, not code

User reported vcop laggy in the headset. `m2stall` over adb read 47–50 fps in gameplay
(cpu 18.2 ms of a 15.7 ms budget), 56.8 in attract — and the live `m2-vk.opt` on the headset had
**`model2_sound_thread = "disabled"`**, despite §1.3/1.4 of `plan_optimization_todo.md` recording it
as enabled-and-intended. (It had drifted — RetroArch persists the menu's last choice; same class of
trap as the Self-Paced Timing note.) vcop is model2o, so the board split applies to it exactly as to
daytona/desert.

Fix: force-stopped RA, flipped the key in `/sdcard/RetroArch/config/m2-vk/m2-vk.opt`, relaunched
vcop by intent, confirmed `[model2] model2_sound_thread=on`. Result over a 150 s attract-demo
capture: **57.5 fps locked**, cpu 11.1–14.4 ms, 2–5 ms throttle sleep as visible headroom. No code
changed. User hand-check CLOSED same evening: "works great". (fps + sound integrity — gunshots/voices/music — the
vcop sound board has not been ear-checked under the thread).

## 2026-09-01 (night) — zerogun 8.7fps → the arm64 DRC back end; rchase2 jutter → billboard park

**zerogun (Model 2B) ran at 8.7 fps on the Quest (cpu 107 ms/frame)** while doaa (2A) held 57.5 —
the delta is the copro: 2B's ADSP-21062 SHARC ran its UML through the C back end because
`build-android.sh` passed `--NOASM=1` (the android.md §4.4 "untested consequence", now tested).
Desktop never showed it: zerogun costs the same ~2.9 ms core/frame as doaa there, native back end.
Fix: dropped `--NOASM=1`; `PLATFORM=arm64` builds `drcbearm64` + asmjit (link verified by symbol
count — the first incremental rebuild silently kept the C back end because removing a global define
dirties nothing; a clean `build/android/obj` wipe was required, same trap as the PROFILER flag).
Result on-device: zerogun 57.5, user-confirmed ("fantastic").

**rchase2 (2B) then read "a little juttery"**: cpu 13.3–15.8 ms against the ~15.5 ms budget — a
few-percent clip, not a deficit. PROFILER=1 ranking on a live run (steady through gameplay):
maincpu 20 / copro_adsp 21 / audiocpu 16–18 / **billboard:billcpu 6** / iocpu 3–4. The SCSP 68K is
NOT dominant on rchase2, so the SCSP-thread megaproject is not the next move; the billboard Z80 —
the cabinet LED marquee, write-only from the main CPU, never rendered by this core — is free CPU.

**Built the billboard park** (`m2vk_billboard.h` + `model2_billboard` "Cabinet Billboard" option),
mirroring the drive-board park: SUSPEND_REASON_DISABLE per-frame reconcile, live both ways, default
enabled (accurate), menu entry hidden on sets without the device (2O/2C/other families). Verified:
- Default path is a NO-OP to the bit: vf2 2500f `a8fdc34e55defa3d` identical on a stash-built
  pre-change core and the new one.
- ab.sh vf2 + srallyc: metrics reproduce the recorded baseline table to the digit, exact PASS.
- The park itself shifts device timing slightly (suspending the Z80 changes the interleave):
  rchase2 digests differ, final frames are the same scene a beat apart in animation phase —
  the lazy-baud class, stated in the option's INFO text.
- ⚠️ ab-baselines.md digest tables predate the lazy-baud default-ON flip and no longer match a
  default run's digests (metrics still reproduce). Regen pending, separate item.

Deployed. User steps: Install-or-Restore, then Core Options → Cabinet Billboard → disabled on the
Quest. Expected: rchase2's worst cpu ~15.8 → ~14.9, inside budget.

**Addendum:** input loss reported on rchase2 after the park was frontend-side, not the core — it
resolved on the user's end (desktop repro had already shown coin+start working with the board
parked, identical digest both arms). And by user call, `model2_billboard` now **defaults to
disabled** (parked): the board's output is invisible in this core on every set, so the accurate arm
buys nothing. get_billboard() tests "enabled" so an unreadable value lands parked. ⚠️ Harness
consequence: a default run now gets the parked machine — a digest that wants the stock machine pins
`M2VK_BILLBOARD=1`. A/B validity is unaffected (both renderers see the same machine either way).

## 2026-09-03 — development moved to Windows: host build, retrohost port, harness green

The Mac is no longer the reference machine. Before today the Windows box could only cross-build the
Android core; the desktop build had never been attempted and every verification instrument was
Mac-only. Now the host core, `retrohost`, `ab.sh` and `state.sh` all work here. Plan and phases in
[windows-move-plan.md](windows-move-plan.md); the standing host reference is [windows.md](windows.md).

1. **The host build needed three things, none of them large.** A per-host Vulkan-header candidate
   list in `libretro_m2.lua` (the old non-macOS default `/usr/include` is `C:\msys64\usr\include`
   under MSYS2 and holds nothing; genie is a *native* binary, so the drive-letter form has to be
   named — it cannot resolve the shell's `/mingw64/include`). One include in
   `m2vk_soundthread.cpp`: **libstdc++ 16** instantiates `std::vector<ui::menu_item>`'s defaulted
   constructor from `osdepend.h`'s `get_slider_list()` declaration alone, which the forward
   declaration cannot satisfy — upstream's own OSD sources already include `ui/menuitem.h` for this,
   and ours is the only file that includes `osdepend.h` directly. And `OS=Windows_NT` in the
   environment, which makefile:144 gates the entire Windows branch on and which a bash started from
   Git-for-Windows bash does not inherit. **The win32 branches in `libretro_m2.lua` were right as
   written** — the predicted `winutf8.cpp` link error and the missing `ws2_32` never materialised.
   Result: `Linking modelizer_libretro.dll`, 25 exported `retro_*` entry points.
2. **`retrohost` ported.** `<dlfcn.h>` → a `dl_open`/`dl_sym`/`dl_error` shim over
   `LoadLibraryA`/`GetProcAddress`; `<mach/mach.h>` RSS → `GetProcessMemoryInfo` (and
   `/proc/self/statm` for Linux, which the file never supported either); the MoltenVK candidate list
   → per-platform with `vulkan-1.dll` on Windows and `M2VK_HOST_VULKAN` as the override name
   (`M2VK_HOST_MOLTENVK` still accepted); `setenv` → `env_default()`. pthreads and
   `clock_gettime(CLOCK_MONOTONIC)` needed nothing.
3. 🚨 **The one real trap: MSYS2 rewrites POSIX paths in command-line arguments and NOT in
   environment variables.** Every path the harness hands the core through an env var —
   `M2_SAVE_DIR`, `M2VK_HOST_SAVE_AT`, `M2VK_HOST_LOAD_AT`, `M2_SYSTEM_DIR` — arrived as the literal
   `/tmp/...`, which native `fopen` resolves against the current drive root and fails to open.
   `state.sh` said `no state file was written`; **`ab.sh` PASSED while writing no NVRAM at all**, so
   the per-run save isolation the script exists to provide was silently not happening. New
   `devnotes/hostenv.sh` carries `CORE_EXT`, `EXE` and `hostpath()` (`cygpath -m` on Windows, a
   no-op elsewhere) and is sourced by all four harness scripts. `perf.sh`'s `ps -Ac -o comm=` and
   `uptime` are host-aware now too (MSYS2's own `ps` needs `-W` to see native processes).
4. **Verification, and it is the good kind.** `ab.sh vf2 2500`: covered 107568/107569, agreement
   **1.0000**, A-only 1, B-only 2, interior disagreements **0**, white **0**, same colour 95.554 %,
   ssim covered **0.996983** — the Mac's `ab-baselines.md` row for vf2 **to the digit**, and the
   background-reference digest `c3aaa56633c1c4f7` bit-identical to the Mac's. `state.sh vf2` PASSes
   all four controls (D == E, N != D, C == D, A == R). Vulkan arm runs on a real ICD:
   `NVIDIA GeForce RTX 3070 api 1.4.329`.
5. ⚠️ **The 3D digests do not match the recorded table and that is not a regression** — Windows
   reads `5035b4ef3a1e1084` / `e8051a92c7b6bc33` against `9c20f1fac9d9fe92` / `de94f44a06151f71`.
   `ab-baselines.md` predates the lazy-baud and billboard-park default flips, which move device
   timing on every host; metrics reproducing while digests move is exactly that signature. **The
   baselines want regenerating here.**
6. **Also written:** `devnotes/deploy-aoj.sh` — strips the Android core and installs it into the Age
   of Joy Unity project. The hand-copy in `Assets/Plugins/Android64` was the unstripped 103 MB
   build. Not yet exercised through a real Unity build.

**Next:** RetroArch for Windows (the play loop, and the `m2-vk.opt` a hand-check has to be read
against), then regenerate `ab-baselines.md`/`res-baselines.md` on this host, then rewrite CLAUDE.md
off its Mac assumptions.

### 2026-09-03 (later) — RetroArch on Windows, and a Game Launcher in front of it

The play loop is closed. RetroArch is at `C:\retroarch-win64` (portable: config beside the exe,
core options at `config\m2-vk\m2-vk.opt`, exactly the path CLAUDE.md names).

1. **The core plays under RetroArch on Windows.** `retroarch.exe -L modelizer_libretro.dll` with
   `--appendconfig video_driver="vulkan"`, vf2, `--max-frames 1800`: `Using HW render, vulkan driver
   forced`, `Using GPU: NVIDIA GeForce RTX 3070`, **1800 frames in 31 s** — 57.52 Hz, full speed —
   and the end-of-run screenshot is the attract fight rendering correctly at 57.546 fps. That is
   W1's real acceptance criterion, not "it compiled".
2. **`devnotes\shortcuts\Game Launcher.bat`** — the play command, standing in for the Mac's
   `Model 2.app`. Numbered list of the 68 installed sets grouped by family, type a number to play,
   quit RetroArch and the list is still there. Details in [windows.md](windows.md) §3; the three
   decisions worth recording:
   - **It runs the core from the repo** (`-L <repo>\modelizer_libretro.dll`) and prints its build
     timestamp in the header. The Mac's installed-core symlink silently reverted to a copy at least
     four times, each time meaning a stale core played while the build log looked healthy. There is
     now no second copy to go stale, and "am I playing what I just built" is on screen.
   - **It clears every `M2VK_*` / `M2OPT_*` variable** before launching, so a switch left in a shell
     cannot pin a play session — the same reasoning as the Mac app's `env -u`.
   - **It forces exactly one setting**, `video_driver = "vulkan"`, through `--appendconfig`. The core
     declares `RETRO_HW_CONTEXT_VULKAN` and will not load under any other driver; everything else
     stays with the menu, which is the whole point of having a separate play command.
   The catalogue is generated from the compiled driver table (`model1`, `model2`, `namcos21`,
   `namcos21_c67`, `namcos22`, `namcos23`) and filtered at startup to the zips actually present, so
   an empty family prints no header. The six device/BIOS zips and `driveyes` (namcos21_de.cpp, not
   compiled into this core) are deliberately absent.
3. **Tested without playing a game**: menu rendering, out-of-range / non-numeric / zero / negative
   input, and the composed command line captured through a stand-in frontend
   (`-L "…\modelizer_libretro.dll" --appendconfig "…\m2vk-launcher.cfg" "…\roms\vf2.zip"`). One
   real bug found and fixed in the process — an unguarded `goto menu` on an empty read spins forever
   at EOF (a piped run), so an empty-read counter now bails after 25.
4. **Noted, not a core problem:** the connected 8BitDo Ultimate 2C has no RetroArch autoconfig
   profile (`not configured, using fallback` in the log), so its buttons come from the fallback
   binding.

### 2026-09-03 (later still) — daytona crashed on Windows: the sound worker's machine was built on UB

First real bug the Windows port turned up, and it is not a Windows bug — it is a latent
out-of-bounds read that macOS and Android had been getting away with since the sound thread shipped.

**Symptom:** every model2o set (daytona, desert, vcop) killed RetroArch with `0xC0000005` the moment
`model2_sound_thread` was on — which is the core's default, so the Game Launcher hit it on the user's
first daytona pick. Reproduced under `retrohost` in one run; `M2OPT_model2_sound_thread=disabled`
booted fine, `M2VK_LAZY_BAUD=0` made no difference, which isolated it to the thread in four runs.

**Cause**, from a gdb backtrace (`driver_device::driver_device` ← `create_driver<m1snd_driver>` ←
`machine_config::machine_config` ← `worker_main`): `driver_device`'s constructor walks its own clone
chain, `driver_list::clone(m_system)` → `find(m_system)` → `assert(index >= 0)` → `driver(index)`.
`m1snd` is deliberately not in the generated driver list, `find()` returns -1, the assert is compiled
out of a release build, and `driver(std::size_t(-1))` indexes `s_drivers_sorted` out of bounds.
Full write-up in [m1audio-thread-plan.md](m1audio-thread-plan.md).

**Fix**, entirely in `m2vk_soundthread.cpp` — no upstream file touched: build the worker from a copy
of the m1snd descriptor renamed to the main system's short name. It is in the list by construction,
so the walk is over the real set's ancestry and the cached search path is the one this board's ROMs
would actually live under. `start()` verifies the name resolves before spawning.

**Verified:** all three model2o sets boot threaded and unthreaded; daytona and desert are
**bit-identical** either way (`4e127a7a92c659de`, `44b335a236d4407f`), vf2 (model2a, never split) is
identical either way, vcop's digest moves between arms — board-split interleave, the lazy-baud class.
RetroArch: daytona 2200 frames in 38 s at full speed with the user's live options. **User hand-check:
"looks and sounds correct."**

**Lessons worth keeping:**
1. **A second host is a test.** Three years of green runs on two platforms did not find this; the
   third platform found it in the first hour of play. The UB was always there.
2. **Release builds delete your asserts.** `assert(index >= 0)` was sitting directly on top of this
   and never fired, because nothing here is built with `NDEBUG` off. Guard with an `if` in code that
   can actually reach the bad path.
3. **The launcher's error hint was wrong and has been fixed** — it blamed `video_driver` for every
   nonzero exit. It now names `0xC0000005` as a core crash and prints the `retrohost` line that
   reproduces it outside the frontend.

**Shipped to the Quest/AoJ side:** Android core rebuilt with the fix and staged into the Unity
project via the new `deploy-aoj.sh` (99M → 67M stripped; the copy that was in
`Assets/Plugins/Android64` carried the UB). Unity APK build and device test are the user's.

⚠️ **Still open and unrelated:** the serial bridge dying ~11 s in (m1audio-thread-plan.md). A daytona
session longer than that in AoJ should be expected to lose SFX; today's fix does not touch it.

**DEVICE-CONFIRMED 2026-09-03:** the fixed core went into an Age of Joy APK (Unity 2022.3.18f1) and
onto the headset — user reports it "works great". So the whole chain is closed on Windows:
edit → `make` → `retrohost`/`ab.sh` → `build-android.sh` → `deploy-aoj.sh` → Unity APK → Quest.
The Mac is not in it anywhere.

⚠️ **The serial-bridge bug's status is now genuinely unclear, and that is the honest statement.** The
user has played daytona at length on the fixed build with no audio trouble at all, which is not what
a bridge dying at 11 s predicts. Two readings, not yet separated: today's fix cured it (the old UB
was constructing strings out of unmapped memory during the worker's setup, which is the right shape
for damage that surfaces much later), or it never reproduced outside the Mac. **Do not quote the
"94 % of the command stream never arrives" figure as current** — it was measured on the Mac, on a
build whose worker machine was constructed on undefined behaviour.

The cheap way to settle it, when it matters: an env-gated counter on `g_to_sound` / `g_to_main`
pushes vs deliveries (both are ours, in `m2vk_soundthread.cpp` — no upstream logging hack needed as
the 2026-09-01 recipe used), over a 6000-frame scripted race, threaded vs unthreaded.

## 2026-09-04 (later) — System 23 Lever 1: JVS HLE I/O board built (O2)

Implemented `plan_system23optimization.md` phase O2: `namco_tssio_hle_device`
(`src/devices/bus/jvs/namcoio.cpp`, next to `namco_em_pri1_01_device`), a game-agnostic
`jvs_hle_device` subclass standing in for the real Hitachi H8/3334 boards (`namco_tssio`/`namco_csz1`)
timecrs2/timecrs2v4a/crszone accept. Registered as `NAMCO_TSSIO_HLE` in namcoio.h and
`"namco_tssio_hle"` in jvs.cpp's `jvs_port_devices` — no build-script edits needed there, since bus/jvs
is already in every genie config.

Gated it the way the sound-thread/lazy-baud levers are gated: new `m2vk_jvs.h`/`.cpp` in
`src/osd/libretro_m2/` (added to `libretro_m2.lua`'s OSD project files, same as m2vk_baud), exposing
`m2vk_jvs::tssio_hle_enabled()` / `set_option_enabled()`, `M2VK_JVS_HLE` env override. The only
namcos23.cpp edit is the three `set_default_option()` call sites in `timecrs2()`/`timecrs2v4a()`/
`crszone()`, each wrapped `#ifdef S23VK` to pick `"namco_tssio_hle"` (or `"namco_csz1"`'s HLE stand-in
for crszone) vs the real board. Also wired a real menu entry, `system23_jvs_hle` ("JVS HLE I/O Board"),
hidden from every non-System-23 family via `apply_family_cascade`; its DEFINITIONS default is the
first platform-conditional default in `retro_options.cpp` — `enabled` under `__ANDROID__`, `disabled`
elsewhere, matching the plan's "default-on Android."

`switch_count=16` (2 bytes) — checked against the actual `JVS_PLAYER1` bit layout in namcos23.cpp's
`timecrs2`/`crszone` INPUT_PORTS (trigger 0x01, enter 0x02, down/up 0x10/0x20, service 0x40, link-id
0x4000, pedal 0x8000, crszone's motor-test 0x2000): every used bit is ≤ bit 15, so the spec'd count is
correct against the driver, not just plausible.

Built clean: `make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j24` on the Windows box,
compiled all six touched files and linked `modelizer_libretro.dll` with no errors.

**Not validated yet** — no `timecrs2`/`crszone` ROMs in `devnotes/roms`, so none of O2's three
validation steps (host A/B digest, boot check, Quest re-profile + hand-check) have run. Do not treat
the Android default as trustworthy until they have; see the plan doc's O2 section for the checklist.

## 2026-09-04 (later still) — O2 continued: ROMs added, real bug found, boot still not reached

`timecrs2.zip`/`crszone.zip` added to `devnotes/roms`. Boot-checked the HLE board (built earlier today,
see the prior entry) — both games hang at a firmware self-test screen instead of reaching attract.

Traced it with `jvshle.cpp`'s `VERBOSE` macro (temporarily enabled, reverted before finishing — never
committed). Root cause: `:subcpu` (H8/3002) is itself the JVS master (its SCI0 is the JVS wire) and its
own POST sends a Namco vendor command, `0x70` (fixed 3-byte body, `70 04 70 02` on the wire), that the
real TSS-I/O/CSZ1 firmware answers but the generic `jvs_hle_device` base doesn't — it came back
UnknownCommand and `:subcpu` restarted the whole JVS enumeration forever. Added a handler in
`namco_tssio_hle_device::execute()` (namcoio.cpp) acknowledging it; this stops the reset-loop cleanly
(pacman animation now runs for hundreds of frames instead of erroring at once) but does **not** get
either game to attract — both still hit "SUBCPU INITIALIZE TIME OUT" a bit later. Tested two response
contents (bare ack, echo-the-request) with identical results (same digest, same frame the error
appears), which argues 0x70 is a periodic keepalive, not the actual readiness gate — so the remaining
block is something else in `:subcpu`'s POST, not yet identified. Candidates noted in the plan doc:
namco_settings/RTC path on SCI1, or something unrelated to JVS entirely.

Also learned two things blocking further validation on this box: `namco_tssio.zip`/`namco_csz1.zip`
(the real board's own small MCU ROM, separate from the game ROM) aren't on hand, so the real-board
comparison that would settle "is this hang pre-existing or HLE-specific" can't run yet; and the GPU
currently has too little free memory for ANY Vulkan-path run to work (confirmed via a known-good `vf2`
control test also hitting `VK_ERROR_OUT_OF_DEVICE_MEMORY`), so Vulkan-path testing is on hold until
that clears.

**Status: O2 code is real progress (a genuine protocol bug found and partially fixed) but is not yet a
working board.** Full detail, including the exact trace and the honest "not sufficient" framing, is in
the comment on `namco_tssio_hle_device::execute()` and in `plan_system23optimization.md`'s O2 section.
Do not ship or rely on the Android default until a game actually reaches attract mode.

## 2026-09-05 — O2 root-caused: timecrs2 boots bit-identical to the real board, crszone boots but not byte-perfect

Continuing 2026-09-04's O2 work: `namco_tssio.7z`/`namco_csz1.7z` (the real JVS I/O boards' own small
MCU ROM sets) were added to `devnotes/roms`, which unlocked the decisive test the prior session was
missing — a real-vs-HLE differential trace.

Enabled `h8_sci_device`'s `LOG_DATA` (the H8 SCI *hardware peripheral*, not the board — this traces a
full-CPU-emulated board too, unlike `jvshle.cpp`'s own VERBOSE which only instruments HLE devices) on
`:subcpu:sci0` against both real boards. This is what actually cracked it: captured the real boards'
exact reply to the `0x70` vendor command that was hanging both games (see 2026-09-04's entry), and it
turned out to be a fixed 11-byte payload, board-specific, not a generic ack — hardcoded it per board via
a new `jvs_70_reply()` virtual. Also found from the same trace: real `command_revision`/`jvs_revision`
are 0x11/0x20 (not jvshle's 0x13/0x30 defaults), real `switch_count` is 12 (not the 16 guessed from
INPUT_PORTS bit positions), and — the big one — crszone's real board is a **CSZ1 MIU-I/O, not a
TSS-I/O**, with its own device_id, 4 output slots (not 3), and an analog output channel TSS-I/O lacks
entirely (the kick motor). Added `namco_csz1_hle_device : public namco_tssio_hle_device`, mirroring the
real boards' own inheritance shape. Also fixed a genuine upstream one-line bug in `jvshle.cpp`'s
`device_start()`: `screen_position_input_ybits()`'s result was being written into
`m_screen_position_input_xbits` again instead of `m_screen_position_input_ybits` — invisible for this
driver (both games use 16 for both), but a real bug for any future user where the two differ.

**Result:** timecrs2, 3000 frames, no input, software renderer: real board and HLE board produce the
**identical digest** (`3a151f72c23d01db`) — full textured 3D attract intro, matching the plan's own
"STARLINE NETWORK" baseline description. crszone reaches the same attract scene as the real board (the
URDA commander) and looks right side-by-side, but the digest doesn't match (`f6b25722eaf584e6` real vs
`8e4577de95811f98` HLE) — confirmed reproducible, confirmed not a gross protocol failure (same commands,
same order, no extra retries), ~13% RMSE between final frames. Not root-caused; leading guess is a
timing artifact from CSZ1's extra ANLOUT round-trip per poll (5 commands vs TSS-I/O's 4) meeting HLE's
near-zero response latency, but that's unverified speculation, not a finding.

Two of O2's three validation steps are now done: host A/B (timecrs2 clean, crszone close), and boot to
attract for both games (no scripted input, static digest/screenshot checks only, per CLAUDE.md). Still
open: the Quest re-profile and the numbered hand-check — needs the headset, which was disconnected for
this session. Full detail, the exact captured bytes, and the honest state of what's unresolved is in
`plan_system23optimization.md`'s O2 section and the class comments in `namcoio.cpp`.

## 2026-09-05 (later) — O2 closed out: Quest re-profile + hand-check, both confirmed

Headset came back online same session as the host-side validation above. Full loop run for both
games: fast build → deploy → user hand-check; profiler build → deploy → user plays → `adb logcat -d -s
m2prof:V` → fast build restored → deploy. Both games' hand-checks came back clean (gun aim/trigger,
pedal, coin/start all working, nothing flagged as broken).

**Profiler result: `:jvs:...:iocpu` is gone from the profile entirely, for both games** — better than
the ~1-2% predicted, because the HLE board has no CPU device at all. Remaining time is `:maincpu`
(35-46%, scales with scene complexity — crszone's higher share matches its 3-4x polygon count vs
timecrs2) and `:subcpu` (20-25%, the *separate* H8/3002 sound MCU this work never touched — that's the
plan's already-identified "Lever 2", not a leftover from this one). Nothing anomalous.

**Fps, user-reported:** timecrs2 "runs smoothly." crszone was ~6fps before this session (never
individually profiled before now — the plan's headline numbers were timecrs2-only) and is now ~45fps,
a ~7.5x jump, with the profiler confirming the remaining shortfall is legitimate CPU cost rather than
a JVS inefficiency.

**Real mistake caught and fixed along the way, worth remembering:** the first "fast" Android rebuild
this session was only `REGENIE=1` (incremental, no `rm -rf build/android/obj`), and turned out
byte-identical to the profiler build — stale `.o` files from before this session silently carried over
despite the regenerated Makefile having different `-DMAME_PROFILER` flags, because plain `make` only
checks source-file timestamps, not compiler-flag changes. Caught it by hashing/`cmp`-ing the two
backup `.so` files before trusting either. **Rule going forward: `rm -rf build/android/obj` on BOTH
sides whenever `PROFILER` changes, not just when turning it on** — verify with `grep -ac m2prof <.so>`
(0 for a real fast build) before deploying either.

O2 is now closed: both validation steps that were open this morning (Quest re-profile, hand-check) are
done. Only remaining loose end is crszone's small host-side digest gap (visually identical to the real
board, not root-caused, not blocking — see the O2 section above) — a follow-up, not a blocker.

---

## 2026-09-04 — savestates disabled core-wide

`retro_serialize_size()` returns **0** for every family; `retro_serialize` / `retro_unserialize`
return **false**. RetroArch greys the save/load slots out for the session. Core builds clean on the
Windows host (`modelizer_libretro.dll`).

**Why.** The feature was never uniformly trustworthy across the four families — `vcop2` never passed,
the Model 1 TGP-copro / `gen_fifo` gap was never verified (`model1update.md`), the pipelined Android
path already dropped states — and every renderer change was being gated on a harness only half the
cores could satisfy. A state that loads a wrong future is worse than no state. This is a decision
about what we *promise*, not a discovery that the code broke.

**What changed, exactly.** Three function bodies in `retro_entry.cpp`, plus the
`RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS` call and the size line in the load-game log. Nothing
else. `m2vk_savestate.cpp` (including the `gen_fifo` trailer), `m2vk_snd::state_*` and
`libretro_m2_osd_interface::state_{size,save,load}` all still compile and still work — re-enabling is
restoring three bodies.

⚠️ **The startup spin loop in `retro_load_game` that waits on `state_size() != 0` was KEPT** and must
stay. It is no longer about savestates; it is the thing that fixes how many frames the machine runs
before `retro_run` #1. Every recorded `ab.sh`/`res.sh` digest and the documented constant −1
host↔emulated frame offset were measured with it in place, so deleting it would silently shift all of
them. Its comment now says so.

**Harness consequence.** `devnotes/state.sh` is retired — it now prints why and exits 2 unless
`STATE_SH_I_KNOW=1`. Savestates are dropped from the exit criteria of `modelizer-plan.md` (the
per-family merge check), `plan_system23.md` 23-7 and `model1update.md` step 5. `ab.sh`, `res.sh` and
`perf.sh` are unaffected and remain the gates. The `M2VK_SAVE_*` switch group is inert.
