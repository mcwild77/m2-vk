# Steering response — the analog curve, as core options

**Status: STEPS 1 AND 2 BUILT (2026-08-07), steps 3–7 planned.** Step 1 is the detector, MAME's
captured analog settings and the `M2VK_STEER_LOG` read-out; step 2 is §3.4's pipeline, **reachable
only through the `M2VK_STEER_*` switches** — with none of them set the chain does not run and the
tree is byte-identical to a build of HEAD. §4's steps 1 and 2 below are struck through with what
actually shipped. **No core option exists yet; that is step 3.** Written 2026-08-07. This is
[user-options.md](user-options.md) §6 item 2, promoted to the head of the queue because `daytona` is
not playable with a thumbstick and that is a defect, not a preference.

The complaint in one sentence: **the full lock-to-lock range of a 270° arcade wheel is mapped onto
about 13 mm of thumb travel, linearly, with no shaping anywhere in the chain.**

---

## 1. What the chain actually does today — measured, not assumed

Every link, in order, for `daytona`'s wheel:

| # | Where | What it does |
|---|---|---|
| 1 | [libretro_m2_input.cpp:310-314](../src/osd/libretro_m2/libretro_m2_input.cpp#L310-L314) | left stick X → `normalize_absolute_axis(raw, -32767, 32767)` → `m_axes[AXIS_LEFT_X]`, range ±65536. **No shaping.** |
| 2 | [libretro_m2_input.cpp:861](../src/osd/libretro_m2/libretro_m2_input.cpp#L861) | the pad registers as `DEVICE_CLASS_JOYSTICK`… |
| 3 | [inputdev.cpp:475-490](../src/emu/inputdev.cpp#L475-L490) | …so `input_device_joystick::adjust_absolute_value` applies deadzone **0.15** and saturation **0.85** ([emuopts.cpp:161-162](../src/emu/emuopts.cpp#L161-L162); this OSD overrides neither). Linear rescale between them. |
| 4 | [libretro_m2_input.cpp:521-531](../src/osd/libretro_m2/libretro_m2_input.cpp#L521-L531) | `add_directional_assignments` binds the primary stick X to `IPT_PADDLE`, `IPT_AD_STICK_X` and `IPT_LIGHTGUN_X` at once. |
| 5 | [model2.cpp:1776](../src/mame/sega/model2.cpp#L1776) | `STEER` is `IPT_PADDLE PORT_MINMAX(0x20, 0xe0) PORT_SENSITIVITY(50)` — **±96 counts** around 0x80. |
| 6 | [ioport.cpp:3597-3604](../src/emu/ioport.cpp#L3597-L3604) | `IPT_PADDLE` → `m_absolute = true`, `m_autocenter = true`. |
| 7 | [ioport.cpp:3893-3899](../src/emu/ioport.cpp#L3893-L3899) | absolute branch: `m_accum = apply_inverse_sensitivity(rawvalue)`, then a linear scale onto the port range. |

**Net: the middle 70 % of stick travel covers lock to lock, linearly.** Steps 3 and 5 compound —
the deadzone and saturation *discard* 30 % of the physical travel without giving back any range, so
the effective resolution is worse than a naive linear map.

### 1.1 🚨 `PORT_SENSITIVITY(50)` looks like the knob and is an exact no-op

It reads like a per-game sensitivity the driver already set for us. It is not, for an absolute
device. `apply_inverse_sensitivity` at intake ([ioport.cpp:3777](../src/emu/ioport.cpp#L3777)),
`apply_sensitivity` at read ([ioport.cpp:3766](../src/emu/ioport.cpp#L3766)), and the clamp between
them is against bounds that were *themselves* inverse-scaled
([ioport.cpp:3744-3752](../src/emu/ioport.cpp#L3744-L3752)) — so for any `|raw| ≤ 65536` the clamp
cannot fire and the round trip is the identity. It only bites on keyboard and relative deltas.

MAME's own UI slider for it would therefore also do nothing, and this core draws no MAME UI anyway.
**Do not "fix" the steering by changing a `PORT_SENSITIVITY` value; it will measure as a no-op and
cost a session.**

---

## 2. How the neighbouring emulators handle it

All four are checked out locally and were read, not recalled.

- **Supermodel** (Model 3 — the closest possible sibling, same Sega racers one generation on):
  linear, with **per-axis, per-direction deadzone and saturation** — `m_negDZone` / `m_negSat` /
  `m_posDZone` / `m_posSat`, each half of the axis scaled independently
  (`Src/Inputs/InputSystem.cpp:2484-2510`). Its config section called "Sensitivity" is *only*
  digital→analog ramp speed for keyboard and d-pad (`InputKeySensitivity`,
  `InputDigitalDecaySpeed`, `Src/OSD/SDL/Main.cpp:1600-1607`) plus mouse deadzone. **No curve.**
- **Flycast**: one core option, `_analog_stick_deadzone`, 0–30 %, default 15 %, applied as a
  **radial** deadzone with rescale in polar coordinates
  (`shell/libretro/libretro.cpp:2639-2666`, lifted from parallel-n64), plus a separate trigger
  deadzone. Linear past that. **No curve** — its driving games have the same problem.
- **FBNeo** — the one that actually solves it, by offering **two mapping models per analog input**:
  - `GIT_JOYAXIS_FULL` (`src/intf/input/inp_interface.cpp:330`) — direct absolute map. What we do.
  - `GIT_JOYSLIDER` (`src/intf/input/inp_interface.cpp:110-152`) — **the stick is a rate, not a
    position.** Per frame `nAdd = axis / 0x80`, scaled by a per-input `nSliderSpeed`, *integrated*
    into `nSliderValue`, clamped to `[0x0100, 0xFF00]`. Auto-centre is a separate per-input knob
    `nSliderCenter`: `1` snaps to centre on release, `N` decays by `(N−1)/N` per frame.
- **RetroArch itself**: `input_analog_deadzone` and `input_analog_sensitivity` exist, but
  sensitivity is a multiplier ≥ 1 that *amplifies* (wrong direction) and both are global rather
  than per-core.

**Nobody in that set implements a gamma curve.** That is a console-racer technique, and it is what
people mean when a racing game offers a "steering sensitivity" slider. It is also the cheapest and
highest-value thing available here.

✅ **An independent survey on 2026-08-08 reproduced every reading above**, including that negative — so
the source-reading was right and this section needs no correction. It adds one identity worth writing
down, because it is the only place the vocabularies differ and it hid a shipped feature in plain sight:

🚨 **Supermodel's "saturation above 100 %" IS `model2_steering_range`, expressed as its reciprocal.**
Both mean *physical full deflection gives less than full lock*: their 150 % is our `70%`, their 130 %
our `80%`. Their players cite it as **the** fix for an arcade wheel on a thumbstick, more often than a
curve — see §3.5, where that turns into a live question about our default. ⚠️ One real difference
remains: Supermodel's deadzone and saturation are **per-axis and per-direction** (`m_negDZone`/`m_negSat`
vs `m_posDZone`/`m_posSat`), where ours are symmetric about centre. That matters only for a stick whose
two halves are worn unevenly, and no evidence here asks for it yet.

---

## 3. Design

### 3.1 All shaping happens in our `update()`, on the primary stick X only

One place, one expression, before MAME ever sees the value:
[libretro_m2_pad_device::update()](../src/osd/libretro_m2/libretro_m2_input.cpp#L310).

**X only, never Y.** `desert`'s brake is `IPT_AD_STICK_Y` on the same stick
([model2.cpp:1812](../src/mame/sega/model2.cpp#L1812)); a curve on Y would bend a pedal.

### 3.2 Which games get it: detect `IPT_PADDLE` in the loaded machine — no table

An unconditional curve on left-X is **wrong**, and provably so:

| Game shape | What left-X feeds | Curve is… |
|---|---|---|
| `daytona`, `srallyc`, `indy500`, `desert`… | `IPT_PADDLE` (steering) | correct |
| `vf2`, `fvipers`, `schamp`, `doa` | `IPT_JOYSTICK_*` via a threshold on the analog item | **wrong** — moves when left/right trips |
| `von` | `IPT_AD_STICK_X` (twin stick) | **wrong** |
| `vcop`, `vcop2`, `gunblade` on a pad | `IPT_LIGHTGUN_X` | **wrong** — and the gun's linearity is a deliberate, measured property ([lightgun.md](lightgun.md) §1.1) |

So gate it. **Detect it from the machine rather than authoring it per game**: walk
`machine.ioport().ports()` → `fields()` for any field whose `type()` is `IPT_PADDLE` or
`IPT_PADDLE_V`. That is exactly right by construction — the curve applies iff the game steers — and
it covers all 90 `GAME` entries including the ~68 on the `generic` layout row, with nothing to
author and nothing to keep in sync.

🚨 **It cannot be done in `configure()`.** `osd().init()` runs at `machine.cpp:156` and
`ioport_manager::initialize()` at `machine.cpp:169`, so at `input_init()` **the port list is
empty** — a detector there reports "no paddle" on a game with one. This is the same trap
`m2vk_inputdump.h:125-126` documents and that `M2VK_GUN_LOG` already resolves.

The hook already exists: the one-shot block in
[libretro_m2_osd.cpp:284-296](../src/osd/libretro_m2/libretro_m2_osd.cpp#L284-L296), which runs
behind `machine().phase() >= machine_phase::RUNNING` and is where `gun_log_frame` and
`input_dump_frame` already resolve. Set the flag there; the pad reads it in `update()`.

- **The flag defaults to off**, so if the ordering is ever wrong the failure is one unshaped frame,
  not a crash. (Step 1 verifies the ordering rather than assuming it — the comment at
  `libretro_m2_osd.cpp:275-283` says the first frame the frontend sees is the first RUNNING one,
  which would put the flag ahead of the first `poll_frontend`, but that is an argument and this
  wants a measurement.)
- Cross-thread write/read is safe under the existing baton discipline: the emulation thread is
  parked when `poll_frontend` runs, which is the same guarantee every other snapshot here relies on.

### 3.3 🚨 Pre-compensate MAME's deadzone/saturation so ours is the only shaping

If we simply curve the value, MAME's 0.15/0.85 still applies **on top**: a double deadzone, and
15 % of travel still thrown away. The tempting fix — set `joystick_deadzone 0` /
`joystick_saturation 1.0` globally — changes behaviour for the gun, the twin sticks and every
digital-from-analog fighting game, which is a large blast radius for a steering fix.

Instead, invert step 3 for this one axis. With `m_range = m_saturation − m_deadzone`
([inputdev.h:257-259](../src/emu/inputdev.h#L257-L259)) MAME's transform is exactly

```
out = (|r| − dz) / (sat − dz)          for dz ≤ |r| < sat
```

so emitting

```
r = sign(w) · ( dz + |w| · (sat − dz) ) · 65536          w ∈ [−1, 1], r = 0 when w = 0
```

makes MAME's shaping the **identity** for the steering axis, and only for the steering axis.
Exact to integer rounding (≲ 2 LSB of 65536, i.e. ≲ 0.003 of a 96-count wheel — unobservable).
`dz` and `sat` are read once from `machine.options()` in
[input_init](../src/osd/libretro_m2/libretro_m2_input.cpp#L840), where the machine is in hand, so a
user who *has* overridden them still gets a correct inversion.

### 3.4 The pipeline

`u = m_axes[AXIS_LEFT_X] / 65536` ∈ [−1, 1], then:

1. **Deadzone** — `v = 0` if `|u| ≤ dz_opt`, else `sign(u) · (|u| − dz_opt) / (1 − dz_opt)`.
   Rescaled, not merely clipped, so slow movements stay available (Flycast's property, and the
   reason its comment calls it out).
2. **Curve** — `w = sign(v) · |v|^γ`. Full lock is still reachable at γ > 1; only the *distribution*
   of travel changes, fine near centre and coarse near the ends.
3. **Range** — `w *= range_opt`. Caps maximum lock. Composes with the curve rather than replacing it.
4. **Pre-compensation** — §3.3, then write back to `m_axes[AXIS_LEFT_X]`.

### 3.5 The core options — four, all live

Named for what a player experiences, never for the maths. Defaults in **bold**.

| Key | Label | Values |
|---|---|---|
| `model2_steering_deadzone` | Steering Deadzone | 0%, 2%, **5%**, 8%, 10%, 15%, 20% |
| `model2_steering_response` | Steering Response | Linear, **Slight**, Medium, Strong, Very Strong |
| `model2_steering_range` | Steering Range | **100%**, 90%, 80%, 70%, 60% |
| `model2_steering_mode` | Steering Mode | **Direct**, Rate — *step 6, optional* |

`Slight/Medium/Strong/Very Strong` = γ of `1.3 / 1.7 / 2.2 / 3.0`. **The words are the interface;
the gammas are an implementation detail and must not appear in the option's value strings** — this
is the same rule that makes `model2_internal_res` list sizes rather than multipliers.

⚠️ **The default is deliberately NOT linear, and that is a decision to confirm at step 5.** Every
rendering option in this core defaults to the accurate path, and the reflex is to do the same here.
It does not transfer: there is no accuracy ground truth for a control that does not exist on the
target device. A real cabinet's wheel is 270° of travel; linear-onto-a-thumbstick is not faithful to
it, it is merely unshaped. Shipping `Linear` by default would mean shipping the defect and asking
the player to find the fix. **`Medium` + 5 % was the proposal; the hand-check in §5.3 is where it is
accepted or moved.**

🚨 **DECIDED 2026-08-08 BY THE HAND-CHECK, AND IT MOVED: THE SHIPPED DEFAULT IS `Slight` (γ 1.30).**
Played back, `Medium` was **"awful and twitchy and barely better than linear"** — which is the failure
mode a *too strong* curve has and not the one a too weak one has: the fine centre is bought with a
coarse outer travel, so every correction that leaves the middle lands nowhere near where the thumb
aimed, and the car darts exactly as `Linear` does but at a different part of the sweep. **Deadzone
stays 5 % and Range stays 100 %, both confirmed by the same session** — Range explicitly and without
qualification, which settles the survey's contention below against `80%`. `detail::g_opt_gamma` in
`m2vk_steer.h` and the default in `DEFINITIONS[]` both say 1.30 now; the guard is `ab.sh srallyc 2500`
reproducing `49f86e1309ca422b` / `6fcc26a931ab2b01` / `172bb47c8ba8f383` byte-exactly, which it does.

✅ **An outside survey on 2026-08-08 backs the non-linear default with independent evidence and moves
two of the three numbers into contention.** The evidence for the decision above is now stronger than
"a judgement": someone driving a **real Model 2 Daytona I/O board from a PS4 pad** added an
exponential curve after comparing against a real wheel's output — the same conclusion reached from
the hardware side. What the survey disputes is the *values*:

- **`Medium` may be one notch too strong.** The recommended expo across racing practice is *mild*,
  which on this ladder is `Slight` (γ 1.30), not `Medium` (γ 1.70).
- 🚨 **`model2_steering_range` is the most commonly recommended fix of the three and it defaults to
  off.** It is the same knob Supermodel players call **saturation above 100 %**, expressed as its
  reciprocal — their recommended 130–150 % is our `80%`/`70%` — and reducing maximum lock is cited
  more often than a curve as *the* fix for an arcade wheel on a thumbstick. So `100%` being the
  default may be shipping the same "make the player find it" defect this section rejects for Response.
- **5 % deadzone is confirmed in band** (practice clusters 5–8 %) and is the one number the survey
  does not argue with.

Both disputes are now arms in [steering-handcheck.md](steering-handcheck.md) — Test 3 drives
`Slight` and `Medium` head to head, Test 5 is reframed from "is Range worth keeping" to "should `80%`
be the default". ⚠️ **Neither is settled by the survey and neither should be taken on it**: this is
other people's taste on other hardware, which is exactly the class of question §5.3 exists to answer
with a pad. It changes what gets *compared*, not what gets shipped.

🚨 **One caveat if a number from that survey is ever quoted here directly: none of those emulators
sits inside MAME**, so none of them has `joystick_deadzone 0.15` / `joystick_saturation 0.85` applied
downstream of its own shaping. §3.3's pre-compensation is what makes our percentages mean the same
thing as theirs — without it a 5 % deadzone stacks to roughly 19 % effective with the top 15 % of
travel a flat plateau, which is precisely the defect step 2 measured and removed.

**All four are live** — they are read in `update()` off globals, exactly like `model2_flat_shading`.
[retro_options.h:11-27](../src/osd/libretro_m2/retro_options.h#L11-L27) states the rule: *anything a
player is meant to play with must be live*, and both options that shipped load-only were reported as
"the options do not work" the same day. A steering feel that needs a content reload to try is
unusable — the whole point is nudging it between laps.

### 3.6 The switch discipline

Standing rule: **the `M2VK_*` switch overrides its option, never the reverse.** One switch is
mandatory for the harness —

- **`M2VK_STEER_LINEAR=1`** — the whole chain becomes the identity, i.e. today's behaviour. This is
  what pins a scripted-input fixture against a remembered menu value.

and three take a value so a run can pin a specific shape:
`M2VK_STEER_DEADZONE=<0..0.5>`, `M2VK_STEER_GAMMA=<1..4>`, `M2VK_STEER_RANGE=<0.5..1>`.

Each must join the override-announcement loop at
[retro_entry.cpp:613-618](../src/osd/libretro_m2/retro_entry.cpp#L613-L618), because
*read `[model2] options:` before believing a result* is the rule the whole harness rests on and that
line would otherwise be able to lie.

---

## 4. Order of work

~~**Step 1 — the detector and the read-out, shaping nothing.**~~ ✅ **DONE 2026-08-07.** Four files,
all `src/osd/libretro_m2/`: the new header **`m2vk_steer.h`** (header-only, so no build-script entry —
the `m2vk_gunlog.h` pattern), plus `libretro_m2_input.{h,cpp}` and `libretro_m2_osd.cpp`. **No
upstream file, no shader, no pixel.** The worklog entry of that date is the record.

- **The detector** is in the OSD's `update()` one-shot behind `safe_to_read()`, exactly where §3.2
  said it had to be. `m2vk::steer().active`; nothing reads it yet.
- **`dz`/`sat`** are `machine.options().joystick_deadzone()/joystick_saturation()`, captured in
  `input_init()`.
- **`M2VK_STEER_LOG`** took a third state the plan did not ask for and that the library sweep needed:
  **unset is silent** (the detector still runs), **`=0` is the one-shot resolve report only**, `=n`
  adds the periodic line. The periodic line carries raw, shaped and the resolved port value.
- The pad publishes port 0's primary stick X into `steer()` through a new `publish_steer()`, called
  **after** the lightgun gate so the sample is what MAME is actually handed. At this step it writes
  the same value to `raw` and `shaped`, and the read-out printing them equal is the check.

✅ **§7 question 2 is answered by measurement, not by argument: `resolved on frame 0 after 0 frontend
poll(s)`** on every set run. The resolve report prints that poll count permanently, so it stays
measured.

✅ **The sweep confirms §1's chain to the count.** `daytona`, deflection → `STEER`: 0.00/0.05/0.10/0.15
all `0x080`, 0.20 `0x087`, 0.40 `0x0a2`, 0.50 `0x0b0`, 0.60 `0x0be`, 0.80 `0x0d9`, and
**0.85/0.90/0.95/1.00 all `0x0e0`**. Every point is `0x80 + round(96·(u−0.15)/0.70)`. 🚨 **That
plateau is the signature §5.1 keys on** — its disappearance is what proves step 2's pre-compensation.

✅ **The deliverable — the sets the detector fires on.** Run at 30 frames with `M2VK_STEER_LOG=0` over
every set in `devnotes/roms`: it fires on **11 of the 35 that boot**, and those 11 cover **all eight**
paddle-bearing port-set macros — `daytona`, `desert`, `manxtt`, `motoraid`(←`manxtt`), `srallyc`,
`indy500`, `sgt24h`(←`indy500`), `overrev`(←`indy500`). Cross-checked against every `GAME` entry in
the driver: **30 of 90 steer**, and the detector's answer is exactly that set. Every other set reports
zero, including all four §3.2 names as wrong to curve. `hotd`, `von` and `segabill` could not be run
and are not gaps — the first two are covered by `hotdo`/`vonj`, the third is not a game.

🚨 **§7 question 1's real answer is a set the plan did not name.** Both lines it flagged are correctly
excluded (`:2036` is `skytargt`'s flight stick, `:2120` is `rchase2`'s turret), as are `rchase2a`,
`gunblade`, `bel`, `skisuprg`, `segawski` and `topskatr`. **The borderline is `waverunr`** — its wheel
is `PORT_NAME("Handle Bar")` on `IPT_AD_STICK_X` (`model2.cpp:2349`), a jet ski handlebar, so the
detector will not fire on it. Whether it should is a §5.3 question with a pad in hand; if it should,
the answer is a per-row override in `input_layouts.json`, not a wider type test. ✅ **§7 question 3's
prediction holds**: `manxtt`, `manxttc`, `manxttdx` and `motoraid` all declare `IPT_PADDLE` and are
all detected.

**Guards, all green and all byte-exact against `ab-baselines.md`:** `ab.sh vf2 2500`
(`c3aaa56633c1c4f7`/`9c20f1fac9d9fe92`/`de94f44a06151f71`, SSIM covered 0.996985), `ab.sh srallyc 2500`
— a *steering* fixture — (`49f86e1309ca422b`/`6fcc26a931ab2b01`/`172bb47c8ba8f383`, 136116 covered,
0.988401), `ab.sh schamp 2500` — the generic row — (`964db6922c299090`/`3a270db490e1bc96`/
`b3c2896438f248d0`, 50696 covered, 0.998384). `padmap-gen.py --check`, `padmap-test.sh` and
`M2VK_HOST_DESCRIPTORS=1` all unchanged.

~~**Step 2 — the pipeline, behind the switches only.**~~ ✅ **DONE 2026-08-07.** Three files, all
`src/osd/libretro_m2/`: `m2vk_steer.h` (the shaping, the config reader) and
`libretro_m2_input.{h,cpp}` (the call site and the write-back); `libretro_m2_osd.cpp` changed one
comment. **No upstream file, no shader, no new file, no pixel.** The worklog entry of that date is
the record.

- **`m2vk::steer_shape()` is §3.4 in one function** and `publish_steer()` became
  `shape_and_publish_steer()`: it runs the chain on `m_axes[AXIS_LEFT_X]`, writes the result back,
  and publishes port 0's before-and-after. Still called last in `update()`, after the lightgun gate.
- 🚨 **The gate is "the machine steers AND a shape was named", and the second half is the whole no-op
  argument at this step.** No switch set means the pipeline does not run *at all*, not "runs with
  default parameters" — so the step is a no-op by construction. That goes away at step 3.
- **`M2VK_STEER_LINEAR` takes a value, not a presence** (the `M2VK_BLEND` discipline), and both
  directions are measured: `=1` beats a named γ, `=0` does not.
- The switches are read in `input_init()` (`m2vk::steer_config()`), beside the existing dz/sat
  capture, so the configuration is complete long before the first frontend sample.

✅ **The pre-compensation inverts MAME's transform to the count.** At γ=1, dz=0, range=1 the
`daytona` sweep is `0x080 0x085 0x08a 0x08e 0x093 0x0a6 0x0b0 0x0ba 0x0cd 0x0d2 0x0d6 0x0e0` — every
point is `0x80 + round(96·u)` exactly, **the dead first 15 % and the `0x0e0` plateau are both gone**,
and the round trip is exact to a single count. That is §5.1 row 2 and it is the proof of §3.3.

✅ **A second steering set was added to the check and is worth keeping: `srallyc`**, whose
`PORT_NAME("Steering Wheel")` is a full-range `IPT_PADDLE` (`0x000..0x0ff`) rather than daytona's
±96, so the arithmetic is exercised on a different port range. Same defect unshaped, same fix.

🚨 **The strongest guard is a binary comparison, not a table: a build of HEAD with neither step in it
gives the same whole-run digest and a byte-identical last frame as this tree over the full analog
sweep** — `2a8f0b31bc690e6c`, and so do this tree silent, under `M2VK_STEER_LOG=5`, and under
`M2VK_STEER_LINEAR=1`. Every shaped arm differs, which is what says the curve reaches the picture.

⚠️ **§5.2's negative control took two attempts and the first was vacuous** — see §5.2 below, which is
corrected in place. ⚠️ **§5.1's expected value for `range=0.7` was a slip** and is corrected there too.

~~**Step 3 — the three core options.**~~ ✅ **DONE 2026-08-07.** Four files, all
`src/osd/libretro_m2/`: `m2vk_steer.h` (the option storage and the composition), `retro_options.{h,cpp}`
(the table and the getters) and `retro_entry.cpp` (the reads, the log line, the switch announcements).
**No upstream file, no shader, no new file, no pixel.** The worklog entry of that date is the record.

- **The three options are `Steering Response` / `Steering Deadzone` / `Steering Range`**, exactly §3.5
  minus `model2_steering_mode`, which is step 6. Defaults **Slight / 5 % / 100 %** (⚠️ **Medium until
  the hand-check moved it on 2026-08-08** — every digest recorded further down this section was taken
  with Medium as the default and is a dated record, not a current expectation), and the words are
  the interface — the gammas live in `STEERING_RESPONSE_GAMMA[]` beside the value strings, one list, so
  there is no second table to drift.
- **`steer_apply()` is the composition and it is a function for `apply_force_solid()`'s reason**: a
  live change and a change at load must mean the same thing, and two copies of the resolve rule is how
  that stops being true. The option values live **outside `steer_state`** — that struct belongs to the
  machine and `steer_close()` resets it, while the options belong to the player and are parked before
  the machine that will read them exists.
- 🚨 **The step-2 no-op argument is GONE and nothing replaces it in the same form.** "No switch set"
  used to mean "no shaping at all"; a default run of a steering game is now shaped. What keeps every
  `ab-baselines.md` fixture byte-exact is narrower: **a centred stick returns 0 at every setting**, and
  no accuracy fixture scripts an analog axis. Both halves are measured below.
- ⚠️ **`declare_variables()` REORDERS and that had been a no-op until now.** The pre-options form wants
  the default first; every option's default was also its first value until `model2_steering_deadzone`,
  whose values run 0 % → 20 % in the order a player scrolls them and whose default is 5 %. The code was
  already right and its comment was not; both are corrected in place.

✅ **The options reach the pipeline, and switches beat them — six digests, `daytona`, one 12-point
sweep script, 4120 frames.** Every arm differs from every other except where an equality is the claim:

| arm | digest | what it establishes |
|---|---|---|
| defaults, no switch | `0ed27e15f8e29021` | the pipeline runs off the **core options** — the resolve report says `gamma=1.70 (core option)` |
| `M2VK_STEER_LINEAR=1` | `04dd594589f84d38` | identity: `raw==shaped` on every read-out line, and the `0x0e0` plateau from 0.85 is back |
| switches γ=1 dz=0 range=1 | `34b7ede0a24688e4` | straight line, `0x80 + round(96·u)` **to the count** — step 2's result through the new composition |
| the same shape as **options** | `34b7ede0a24688e4` | **byte-identical to the row above**: one pipeline, two sources |
| option `Very Strong` alone | `242ac01a4f2e3600` | an option alone reaches the picture |
| option `Very Strong` + `M2VK_STEER_GAMMA=1.7` | `0ed27e15f8e29021` | **the switch overrides the option, byte-exactly** — equal to the default arm |

🚨 **THE STRONGEST GUARD IS A BINARY COMPARISON AND IT STILL HOLDS AT STEP 3: a build of HEAD, with
none of the three steps in it, gives `04dd594589f84d38` and a byte-identical last frame to this tree
under `M2VK_STEER_LINEAR=1`.** That is what says the harness can still pin a run to the pre-steering
behaviour now that shaping is the default.

✅ **§3.5's "all three are live" is measured, and the obvious test for it passed vacuously first.**
`M2VK_HOST_OPT_AT=3800:model2_steering_response=Very Strong` produced the **same** digest as the static
Very Strong arm — the change was applied (the `core options changed:` line fires) but every frame before
it was pixel-identical anyway, because the sweep's early points are inside the deadzone or barely off
centre with the car not yet moving. The discriminating run is **`M2VK_HOST_OPT_AT=3900:model2_steering_range=60%`**:
static 100 % `0ed27e15f8e29021`, static 60 % `610a63398586c048`, **live `936a8df95c4d1ac9` — different
from both**, with the resolve report showing the run *started* at `range=1.000`. ⚠️ **Pick a live-change
point where the two static arms visibly differ afterwards**, or the test cannot fail.

✅ **The negative control holds with all three arms (§5.2), on the OPTION path rather than the switch
path.** `vf2`, coins at 600 and 700, Start pulsed to 2400: no-stick `8a2b4fe8d155ca9e` **differs from**
sweep `2b469cf302928efa`, so the stick is doing something; and sweep under the defaults, under
`Very Strong` + 20 % deadzone, and under `M2VK_STEER_LINEAR=1` are **all `2b469cf302928efa`** — one
digest, three configurations, which is the detector excluding `vf2`. ⚠️ Those digests are not step 2's
(`087eb9a69ffd15d6` / `c721a3b11e35d07d`): the sweep script has different deflection points and a
negative half. **The invariant is the equality, never the number.**

**Guards, all byte-exact against `ab-baselines.md`:** `ab.sh vf2 2500`
(`c3aaa56633c1c4f7`/`9c20f1fac9d9fe92`/`de94f44a06151f71`, 107568 covered, SSIM covered 0.996985),
`ab.sh srallyc 2500` — the steering fixture, now shaped by default and unchanged because it scripts no
analog input — (`49f86e1309ca422b`/`6fcc26a931ab2b01`/`172bb47c8ba8f383`, 136116, 0.988401), `ab.sh
schamp 2500` (`964db6922c299090`/`3a270db490e1bc96`/`b3c2896438f248d0`, 50696, 0.998384).
**`padmap-gen.py --check` and `padmap-test.sh` both pass**, which is this step's stated evidence that no
layout data moved. The `[model2] options:` line carries all nine options and the three new ones are
visible in every `ab.sh` report.

~~**Step 4 — labels and discoverability.**~~ ✅ **DONE 2026-08-07.** Two files —
`src/osd/libretro_m2/input_layouts.{json,ipp}`, the second generated from the first — plus
`devnotes/tools/padmap-gen.py`. **No C++, no upstream file, no shader, no pixel.** The worklog entry of
that date is the record. No new table: the fix is a row in the table that already exists and a check in
the generator that already reads both halves.

- ✅ **The label reads correctly under a curve, and "under a curve" turned out not to be a variable.**
  Descriptors are a property of the layout row, not of the shaping, so `M2VK_HOST_DESCRIPTORS=1` gives
  a **byte-identical** analog descriptor list with the pipeline on and under `M2VK_STEER_LINEAR=1` on
  every set checked. That is the whole of the "under a curve" half and it is worth stating as an
  equality rather than an impression.
- 🚨 **The cross-tab is the deliverable, and it found exactly one gap: `desert`.** Resolving all 90
  `GAME` entries through `layout_for()`'s own rule (exact name, then parent, then generic) against the
  machines' own paddle fields: **29 of the 30 steering entries already name the wheel** (`Steering` on
  daytona/manxtt/motoraid, `Steering Wheel` on srallyc/indy500/stcc/sgt24h/overrev, inherited by every
  clone through the parent pass), and **`desert` was the one on the generic row** — shaped by the curve
  while being offered the hedge.
- **`desert` now has a row, and its buttons are the generic row's order unchanged** — B/A/Y/X/R/L for
  MAME buttons 1–6 — so nothing a player has learned moves and only the wording arrives: *Machine
  Gun · Cannon · Shift · VR1 (Blue) · VR2 (Green) · VR3 (Red)*, `LSTICK_X` **Steering**. Slots 7–9 are
  `NONE` (the machine declares no such buttons; the generic row had them on the trigger thresholds and
  R3, where they bound nothing).
- 🚨 **Two labels the generic row could not have got right, and both are about pedals.** `desert`'s
  BRAKE port is an **`IPT_AD_STICK_Y`** — the left stick's Y axis, not a trigger — so `LSTICK_Y` is
  labelled **Brake** where the hedge said "Stick Y"; and the set declares **no `IPT_PEDAL2` at all**, so
  L2 does nothing and is now unlabelled where the hedge said "Brake / Button 7". Descriptor count 76
  → 44: the fallback labels every control, the row labels the ones that exist.
- ✅ **The coupling is a check now, in `padmap-gen.py --check`, and it fires in both directions.** A
  paddle-bearing set with no row (or a row whose `LSTICK_X` does not say steering) is refused, and so is
  a row saying steering whose sets declare no paddle. The paddle half is read from `padmap-data.js`,
  i.e. from the machines, so it cannot drift from what the detector will do; it is silent when that file
  is absent and treats an undumped port set as unknown rather than paddle-free.
  ⚠️ **All three failure arms were fired by mutation and restored** — see
  [padmap-tool.md](padmap-tool.md) §1.2. In the "row removed" arm the checker named **`desert` and
  nothing else**, which is the 29-of-30 claim above measured rather than asserted.
- **The generic hedge stays `Steering / Stick X` and that is a decision.** No paddle-bearing set can
  reach it any more, so its steering half now only ever appears on machines the curve does not touch —
  but the one borderline case is `waverunr`, whose `PORT_NAME("Handle Bar")` on `IPT_AD_STICK_X` **is**
  a steering control the detector does not fire on (§7 question 1). The hedge is the honest wording for
  a cabinet nobody has authored, and narrowing it to `Stick X` would state the wrong thing there.
  §5.3 item 8 is unchanged and is where that gets decided with a pad.
- **Guards:** `ab.sh desert 2500` — the changed set — `0c5533aa763ce5c3` / `bcf3237a5747a53b` /
  `444c4d30a83c91f4`, covered 138222, SSIM covered 0.999376; `ab.sh vf2 2500`
  `c3aaa56633c1c4f7` / `9c20f1fac9d9fe92` / `de94f44a06151f71`, covered 107568, 0.996985. Both
  byte-exact against `ab-baselines.md`. `padmap-gen.py --check` (23 rows / 27 sets, was 22 / 26) and
  `padmap-test.sh` (32 port sets, 31 dumps) pass.

**Step 5 — the RetroArch hand-check (§5.3) and the default decision.**
The user's, not scriptable. Everything before this is numbers. ⚠️ **[steering-handcheck.md](steering-handcheck.md)
is the script for it, and it was amended 2026-08-08** — Tests 3 and 5 each grew a head-to-head arm
after an outside survey said both decisions are closer-run than this file assumed. See §3.5 and that
file's "Where these arms come from".

🛑 **~~Step 6~~ IS CLOSED, 2026-08-08. IT WAS BUILT, PLAYED, AND REMOVED THE SAME EVENING — "rate
mode is unusable and atrocious". DO NOT REBUILD IT.** This is a decided question now, not an open
option, and the whole of what it cost is written here so the next session spends nothing on it.

- **It was built exactly as scoped below and it worked**: `model2_steering_mode` = `Direct`/`Rate`
  plus `model2_steering_rate_speed`, per-port accumulator in the pad device, integrate on hold and
  unwind to centre on release, gamma shaping the rate. Every guard passed — `ab.sh srallyc` byte-exact,
  `vf2` untouched, all four option/switch override arms correct, and the integrator's arithmetic
  matched `M2VK_STEER_LOG` to the last digit. **None of that is why it was removed. It was removed
  because it plays badly**, which is the only test that could ever have decided it.
- 🚨 **The first speed band was 4x too slow and that is NOT the reason either.** It shipped at
  1.00/0.65/0.40/0.25 s to lock — shaped by Supermodel's *digital* ramp defaults, which is a keyboard
  control and the wrong reference for a thumbstick. Rebuilt at 0.25/0.20/0.15/0.10/0.05 s on the
  user's own numbers, and the verdict on that band was the one quoted above. **A third band is not
  the missing piece.**
- ⚠️ **The argument that motivated it still looks sound and is still wrong in practice, which is the
  part worth keeping.** It is true that a positional map can only redistribute sensitivity across
  ~13 mm of thumb travel and never reduce it, and true that decoupling travel from angle escapes that
  constraint. What the reasoning left out is that **it also throws away the stick's self-centring and
  its absolute correspondence**, and on a racing wheel those are worth more than the sensitivity
  budget they cost. §2's survey said this in advance — every neighbouring emulator uses rate for
  keyboard and d-pad and positional for a stick — and this is that reading confirmed by playing it.
- **So the trade the other three options live inside is the real one and there is no way out of it.**
  Steering feel is now entirely a question of where Response and Range are set. See §3.5.

~~**Step 6 — OPTIONAL, and a second phase: `model2_steering_mode = Rate`.**~~
FBNeo's `GIT_JOYSLIDER` model — integrate deflection into a wheel position, auto-centre on release
at a configurable rate. Deliberately *not* in the first four steps: it needs per-port accumulator
state living outside the machine, so it is invisible to savestates and makes a run non-deterministic
under `retrohost`. Fine for playing, awkward for the harness, and it should not hold up three
options that are stateless and cost nothing.

⚠️ **The 2026-08-08 survey supports deferring this but FLIPS THE REASON, and the new reason is the
stronger one.** Player practice across the racing emulators is that the **positional model wins for a
thumbstick** and rate is treated as the fallback for **d-pad and keyboard** — so this is not "the
better model, postponed for harness reasons", it is the *wrong* model for the control it was scoped
against. What is left of it is genuinely valuable and is a different feature: a rate path for digital
input, which today has no shaping at all.

🚨 **And that lands on the layouts before it lands here.** `daytona` spends its entire d-pad on VR1–4
([input_layouts.json](../src/osd/libretro_m2/input_layouts.json)), so on the phase's own testbed
there is no digital control left to steer with. `srallyc`'s d-pad is free. Anyone picking this up
decides a layout question first.

**Step 7 — docs.** Worklog entry, this file struck through as as-built, CLAUDE.md's *Where we are*
and *Next step*, and `user-options.md` §6 item 2 marked done.

---

## 4a. The read-out bar — built 2026-08-08, outside the seven steps

`model2_steering_display` (`m2vk_steerbar.{h,cpp}`, `renderer_vk/shaders/steerbar.frag`). Asked for
directly rather than planned here, and it belongs to step 5 in everything but numbering: **the curve is
invisible to the person being asked to judge it**, which is the one weakness §5.3 could not design
around. The bar draws the resolved `IPT_PADDLE` port value as green out from the centre, red for the
travel the game is not getting, and a white notch at the raw stick — so the **gap between the notch and
the end of the green is γ**, live, while driving.

It changes no input and is not part of the chain: §3.4's pipeline is untouched, and the option is off
by default with the paddle port not read at all when it is. The worklog's 2026-08-08 (2) entry is the
record; [steering-handcheck.md](steering-handcheck.md) §0.5 is how to turn it on.

## 5. Verification

### 5.1 Static — the sweep, and it is the real test

The measurement this whole feature needs is *deflection in → port value out*, and `retrohost` can
already produce it: half-axes take a deflection fraction
([retrohost.c:25-28](retrohost.c#L25-L28)), so a sweep is a script.

```sh
# 11 points from centre to full lock, 30 frames each, with the read-out on.
M2VK_STEER_LOG=10 ./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/daytona.zip 4200 /dev/null \
  "600:select,900:start,1200:start,1500:start,1800:start,\
3600:lx+=0.1:30,3640:lx+=0.2:30,3680:lx+=0.3:30,3720:lx+=0.4:30,3760:lx+=0.5:30,\
3800:lx+=0.6:30,3840:lx+=0.7:30,3880:lx+=0.8:30,3920:lx+=0.9:30,3960:lx+=1.0:30"
```

Run it once per configuration and compare the curves. What each run proves:

| Run | Expected | ✅ measured 2026-08-07 (step 2) |
|---|---|---|
| `M2VK_STEER_LINEAR=1` | straight line `0x80`→`0xe0`, **with** the 0.85 saturation plateau — today's behaviour | holds, and byte-identical to a HEAD build |
| `γ=1, dz=0, range=1` | straight line `0x80`→`0xe0`, **no plateau** — the pre-compensation works | holds, every point `0x80 + round(96·u)` to the count |
| `γ=2.2` | monotone, concave, reaching exactly `0xe0` at full deflection | holds |
| `range=0.7` | ~~same shape, topping out near `0xa4`~~ **`0xc3`** | holds at `0x0c3`; **`0xa4` was a slip** — `0x80 + 0.7·96` is `0xc3` |
| `dz=0.05` | `0x80` held to 5 % deflection, then the curve, still reaching `0xe0` | holds; and 0.10 lands on `0x085` not `0x08a`, because the deadzone is *rescaled* and gives its travel back |

🚨 **This is not the banned press-sweep.** The ban at the top of CLAUDE.md is on *pressing buttons to
find out what a button does* — a multi-thousand-frame run per arm for a null result. This is an
analog read-out with a known expected value per point, answered numerically, which is exactly what
`M2VK_INPUT_DUMP` and `M2VK_HOST_DESCRIPTORS` exist to make possible.

⚠️ **Getting `daytona` onto the track matters** or the sweep measures an attract screen. The coin and
four Start pulses above are the shape that worked for `vcop`; confirm with a screenshot of the last
frame before believing any null result. This is the lightgun phase's §2 lesson and it has already
cost a session once.

### 5.2 The no-op guards

- **`ab.sh` is a guard here and nothing more.** Input changes no pixel, so a green table is not
  evidence of anything working — it is evidence of nothing *breaking*. `vf2`, `daytona` and one
  generic-row set must reproduce `ab-baselines.md` byte-exactly.
- **The negative control is a non-steering game.** `vf2` under a strong curve must be
  byte-identical to `vf2` under `M2VK_STEER_LINEAR=1` — that is what proves §3.2's detector excludes
  it. Without this arm, "the curve works" and "the curve is applied to everything" fit the same
  evidence. ✅ **Holds at `c721a3b11e35d07d` (γ=2.2, dz=0.2), 2026-08-07.**

  🚨 **AND IT NEEDS A THIRD ARM, which the plan did not ask for and the first attempt did not have.**
  Coin at 600 plus Start pulses leaves `vf2` on the staff-roll attract screen reading `CREDIT 1/2` —
  it wants **two coins** — where the stick moves nothing, so a run with *no stick input at all* gave
  the same digest as the sweep and the control passed vacuously. With coins at 600 and 700 and Start
  pulsed to 2400 the game is in a real round, and `no stick` (`087eb9a69ffd15d6`) now differs from
  `sweep` (`c721a3b11e35d07d`) while `sweep` and `curve` still agree. **Run the no-stick arm first;
  without it, "correctly excluded" and "the stick did nothing here" fit the same evidence.**
- **`M2VK_STEER_LINEAR=1` must reproduce the pre-step-2 binary** on a scripted-input daytona run.
  ✅ **Done better than asked: a build of HEAD, with neither step 1 nor step 2 in it, gives the same
  whole-run digest `2a8f0b31bc690e6c` and a byte-identical last frame** — as do this tree silent,
  under `M2VK_STEER_LOG=5`, under `M2VK_STEER_LINEAR=1`, and under `LINEAR=1` with a γ and a deadzone
  also named. Every shaped arm has its own digest.
- ⚠️ **Write the sweep as a `#!/usr/bin/env bash` script.** An ad-hoc `for p in $pts` in the
  interactive shell does not word-split (this shell is zsh), which produced a whole round of runs
  whose input script was one malformed entry, so every arm had *no analog input* and every digest
  agreed. It reads exactly like a clean no-op. CLAUDE.md gotcha 8, one round further on: it is not
  only `env $e`, it is any unquoted list.
- `padmap-gen.py --check`, `padmap-test.sh`, `M2VK_HOST_DESCRIPTORS=1`.

### 5.3 In RetroArch, on this Mac — the hand-check

Everything above is numbers on a curve. Whether the car is drivable is not, and per CLAUDE.md that
part is the user's with a pad in their hands.

**Before anything else, check the installed-core symlink** — it has reverted to a plain byte-identical
copy **four** times, and the failure is silent until the next rebuild:

```sh
ls -la ~/Library/Application\ Support/RetroArch/cores/model2_libretro.dylib   # must print '-> …/mame-model2-vk/…'
```

Then launch — and **do not pin options**; the menu has to be in charge or a setting changed in it
appears not to work:

```sh
./devnotes/tools/padmap-serve.py     # then ▶ Play, which launches ./model2_libretro.dylib BY PATH
```

The ▶ Play button is the right vehicle here for three reasons that are already written down in
[padmap-tool.md](padmap-tool.md) §3.2.2: it bypasses the reverting symlink entirely, it strips every
`M2VK_*`/`M2OPT_*` from the environment by prefix (and a stale switch **beats** the options menu by
design, so one left over reads exactly like a broken option), and it refuses to launch a core older
than the generated table. Failing that, `~/Desktop/Model 2.app`.

⚠️ **RetroArch rewrites `config/m2-vk/m2-vk.opt` on exit regardless of `config_save_on_exit`** —
core options are saved separately. Whatever is set during the check is what the next launch starts
from.

The check itself, in `daytona`:

1. **Options → Steering Response = Linear, Deadzone = 0%.** Drive. This is the current defect and
   the baseline to compare against — expect the car to dart.
2. **Response = Medium**, live, without reloading. It must change *on the next corner*, not on the
   next load. If it needs a reload, §3.5 was not honoured and that is a bug, not a preference.
3. Walk `Slight → Medium → Strong → Very Strong` over a few laps. Which one lets you hold a line
   through Daytona's banking without sawing at the stick?
4. **Deadzone**: with it at 0%, does the car wander on the straight with your thumb off the stick?
   Raise until it stops. That number is the right default for this pad.
5. **Range 80%**: does full deflection still get you around the hairpin? If yes, it is a useful
   option; if no, drop the low end of the value list.
6. **Negative control — `vf2`.** Movement must feel exactly as it does today. If a curve is
   reaching the fighting games, §3.2's detector is wrong and the sweep in §5.1 will not show it.
7. **Second steering game — `srallyc` or `indy500`** — to confirm the detector fires on more than
   the one game it was tuned against.
8. **`waverunr`** — the one set that steers and is deliberately *not* detected (§7 question 1, found
   at step 1: its handlebar is `IPT_AD_STICK_X`). Does its unshaped handlebar have the same complaint
   `daytona`'s wheel does? If yes, the fix is a per-row override, not a wider type test.

Report back which response step and which deadzone; those become the defaults in §3.5.

---

## 6. Out of scope, and why

- **Analog triggers (the pedals).** The same argument applies with less force — a trigger has more
  travel than a stick and a pedal has less range than a wheel. Worth a look later; folding it in now
  would give the steering work no clean no-op guard, which is the same reason
  [per-game-input.md](per-game-input.md) kept the curve and the layouts apart.
- **Speed-sensitive steering** (reduce authority as speed rises). It is what the console ports of
  these games do, and it needs the car's speed — which means reading emulated RAM per game. That is
  a per-set data table and a new class of coupling to the driver, for a refinement on top of a fix
  we do not have yet.
- **Force feedback / rumble.** Unrelated, and `RETRO_ENVIRONMENT_SET_RUMBLE_INTERFACE` is not wired.
- **An output slew limit** — a cap on how fast the shaped value may move per frame, which sim-racing
  configs almost always carry alongside a curve and which the 2026-08-08 survey names as the one piece
  of standard practice missing here. ✅ **BUILT 2026-08-11 as `model2_steering_damp_drive` /
  `model2_steering_damp_return`** — asked for directly after the user timed the official emulator's
  displayed wheel: **~4 frames to full lock, ~7 to recentre**, an asymmetric slew limit applied in the
  input layer before the game reads the axis. That asymmetry is why it is **two knobs** (frames-to-lock,
  one rate growing and one shrinking) rather than one, and the reference is a hard rate limit, not an
  exponential low-pass — a stick reaches lock in one frame and feels twitchy without it. `m2vk_steer.h`
  `steer_damp()` runs it against a per-seat carry; switches `M2VK_STEER_DAMP_DRIVE` / `_RETURN` (frame
  counts) override the options and `M2VK_STEER_LINEAR` bypasses it whole.
  - ⚠️ **The no-op guard held without re-arguing** — the concern below was that a limiter converges to
    zero but not in one frame. It is not gated on `s.shaping`, but it IS an identity when both rates are
    `Off` (the default) *and* under `M2VK_STEER_LINEAR`, and it tracks its target exactly the moment
    damping is off. Since no [ab-baselines.md](ab-baselines.md) fixture scripts an analog axis, the
    limiter never leaves its rest state on any of them: `ab.sh srallyc 2500` reproduced
    `49f86e1309ca422b` / `6fcc26a931ab2b01` / `172bb47c8ba8f383` and `ab.sh vf2 2500` its baseline,
    byte-exact. `steer_shape()` stays a pure function of the current sample; the *state* lives in the
    pad device, not the shaper.
  - 🚨 **Both default `Off`, and step 5's hand-check picks the real default** — `Slight`'s Medium→Slight
    precedent. The measured reference (4 drive / 7 return, in our ~57.5 Hz frames vs the 60 fps it was
    timed at — a <7 % rate difference, imperceptible) is the number to start the hand-check from, not a
    shipped default. Verified by the read-out: `M2VK_STEER_DAMP_DRIVE=4 M2VK_STEER_DAMP_RETURN=7` on a
    scripted full-right-then-release `daytona` climbs the port `0x080→0x08e→0x0b0→0x0d2→0x0e0` in four
    frames and falls back in seven.
  - ~~⚠️ Added to this list rather than to the steps, and the reason is the no-op guard.~~ (Kept struck
    as the record of the concern the build then cleared.)
- **Touching `joystick_deadzone` / `joystick_saturation` globally.** §3.3 — the blast radius covers
  the gun and every digital-from-analog game, and the pre-compensation gets the same result with
  none of it.

## 7. Open questions

1. ~~**Does the detector fire on every set that steers?**~~ ✅ **ANSWERED at step 1 — it fires on all
   30 of the 90 `GAME` entries that declare `IPT_PADDLE`, and on nothing else.** Both lines named
   here are correctly excluded. **The one borderline is `waverunr`'s `PORT_NAME("Handle Bar")` on
   `IPT_AD_STICK_X`** ([model2.cpp:2349](../src/mame/sega/model2.cpp#L2349)) — a jet ski's handlebar,
   which steers and is not detected. Carried to §5.3 as a hand-check item; the fix, if it is one, is
   a per-row override rather than a wider type test. See step 1 above for the full table.
2. ~~**Does the flag get set before the first `poll_frontend`?**~~ ✅ **ANSWERED at step 1 by
   measurement: frame 0, 0 polls, on every set run.** The read-out prints the poll count at resolve
   time so it stays measured rather than remembered.
3. **`manxtt` / `motoraid` — a motorcycle's lean is not a wheel.** Both declare `IPT_PADDLE`, so the
   detector will fire — ✅ **confirmed at step 1, on `manxtt`, `manxttc`, `manxttdx` and `motoraid`.**
   Whether the same curve suits them is a step 5 question, and if it does not, the answer is a
   per-row override in `input_layouts.json` rather than a second mechanism.
4. **Default confirmation** — §3.5, decided at step 5. ⚠️ **Narrowed 2026-08-08 from "confirm three
   numbers" to two specific head-to-heads**, because the survey in §3.5 put `Slight` vs `Medium` and
   `100%` vs `80%` in contention and left the 5 % deadzone alone.
   [steering-handcheck.md](steering-handcheck.md) Tests 3 and 5 are the arms; its answers 1b and 3b are
   the new blanks. **The survey does not decide either** — it decides what gets compared.
