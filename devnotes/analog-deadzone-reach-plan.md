# Analog Deadzone + Reach — plan (non-racing analog games)

**Status: BUILT + statically verified (2026-08-26); user hand-check open.** Two live core options for
the analog-stick games (Star Blade, twin-stick, flight sets) that today eat MAME's raw 15%/85%
deadzone/saturation with no compensation. Mirrors the steering pipeline; leaves the wheel games untouched.

**Files:** `m2vk_analog.h` (new shaper, sibling of `m2vk_steer.h`); wired in `retro_options.{h,cpp}`
(two keys + getters + DEFINITIONS), `libretro_m2_input.{h,cpp}` (detector capture + `shape_analog()`),
`libretro_m2_osd.cpp` (`analog_frame`/`analog_close`), `retro_entry.cpp` (load/live apply + visibility
block + switch-override log). All three subtargets build clean.

⚠️ **The "mutually exclusive with wheels" premise was FALSE for desert (Desert Tank).** The input dump
shows desert declares BOTH `P1_PADDLE` (steering → left X) AND `P1_AD_STICK_Y` (turret/throttle → left Y),
so both detectors fire. `shape_analog()` therefore **skips left X whenever `steer().active`** — the
paddle axis is already shaped by `shape_and_publish_steer()`, and compounding both transforms on one
axis would corrupt the steering. desert's AD_STICK_Y is still shaped (the throttle caveat below).

**Static verification done:** math replica proved full deflection → MAME full lock (pre-comp identity),
first non-zero output lands exactly on MAME's 0.15 deadzone floor, monotonic + sign-symmetric, diagonals
preserved (x==y ⇒ out_x==out_y). Visibility gate confirmed via the option-display log: shown on
skytargt/gunblade/waverunr/desert **and the real S21/S22 targets — starblad, cybsled, cybrcomm, propcycl**
(ROMs live in `devnotes/roms/system22/`, run starblad/cybsled with `namcos21_libretro.dylib`, the S22
sets with `namcos22_libretro.dylib`) — hidden on daytona (wheel) / vf2 (fighter) / von (digital joystick)
/ motoraid / powsled. Input-dump roles: starblad AD_STICK_X/Y (centred aim), cybsled & cybrcomm P1+P2
AD_STICK_X/Y (twin-stick), propcycl AD_STICK_X/Y + AD_STICK_Z "Cycle Pedal". Switch overrides
(`M2VK_ANALOG_DEADZONE/REACH/LINEAR`) log and apply. `ab.sh vf2` exact-match criterion 1 still holds
(input moves no pixel). The in-game edge-reach / centre-drift / diagonal feel is the user hand-check.

**Throttle caveat (confirmed, not just suspected):** desert `AD_STICK_Y` (center≈20/255), gunblade &
waverunr `AD_STICK_Y` (Pitch/Throttle, center≈15) rest near minimum rather than centred. Deadzone/reach
shape them symmetrically around the rest point — harmless, but note a small deadzone now trims the
throttle's low end.

## The defect
MAME maps every absolute axis through `out = (|raw| − 0.15) / (0.85 − 0.15)` — dead below 15% travel,
full at 85%. [m2vk_steer.h](../src/osd/libretro_m2/m2vk_steer.h) `steer_shape()` already
pre-compensates this for the wheel axis, but it only fires on `IPT_PADDLE`. The `IPT_AD_STICK` games
get nothing: 15% of centre travel is dead and the stick's outer 15% is wasted. Star Blade is the
motivating case (aims with `IPT_AD_STICK_X/Y`).

## Design

### New shaper — `m2vk_analog.h` (new file, mirrors `m2vk_steer.h`)
Per-axis, no gamma, no damping. **Reach replaces range** (range multiplies max output *down*; reach is
the saturation point — where full deflection is reached):
```
u = raw / ABS_MAX;  mag = |u|
if mag <= dz: return 0
a = clamp((mag − dz) / (reach − dz), 0, 1)          // dz, reach = our options
out = mame_dz + ceil(a * (mame_sat − mame_dz))       // same pre-comp as steer_shape()
return sign(u) * out
```
Defaults `dz = 0.05`, `reach = 1.00` → net effect dead below 5%, full at 100% stick. Replaces MAME's
15/85 for these axes.

