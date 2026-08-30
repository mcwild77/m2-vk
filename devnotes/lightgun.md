# The lightgun — plan, and as built

✅ **ALL SEVEN STEPS DONE, 2026-07-28.** §3 is the record: every step is struck through with what
actually shipped under it, which is more than once *not* what the step said. `vcop` and `vcop2` aim
with a real gun on either port, reload by shooting off the screen, carry our own reticle, and reach
their test menus from a pad layout and a service combo that match FBNeo. **No upstream file was
touched in the whole phase** — the diff against mame0288 is still 30 lines — and vf2 at 2500 frames
reproduces both accuracy baselines to the digit, which is §4 check 5 and the thing this phase could
most easily have broken.

✅ **All of it is committed.** Steps 1–3 are `607d9f6528b` + `e905bf4b159`; steps 4 and 6 went in
together as **`89bdbde4d33`**, which is the single commit §3 step 7's last bullet said would be the
honest outcome — they are not separable by file. Step 7 is `devnotes/` only and never ships.

**Written 2026-07-27**, against HEAD `eaa1355f451`. Scoping is
[user-options.md §7](user-options.md); this file is the *order of work* and the decisions §7 left
open. Line numbers are at `eaa1355f451`.

Everything in §1 was read out of the tree during planning, not assumed. Three of §7's four open
questions are now closed by that reading, and **one of them closes in the opposite direction to the
way §7 framed it** — §1.3.

The reference implementation is `../flycast-aoj`, a Flycast fork with a working libretro gun. It
answers the *libretro* half exactly and the *emulator* half not at all, for the reason in §1.6.

**§2.5 and step 6 were added later the same day**, after a smoke test asked whether the pad mapping
matches other libretro arcade cores. The references there are **FBNeo** and **MAME 2003-Plus**, read
at their `master` on 2026-07-27; the decision was **match FBNeo**, and §2.5 records the three places
where "exactly like FBNeo" cannot be taken literally on this hardware.

---

## 1. What is settled, and the evidence

### 1.1 Device default assignments **add**; they never replace

