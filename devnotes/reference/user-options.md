# User-facing options — the input survey, and everything the player should be able to change

**Status: a proposal, not a decision.** Nothing here is built. It exists because the plan had no
schedule for any of it: P1 built a generic input layer and two core options, and P0–P6 never says when
the rest happens. **Target phase: P6**, which is where core options get their final shape.

Two core options exist today and no more — `model2_renderer` and **`model2_diagnostic_input`**.
⚠️ **`model2_service_buttons` was retired on 2026-07-28** by the lightgun phase's step 6, which
replaced it with FBNeo's diagnostic-combo list (§7, [lightgun.md](lightgun.md) §2.5.3). Every mention
of it below this line is a dated record of what was true when it was written, not current — the
service coin is still on L3, but gated on the *new* option being anything other than `None`.

---

## 1. The input survey — 83 GAME entries, 32 input-port sets, 6 tiers

**[measured 2026-07-26, parsed out of `src/mame/sega/model2.cpp` at mame0288.]** Grouped by what the
OSD has to *do*, not by genre. "not flagged" = MAME does not mark the set `MACHINE_NOT_WORKING`.

| tier | sets | not flagged | what the driver actually declares | handled today? |
|---|---|---|---|---|
| **buttons** | 29 | 12 | digital only — `IPT_BUTTON1..n` | ✅ yes, fully |
| **driving** | 26 | 5 | `IPT_PADDLE` + `IPT_PEDAL` + `IPT_PEDAL2` (+ `gears`, + `PEDAL3` handbrake on srallyc) | ⚠️ mapped, but bad — §2 |
| **exotic** | 11 | 0 | see §1.2 | ⚠️ partly, by accident |
| **adstick** | 7 | 3 | `IPT_AD_STICK_X/Y`, one or two players | ✅ probably fine |
| **lightgun** | 6 | 3 | `IPT_LIGHTGUN_X/Y` ×2 | ❌ on an analog stick — §3 |
| **twinstick** | 4 | 0 | Virtual On — *digital* pairs, not analog | ✅ dedicated path exists |

**The important structural fact: MAME has already normalised every exotic controller into a standard
analog type.** The OSD never sees "bat controller" or "ski platform" — it sees `IPT_PEDAL` and
`IPT_AD_STICK_X`. That is why the generic layer covers as much as it does, and it means **no tier
below needs new plumbing, only better mapping**.

### 1.1 Tier membership

- **buttons** — `vf2` (11: vf2/fvipers/lastbrnx/hpyagu98), `doa` (5), `dynamcop` (5), `zerogun` (4),
  `vstriker` (2), `schamp` (2), `pltkids` (2), `airwlkrs`, `rascot2`
- **driving** — `daytona` (8), `indy500` (7: indy500/stcc), `srallyc` (5), `manxtt` (3), `overrev` (3),
  `motoraid` (2), `sgt24h`
- **adstick** — `gunblade`, `bel`, `rchase2`, `rchase2a`, `skytargt`, `waverunr`, `desert`
- **lightgun** — `vcop` (2), `vcop2`, `hotd` (3)
- **twinstick** — `von` (4)
- **exotic** — §1.2

### 1.2 The exotic tier, one by one — and it is smaller than it looks

**Every set here is `MACHINE_NOT_WORKING` at mame0288**, so none of it is on the critical path.

| set | the real cabinet | what MAME declares | verdict |
|---|---|---|---|
| `dynabb` (2) | bat you pull back and snap | `IPT_PEDAL` / `IPT_PEDAL2`, 8-bit `0x00→0xff`, named "P1/P2 Bat Swing" | **already works** on a trigger — §1.3 |
| `topskatr` (4) | skateboard deck | `AD_STICK_X` ×2 ("Curving", "Slide") + Jump Front/Tail buttons | maps to two stick axes; fine |
| `segawski` (1) | ski platform | `AD_STICK_X` ("Slide") + Pitch L/R buttons | fine |
| `skisuprg` (1) | ski platform + foot sensors | `AD_STICK_X` "Inclining", `AD_STICK_Y` "Swing", **foot sensors as digital nibbles** on `IN2` | odd but mappable; `UNEMULATED_PROTECTION` too |
| `powsled` (3) | **4 linked sled cabinets** | `PEDAL`/`PEDAL2` ×2, plus "P1 Entry", "P1 Call", **"Cancel Network Check"**, `m2comm` link board | ❌ **out of scope — §1.4** |

### 1.3 The baseball bat, since it prompted this