**Detector:** reuse `steer_resolve()`'s port loop, matching `IPT_AD_STICK_X/Y/Z` → `analog.active`.
Mutually exclusive with wheels in practice (driving = `IPT_PADDLE`, no AD_STICK), so **driving games
are untouched by construction** and their menu never shows these options.

### Diagonals are provably safe (the explicit concern)
Same scalar transform applied to each axis **independently (axial, exactly as MAME does today)**. For
any diagonal the stick reads `raw_x == raw_y`, so `f(raw_x) == f(raw_y)` → output stays true 45°.
Diagonals break only if X and Y get *different* transforms, or if we switch to a radial magnitude
model — we do neither. Verified statically with a value-sweep table (`x == y` ⇒ `out_x == out_y`,
monotonic), not in-game.

### Hook
In `libretro_m2_pad_device::update()` tail, after the lightgun-zero gate and alongside
`shape_and_publish_steer()` ([libretro_m2_input.cpp:404](../src/osd/libretro_m2/libretro_m2_input.cpp#L404)),
applied to the four stick axes `m_axes[0..3]` (Star Blade uses left X/Y; twin-stick uses both sticks),
only when `analog.active`. The steer path is unchanged.

⚠️ Some flight sets bind `IPT_AD_STICK_Y` to a throttle/pedal (steer's Y/pedal caveat). Confirm the
per-axis roles with `M2VK_INPUT_DUMP` before shaping Y on those; a reduced deadzone on a throttle is
harmless but note it.

### Options (`retro_options.cpp` / `.h`)
| Key | Label | Values | Default |
|---|---|---|---|
| `model2_analog_deadzone` | Analog Deadzone | 0%, 2%, 5%, 10%, 15% | **5%** |
| `model2_analog_reach` | Analog Reach | 100%, 95%, 90%, 85%, 80%, 75% | **100%** |

- Live-apply (like steering), via a `set_option_analog(dz, reach)` parker + `analog_apply()`.
- Env overrides `M2VK_ANALOG_DEADZONE` / `M2VK_ANALOG_REACH` for static harness testing (presence
  overrides the option, matching the steer-switch discipline).
- **Visibility:** new block in `retro_run` mirroring the steering block at
  [retro_entry.cpp:1017](../src/osd/libretro_m2/retro_entry.cpp#L1017) — show only when
  `analog.active`, hidden on wheels/guns/fighters. Gate on the **detector, not family**, so Star Blade
  (System 21) shows them. Do **not** add these keys to the per-family `hide_option` lists.

("Analog Reach" chosen over Sensitivity/Full Deflection/Saturation on 2026-08-26 — it reads directly
against the %: 100% = push the stick all the way. Distinct from "Steering Range", which lowers max
lock in the opposite direction.)

## Testing — no gameplay scripting (CLAUDE.md ban)
1. `M2VK_INPUT_DUMP` on `starblad` + candidates (`cybsled`, `von`, flight sets) → confirm which axes
   are AD_STICK; flag any AD_STICK_Y-as-throttle before shaping Y.
2. Static transform table (math replica): tabulate output for `x == y` and check monotonicity → proves
   diagonals + nothing dead past reach.
3. `M2VK_HOST_DESCRIPTORS` → options present on Star Blade, absent on `daytona` (wheel) and `vf2`
   (fighter).
4. `ab.sh` green on a fixture — regression guard only (input moves no pixel).
5. Then a numbered hand-check list for the user: Star Blade edge reach at 100%, no centre drift, true
   diagonal aim, and negative controls (wheel/fighter menus show no analog options).

## Build
`make SUBTARGET={model2|namcos22|namcos21} OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10`