`ioport_manager::apply_device_defaults` ([ioport.cpp:2685](../src/emu/ioport.cpp#L2685)) starts from
the type's existing sequence and ORs onto it:

```cpp
input_seq remapped(found->seq(seqtype));
if (!remapped.empty())
    remapped += input_seq::or_code;
```

So our pad's `add_directional_assignments`
([libretro_m2_input.cpp:208](../src/osd/libretro_m2/libretro_m2_input.cpp#L208)) does **not** displace
the core default at [inpttype.ipp:831](../src/emu/inpttype.ipp#L831), which is
`GUNCODE_X_INDEXED(n) | MOUSECODE_X_INDEXED(n)`. `IPT_LIGHTGUN_X` for player 1 already reads
**gun OR mouse OR left-stick-X** today, on every one of the 83 sets.

This is the answer to §7.3, and it is the good half. It means adding a `DEVICE_CLASS_LIGHTGUN` device
requires **no assignment surgery at all**: the binding the core is looking for already exists.

### 1.2 🚨 But OR'd **absolute** axes SUM — that is the actual hazard

`accumulate_axis_value` ([input.cpp](../src/emu/input.cpp), called from
`input_manager::seq_axis_value`):

```cpp
else if (ITEM_CLASS_ABSOLUTE == valueclass)
{
    // absolute values override relative values
    if (ITEM_CLASS_ABSOLUTE == resultclass)
        result += value;          // <-- two absolute axes ADD
    ...
}
// ... and at the end:
if (ITEM_CLASS_ABSOLUTE == itemclass)
    result = std::clamp(result, ABSOLUTE_MIN, ABSOLUTE_MAX);
```

§7.3 guessed the failure would be "the stick and the gun fight over the port". It is more specific
than that: **they are summed and then saturated.** With a gun aiming at screen centre and the stick
pushed left, the aim goes hard left. With the gun at the left edge and the stick nudged left, the aim
saturates and the reticle stops moving. Both look like calibration bugs.

**Corollary that makes the fix trivial:** a contributor at exactly 0 adds nothing. So the rule is
**exactly one absolute source is allowed to be non-zero at a time**, and it is enforced in our own
`update()`, not by touching assignments. See §2.2.

### 1.3 A disabled device class contributes 0 — so `-nomouse` stays and `-lightgun_device` is unneeded

`input_manager::code_value` ([input.cpp:479](../src/emu/input.cpp#L479)) returns 0 before reading any
item when `!devclass.enabled()`. Two consequences:

- The `MOUSECODE` half of the core default is **already inert** because `-nomouse` is in the argument
  vector ([retro_entry.cpp:359](../src/osd/libretro_m2/retro_entry.cpp#L359)). §7.4 expected `-nomouse`
  to "need a second look". It does not — leave it exactly as it is.
- `DEVICE_CLASS_LIGHTGUN` is enabled by `machine().options().lightgun()`
  ([inputdev.cpp:656](../src/emu/inputdev.cpp#L656)), which defaults to `"0"`
  ([emuopts.cpp:155](../src/emu/emuopts.cpp#L155)). So the GUNCODE half is inert today too.

`init_autoselect_devices` ([ioport.cpp:1919](../src/emu/ioport.cpp#L1919)) only ever *enables the
class* — and it returns early at `if (autoenable_class->enabled()) return;`. **So `-lightgun_device`
does nothing once `-lightgun` is passed.** §7.4 expected both. Pass `-lightgun` only, and pass it
**unconditionally** (§2.1).

### 1.4 ✅ Offscreen reload is the driver's job and it is already written — §7.6 is closed

This is the finding that changes the shape of the work. `model2_state::lightgun_offscreen_r`
([model2.cpp:1136](../src/mame/sega/model2.cpp#L1136)) — reached through `lightgun_mux_r` when the
game selects mux ≥ 8 — computes a **5 % border from each port's own `PORT_MINMAX`** and sets the
player's bit when the axis lands inside it:

```cpp
const float BORDER_SIZE = 0.05f;
const int BORDER_P1X = (…field(0x3ff)->maxval() - …field(0x3ff)->minval()) * BORDER_SIZE;
…
if (P1X <= (…minval() + BORDER_P1X)) data |= 1;
if (P1X >= (…maxval() - BORDER_P1X)) data |= 1;
```

So **offscreen is not "a value outside the range" — it is "a value pinned at the edge of the
range"**, which is exactly what a clamp produces. Nothing has to survive the clamp; the clamp is the
mechanism.

Worked example, `vcop` P1_X (`PORT_MINMAX(0x083, 0x276)`,
[model2.cpp:1796](../src/mame/sega/model2.cpp#L1796)): border = `(0x276 − 0x083) × 0.05` = 24, so the
trip point is `P1X ≤ 0x09B`. Driving our OSD axis to `ABSOLUTE_MIN` gives `P1X = 0x083`. ✅

**Implementation, therefore:** `IS_OFFSCREEN` / `RELOAD` → force the device's X *and* Y items to
`osd::input_device::ABSOLUTE_MIN`. That is it. Flycast's `(0,0)` sentinel
([libretro.cpp:2877](../../flycast-aoj/shell/libretro/libretro.cpp#L2877)) is the same trick against a
different target.

⚠️ **Two corrections to §7.6, whose conclusion held but whose mechanism was wrong.**

1. The clamp in `apply_min_max` ([ioport.cpp:3744](../src/emu/ioport.cpp#L3744)) is against
   `m_minimum`/`m_maximum`, which for an **absolute** field are left at
   `ABSOLUTE_MIN`/`ABSOLUTE_MAX` (±65536) by the constructor
   ([ioport.cpp:3577](../src/emu/ioport.cpp#L3577)) — the `if (m_absolute)` branch never touches
   them. `PORT_MINMAX` arrives as `m_adjmin`/`m_adjmax`, which set `m_scalepos`/`m_scaleneg`
   ([ioport.cpp:3657-3687](../src/emu/ioport.cpp#L3657)). Same practical result — full-scale OSD
   input maps *exactly* onto the calibrated window and cannot exceed it — different reason, and the
   reason is what a future reader needs.
2. **The outer 5 % of the playfield reads as offscreen.** Aim at the screen edge with a real pointer
   and the game reloads. That is MAME's behaviour for every frontend and not something to "fix" on
   our side. Expect it to be reported as a bug; it is in §5's acceptance notes for that reason.

### 1.5 ⚠️ MAME's crosshair is inert in this core — §7.8's first acceptance check does not exist

`render_crosshair::draw` adds a quad to `screen.container()`
([crsshair.cpp:444](../src/emu/crsshair.cpp#L444)) — a render-container primitive. Our OSD reads
pixels straight off `screen->curbitmap()`
([libretro_m2_osd.cpp:276](../src/osd/libretro_m2/libretro_m2_osd.cpp#L276)) and composites the layers
itself, so **container quads are never drawn by anything**. `PORT_CROSSHAIR` on all six gun sets is
dead weight here.

Flycast has the same problem and solves it the same way we will have to: it ships its own reticle
([vmu_xhair.cpp](../../flycast-aoj/shell/libretro/vmu_xhair.cpp)) and blits it in the frontend layer.

This retires §7.8's acceptance check 1 as written and is why §3 step 1 (the instrument) comes before
everything else: **with no crosshair there is nothing to eyeball either.**

### 1.6 What flycast answers, and what it cannot

Answers, and worth copying verbatim:

| | flycast | reference |
|---|---|---|
| Two device types, gun **and** pointer | `RETRO_DEVICE_LIGHTGUN` + `RETRO_DEVICE_POINTER`, same emulated gun | [libretro.cpp:2549](../../flycast-aoj/shell/libretro/libretro.cpp#L2549) |
| `RELOAD` = offscreen **plus a synthetic trigger press** | `force_offscreen = true;` then presses the trigger itself | [2866-2873](../../flycast-aoj/shell/libretro/libretro.cpp#L2866) |
| Pointer reload = two fingers down | `POINTER_COUNT > 1` | [2904](../../flycast-aoj/shell/libretro/libretro.cpp#L2904) |
| The stick path is **kept**, not replaced | `updateLightgunCoordinatesFromAnalogStick` still runs on a JOYPAD port | [2821](../../flycast-aoj/shell/libretro/libretro.cpp#L2821) |
| A gun port has no L3/R3, so no TEST/SERVICE | the AoJ patch polls `RETRO_DEVICE_JOYPAD` L3/R3 alongside the gun ids | [2853-2862](../../flycast-aoj/shell/libretro/libretro.cpp#L2853) |

Cannot answer: everything downstream of the coordinate. Flycast writes `mo_x_abs/mo_y_abs` straight
into a JVS register where `(0,0)` is the hardware's own offscreen sentinel
([maple_jvs.cpp:303-315](../../flycast-aoj/core/hw/maple/maple_jvs.cpp#L303)) — no ioport layer, no
`PORT_MINMAX`, no clamp, no assignment defaults. §1.1–§1.4 had to come out of MAME.

### 1.7 🚨 The reticle: same shape, our own bytes

`vmu_xhair.cpp` is **GPL-2.0-or-later** (Flycast's header). [legalstuff.md](legalstuff.md) rests the
release on "585 of 606 linked objects BSD-3-Clause, **zero GPL-tagged**", and copying that file's
array into `src/osd/libretro_m2/` would put a GPL-tagged object in the link for a 16×16 bitmap.

The bitmap is a plain gapped cross and needs no copying — 16×16, arms 2 px thick and 6 px long, a
4 px gap at the centre ([vmu_xhair.cpp:85](../../flycast-aoj/shell/libretro/vmu_xhair.cpp#L85)).
**Generate it procedurally**, from the geometry, in our own BSD-3 file. Same reticle on screen,
nothing borrowed, and it scales cleanly for `M2VK_SS` for free.

---

## 2. The design

Four decisions, each following from §1.

### 2.1 Always pass `-lightgun`; always create the gun devices

MAME options are fixed when the machine is created, but `retro_set_controller_port_device` can arrive
at any time — including mid-run, and RetroArch will do exactly that from its input menu. Making
`-lightgun` conditional would mean a device change needed a reload.

It does not have to be conditional, because of §1.3 in reverse: **an enabled class with a device
reporting 0 contributes 0.** So enable it always, create both gun devices always, and make device
selection a pure gate on which device *moves*. No restart, no argv timing problem, no
`-lightgun_device`.

### 2.2 Exactly one absolute source is non-zero at a time

The rule §1.2 forces, enforced in `update()` on both devices and nowhere else:

| port's selected device | pad `m_axes[LEFT_X/Y]` | gun `m_axes[X/Y]` |
|---|---|---|
| `RETRO_DEVICE_JOYPAD` (default) | read from the stick | **forced 0** |
| `RETRO_DEVICE_LIGHTGUN` / `POINTER` | **forced 0** | read from the frontend |

Both sources at 0 means the port sits at its default value, which for every gun set is the centre of
`PORT_MINMAX` — so the neutral state is well-defined and the switch is glitch-free in both
directions.

⚠️ **Only the two stick axes are gated.** Triggers, buttons, D-pad and the service pair keep working
on a gun port — that is §2.4, and it is deliberate.

### 2.3 The gun device

A second device class in `libretro_m2_input.cpp`, created alongside the two pads:

- `DEVICE_CLASS_LIGHTGUN`, **`MAX_GUNS`** of them — 2, matching the two-player cabinets. ⚠️ **Not
  `MAX_PADS`, which is now 4** for `airwlkrs`. Both constants already exist
  ([libretro_m2_input.h:86-95](../src/osd/libretro_m2/libretro_m2_input.h#L86)); `MAX_GUNS` was added
  on 2026-07-27 with no reader yet, and this is its reader. Using `MAX_PADS` here would quietly create
  four guns.
- Items: absolute `ITEM_ID_XAXIS` / `ITEM_ID_YAXIS`, plus `ITEM_ID_BUTTON1` for the trigger and
  `ITEM_ID_BUTTON2`/`3` for AUX_A/B.
- **Default assignments: `IPT_BUTTON1` → the trigger item, and nothing else.** The axes need no
  assignment — `GUNCODE_X_INDEXED(n)` is already in the core default (§1.1). Adding one would be
  harmless but would put a second copy of a binding that already exists into our file.
- `SCREEN_X`/`SCREEN_Y` are `-0x8000..0x7fff`; map to `ABSOLUTE_MIN..ABSOLUTE_MAX` via
  `normalize_absolute_axis`, exactly as the pad does for a stick. **No scale factor of ours anywhere**
  — §7.2's rule, and §1.4 explains why obeying it is also what makes reload work.

Buttons are `ITEM_CLASS_SWITCH`, so OR-ing them with the pad's is a genuine "either works" and has
none of §1.2's summing problem. The trigger therefore works on the pad's B *and* the gun's trigger
simultaneously, which is what a player with a gun in one hand and a pad on the table wants.

### 2.4 Service buttons on a gun port — the AoJ fix, and we have the same hole

`model2_service_buttons` binds L3/R3 on the pad
([libretro_m2_input.cpp:270-278](../src/osd/libretro_m2/libretro_m2_input.cpp#L270)), and a port set
to `RETRO_DEVICE_LIGHTGUN` reports no L3/R3. Since this OSD draws no MAME UI, that would leave the
gun games — the ones whose calibration menus you most want — with no way into test mode at all.

Fix is flycast's: the pad device keeps polling `RETRO_DEVICE_JOYPAD` for **buttons** on a gun port
regardless of selected device. Only the stick axes are gated (§2.2). Costs nothing and no new code
path — it is the *absence* of a gate.

⚠️ §2.5's diagnostic combo lands on Start/Select, which a gun port **does** report, so it solves this
hole a second and better way. Keep both: the ungated buttons are free, and a combo the player has set
to `None` must not take the test switch away from a pad user.

### 2.5 FBNeo parity — pad layouts as device types, service as a combo

Decided by the user 2026-07-27: **match FBNeo**. Both reference cores were read at their `master` on
that date, and the two that matter are `FBNeo/src/burner/libretro/retro_input.cpp` and
`mame2003-plus-libretro/src/mame2003/mame2003.c`.

**Coin and start need no work and that is the finding.** FBNeo maps `"coin"` →
`RETRO_DEVICE_ID_JOYPAD_SELECT` and `"start"` → `..._START`; mame2003-plus maps
`JOYCODE_n_SELECT → IPT_COIN(n)` and labels the descriptor `"Coin"`. We reach the identical result
through MAME's own defaults — `COIN1 = input_seq(KEYCODE_5, JOYCODE_SELECT_INDEXED(0))`
([inpttype.ipp:598](../src/emu/inpttype.ipp#L598)) — with no code of ours involved.
**Verified end to end**: a scripted `select` press on port 0 takes vf2 from `CREDIT 0/2` to
`CREDIT 1/2` (`retrohost … 2200 out.ppm "1200:select:20"`). ⚠️ **The `KEYCODE_5` half of that seq is
dead in this core** — the OSD registers no keyboard device
([libretro_m2_osd.cpp:136](../src/osd/libretro_m2/libretro_m2_osd.cpp#L136) points the keyboard slot
at the same joystick module), so the RetroPad Select button is the only route to a coin, and on a
keyboard that is whatever the frontend binds Select to (RetroArch default: `rshift`).

#### 2.5.1 The layout table, verbatim

FBNeo indirects **at read time** through macros, which is why its device type can change mid-run:

```c
#define RETRO_DEVICE_ID_1ST_COL_TOP    RETRO_DEVICE_ID_JOYPAD_Y
#define RETRO_DEVICE_ID_1ST_COL_BOTTOM RETRO_DEVICE_ID_JOYPAD_B
#define RETRO_DEVICE_ID_2ND_COL_TOP    RETRO_DEVICE_ID_JOYPAD_X
#define RETRO_DEVICE_ID_2ND_COL_BOTTOM RETRO_DEVICE_ID_JOYPAD_A
#define RETRO_DEVICE_ID_3RD_COL_TOP    (nDeviceType[nPlayer] == RETROPAD_MODERN ? RETRO_DEVICE_ID_JOYPAD_R  : RETRO_DEVICE_ID_JOYPAD_L )
#define RETRO_DEVICE_ID_3RD_COL_BOTTOM (nDeviceType[nPlayer] == RETROPAD_MODERN ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_R )
#define RETRO_DEVICE_ID_FIRE01 (nDeviceType[nPlayer] == RETROPAD_6PANEL ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_1ST_COL_BOTTOM)
/* …FIRE02..06: X|2ND_COL_BOTTOM, L|1ST_COL_TOP, B|2ND_COL_TOP, A|3RD_COL_BOTTOM, R|3RD_COL_TOP */
```

Resolved, against what this core does today:

| button | **Classic** (FBNeo default) | **Modern** | **6-Panel** | **ours today** |
|---|---|---|---|---|
| 1 | B | B | Y | B |
| 2 | A | A | X | A |
| 3 | Y | Y | L | Y |
| 4 | X | X | B | X |
| 5 | **R** | **R2** | A | **L** |
| 6 | **L** | **R** | R | **R** |

🚨 **Buttons 1–4 already match Classic exactly; 5 and 6 are swapped, and fixing them changes existing
bindings for every set that has them.** That is the only behaviour change in this section, so it is
the one that needs a before/after run rather than an argument. The blast radius is small and known —
`IPT_BUTTON5`/`6` appear **4 and 3 times** in `model2.cpp`, all of them view-change or gear buttons
on driving sets (`daytona` VR1/VR2/VR3, `desert` VR2/VR3, `srallyc` VR, `gears` GEAR 4, `segawski`,
`skisuprg` Zoom In). Buttons 7 and 8 are one set each (`daytona`).

⚠️ **6-Panel has no customer on Model 2 and should not be offered.** It exists for six-button
fighters; the whole platform's button histogram is 37 / 30 / 27 / 11 / 4 / 3 / 1 / 1 for
`IPT_BUTTON1..8`, and **vf2 is a three-button game** (Punch / Kick / Guard). Shipping a layout that
cannot change anything is a support question with no upside. Offer **Classic** and **Modern** only,
and say so in the option text rather than leaving the reader to wonder where the third went.

⚠️ **Modern collides with the pedals and FBNeo cannot tell us what to do about it**, because FBNeo
has no pedals. Modern wants `R2` as button 5, and `R2` is the accelerator
([libretro_m2_input.cpp:229](../src/osd/libretro_m2/libretro_m2_input.cpp#L229)); the two would both
fire, because device defaults OR (§1.1) and a switch OR-ing a switch is harmless but confusing. **Not
a reason to drop Modern** — it is a per-port choice the player made, and the driving sets are exactly
the ones with a button 5. Document it; do not add a gate that second-guesses the selection.

#### 2.5.2 Implement the layout as a read-time indirection, not a re-`configure()`

The obvious implementation — rebuild the assignment vector when the device changes — fights MAME:
items are added once in `configure()` and their state pointers are fixed. Copy FBNeo's shape instead,
which is a table lookup on every read:

- `m_buttons[]` stops being indexed by RetroPad id and becomes indexed by **MAME button number**;
  `update()` fills slot *n* from `state_cb(port, JOYPAD, 0, layout[n])`.
- `configure()` is unchanged in structure — the six numbered items still point at fixed slots.
- `retro_set_controller_port_device` then only writes `layout` for that port, so a mid-run change is
  free and needs no reload. This is the same property §1.3 buys by passing `-lightgun`
  unconditionally, for the same reason.

The gun/pointer device types from §2.3 select the same way, so **one dispatch handles both** — which
is why this belongs in the lightgun phase and not in a phase of its own.

#### 2.5.3 The diagnostic combo

FBNeo's option, verbatim from `src/burner/libretro/retro_common.cpp`:

```
"fbneo-diagnostic-input" — "Configure button combination to enter cabinet service menu"
  None | Hold Start | Start + A + B | Hold Start + A + B | Start + L + R | Hold Start + L + R |
  Hold Select | Select + A + B | Hold Select + A + B | Select + L + R | Hold Select + L + R
```

Take the value list unchanged as **`model2_diagnostic_input`**, defaulting to `None`, and **retire
`model2_service_buttons`**. The current L3/R3 binding is not merely unconventional, it is
*unreachable on a keyboard* — RetroArch leaves `input_playerN_l3`/`r3` at `"nul"` by default, and
this OSD draws no MAME UI, so today a keyboard-only player has no route into any test menu.

Three details that are not in the option list and will otherwise be discovered by hand:

- **A fired combo must consume its buttons that frame.** Otherwise `Start + A + B` also presses
  Start. Evaluate the combo in `update()` after the button read and zero the constituents.
- **`Hold …` needs a timer.** A frame count in the device, reset when the button set breaks. One
  second at 57.52 Hz ≈ 58 frames; pick it once, name it, do not scatter it.
- ⚠️ **Model 2 has two switches where FBNeo models one**, and this is the one place the parity is not
  literal. The combo drives **`IPT_SERVICE`** — the test switch, the one that opens the menu.
  **`IPT_SERVICE1`** (service coin, free credit) has no equivalent in FBNeo's scheme; keep it on L3,
  gated on the option being anything other than `None`. Losing it is not acceptable and inventing a
  second combo vocabulary is worse.

Mechanically the combo is a **synthetic item**: add one more button item whose state is the combo
result and assign `IPT_SERVICE` to it, so MAME's own assignment machinery does the rest and no new
input path exists. Same trick as `trigger_button_get_state`
([libretro_m2_input.cpp:68](../src/osd/libretro_m2/libretro_m2_input.cpp#L68)) reading an axis as a
switch.

⚠️ **This makes three core options where `CLAUDE.md` says "Two core options exist and no more".**
That line is a statement of fact about the build, not a budget — but it is load-bearing for anyone
reading the file cold, so it moves in the docs step, not later.

---

## 3. Order of work

Seven steps. Step 1 is first because §1.5 removed the only way to see anything.

### ~~Step 1 — the instrument, before any core code~~ ✅ DONE 2026-07-27

~~`retrohost`'s script knows 16 digital buttons and 8 half-axes~~
~~([retrohost.c:996-1014](retrohost.c#L996)) and cannot express an absolute pointer, so today there is~~
~~no way to verify a gun at all. Add to `devnotes/` only (never ships):~~

- ~~`gun <port> <x> <y>` in the script — normalised `0.0..1.0` screen coordinates, held for a frame~~
  ~~range like the existing presses, reported as `SCREEN_X`/`SCREEN_Y`.~~
- ~~`trigger`, `reload`, `offscreen` as gun-device controls.~~
- ~~`--gun <port>` to make `retro_set_controller_port_device` fire for that port.~~
- ~~**A read-out, which is the actual point**: `M2VK_GUN_LOG=1` printing the four port values MAME~~
  ~~resolved, per frame, so a scripted aim can be checked against a number instead of a screenshot.~~

~~Exit: a script that sweeps the pointer across the screen prints port values that sweep monotonically~~
~~across `PORT_MINMAX`, on `vcop` and `vcop2`, with the reticle not yet drawn.~~

**As built** — commit `607d9f6528b`. All four items, plus one the plan did not ask for and the exit
criterion needed. New file **`src/osd/libretro_m2/m2vk_gunlog.h`** (header-only, so no build-script
entry; ⚠️ include it **after `emu.h`**), called from the OSD's `update()` and reset in `osd_exit()`.
`retrohost.c` gained `gun=<x>/<y>`, `trigger`, `reload`, `offscreen` and `--gun <port>`. **No upstream
file touched.**

- **The syntax is `gun=<x>/<y>`, not `gun <port> <x> <y>`** — a script item is already
  `frame:control[:held][:port]`, so the port has a field and the payload rides on the control name.
- 🚨 **Added beyond the plan: a half-axis takes a deflection fraction, `lx+=0.35`.** Without it the
  script can express three points and not a sweep, and the sweep *is* the exit criterion. It is also
  what the analog-steering item queued behind this phase will want.
- **`M2VK_GUN_LOG=<n>` is a period, not a flag** (`=1` is every frame). It prints a reference line
  once — each axis's range and the two offscreen trip points — then one line per period with all
  four values and a per-player offscreen bit.
- **The offscreen column is `lightgun_offscreen_r` transliterated, not tapped** (§1.4). Copying it
  keeps the upstream diff at 30 lines; the cost is that a change to the driver's border rule would
  have to be mirrored, which a comment in the file says.
- ⚠️ **Resolution waits for `safe_to_read()`.** The first `update()` lands before the port list is
  complete, and asking then finds no lightgun fields on a set that has four. The first version did
  exactly that and reported "this set has no IPT_LIGHTGUN_X/Y ports" for `vcop`.

**Exit met, and it produced a finding.** vcop and vcop2 sweep monotonically end to end, **full-scale
input landing exactly on `PORT_MINMAX`** — §7.2's rule, checked for the first time. But the sweep is
**not linear**: `input_device_joystick::adjust_absolute_value` applies MAME's
`-joystick_deadzone 0.15` / `-joystick_saturation 0.85`, so the stick has a dead centre and a
saturated outer 15 %. It is a `DEVICE_CLASS_JOYSTICK` property only —
`input_device::adjust_absolute_value` is the identity — so **step 2's gun device gets none of it**,
which is one more reason §2.3 says no scale factor of ours anywhere. Two other constants worth
carrying: **the read-out lags the script by exactly 4 frames**, and **`gun=` reaches nothing yet**,
correctly, because `retro_set_controller_port_device` is still a stub. Numbers in the worklog.

### ~~Step 2 — the gun device~~ ✅ DONE 2026-07-27

~~§2.1 + §2.2 + §2.3, plus the libretro-side plumbing:~~

- ~~`RETRO_ENVIRONMENT_SET_CONTROLLER_INFO` with `RetroPad` / `Light Gun` / `Pointer` per port — it is~~
  ~~sent nowhere today. ⚠️ **Leave room for step 6's two pad layouts in the same array** rather than~~
  ~~sending it twice; they are subtypes of the same port, and step 6 only adds entries.~~
- ~~`retro_set_controller_port_device` stops being a stub. 🚨 **Rewrite its comment; do not delete it**~~
  ~~— it explains why the stub was correct, and the next reader needs to know a real choice is now~~
  ~~being made.~~
- ~~`-lightgun` into the argument vector. `-nomouse` stays (§1.3).~~
- ~~Gun entries in `INPUT_DESCRIPTORS`.~~

~~Exit: step 1's sweep tracks under `--gun 0`, both players, `vcop` / `vcopa` / `vcop2`; and 🚨 **a~~
~~scripted `vf2` run is byte-identical to before** (§4).~~

**As built.** All four items. Three files, all in `src/osd/libretro_m2/` —
`libretro_m2_input.{h,cpp}` and `retro_entry.cpp`. **No new file, no upstream file, no shader, no
pixel; the diff against mame0288 is still 30 lines.** Numbers in the worklog.

**Pointer: decided against, 2026-07-27.** §6's open item, answered `Light Gun only`. Nothing here can
drive a touch pointer, so it would have shipped untested. `PORT_DEVICES` in `retro_entry.cpp` is the
one array to add it to, alongside step 6's layouts.

- **The structural change the plan did not price.** `input_module_impl<>` templates on one device
  type, so the guns needed a common base: **`libretro_m2_device`**, carrying the port and a virtual
  `update(state_cb, device)`, with the pad and the new `libretro_m2_gun_device` under it. One device
  list, one `for_each_device`, both kinds driven by one call per frame.
- **`s_port_device[MAX_PADS]` lives in `retro_entry.cpp`, not in the input module**, and
  `poll_frontend()` takes it as a parameter. The frontend owns the ordering: the call is allowed to
  arrive before content is loaded, when there is no module to tell, and it has to survive
  `retro_unload_game` so a second load keeps the player's choice. Both ends are the libretro thread,
  so there is nothing to synchronise.
- 🚨 **The gun adds ONE assignment, and it is not the one §2.3 named.** §2.3 says "`IPT_BUTTON1` → the
  trigger item, and nothing else", having ruled out an axis assignment because `GUNCODE_X_INDEXED(n)`
  is already the core default. **The trigger is in exactly the same position**:
  [inpttype.ipp:34](../src/emu/inpttype.ipp#L34) and
  [:151](../src/emu/inpttype.ipp#L151) give `IPT_BUTTON1` a default of
  `… | GUNCODE_BUTTON1_INDEXED(n)`, and `IPT_BUTTON2` likewise. So the trigger and AUX_A bind
  themselves and an assignment for them would be the second copy §2.3's own rule rejects.
  **`IPT_BUTTON3` has no such default**, so AUX_B is the one that is written out — by hand, because
  `joystick_assignment_helper::make_code()` hardcodes `DEVICE_CLASS_JOYSTICK`. ⚠️ **The device index
  in that code must be 0**: `apply_device_defaults` asserts on it
  ([ioport.cpp:2740](../src/emu/ioport.cpp#L2740)) and then rewrites it to the real index, which is
  how one assignment serves both players.
- **`RETRO_DEVICE_NONE` reports nothing**, which the plan does not mention. It states an intent
  rather than fixing anything — a frontend with a port set to None already answers 0 to every
  `state_cb` — which is also why it is safe unexercised, `retrohost` having no way to select it.

**Exit met, on `vcop` and `vcop2` only.** Both players, both axes, every value landing on that port's
own `PORT_MINMAX`, centre landing on the port's default. ⚠️ **`vcopa` and `hotd` could not be run for
romset reasons, not code ones** — `vcopa` is in neither rompath, and `hotd` fails audit on two files
and wants clone `hotdo` ([roms.md](roms.md)), also absent, separately from being
`MACHINE_NOT_WORKING`. The port set is wired for them; nothing is claimed.

🚨 **§1.2's summing hazard is confirmed real and the gate closes it, measured.** `vcop`, `--gun 0`,
gun aimed at 0.75 **and** the left stick held hard left in the same frames → the port reads `0x1f9`,
the gun's value alone. Ungated, the two absolute sources sum and saturate and it would have read
`0x083`, the opposite end of the screen.

**The gun's sweep is linear where the stick's is not** — 0.25 of `vcop` p1 X's `131..630` is 255.75
and the port reads `0x0ff` = 255. That is step 1's finding confirmed from the other side, and it is
why §2.3's "no scale factor of ours anywhere" is cheap to obey.

**The trigger reaches `IPT_BUTTON1`, proved by an equality.** `M2VK_GUN_LOG` does not read buttons,
so the check is behavioural: `vcop` driven into gameplay, whole-run digests over 3600 frames. Pad B
pulsed and gun trigger pulsed give the **same** digest; `--gun 0` with no trigger gives the
**no-input** digest. The second of those is §2.2's neutral state measured over a run, and it also
carries §2.4 — that run's coin and Start arrived on a port set to `RETRO_DEVICE_LIGHTGUN`.

⚠️ **Getting `vcop` into gameplay is not obvious and a run that fails to costs an hour.** Start at
frame 700 is **too early** — the credit has not counted and the run sits on the title showing
`CREDIT 1`, where the pad's own button changes no digest either, so the null result looks like a
broken gun. Coin at 600, Start pulsed at 900/1200/1500/1800 gets in. Screenshot the last frame before
believing any null result here.

### ~~Step 3 — offscreen and reload~~ ✅ DONE 2026-07-27

~~§1.4. `IS_OFFSCREEN` or `RELOAD` → both axes to `ABSOLUTE_MIN`; `RELOAD` additionally asserts the~~
~~trigger item, per flycast.~~

~~Exit: with `vcop`'s DSW1:1 in **Normal**, a scripted `reload` makes the offscreen bit appear in the~~
~~`M2VK_GUN_LOG` read-out and the magazine refills. This is the step §7.6 called the one where the~~
~~obvious implementation looks correct and the game is unplayable — so it gets a scripted check, not an~~
~~impression.~~

**As built, exactly as §1.4 predicted: four lines**, all inside `libretro_m2_gun_device::update()`.
No new file, no upstream file, no shader, no pixel; the diff against mame0288 is still 30 lines.
Cheap because the driver already implements offscreen — the step only had to produce a value MAME
recognises. Numbers in the worklog.

- **`ABSOLUTE_MIN` rather than a coordinate near the edge**, and this is the part that would be got
  wrong on a rewrite. The border is 5 % of each port's *own* `PORT_MINMAX`, so the target moves per
  set and per player; `ABSOLUTE_MIN` is the one value MAME's scaling lands exactly on `minval` for
  all of them. Choosing a point inside the border by hand is the scale factor §5 forbids, pointed at
  a moving target.
- **Both axes, not only X.** Either alone sets the bit — the driver ORs them — but the gun is
  supposed to be pointing away from the screen, and leaving Y at the aim would have the game see a
  shot at a real place on the playfield.
- **The synthetic trigger ORs into the real one**, so holding the trigger and pressing reload is one
  press rather than a dropped one.

**Exit met on `vcop`, whose DSW1:1 already defaults to `Normal` so the precondition needed no config
change.** `reload` and `offscreen` both pin p1 to `0x083`/`0x024` — that port's two `minval`s to the
digit — with `off=1`, p2 untouched; repeated on `vcop2` with `--gun 1`, where p2 pins to its own
`0x086`/`0x024` and p1 does not move. Step 1's 4-frame lag holds exactly.

🚨 **The magazine check needed a negative control, and §7.6 was right that it would.** Four runs
identical through frame 2249, cropped to the revolver cylinder
(`screenshots/2026-07-27-gun3-vcop-magazine.png`): six shots leaves **1 round**; `reload` refills to
**6**; **`offscreen` alone leaves it at 1**; `offscreen` + a separate `trigger` refills to 6. The
third is the one that matters — the game reloads on *a shot fired* off the screen, not on being
pointed away, so `RELOAD` has to be both halves and the synthetic trigger is load-bearing rather than
decoration. With only the first two runs, "any offscreen value reloads" would have fit the evidence
and it is false.

⚠️ **The cylinder is the read-out; the whole-run digest is not.** All four runs differ in digest,
including the two whose cylinders match — because moving the aim moves **the game's own yellow
reticle**, which is drawn into the frame. (That reticle is free independent evidence the aim
arrives. It is the game's; ours is step 4.)

### ~~Step 4 — the reticle~~ ✅ DONE 2026-07-27

~~§1.7 and §1.5. Procedural gapped cross, drawn only for ports whose device is a gun.~~

~~Two blitters, and that is accepted rather than worked around: an alpha-blended quad after the OVER~~
~~layer on the Vulkan path (`vk_geom.cpp` / a small `reticle.frag`), and a 16×16 CPU blit in~~
~~`capture_frame` for `renderer=software`. Shared geometry constants, so the *asset* cannot drift even~~
~~though the two blits are separate.~~

~~⚠️ **It must be off when no gun is selected**, or every `ab.sh` / `res.sh` background reference~~
~~changes and the whole accuracy harness is invalidated. That is the one way this step can break~~
~~something far away from itself.~~

~~A per-port colour option (flycast has five) is a nice-to-have; fold it into the `M2VK_*`-switches-to-~~
~~core-options item already queued behind this work rather than inventing a second options mechanism.~~

**As built.** Both blitters, as scoped. New files `src/osd/libretro_m2/m2vk_reticle.{h,cpp}` and
`renderer_vk/shaders/reticle.frag` (+ its generated `_spv.h`); edits to `retro_entry.cpp`,
`libretro_m2_osd.cpp`, `renderer_vk/vk_present.cpp` and the build script. **No upstream file, no
pixel outside the cross; the diff against mame0288 is still 30 lines.** Numbers below and in the
worklog.

- **The Vulkan half went in `vk_present.cpp`, not `vk_geom.cpp`.** The plan named the wrong file:
  `vk_geom` is polygons — vertex buffers, batches, the depth key — and the reticle is a fourth
  fullscreen-triangle draw sharing the render pass, the pipeline layout and the `build_pipeline()`
  structs with the two 2D layers and the resolve. Building it beside them is one `stages[1].module`
  assignment; building it in `vk_geom` would have been a second copy of a 60-line pipeline
  description.
- **The asset is shared as *constants*, not as code.** `m2vk::RETICLE_SHAPE` — four floats,
  `half_thick / gap / arm / outline` — is the only definition of the cross, and the shader is handed
  it in a push block rather than carrying its own. What *is* duplicated is the four-line predicate
  that turns those numbers into a pixel test, once in C++ and once in GLSL, because there is no way
  to share it; each names the other. That duplication is safe in the way the P4 step 1 pipeline
  predicate was not: it is on screen the entire time a gun is selected, so a drift shows up as the
  two renderers disagreeing about a shape you are looking at.
- 🚨 **Not alpha-blended, and that is a decision.** The CPU blitter writes into MAME's finished frame
  and the shader writes into the composite, so an alpha blend would be against different backgrounds
  on the two paths and they would stop producing the *same pixels* — which is the check that makes
  the software path a reference at all. Opaque cross, opaque 1 px black border, `discard` everywhere
  else. The border is not decoration: a white cross is invisible on `srallyc`'s sky and `desert`'s
  sand, and it costs one extra evaluation of a predicate that is already there.
- **The centre is published normalised (0..1), not in pixels**, from `retro_run` via
  `publish_reticles()`. The publisher does not know the picture's size, both consumers do, and under
  `M2VK_SS` the Vulkan one is drawing into an attachment that is *n* times it. The shader divides
  `gl_FragCoord` by the scale, so one set of constants serves every internal resolution and the cross
  grows with the picture instead of shrinking into it.
- ⚠️ **The pointer is read twice a frame and deliberately not plumbed through the input module.** The
  gun device turns the same two axes into ioport values and keeps no state a renderer could reach —
  it lives behind the OSD, and the Vulkan side is on the far side of that. Working back from the
  *port* value instead would put `PORT_MINMAX` between the pointer and the cross, which is the
  calibration fudge §5 forbids.
- **`M2VK_NO_RETICLE=1` is the off switch**, and it is what lets a gun game go through `ab.sh` at all.
  Measured: two otherwise identical `vcop --gun 0` runs differ in **exactly 124 pixels** — the whole
  reticle and nothing else.

  🚨 **The DEFAULT flipped on 2026-08-07 and the reticle now draws only under `M2VK_RETICLE=1`.**
  Asked for directly, after playing `vcop2` on the Odin with a finger: RetroArch Android reports a
  lightgun position only **while a finger is down** and holds the last one after release, so the cross
  parks where the last shot landed and stays there. Nothing here can tell that from a deliberate aim —
  the on-bit is true because the port is a `RETRO_DEVICE_LIGHTGUN`. And on `vcop`/`vcop2` it was a
  second crosshair over the game's own yellow one, which is what ours was coloured to be
  *distinguishable from in a screenshot* rather than to replace. `M2VK_NO_RETICLE=1` still means off,
  so nothing in the harness changed. The 124-pixel figure above is unchanged and is now the check that
  `M2VK_RETICLE=1` still restores the asset intact. See the 2026-08-07 worklog entry; the honest fix
  for touch is a hide-when-not-touching gate, and the player-facing home for the choice is a core
  option next to the per-port colour in §5's open list.
- **The scissor is what makes it cheap**: one `vkCmdSetScissor` to the 18×18 bounding box per gun, so
  the draw shades ~400 fragments rather than 190464 of them discarding.

**Exit met, and measured against the geometry rather than eyeballed** (a script that walks every
pixel of the box, asserts each is cross / border / untouched per `reticle_covers`, and compares the
two renderers). `vcop --gun 0` aimed at 0.25/0.4 and `vcop2 --gun 1` aimed at 0.7/0.65: **48 cross
pixels and 76 border pixels, 0 wrong, 0 disagreeing between `renderer=software` and
`renderer=vulkan`** — the two paths are pixel-identical over the whole cross, which is what makes
"two blitters" acceptable. 48 is 4 arms × 2 × 6, i.e. the shape asked for.

- **`M2VK_SS=3 M2VK_SS_POINT=1` is pixel-identical to the 1× software blit.** At an odd scale the
  centre subpixel is the 1× sample point, so the reticle drawn at 3× and point-resolved has to land
  on exactly the same pixels — and does, 124/124.
- **Offscreen draws nothing.** A run with a scripted `offscreen` over the last frames is
  **byte-identical** with the reticle on and off, which is the negative control: `RELOAD` pins both
  axes to `ABSOLUTE_MIN`, so the shot really is going into the corner and a cross where the player is
  pointing would say otherwise.
- 🚨 **§4 check 5 passes**: `vf2` 2500 frames is `16af05bb8d02a9a5` / `55da761fecca5c01`, both
  baselines to the digit, so nothing about the accuracy harness moved.
- **Colours: player 1 white, player 2 cyan, both over black.** Cyan rather than yellow or red
  because `vcop` and `vcop2` draw their *own* aiming reticle in yellow, and the whole value of ours
  in a screenshot is telling the two apart. Screenshots:
  `screenshots/2026-07-27-gun4-{vcop-reticle-p1,vcop2-reticle-p2,vcop-reticle-ss3}.png`.
- ⚠️ **Not exercised under RetroArch.** There is no working way to screenshot it there (gotcha 6) and
  no pointer in `retrohost`; what is verified is the coordinate path, end to end, from `state_cb` to
  the pixel. A frontend that reports `SCREEN_X/Y` differently would show up as an offset, not as a
  missing cross.

A per-port colour option (flycast has five) is still a nice-to-have; it is folded into the
`M2VK_*`-switches-to-core-options item already queued behind this work rather than inventing a second
options mechanism. The colours live in one table in `m2vk_reticle.cpp` for it to read.

### ~~Step 5 — service buttons on a gun port~~ ✅ DONE 2026-07-27

~~§2.4. Should be the absence of a gate rather than new code; verify it is.~~ ⚠️ **Do this before step 6
and do not skip it on the grounds that the combo supersedes it** — the combo can be set to `None`,
and then this is the only thing keeping a gun port's buttons alive.

**As built: nothing was built.** The step's prediction held — **zero lines changed**, no file
touched, and the whole deliverable is the measurement below plus three screenshots. That is the
result to record, because "we think the gate is narrow enough" and "the test menu opens on a gun
port" are different claims and only the second one is checkable.

**Both service controls reach their ioport types on a `RETRO_DEVICE_LIGHTGUN` port, shown in one
picture.** `vcop --gun 0`, `model2_service_buttons=enabled`, R3 held from frame 900 to the end of a
1500-frame run and L3 pulsed three times inside it: the last frame is **TEST MODE with the cursor on
`COIN ASSIGNMENT`** — three items down from `EXIT`, one per L3 pulse. R3 alone gets the menu, so it
reached `IPT_SERVICE`; the cursor having *moved* is what separately proves `IPT_SERVICE1`, and the
menu's own instruction line ("SELECT BY SERVICE BUTTON AND PUSH TEST BUTTON") is the game saying
which is which. Screenshot `screenshots/2026-07-27-gun5-vcop-testmenu.png`.

- **The service coin is checked away from the menu too**, because inside test mode a stuck L3 and a
  working one look similar: L3 pulsed with no R3 takes the attract screen from `CREDIT 0` to
  **`CREDIT 1`** (`…-gun5-vcop-servicecoin.png`). Both runs are `--gun 0`, so the credit arrived from
  a port with no pad selected on it at all.
- 🚨 **The negative control is the option, not the press.** The identical R3/L3 script with
  `model2_service_buttons=disabled` gives `381dd936274223e0` — **byte-identical to the no-press run
  over 1500 frames**. Without that, "R3 changed the picture" is also consistent with R3 landing on
  something else entirely; with it, the change is the binding.
- **The strongest form of §2.4's claim is an equality: a gun port and a pad port produce the *same
  frames*.** `--gun 0` with `M2VK_NO_RETICLE=1` against the same run with no `--gun` at all —
  `370b0991aecc9a80` both, no press, and `46afe762908e21e6` both with the R3 script. Selecting a
  lightgun on port 0 changes exactly one thing about the output, and it is our own reticle.
- **`vcop2` agrees**, `--gun 0`, R3 held + one L3: TEST MENU with the cursor moved one item to
  `MEMORY TEST`, and its disabled-option control is byte-identical to its no-press run
  (`40c6ec1e9d3d07b7`). `…-gun5-vcop2-testmenu.png`.
- **§4 check 5 passes on the rebuilt binary**: vf2 2500 frames is `16af05bb8d02a9a5` /
  `55da761fecca5c01`, both baselines to the digit.

⚠️ **The vcop2 run must put the gun on port 0, and the obvious `--gun 1` run measures nothing.**
`IPT_SERVICE1`/`IPT_SERVICE` are player-0 types, so `apply_device_defaults` lands them on pad 1 alone
(§2.3's note, the same reason pad 2's copy is skipped) — a run with the gun on port 1 sends its L3/R3
from a port that is still an ordinary pad, and it opens the test menu whether or not any of this
works. One such run was done here before the mistake was spotted; its picture is indistinguishable
from the real one.

✅ **The frontend behaviour this rests on is confirmed in a shipping core, not assumed.**
`retrohost` reports `RETRO_DEVICE_JOYPAD` state on a port set to a gun, and the question was whether
a real frontend does. `flycast-aoj` answers it: `UpdateInputStateNaomi`'s `MDT_LightGun` branch
([libretro.cpp:2857](../../flycast-aoj/shell/libretro/libretro.cpp#L2857)) polls
`RETRO_DEVICE_ID_JOYPAD_L3`/`R3` explicitly on a gun port, with a comment saying stock flycast's
omission is what leaves its gun games with no test-menu access. So a shipping core depends on the
same behaviour we do. It also shows the cost of switching on the device type wholesale: flycast needs
two lines *back*, where our pad device never stopped reading them.

⚠️ **Our L3/R3 are the opposite way round from flycast's, and step 6 is where that is decided.**
AoJ's joymaps carry **L3 → TEST, R3 → SERVICE**; ours is L3 → `IPT_SERVICE1` (the service coin),
R3 → `IPT_SERVICE` (the test switch). Neither is wrong — MAME's own default is F2 for test and 9 for
service coin, which settles nothing — but §2.5.3 retires `model2_service_buttons` for a combo and
that is the moment to pick one deliberately rather than inherit this one by accident.

### ~~Step 6 — FBNeo parity: pad layouts and the diagnostic combo~~ ✅ DONE 2026-07-28

§2.5, and it is last of the code steps because it reuses step 2's dispatch: by here
`retro_set_controller_port_device` already exists, already writes per-port state, and already has
`SET_CONTROLLER_INFO` behind it. Doing this first would mean building that twice.

**Scope grew on 2026-07-27** with the four gaps from the coverage audit
([user-options.md §3.1](user-options.md)) — and then **two of them were fixed the same day, ahead of
this step**, once it turned out they did not need anything the step builds. Current state:

- ✅ **`IPT_BUTTON9` (daytona VR4) — DONE.** It did not need the combo after all: **R3 is already
  unassigned when `model2_service_buttons` is off**, which is the default, so the binding goes in the
  `else` branch beside `IPT_UI_MENU` and collides with nothing. Verified in a real daytona race — R3
  changes the camera, and **L3 held the same way is byte-identical to no press**, which is the
  control that makes it a binding result rather than emulation drift.
- ✅ **`COIN3`/`COIN4`/`START3`/`START4` (airwlkrs) — DONE.** `MAX_PADS` 2 → 4 and the new
  **`MAX_GUNS` = 2**; ports 2 and 3 added to `INPUT_DESCRIPTORS`. ⚠️ **Unverifiable and left that
  way** — airwlkrs is `MACHINE_NOT_WORKING` and is not in either local romset. What *is* verified is
  that it cost nothing: vf2 2500 frames is `16af05bb8d02a9a5` / `55da761fecca5c01`, both baselines to
  the digit, with two extra pad devices now created on every set.
- 🚨 **`IPT_BUTTON7`/`8` collide with the pedals on daytona — NOT fixed, and decided rather than
  scheduled.** See the box below the pieces.
- **`IPT_SERVICE2` (powsled)** — **won't fix**, recorded so it is not re-raised; §1.4 of
  user-options.md already has powsled out of scope.

So this step is back to the three FBNeo-parity pieces it was scoped for. They are in this order
because each is separately verifiable and the first is the only one that can move an existing
binding:

1. ~~**`m_buttons[]` re-indexed by MAME button number** (§2.5.2), with `layout` still the identity
   permutation `B, A, Y, X, L, R`. A pure refactor — nothing may change yet.~~ ✅
2. ~~**`Classic` / `Modern` device types**, Classic the default. This is where buttons 5 and 6 swap to
   `R, L` (§2.5.1). 6-Panel is deliberately not offered.~~ ✅
3. ~~**`model2_diagnostic_input`** (§2.5.3), FBNeo's eleven values verbatim, default `None`;
   `model2_service_buttons` retired; `IPT_SERVICE1` stays on L3 gated on the option being non-`None`.
   🚨 **`IPT_BUTTON9` currently lives in that same `else` branch** — retiring the option must not
   drop daytona's VR4 on the way past. With the test switch on a combo, R3 is free *unconditionally*,
   so the binding should come out of the branch entirely rather than move with it.~~ ✅

⚠️ **Gap 2 (daytona's pedal/VR collision) is deliberately NOT in the list above, and that is a
decision, not an oversight.** A RetroPad has no free control for it: daytona wants **9 buttons +
steering + 2 pedals**, and after coin/start/d-pad/six-face-and-shoulder there is nothing left. The
honest fixes are (a) a **daytona-specific layout** — its buttons 1–5 are the gearbox and 6–9 are the
VR cameras, so a per-set table could put the VR buttons somewhere sane — or (b) accept the collision
and document it. (a) is the per-set override table `user-options.md` §6 proposes and §6 of this file
puts out of scope. **Do (b) in this step: leave the mapping alone, and put the collision in the
release notes as a known limitation of daytona on a gamepad.** Revisit when the tier table exists.

Exit, and the three are separate checks on purpose:

- After 1, a scripted `vf2` run is **byte-identical** — same digests as §4 check 5. A refactor that
  moves a pixel or a port value is a bug in the refactor, and it is much cheaper to find here than
  after 2 has legitimately changed something.
- After 2, `daytona` and `srallyc` report the view button on the *other* shoulder, read out of a
  scripted press, and `M2OPT_`-selected `Modern` puts button 5 on R2 with the accelerator still
  working (both, simultaneously — §2.5.1 says that is correct, not a collision to fix).
- After 3, each of the eleven values reaches `IPT_SERVICE` from a scripted combo and **only** from
  that combo: the negative check is that `Start + A + B` does not also register Start, which is the
  failure §2.5.3 says to expect. `None` must behave exactly like today's `model2_service_buttons =
  disabled`. 🚨 **And `daytona`'s VR4 still responds to R3** — it is bound in the branch this piece
  deletes, so it is the thing most likely to be lost silently here.

**As built, 2026-07-28.** All three pieces, in the order above; pieces 1 and 2 landed in the working
tree on 2026-07-27 with no worklog entry, piece 3 and the verification of all three on 2026-07-28.
Six files, all `src/osd/libretro_m2/` — `retro_options.{h,cpp}`, `libretro_m2_input.{h,cpp}`,
`libretro_m2_osd.{h,cpp}`, `retro_entry.cpp`. **No new file, no upstream file, no shader, no pixel:
the diff against mame0288 is still 30 lines** and vf2 2500 frames reproduces both baselines. Numbers
and screenshots in the worklog; four things worth carrying:

- **The option's value list and the combo table are one list.** `DIAGNOSTIC_VALUES[]` plus the enum
  that numbers it live in `retro_options.h`; the option table writes its `values[]` out of them and
  the input module indexes its control table by the same enum. Two parallel string lists is the
  obvious build and its failure mode is a combo the menu offers and nothing implements.
- **The combo is a synthetic button item** (`ITEM_ID_BUTTON11`, "Diagnostic Combo") carrying
  `IPT_SERVICE`, so nothing about how the switch reaches the machine is new. It is added under every
  option value, including `None`: an item list that moves with an option is a saved remap that changes
  meaning underneath the player.
- ✅ **`IPT_BUTTON9` came out of the branch entirely**, as this piece said it should, and daytona's
  recorded VR4 digests reproduce **to the digit** under both `None` and a set combo.
- 🚨 **Consumption cannot be measured from the credit counter and the plan's phrasing invites trying.**
  The game stays in the test menu after the switch is released, so the counter is never on screen
  again; and a coin held long enough to be a `Hold …` combo registers nothing anyway. The read-out is
  the game's own **INPUT TEST** screen, which reports the port bits directly — `START : OFF` with all
  three of the combo's controls physically down is the check, and it passes.

⚠️ **Piece 2's exit check was met in the daytona half only** — VR1 (`IPT_BUTTON6`) moves from L to R
with the device type, proved by digest *equality* across the two layouts. `srallyc` was not run, and
neither was Modern's button-5-on-R2-with-the-accelerator case; the collision it would demonstrate is
already documented above as accepted rather than fixed.

### ~~Step 7 — docs~~ ✅ DONE 2026-07-28

~~worklog entry, this file struck through and rewritten as-built, `CLAUDE.md`'s "Where we are" and~~
~~"Next" — 🚨 **including the "Two core options exist and no more" sentence, which step 6 makes false**~~
~~(§2.5.3) — `user-options.md` §7 marked done with the three corrections from §1 folded back into it,~~
~~its §3.1 and §6 updated for what step 6 actually shipped, and~~
~~screenshots (`devnotes/screenshots/`, from `retrohost --vk`) — a gun game with the reticle on it is~~
~~exactly the kind of change the screenshot rule exists for.~~

**As built, and it changed no executable code.** Five writes: the worklog entry, this file, `CLAUDE.md`,
`user-options.md`, and two missing sections in [screenshots/README.md](screenshots/README.md). Nothing
was re-measured — step 6 reproduced both vf2 baselines at this working tree, and P3 step 8 / P4 step 3
already set the precedent that a docs pass which re-runs the harness has changed something it should
not have.

- 🚨 **The step's headline task does not exist, and that is the finding.** §2.5.3's flag — "this makes
  three core options where `CLAUDE.md` says two" — was a prediction, and **piece 3 made it false by a
  different route**: it *retired* `model2_service_buttons` in the same change that added
  `model2_diagnostic_input`, so the count is still **two** (`KEY_RENDERER` and `KEY_DIAGNOSTIC_INPUT`
  are the whole of `retro_options.h`) and only the names moved. The general rule, since this will
  recur: **a plan item saying "X will make document Y false" is a prediction — check Y, not the
  prediction.**
- **§7's three corrections are marked in place, struck rather than rewritten**, because §1 of this file
  cites them by section number and a silently corrected source makes §1 read as if it were arguing with
  nothing. They are §7.4 (`-lightgun_device` unnecessary, `-nomouse` stays), §7.6 (conclusion held,
  mechanism wrong — and the driver already implements offscreen, which is why step 3 was four lines)
  and §7.8 check 1 (MAME's crosshair is inert here, so the check cannot be performed at all).
- **No new screenshots, and the gap was the index rather than the pictures.** This step moves no pixel;
  steps 3, 4, 5 and 6 took nine between them, all `retrohost --vk`. `screenshots/README.md` had
  stopped at step 4, so the step-5 test-menu pair and the step-6 INPUT TEST set were on disk with
  nothing saying what they show. ⚠️ **The four INPUT TEST shots look like a duplicate set and are not**
  — they are the four rows of step 6's consumption table, which has no other read-out, so the pictures
  *are* the evidence. The README now says so.
- ✅ **Steps 4 and 6 went in as one commit, `89bdbde4d33`, and that was the right call.** They are not
  separable by file: `libretro_m2_osd.cpp` carries step 4's reticle blit and step 6's diagnostic
  plumbing in different hunks, and `retro_entry.cpp` carries both in twelve. A hunk-level split would
  have produced two commits neither of which builds. 14 files, all `src/osd/libretro_m2/` plus the lua;
  **no upstream file**, so the diff against mame0288 is still 30 lines. The docs are `devnotes/` and
  never ship, so they were outside that decision either way.

---

## 4. What "done" means — and the A/B harness is not the instrument

Input does not change rendering, so a green `ab.sh` table is evidence of nothing here. Replacing
§7.8's list, with check 1 rewritten because §1.5 killed it:

1. **The resolved port value tracks the frontend pointer** across the full sweep, both players, on
   `vcop`, `vcopa`, `vcop2` — read out of `M2VK_GUN_LOG`, not eyeballed. (`hotd`/`hotdo`/`hotdp` are
   `MACHINE_NOT_WORKING`; wire the port set, do not claim a result from it.)
2. **The reticle lands where the port value says it does** — the visual check, made honest by being
   cross-checked against 1 rather than standing alone.
3. **The trigger fires and hits what the reticle is on.**
4. **Offscreen reload works with DSW1:1 in Normal** (§3 step 3).
5. 🚨 **The pad path is provably unchanged.** `libretro_m2_input.cpp` is shared by all 83 sets. A
   scripted `vf2` run must be **byte-identical** — whole-run digest `16af05bb8d02a9a5` under
   `renderer=software`, and `55da761fecca5c01` under `renderer=vulkan` at 2500 frames. Regenerate,
   do not retype: those are dated records and [ab-baselines.md](ab-baselines.md) says why.
6. **The 30-line upstream diff does not move.** Nothing here touches `model2_v.cpp` or `model2.h`:
   §1.4's offscreen logic is *already in the driver*, which is the whole reason this phase is cheap.
7. **Coin still works, on both ports, and the check is not "I pressed Select".** `retrohost … "1200:
   select:20"` on `vf2` must take the credit counter from `CREDIT 0/2` to `CREDIT 1/2`, and
   `…:1` the same for player 2. It is in the list because step 6 re-indexes the array every button
   read goes through (§2.5.2), and Select is the one button whose breakage looks like a dead core
   rather than a wrong button.
8. **Step 6's layout change is visible exactly where §2.5.1 says and nowhere else.** The sets without
   a button 5 or 6 — which is most of them, including every fighter — must be byte-identical.

## 5. Notes for whoever does the acceptance run

- **The outer 5 % of the screen reads as offscreen** (§1.4). Not a bug, not ours, do not compensate.
- **`M2VK_OPAQUE_ONLY`, `M2VK_FORCE_SOLID` and friends are irrelevant here** — none of this touches
  the renderer. The one switch that matters is the reticle's own on/off, and it must default off.
- **Do not add a scale factor to fix an aim offset.** If the reticle and the shot disagree, the
  mapping into `ABSOLUTE_MIN..MAX` is wrong; `PORT_MINMAX` is the cabinet's calibration and is
  already correct ([user-options.md §7.2](user-options.md)).
- **`retrohost` gotchas still apply** — own `M2_SAVE_DIR` per run above all, or two runs cross each
  other's NVRAM and the guns start from different emulated state.

## 6. Open, and deliberately not decided here

- ~~**Pointer device on top of lightgun.** §1.6 says flycast offers both and it costs little, and step~~
  ~~2 plans for it — but nothing on this machine can exercise a touch pointer, so it ships untested or~~
  ~~it ships later. Worth a decision before step 2, not during it.~~ **Decided 2026-07-27, before step
  2: Light Gun only.** Not rejected, deferred — it would have shipped untested and outside step 2's
  exit criterion, since `retrohost` cannot drive a touch pointer. It is two entries in
  `PORT_DEVICES` (`retro_entry.cpp`) and a second read path in `libretro_m2_gun_device::update()`
  whenever something can exercise it; step 6's layouts go in the same array.
- **Per-port reticle colour.** Folded into the queued core-options work (§3 step 4).
- ~~**Four-player.** `MAX_PADS` is 2 and no Model 2 gun cabinet is more; not a gap.~~ **Half wrong,
  corrected and fixed 2026-07-27.** It is not a gap *for guns* — no Model 2 gun cabinet is more than
  two — but `airwlkrs` is a genuine four-player set whose `COIN3`/`COIN4`/`START3`/`START4` bound to
  nothing. `MAX_PADS` is now 4 and `MAX_GUNS` is 2; §2.3 is the reader for the second one.
- **Whether `Modern` earns its place** (§2.5.1). It is two table entries, so the cost of shipping it
  is nothing — but it is the layout that collides with the pedals, on the only sets that have a
  button 5. Ship it, and revisit if it generates support traffic; do not pre-emptively gate it.
- **Per-game default layouts.** FBNeo picks `6PANEL` per game from the driver's button count; the
  equivalent here would be a per-set default in the tier table `user-options.md` §6 already proposes.
  Out of scope for this phase — it needs the tier table to exist first, and with 4 sets using button
  5 the payoff is small.