[model2.cpp:2246-2249](../../src/mame/sega/model2.cpp#L2246-L2249):

```
PORT_START("BAT1")
PORT_BIT(0xff, 0x00, IPT_PEDAL)  PORT_SENSITIVITY(100) PORT_KEYDELTA(50) PORT_PLAYER(1) PORT_NAME("P1 Bat Swing")
PORT_START("BAT2")
PORT_BIT(0xff, 0x00, IPT_PEDAL2) PORT_SENSITIVITY(100) PORT_KEYDELTA(50) PORT_PLAYER(2) PORT_NAME("P2 Bat Swing")
```

One 8-bit value. The pull-back-and-snap is cabinet feel; the board reads a number. Our layer already
maps `IPT_PEDAL` → R2 and `IPT_PEDAL2` → L2
([libretro_m2_input.cpp:229-230](../../src/osd/libretro_m2/libretro_m2_input.cpp#L229-L230)), so the
swing works on a trigger today.

⚠️ **One suspected bug, unverified.** `apply_device_defaults()` matches
`entry.player() == device->devindex()`, and BAT1/BAT2 are different *types* (`PEDAL`/`PEDAL2`) as well
as different players. So P1's bat should land on pad 1's **right** trigger and P2's on pad 2's
**left** — asymmetric between the two players. Untested; `dynabb` is `NOT_WORKING`, so this is a code
reading, not a measurement. It is the cleanest example of why a per-game override table is needed.

### 1.4 Explicitly out of scope

**Networked / multi-cabinet sets are dropped**, decided 2026-07-26. `powsled` (3 sets) needs four
linked cabinets through the `m2comm` board and has a "Cancel Network Check" input; `rascot2` (Royal
Ascot 2) is a betting terminal. All four are `MACHINE_NOT_WORKING` upstream. **They are not targets and
no option, mapping or test fixture should be spent on them.** If one ever boots, it boots on the
generic layer and that is the end of the support commitment.

---

## 2. Steering — the sharpest real problem, and it is not a mapping problem

**Daytona on a thumbstick is bad, and correctly mapping it does not fix that.** The cabinet wheel is
~270° of travel; a thumbstick is a few millimetres and self-centres hard. The driver asks for a linear
absolute axis — [model2.cpp:1739](../../src/mame/sega/model2.cpp#L1739):

```
PORT_BIT(0xff, 0x80, IPT_PADDLE) PORT_MINMAX(0x20, 0xe0) PORT_SENSITIVITY(50) PORT_KEYDELTA(10)
```

`PORT_SENSITIVITY`/`PORT_KEYDELTA` do **not** help: they govern MAME's *digital-increment* emulation of
an analog control, not the shaping of a real absolute axis. So the fix has to be ours.

**Proposed: `model2_analog_curve` + `model2_analog_deadzone`, applied in the OSD's axis read** (the one
place every analog input already passes through,
[libretro_m2_input.cpp:105-125](../../src/osd/libretro_m2/libretro_m2_input.cpp#L105-L125)):

| option | values | why |
|---|---|---|
| `model2_analog_curve` | `linear` (default) / `soft` / `softer` | exponent on the normalised axis, e.g. `sign(x)·|x|^1.5` and `^2.0`. Gives fine control near centre and full lock at the edge — the standard fix for wheel-on-stick |
| `model2_analog_deadzone` | `0..30 %` | stick slop reads as constant drift on an absolute axis; a wheel has none |
| `model2_analog_saturation` | `70..100 %` | reach full lock before the physical edge, which matters more on a short-throw stick than the curve does |

**Scope it to the axes that want it.** A curve is right for `IPT_PADDLE` (steering) and wrong for
`IPT_LIGHTGUN_X/Y` (a pointer must stay linear or aim goes non-linear with position). The per-type
split already exists in the assignment vector, so this is cheap.

**Do not do force feedback or wheel passthrough here.** A real wheel plugged into the frontend already
presents as an absolute axis; `linear` + `0 %` deadzone is exactly right for it, which is why that is
the default.

### ✅ BUILT 2026-08-07/08, and three things here came out differently

[steering-curve.md](steering-curve.md) is the plan as built and the option list in CLAUDE.md is the
authority. What this section got wrong is worth keeping visible:

- **The names and the count.** Three options, not three: `model2_steering_response` (words, never
  gammas), `model2_steering_deadzone`, `model2_steering_range`. "Saturation" became "range" because
  what it caps is lock, not signal.
- **The default is NOT `linear`**, and the reasoning above is exactly why it looked like it should be.
  There is no accuracy ground truth for a control the cabinet does not have, so shipping linear is
  shipping the defect. Default is `Medium` (γ 1.7).
- **The scoping is not "the per-type split in the assignment vector".** It is a **detector**: any field
  whose `type()` is `IPT_PADDLE`/`IPT_PADDLE_V`, asked of the machine at runtime. 30 of 90 GAME
  entries, nothing authored, nothing to keep in sync — and it correctly excludes the gun sets this
  section flags, without a table saying so.
- **A fourth option followed on 2026-08-08 that is not on this list at all: `model2_steering_display`**,
  the read-out bar. Not a feel setting — it is the instrument for setting the other three, and it
  exists because the curve is otherwise invisible to the person judging it.

---

## 3. The rest of the input gaps, ranked

1. ✅ ~~**Lightgun games have no pointer device.**~~ **BUILT 2026-07-27/28 — the whole of
   [lightgun.md](lightgun.md), seven steps.** `retro_set_controller_port_device` is real,
   `SET_CONTROLLER_INFO` offers `RetroPad (Classic)` / `RetroPad (Modern)` / `Light Gun` on all four
   ports, and a port set to a gun aims, reloads and draws a reticle. ⚠️ **`RETRO_DEVICE_POINTER` was
   deliberately not built** (lightgun.md §6): nothing on this machine can drive a touch pointer, so it
   would have shipped untested. It is two entries in `PORT_DEVICES` and a second read path in
   `libretro_m2_gun_device::update()` whenever something can exercise it.
2. **Input descriptors are generic.** They *are* sent
   ([retro_entry.cpp:287](../../src/osd/libretro_m2/retro_entry.cpp#L287)) and deliberately say "Button 1"
   — but MAME knows the field is called "P1 Bat Swing", "Hand Brake", "Machine Gun". Per-game
   descriptors, driven off the loaded machine's `ioport_field` names, would cost little and show up in
   the frontend's remap UI immediately.
3. **No per-game override table.** The P1 decision to avoid one was right at 29 sets of *unknown*
   shape; this survey makes the shape known, and it is 6 tiers, not 32. A small per-tier table with a
   handful of per-set exceptions (§1.3's bat, srallyc's handbrake on `PEDAL3`) is now tractable.
4. **Whether MAME's own remap UI is reachable is unverified**, and it is now *very likely no*.
   `IPT_UI_MENU` still holds L3 whenever `model2_diagnostic_input` is `None`, but the lightgun phase
   established the mechanism that decides this: **this OSD draws no MAME UI at all**, and the reason
   is the same one that killed the crosshair — `libretro_m2_osd_interface::capture_frame()` reads
   pixels straight off `screen->curbitmap()` and never renders the machine's render container
   ([lightgun.md](lightgun.md) §1.5). Anything MAME draws as a container primitive, menu included, has
   nothing drawing it. **Treat the frontend's remapper as the only route** until someone presses L3
   and sees a menu; the `ctrlr`-file escape hatch P1 assumed is closed.

### 3.1 The coverage matrix — measured 2026-07-27, and it replaces a play-through

Prompted by "should I play all 83 sets and check the mappings?". **The answer is no, not for this
question**: the class of fault a play-through is *worst* at finding — a control with no mapping at
all — is answerable statically in two minutes, and the class it is uniquely good at (a mapping that
exists but feels wrong) is a **6-tier** question, not an 83-set one.

Method, reproducible: every `IPT_*` in `model2.cpp` against everything our `configure()` assigns plus
what MAME's own helpers add (`add_directional_assignments` and `add_twin_stick_assignments`,
[assignmenthelper.cpp:180](../../src/osd/modules/input/assignmenthelper.cpp#L180) and
[:389](../../src/osd/modules/input/assignmenthelper.cpp#L389)).

```sh
grep -oE "IPT_[A-Z0-9_]+" src/mame/sega/model2.cpp | sort | uniq -c | sort -rn
awk '/^void joystick_assignment_helper::add_directional_assignments/,/^}/' \
  src/osd/modules/input/assignmenthelper.cpp | grep -oE "IPT_[A-Z0-9_]+" | sort -u
```

**Covered, and no action needed:** `BUTTON1`–`BUTTON6`, `AD_STICK_X/Y`, `PADDLE`, `PEDAL`, `PEDAL2`,
`PEDAL3`, `JOYSTICK_UP/DOWN/LEFT/RIGHT`, `JOYSTICKLEFT_*`/`JOYSTICKRIGHT_*` (twin stick),
`LIGHTGUN_X/Y` (bound by helper; the *device* is the lightgun phase), `COIN1`/`COIN2`,
`START1`/`START2`, `SERVICE1`. `UNUSED`/`UNKNOWN`/`CUSTOM` are not player controls.

**The four gaps — two fixed on 2026-07-27, one won't-fix, one accepted as a limitation.**

| # | gap | set | detail |
|---|---|---|---|
| 1 | ✅ **FIXED** — `IPT_BUTTON9` had no assignment | `daytona` | "VR4 (Green)". The comment said "no Model 2 game has nine buttons"; **daytona has exactly nine** — 1–5 are the gearbox, 6–9 the VR cameras. Bound to **R3**. ⚠️ **Correction, 2026-07-28:** it first landed inside the `model2_service_buttons`-off branch, and step 6 of the lightgun phase took it **out of any branch** when that option was retired — with the test switch on a combo, R3 is free unconditionally, so VR4 no longer depends on an option being off. Verified in a real race twice: R3 moves the camera, L3 held identically is byte-identical to no press, and both recorded digests reproduce under `None` *and* under a set combo. |
| 2 | 🚨 **ACCEPTED, not fixed** — `IPT_BUTTON7`/`8` collide with the pedals | `daytona` | VR2/VR3 are buttons 7/8, which we put on the L2/R2 *trigger thresholds* ([:171-183](../../src/osd/libretro_m2/libretro_m2_input.cpp#L171)) — and daytona also has `IPT_PEDAL`/`PEDAL2` on those same axes. **Flooring the accelerator presses VR3.** |
| 3 | ✅ **FIXED** — `COIN3`/`COIN4`/`START3`/`START4` unreachable | `airwlkrs` | Genuine 4-player cabinet (4 × 3 buttons + 4-way stick) against `MAX_PADS = 2`. Now **`MAX_PADS = 4`** plus a separate **`MAX_GUNS = 2`**, and ports 2/3 added to `INPUT_DESCRIPTORS`. |
| 4 | `IPT_SERVICE2` unassigned | `powsled` | **Won't fix.** §1.4 already puts `powsled` out of scope (networked cabinet). Recorded so the next audit does not re-raise it. |

⚠️ **Gap 2 is not fixable by moving something, which is why it is accepted rather than scheduled.**
Daytona wants **nine buttons, steering and two pedals**; once coin, start and the d-pad are spoken
for, a RetroPad has nothing left. The real fix is the per-set layout table §6 proposes — buttons 1–5
are the gearbox and 6–9 the VR cameras, so a per-set map could place them sanely — and that is a
later phase. **It belongs in the release notes as a known limitation of daytona on a gamepad**, and
the collision is written into the code beside the pedal assignment so it is not rediscovered.

⚠️ **Gap 3's fix is deliberately unverified and must stay that way until the set runs.** `airwlkrs`
is `MACHINE_NOT_WORKING` ([model2.cpp:7674](../../src/mame/sega/model2.cpp#L7674)), is the only set using
any of those four types (one reference each), and is in neither local romset. §7.1's rule for `hotd`
applies verbatim: **wire the port set, do not claim a result from it.** What *was* verified is that it
costs nothing — vf2 at 2500 frames is `16af05bb8d02a9a5` (software) and `55da761fecca5c01` (vulkan),
both documented baselines to the digit, with two extra pad devices now created on every set.

**The reverse direction found one thing too:** we assign `IPT_AD_STICK_Z`
([libretro_m2_input.cpp:218-226](../../src/osd/libretro_m2/libretro_m2_input.cpp#L218)) and **no Model 2
set uses it**. Harmless, but it is dead code with a fallback branch, and anyone reading it will assume
a game needs it.

**What this leaves for an actual play session** — and step 6 has now landed, so this is live: one set
per tier (§1.1) plus the two sets the matrix names — roughly ten games, each with a specific
question, instead of 83 with "does this feel right?". ⚠️ **Two of those questions are already
written and unanswered:** `srallyc` under the two pad layouts (step 6 verified the layout dispatch on
`daytona` only), and `Modern`'s button 5 on R2 firing *with* the accelerator, which
[lightgun.md](lightgun.md) §2.5.1 says is correct behaviour and not a collision to fix.

---

## 4. Display options

- ~~**`model2_internal_res` — `1x` (default) / `2x` / `3x` / `4x`.**~~ ✅ **BUILT 2026-07-28**, exactly
  those four values, and the one blocker in the original scoping was fixed with it — see the box below.
  The renderer half had existed since P4 step 2 as `M2VK_SS=<n>`; this step was the option, the
  stipple, and the measurement. ⚠️ `performance.md` §9 still stands: on Quest 3, 4× is ~16× the
  fragments at ~16 manual texel fetches each, so **2× is likely the ceiling there**. Desktop has room
  (§2a) — vf2 at 2× runs at RetroArch's full 104.58 %.

  **The original scoping listed three shader-side costs. Their status, because two of them turned out
  not to be costs at all:**
  - 🚨 **The `checker` stipple was the real one and it is fixed.** It does not "turn into a grey dither
    as it gets finer" — it turns into a uniform 50 % *blend*, which is a different and worse thing: at
    an even scale a box resolve of a finer checkerboard covers every output pixel. Measured on one
    vcop2 quad at P4 step 2: **78968 px at 1×, 157945 at 2× — the whole hull, 0.000 % of the overlap
    the same colour.** The fix is one integer divide in `poly.frag` — test the parity in **picture**
    pixels, `ivec2(gl_FragCoord.xy) / scale`, so all n² subpixels of a picture pixel share a parity and
    the screen door survives the resolve. That quad now draws **78968 at 2× box, 100.000 % identical to
    1×**. The scale rides in on the push constant block, which grew a third word and both stage flags.
  - **Mip selection is resolution-blind and that is CORRECT, not a bug.** It follows the hardware's
    picture scale, so a supersampled frame gets the mip the arcade board would have picked — edges
    smooth, texture detail deliberately unchanged. It is also why the 3× point runs score 98.97–99.97 %
    bit-identical against 1×: if mip selection tracked the attachment, they could not.
  - **"It must be a runtime option because the A/B harness only means anything at 1×" was already
    satisfied** by `M2VK_SS` being a switch, and the option inherits it: the default is `1x` and every
    `ab.sh` fixture is unaffected.
- **`model2_scanlines` / CRT-ish filters — do not build.** `p1-libretro-core.md` already settled that
  what a frontend does better (scaling, shaders, audio latency, remapping) is the frontend's job.

**Rejected, decided 2026-07-26 — do not re-propose:**

- **Widescreen / FOV widening.** Model 2 projects inside the copro emulation, upstream of our seam, so
  it would mean patching MAME's copro code and **breaking the 30-line upstream-diff budget** that the
  whole mergeability strategy rests on. Not worth reopening the seam decision for.

## 5. Rendering / debug options — cheap, and mostly already built

**Most of this exists as `M2VK_*` environment switches from P3 and just needs promoting to core
options.** That is genuinely small work.

✅ **The first one shipped on 2026-07-28 as `model2_flat_shading`** (`off` / `flat`), alongside
`model2_internal_res`. Two decisions in it worth not undoing:

- **Only `M2VK_FORCE_SOLID=2` is reachable from the option.** Mode 1 clears the textured bit but leaves
  translucency meaning "draw nothing", so the picture comes out with holes in it — that is a *measuring*
  shape, useful because it removes the same polygons from both renderers, and not something a player
  should be able to pick by accident. Mode 1 stays a switch.
- 🚨 **The switch overrides the option, never the other way round.** `ab.sh`'s `MODE=` and `res.sh`'s
  scale arrive in the environment, and a `.opt` file left in a non-default state by an interactive
  session must not be able to rewrite a baseline. Proven by digest rather than argued: with the option
  set to `flat`, `M2VK_FORCE_SOLID=0` gives byte-identical output to no options at all, and the option
  alone gives byte-identical output to `M2VK_FORCE_SOLID=2`. The core also logs a line naming any
  switch that is overriding an option, because "read `[model2] options:` before believing a result" is
  the standing rule and that line would otherwise be able to lie.

| option | already exists as | note |
|---|---|---|
| ~~`model2_textures=off`~~ ✅ **`model2_flat_shading`** | **`M2VK_FORCE_SOLID`** | flat-shaded. Also the step-3 regression guard. **Built 2026-07-28** |
| `model2_3d=off` | **`M2VK_NO_3D`** | the background reference the whole A/B harness rests on. **Deliberately NOT promoted** — a player who picks it sees the 2D layers with a hole between them and reads it as a broken core |
| `model2_filtering` = `trilinear`/`bilinear`/`nearest` | — | `performance.md` §4.4. Per-polygon flags exist |
| `model2_microtexture=off` | — | §4.3; `FLAG_UTEX` already per-polygon |
| ✅ **`model2_flat_luma`** | **`M2VK_FLAT_LUMA`** | force `poly.luma` constant. **Named for what it does.** Model 2 lighting is baked into `luma` by MAME's copro emulation *before* the seam (`p3-hw-geometry.md` drops the geometry half of `model2_lighting.md` for exactly this reason), so there is no lighting stage here to switch off — flattening the per-polygon luma is the whole of it. **Built 2026-07-28**, label **"No Lighting"** |

⚠️ **Every one of these must keep acting on *both* renderers**, which is the standing rule the `M2VK_*`
switches already encode: *when an option removes a feature, it removes it from both paths, so the A/B
harness stays meaningful.* An option that only the Vulkan path honours silently invalidates every
comparison in `ab-baselines.md`.

✅ **`model2_flat_luma` shipped 2026-07-28**, as this row scoped it and asked for directly. Four files,
all `src/osd/libretro_m2/`; no new file, no shader, and **no upstream line** — the diff against
mame0288 is still 30. Live from the options menu. Three things from it worth carrying:

- 🚨 **It costs no upstream line because the seam already hands us a mutable `extra`.** The two
  renderers read the luma from *different places* — MAME's rasteriser from `object.luma` in
  `m2_poly_extra_data`, the record from the polygon — so neither one alone reaches both. `submit()`'s
  parameter went `Extra const &` → `Extra &` and writes `extra.luma`; `p.luma` covers the record. The
  write sits **above the `active()` test** with `force_solid`, because the software rasteriser must
  obey it when nothing is recording — which is the whole of the both-renderers rule in one line.
- **`FLAT_LUMA` is 0xff, and full scale is what makes the result "texture and tint"**: untextured
  polygons land on `0x3f`, the top of their `palcolor` ramp, and textured ones lose the second factor
  of `lumaram[...] * luma / 256`, leaving the texel. ⚠️ It is ×255/256 and **not** ×1 — an 8-bit field
  cannot express unity — so a texel at the very top of the ramp lands one of 64 entries low. Not an
  off-by-one; the fix would be special-casing the multiply in two rasterisers.
- **The both-renderers rule is the check that actually caught anything.** With the switch on, *both*
  whole-run digests move and the two paths still agree to coverage 1.0000 with 0 real interior
  disagreements. A Vulkan-only implementation passes every other check in the harness and fails this
  one loudly.

**Rejected, decided 2026-07-26 — do not re-propose:**

- **Wireframe.** No `wideLines` on this device and `fillModeNonSolid` unverified on MoltenVK, so it is
  a probe plus a second pipeline for a debug view the coverage heatmap in `ppmdiff.py` already covers
  better.

---

## 6. Suggested order

1. ~~**Lightgun device** (§3.1) — biggest user-visible win, 6 sets, self-contained.~~ ✅ **DONE
   2026-07-27/28**, and it grew a seventh step on the way: FBNeo parity for the pad mapping
   ([lightgun.md](lightgun.md) §2.5), which is why `model2_service_buttons` is gone and
   `model2_diagnostic_input` exists. **Not self-contained after all** — it rewrote `m_buttons[]`'s
   indexing, widened `MAX_PADS` to 4, added `MAX_GUNS`, and added the first pixels this core draws
   that are not the emulated hardware's.
2. **Analog curve / deadzone / saturation** (§2) — fixes 26 driving sets, one function, no per-game
   data. ⚠️ **This was the queue head until 2026-07-28 and is now second**, behind the per-game
   layout table ([per-game-input.md](../plan_finished/per-game-input.md), item 4 below, promoted). Both land on
   `daytona`, and they are deliberately **not** folded together: this one changes the *value* on an
   axis, that one changes *which control feeds a button*, and entangling them leaves neither with a
   clean no-op guard. Step 1 of the lightgun phase already built half its instrument: a
   `retrohost` half-axis takes a deflection fraction (`lx+=0.35`), so a curve can be measured against
   a scripted sweep instead of a feeling. ⚠️ **Scope it to `IPT_PADDLE` and keep it off
   `IPT_LIGHTGUN_X/Y`** — §2 says so already, and the gun phase measured why: the gun's sweep is
   linear to the unit where the *stick's* is not, and a curve on a pointer makes aim non-linear with
   position.
3. **Promote the `M2VK_*` switches to core options** (§5) — ⚠️ **PARTLY DONE 2026-07-28, and it was
   not "nearly free".** `model2_internal_res` and `model2_flat_shading` shipped, which is the whole of
   what a player should be offered from that set; the rest are diagnostics and stay switches. The
   plumbing *was* nearly free — a table entry, a getter, a setter each side. The cost was the
   **`checker` stipple**, which had to be fixed before an internal-resolution option could honestly be
   offered (§4's box). Still open under this item: the per-port reticle colour (lightgun.md §3 step 4),
   whose table is already in `m2vk_reticle.cpp` waiting for a reader.
   ✅ **A third option shipped 2026-07-28 that this list did not contain: `model2_transparency`** — the
   `checker` 50 % screen door drawn as a real blend. Asked for directly rather than proposed here, and
   it is the first player option in the set that was **not** a promoted `M2VK_*` switch: the switch
   (`M2VK_BLEND`) was written *for* the option, to keep the override discipline, rather than the option
   being put on top of an existing diagnostic. 🚨 **It is also the one that needed real renderer work**
   — the stream is front-to-back, so blending cannot happen where the polygon stands and the feature is
   a deferred second pass. **[blended-transparency.md](blended-transparency.md)** is the record.
4. **Per-game descriptors + the override table** (§3.2, §3.3) — needs the tier table above, which now
   exists. It is also where daytona's accepted pedal/VR collision (§3.1 gap 2) and FBNeo-style
   per-game default layouts (lightgun.md §6) both get their fix.
   🚨 **PROMOTED TO THE QUEUE HEAD, 2026-07-28, and it has its own plan:
   [per-game-input.md](../plan_finished/per-game-input.md).** `daytona` is the testbed — it is the only set that
   exercises the nine-button case, the collision and all three analog types at once, and it *works*.
   The design turns on a driver fact this list did not know: **`daytona_gearbox_r` latches**
   (`model2.cpp:1610`), so the five gear bits are "select gear i", not "hold gear i" — which makes a
   sequential shifter a held bit with no pulse timing, and lets direct-select and shift-up/down both
   be live at once. The collision is fixed by making buttons 7–9 **ordinary layout slots** rather than
   welding them to the trigger thresholds, which is what §3.1 gap 2 meant by "not fixable by moving
   something".
5. ~~**Internal resolution** (§4) — P5, has its own scoping.~~ ✅ **DONE 2026-07-28 — twice, and only
   the second one counts.** It first came with item 3 on the reasoning that "the option and the switch
   are the same feature". 🚨 **They are not, and that sentence is the error.** `M2VK_SS` renders at n×
   and resolves back **down** to 496×384, because the accuracy harness needs the output to stay at
   MAME's resolution; as a player option that is antialiasing wearing an internal-resolution label,
   and the frontend still received a 496×384 picture. The real thing — draw into the chosen
   framebuffer, hand the frontend **that**, nine absolute Flycast-style resolutions to 2848×2136 —
   went in the same day. **[p5-internal-resolution.md](../plan_finished/p5-internal-resolution.md)** is the record.
   The stipple fix under item 3 was still necessary and is unaffected: it belongs to the `M2VK_SS`
   path, which is unchanged.

Nothing here is a P4 blocker. P4 is depth, decals and coplanar ordering — and it is done.

---

## 7. The lightgun, scoped — ✅ BUILT, and three of these claims were wrong

**Assigned 2026-07-27** as the work after P4. Everything below was read out of the tree, not assumed;
line numbers are at `eaa1355f451`.

🚨 **DONE 2026-07-28, in seven steps. [lightgun.md](lightgun.md) is the plan and the as-built
record — read that, not this.** This section is kept because `lightgun.md` §1 cites it by subsection
number and answers it point by point; deleting it would leave §1 arguing with nothing. **Three of the
subsections below are wrong and are marked in place** — §7.4, §7.6's mechanism, and §7.8's first
check. §7.1, §7.2, §7.3, §7.5 and §7.7 held up: the port shape, the "do not duplicate `PORT_MINMAX`"
rule, the structural finding that the core defaults already look for a `DEVICE_CLASS_LIGHTGUN`
device, the libretro plumbing, and the fact that the harness had to grow a pointer before any of it
could be checked.

### 7.1 The games — 6 sets, and only 3 of them can be verified

| port set | sets | driver state | flags |
|---|---|---|---|
| `vcop` | `vcop` (Rev B), `vcopa` (Rev A) | `model2o_state` | none — **works** |
| `vcop2` | `vcop2` | `model2a_state` | none — **works** |
| `hotd` (`PORT_INCLUDE(vcop2)`) | `hotd`, `hotdo`, `hotdp` | `model2c_state` | **`MACHINE_NOT_WORKING`** |

So wire all three port sets, but **the acceptance runs are `vcop`, `vcopa` and `vcop2`**. A House of
the Dead that does not respond proves nothing about the gun.

### 7.2 The port shape is identical across all three, and the calibration is already MAME's

Four analog ports per set — `P1_X`, `P1_Y`, `P2_X`, `P2_Y` — carrying `IPT_LIGHTGUN_X` / `_Y`, 10-bit
(`0x3ff`), with `PORT_CROSSHAIR`, `PORT_SENSITIVITY(50)`, `PORT_KEYDELTA(13)`/`(10)` and `PORT_PLAYER`.
The trigger is `IPT_BUTTON1` per player on `IN1` bits 0 and 1, named "P1 Trigger" / "P2 Trigger".

**`PORT_MINMAX` differs per set and per axis** — `vcop` X `0x083`–`0x276`, Y `0x024`–`0x1a9`; `vcop2`
X 137–630, Y 36–425; `hotd` X 173–596, Y 87–380 — because it is the cabinet's calibrated on-screen
range. 🚨 **That is per-game calibration data and it already lives in the driver. Do not duplicate it
in the OSD**, and do not "correct" an aim offset by adding a second scale factor on our side; if the
crosshair is wrong, the mapping into MAME's absolute range is wrong.

### 7.3 The structural finding: MAME may bind the gun with no assignment code at all

`src/emu/inpttype.ipp:831` and `:845` already default `IPT_LIGHTGUN_X` / `_Y` to
`GUNCODE_X_INDEXED(n)` **or** `MOUSECODE_X_INDEXED(n)`. So an OSD device of class
`DEVICE_CLASS_LIGHTGUN` exposing absolute `ITEM_ID_XAXIS` / `ITEM_ID_YAXIS` is what those defaults are
already looking for.

⚠️ **The first thing to settle, before writing anything:** our pad device currently supplies the
lightgun defaults itself — `add_directional_assignments` at
[libretro_m2_input.cpp:208](../../src/osd/libretro_m2/libretro_m2_input.cpp#L208), whose comment says in
so many words that this is where "`IPT_LIGHTGUN_X/Y` (the gun games)" get their defaults. **Whether a
per-device default assignment replaces or merely adds to the core default decides the whole shape of
this work.** Get it wrong and either the stick and the gun fight over the same port, or the gun never
binds and the failure looks like a dead device.

### ~~7.4 Two MAME options that are probably load-bearing and are not set today~~ ❌ WRONG — one option, and `-nomouse` stays

🚨 **Corrected 2026-07-27 by reading `input.cpp` ([lightgun.md](lightgun.md) §1.3).** Only
**`-lightgun`** is needed, and it is passed **unconditionally** so that a mid-run device change needs
no reload. `-lightgun_device` does nothing once the class is enabled — `init_autoselect_devices`
early-outs at `if (autoenable_class->enabled()) return;`. And `-nomouse` needed no second look at
all: `input_manager::code_value` returns 0 before reading any item when the class is disabled, so the
`MOUSECODE` half of the core default was already inert. The original text follows.

~~`src/emu/ioport.cpp:1869` runs~~
`init_autoselect_devices({ IPT_LIGHTGUN_X, IPT_LIGHTGUN_Y }, OPTION_LIGHTGUN_DEVICE, "lightgun")` —
MAME **auto-selects which device class drives these types** from `-lightgun_device`, and `-lightgun`
gates lightgun input at all. Neither is in the argument vector at
[retro_entry.cpp:349](../../src/osd/libretro_m2/retro_entry.cpp#L349), and **`-nomouse` is**. Expect to
add `-lightgun` and `-lightgun_device lightgun`; expect `-nomouse` to need a second look, since the
core defaults bind `MOUSECODE` as the alternative.

### 7.5 The libretro side

`retro_set_controller_port_device` is an empty stub at
[retro_entry.cpp:268](../../src/osd/libretro_m2/retro_entry.cpp#L268) **with a comment explaining why it
is empty** — every port is a RetroPad, so `RETRO_DEVICE_ANALOG` and `RETRO_DEVICE_JOYPAD` are the same
device and there is nothing to switch between. That reasoning stops being true here. **Rewrite the
comment, do not delete it**; the next reader needs to know a real choice is now being made.

`RETRO_ENVIRONMENT_SET_CONTROLLER_INFO` is not sent anywhere today. The ids are all in the bundled
header: `SCREEN_X` 13 / `SCREEN_Y` 14 (absolute, `-0x8000`..`0x7fff`), `IS_OFFSCREEN` 15, `TRIGGER` 2,
`RELOAD` 16, `AUX_A`/`B`/`C`, `START` 6, `SELECT` 7 (`libretro.h:411-427`).
`MAX_PADS = 2` ([libretro_m2_input.h:87](../../src/osd/libretro_m2/libretro_m2_input.h#L87)) already
matches the two-player gun cabinets, so nothing there needs widening.

### 7.6 ⚠️ Offscreen reload is the real design question — ✅ settled, and it was **four lines**

🚨 **The conclusion held and the mechanism below is wrong** ([lightgun.md](lightgun.md) §1.4). It was
right that this is the part where the obvious implementation looks correct and the game is
unplayable, and right to demand a scripted check — which found that **`offscreen` alone does not
reload**; the game reloads on *a shot fired* off the screen, so `RELOAD` has to assert a synthetic
trigger too. Two corrections to the reasoning:

1. **The driver already implements offscreen.** `model2_state::lightgun_offscreen_r`
   ([model2.cpp:1136](../../src/mame/sega/model2.cpp#L1136)) sets the player's bit when the axis lands
   within a **5 % border of that port's own `PORT_MINMAX`**. So offscreen is a value *pinned at* the
   edge of the range, not one outside it — **the clamp is the mechanism, not the obstacle** — and the
   implementation is "drive both axes to `ABSOLUTE_MIN`". That is why the step cost four lines and
   touched no upstream file.
2. **The clamp described below is the wrong clamp.** `apply_min_max` clamps against
   `m_minimum`/`m_maximum`, which for an absolute field stay at `ABSOLUTE_MIN`/`ABSOLUTE_MAX`;
   `PORT_MINMAX` arrives as `m_adjmin`/`m_adjmax` and sets the *scale factors*. Same practical result
   — full-scale input maps exactly onto the calibrated window — different reason.

⚠️ **The consequence to expect as a bug report: the outer 5 % of the playfield reads as offscreen.**
That is MAME's behaviour on every frontend and is not ours to compensate for. The original text
follows.

`vcop`'s DSW1:1 is **"Reloading: Normal / Auto Reload"**, so in Normal the player reloads by shooting
off the screen. The two sides disagree about how that is expressed:

- **libretro states it explicitly** — `IS_OFFSCREEN` and a separate `RELOAD` ("forced off-screen
  shot").
- **MAME expects it to fall out of the axis value.** `analog_field` marks lightguns absolute,
  **non-autocentering and non-interpolating** (`ioport.cpp:3619-3622`) and clamps to the port's range
  in `apply_min_max` (`ioport.cpp:3744`) — and the range is `PORT_MINMAX`, the *on-screen* window.

So "point outside the screen" has to survive a clamp that exists to keep the aim on screen. **Do not
guess this.** It is the one part of the job where the obvious implementation can look completely
correct — trigger works, crosshair tracks — and the game is simply unplayable past the first magazine.

### 7.7 There is no way to test this headlessly today, and that is part of the work

`retrohost`'s control script understands **only RetroPad digital buttons and half-axes**
(`retrohost.c:996-1010`). A gun needs an absolute pointer, so **the script format has to grow one** —
a `devnotes/` change, which never ships. Without it there is no scripted verification at all and the
feature rests on eyeballing RetroArch, which this project's own rules distrust for exactly this reason.

### 7.8 What "done" looks like — and the A/B harness is *not* the instrument

Input does not change rendering, so `ab.sh` has nothing to say here and a green A/B table is not
evidence. The checks that mean something:

1. ❌ ~~**MAME's own crosshair lands where the frontend's pointer is**, both players, on `vcop`,
   `vcopa` and `vcop2` — the crosshair is drawn from the same port value the game reads, so it is the
   honest read-out.~~ 🚨 **This check does not exist.** `render_crosshair::draw` adds a quad to the
   screen's *render container* and this OSD reads pixels straight off `screen->curbitmap()`
   ([lightgun.md](lightgun.md) §1.5), so nothing ever draws it — `PORT_CROSSHAIR` on all six gun sets
   is dead weight here. **Replaced by two checks:** step 1's `M2VK_GUN_LOG` read-out of the resolved
   port value (a number, not a picture), and step 4's reticle, which is ours to draw and is
   cross-checked against that number rather than standing alone. ⚠️ `vcopa` could not be run either —
   it is in neither rompath. The port set is wired; nothing is claimed for it.
2. **The trigger fires** and hits what the reticle is on.
3. **Offscreen reload works with DSW1:1 in Normal** (§7.6), which is the check that separates a
   working gun from a plausible one.
4. 🚨 **The pad path still behaves exactly as before.** `libretro_m2_input.cpp` is shared by every
   other set — 26 driving sets, the twin-stick, the buttons tier — and a regression there is far more
   expensive than the feature is worth. A scripted `vf2` run should be unchanged.

Nothing here touches the renderer, the seam or the 30-line upstream diff.

## Self-Paced Timing (`model2_self_throttle`) — default ON since 2026-09-01

Maps to MAME's `-throttle` (the core paces itself) instead of `-nothrottle` (the frontend's limiter
paces it). Reload-gated. **Default was Android-only; it is now on everywhere**, by user call.

The desktop reason is the mirror image of the Android one. A Model 2 screen is
`set_raw(32_MHz_XTAL/2, 656, …, 424, …)` = **57.5242 Hz**, and the core reports exactly that in
`retro_get_system_av_info`. But with vsync to a 60 Hz panel and RetroArch's "Sync to Exact Content
Framerate" off, the frontend calls `retro_run` 60 times a second anyway — the game runs **~4.3 % fast**
and only dynamic audio rate control hides it. The core's own `model2_fps_display` reads wall-clock
`retro_run` rate, so a steady `60.0` on a 57.5 Hz machine is exactly this and is worth recognising.
(Android's fault was the same limiter undershooting: 54 of 57.5 fps on a Quest 3.)

⚠️ **It is wrong for a measurement run** — pacing to wall clock caps `retrohost` at 1× when digest
sweeps run 4×+. `retrohost` therefore pins the option off for itself in `option_value()`;
`M2OPT_model2_self_throttle=enabled` still overrides that. Digests do not move either way (daytona
600 frames: `2a3ccdffd51dcdeb` at both 420 % and 99.95 %) — emulation is deterministic and this only
decides when the OSD sleeps.

⚠️ **A new default does not reach an existing install.** RetroArch persists every chosen value, so a
`config/m2-vk/m2-vk.opt` that already names the key keeps its old value. Change it in the core options
menu, or delete the line with RetroArch closed.


## Fast Sound-Link Timing (`model2_lazy_baud`) — default ON, reload-gated

The core option half of the demand-gated baud clock ([lazy-baud.md](lazy-baud.md)). Replaces the
500 kHz `CLOCK` feeding an i8251's TxC/RxC with a generator that only arms a timer for an edge the UART
can act on. Worth **35–48 % of emulation-thread time** on desktop, and it is the lever that matters on a
CPU-bound device.

Visible on **Model 2 and Model 1** only — those are the two families whose machines carry such a clock
(`model2.cpp` x2, `shared/segam1audio.cpp`). Hidden from System 22/21/23, where it would be a dead entry.

**It exists as an option because `getenv` is dead on Android**, so `M2VK_LAZY_BAUD` cannot reach the
Quest. The switch still overrides the option in both directions on desktop:

| option | switch | result |
|---|---|---|
| (default) | — | on |
| `disabled` | — | off |
| `disabled` | `M2VK_LAZY_BAUD=1` | **on** |
| `enabled` | `M2VK_LAZY_BAUD=0` | **off** |

⚠️ What it changes besides speed: MAME's **device interleave**, not the serial link. The bytes on the
link are identical (verified byte-for-byte through a scripted race); but a few games render a frame
slightly differently. See lazy-baud.md for the control that separates the two.
