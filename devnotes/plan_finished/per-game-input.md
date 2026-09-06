# Per-game input layouts — the plan, with `daytona` as the testbed

**Assigned 2026-07-28.** Line numbers are at HEAD (`bdee1aa5cb5`).

✅ **STATUS 2026-07-30: BUILT — steps 1, 2, 4 and 6 are done; step 3 is open and step 5 is impossible.**
See "Order of work" below for what actually shipped under each, and
**[padmap-tool.md](../reference/padmap-tool.md) for the as-built record** — this file is the plan and is kept for its
reasoning, not as a description of the tree.

🚨 **Two of this plan's design decisions were WITHDRAWN, not merely adjusted, and §3.3 is the important
one.** It proposed a third device type, `RetroPad (Cabinet)`, as the way a player reaches the per-game
row. That is void: the user's direction was that a layout you have to go and select is not a default, so
there is now **one** pad device type and every set resolves to its own row on it. `RetroPad (Classic)` and
`RetroPad (Modern)` were removed in the same change. §4 item 4's plan to build descriptors from
`ioport_field::name()` at runtime is void too — descriptors are sent before there is a machine.

🛑 **And the verification method changed: automated button-press testing is now banned** (CLAUDE.md,
2026-07-30). §5 and §6 below are still the right *questions*; the answers come from a human with a pad.
The reason is in this file's own §5 step 0, which recurred: the first collision test came back clean on
both arms because daytona is already in VR3's view at frame 3500, so the press changed nothing — a
vacuous result that reads exactly like a failure.

The mapping this replaces is documented in [input-map.md](../reference/input-map.md) — read §1 (the constant
RetroPad→MAME table) and §3.2 (daytona's row) before this file.

---

## 1. Why daytona is the testbed and not just the first customer

It is the only set in 90 that exercises everything this work has to get right, at once:

- **Nine buttons** — the only set with a `BUTTON9`, and the reason `MAX` anything ever went past 8.
- **The one accepted collision.** VR2/VR3 are `IPT_BUTTON7`/`8`, which `configure()` hardwires onto
  the L2/R2 **trigger thresholds** ([libretro_m2_input.cpp:374-386](../../src/osd/libretro_m2/libretro_m2_input.cpp#L374-L386))
  — and daytona also has `IPT_PEDAL`/`PEDAL2` on those same two axes. **Flooring the accelerator
  presses VR3.** user-options.md §3.1 gap 2 accepted this rather than fixing it, because it is not
  fixable by *moving* something; it needs the mechanism below.
- **Every analog type in the driver**: `IPT_PADDLE` + `IPT_PEDAL` + `IPT_PEDAL2`.
- **It works.** 4 of its 8 sets are not `MACHINE_NOT_WORKING`, and it is already a
  [roms.md](../reference/roms.md) set that boots. Half the tiers are entirely `MACHINE_NOT_WORKING`, so most
  candidate testbeds cannot verify anything.
- **A whole free D-pad.** daytona declares no `IPT_JOYSTICK_*` at all — IN1's stick bits are the
  gearbox and VR4, IN2 is entirely `IPT_UNUSED`. Four inputs of headroom that no other design gets.
- **The mechanism generalises immediately.** `srallyc` (5 sets) shares the `gears` port set, so one
  table serves **13 of 90** entries before anything bespoke is written.

## 1.1 🚨 The fact the whole design rests on: the gearbox latches in the driver

[model2.cpp:1610-1625](../../src/mame/sega/model2.cpp#L1610-L1625):

```cpp
ioport_value model2_state::daytona_gearbox_r()
{
	u8 res = m_gears.read_safe(0);
	const u8 gearvalue[5] = { 0, 2, 1, 6, 5 };
	for (int i = 0; i < 5; i++)
		if (BIT(res, i)) { m_gearsel = i; return gearvalue[i]; }
	return gearvalue[m_gearsel];          // nothing pressed: the last gear holds
}
```

The five `GEARS` bits are **"select gear i", not "hold gear i"**. Consequences, both load-bearing:

1. A **sequential shifter needs no protocol invention.** The OSD keeps a gear index and holds the bit
   for whatever gear it believes it is in. Holding is *idempotent* — the driver re-latches the same
   value every read — so it survives a dropped frame, a mid-run device change and a resync. **Do not
   build a pulse**; a pulse needs a frame counter and can be missed.
2. **Direct select and sequential select can coexist**, because both are just writes to one index.
   That is what makes §2 able to offer both at once rather than as an option.

---

## 2. The map

One RetroPad, port 0. daytona is a one-player cabinet — IN2 is entirely unused and there is no
`START2` — so port 1 has nothing to do.

| RetroPad | daytona | MAME | how |
|---|---|---|---|
| **Left stick X** | Steering | `IPT_PADDLE` | unchanged |
| **R2** | Accelerator | `IPT_PEDAL` | unchanged |
| **L2** | Brake | `IPT_PEDAL2` | unchanged |
| **B** | **GEAR 1** | `BUTTON2` | gear index ← 1 |
| **A** | **GEAR 2** | `BUTTON3` | gear index ← 2 |
| **Y** | **GEAR 3** | `BUTTON4` | gear index ← 3 |
| **X** | **GEAR 4** | `BUTTON5` | gear index ← 4 |
| **R** | **Shift Up** | — | gear index + 1, clamped to 4 |
| **L** | **Shift Down** | — | gear index − 1, clamped to 0 (**N**) |
| **D-pad Up** | **VR1 (Red)** | `BUTTON6` | |
| **D-pad Right** | **VR2 (Blue)** | `BUTTON7` | **off the brake axis** |
| **D-pad Down** | **VR3 (Yellow)** | `BUTTON8` | **off the accelerator axis** |
| **D-pad Left** | **VR4 (Green)** | `BUTTON9` | |
| Select | Coin | `COIN1` | unchanged |
| Start | Start | `START1` | unchanged |
| L3 | service coin | `IPT_SERVICE1` | unchanged |
| the diagnostic combo | test switch | `IPT_SERVICE` | unchanged |
| **R3** | *free* | — | deliberately left free |

**Both shifters are live at the same time.** ABXY sets the gear absolutely; L/R steps it. They write
one index (§1.1), so there is no mode to choose and no way for them to disagree.

### 2.1 ⚠️ MAME's gear numbering is offset by one and this is the trap

`GEAR N` is `IPT_BUTTON1`, so **`GEAR 1` is `IPT_BUTTON2`**. The map above is deliberately written in
the *player's* numbering — B is "gear 1" — which means the layout row points B at MAME button **2**.
That is the first row in the codebase where the pad's B is not MAME's button 1, and it is exactly what
a per-set layout is for; it is called out here because a reader checking the row against
[input-map.md](../reference/input-map.md) §1.1 will otherwise read it as an off-by-one.

**Neutral is reachable only by shifting down from gear 1** (L at index 1 → 0). That is deliberate:
neutral exists on the cabinet and the game starts in it, but no player wants a face button that stalls
them. **Open question for the testbed**: whether daytona requires leaving N to move at all, and
whether the gear index should therefore *start* at 0 or at 1. Answer it in the game, not from the
driver.

### 2.2 The D-pad assignment is the one arbitrary choice in the map

The cabinet has VR1–VR4 in a **row**, left to right, and a D-pad is not a row. Clockwise from Up
(VR1/VR2/VR3/VR4 = Up/Right/Down/Left) is chosen because it is memorable, not because it is faithful.
Left-to-right (Left/Up/Down/Right, or Left/Down/Up/Right) is equally defensible. **Settle it on the
testbed by pressing them and reading the in-game camera**, and record which was picked and why.

### 2.3 ⚠️ Pointing a numbered button at a D-pad control is only safe on sets with no joystick

The D-pad slots keep their `ITEM_ID_HAT1*` items and their `IPT_JOYSTICK_*` assignments from
`add_directional_assignments`. A layout row that *also* points a numbered button at D-pad Up makes one
pad control feed two items — harmless on daytona and `srallyc`, which declare no `IPT_JOYSTICK_*` at
all, and a genuine double-press on any set that does.

**Rule for the table: a row may name a D-pad control as a numbered-button source only if the set
declares no `IPT_JOYSTICK_*`.** Both rows this step writes qualify. Check it per row; there is no way
to check it automatically at this seam.

---

## 3. The mechanism

### 3.1 The structural change that unblocks everything: buttons 7–9 become ordinary slots

The collision exists **only** because buttons 7 and 8 are not layout-table entries — they are welded
to `triggeritems[]` in `configure()`, so no layout can move them. Likewise button 9 is welded to
`BUTTON_R3` in `FIXED_BUTTONS`.

Fix: **`NUMBERED_BUTTONS` 6 → 9**, and the trigger threshold becomes just another *source* a layout
row may name, alongside the RetroPad ids. Then `IPT_BUTTON1..9` are nine ordinary slots and a layout
is one row of nine sources.

- **Item ids shift**: numbered buttons take `ITEM_ID_BUTTON1..9`, L3/R3 move to `BUTTON10`/`11`, the
  diagnostic combo to `BUTTON12`. ✅ **The existing safety argument survives the move** — it was that
  `IPT_BUTTON11`'s default in `inpttype.ipp` is `KEYCODE_M`, a keyboard code, and this OSD registers
  no keyboard. `IPT_BUTTON12`'s default is `KEYCODE_COMMA`
  ([inpttype.ipp:45](../../src/emu/inpttype.ipp#L45)), so the same argument holds verbatim. Player 2's
  defaults for all of these are empty sequences.
- **`FIXED_BUTTONS` loses its button-9 special case** and the `IPT_BUTTON9` → `BUTTON_R3` assignment
  goes with it. ⚠️ **That binding is the one thing in this refactor that is currently *verified
  working*** — daytona's VR4 was measured in a real race twice (user-options.md §3.1 gap 1) — so the
  no-op check in step 1 has to cover it specifically.
- **It stays mid-run switchable**, which is the property worth protecting. The layout table is read
  only in `update()`, never in `configure()`; no assignment changes, so no content reload. Same reason
  Classic/Modern is free today ([libretro_m2_input.cpp:41-42](../../src/osd/libretro_m2/libretro_m2_input.cpp#L41-L42)).

### 3.2 Where the table lives

Keyed on `machine.system().name` with a fallback to `machine.system().parent`, looked up once in
`input_init`. **One row covers all 8 daytona entries** (7 clones all name `daytona` as parent).

### 3.3 How the player reaches it

A third entry in `PORT_DEVICES` — **`RetroPad (Cabinet)`** — rather than silently overriding Classic.
`SET_CONTROLLER_INFO` is sent from `retro_load_game` **after** the system name is known
([retro_entry.cpp:587](../../src/osd/libretro_m2/retro_entry.cpp#L587)), so the list can be per-game:
offer Cabinet **first** on the 13 sets that have a row, and leave the other 77 with exactly today's
two entries and today's ordering. An unaware frontend takes the first entry, which is what makes this
the default without the core having to force a device on the frontend.

### 3.4 The gear state

An index `0..4` on `libretro_m2_pad_device`, edge-triggered on the shift pair, written absolutely by
the gear buttons, and driving `m_buttons[]` for the numbered slot of `IPT_BUTTON1 + index` **held
continuously** (§1.1). Reset with the device. ~15 lines, all in `update()`.

---

## 4. Order of work

1. ~~**Widen the numbered buttons to 9.**~~ ✅ **DONE 2026-07-29.** Built as scoped: nine slots, nine
   identical items, the trigger thresholds as tagged layout sources (`SOURCE_L2_AXIS`/`SOURCE_R2_AXIS`,
   above every RetroPad id so the combo's id comparison cannot confuse them), item ids shifted
   (L3/R3 → `BUTTON10/11`, combo → `BUTTON12`), `FIXED_BUTTONS` reduced to Select/Start/D-pad/L3/R3,
   and §5.1's descriptor inversion fixed. `trigger_button_get_state` is gone; the threshold lives in
   `read_source()`.
   - ⚠️ **One thing the step description did not anticipate: `update()` had to be reordered.** The
     numbered-button read now runs **after** the axes, because a threshold source reads `m_axes`. Both
     orders see the same frontend snapshot, so it is safe — but a future edit that moves the button
     loop back above the axes would silently make slots 7/8 read the previous frame.
   - ⚠️ **`IPT_BUTTON9` must be assigned exactly once.** The old explicit R3 assignment had to go, not
     merely be left alone: the layout carries button 9 now, and keeping both binds one type to two
     items.
   - 🚨 **The first verification pass was VACUOUS and reported eight clean equalities** — every
     daytona press was at frames 1100–1750, where the game is on its attract screens and no button
     does anything. The tell was that `Classic` and `Modern` produced the *same* digest. §6's warning
     is what caught it, and §5 now carries the discrimination check as a required step.
2. ~~**The layout table, the `Cabinet` device type, and per-game `CONTROLLER_INFO`.**~~ ✅ **DONE
   2026-07-30, and the `Cabinet` device type was WITHDRAWN rather than built.** §3.3's whole delivery
   mechanism is void: the user's direction was that a per-game layout a player has to *select* is not a
   default, so there is **one pad device type** (`RetroPad`) and the row is simply what it does.
   `RetroPad (Classic)` and `RetroPad (Modern)` went with it — with rows in place they were choices
   between two wrong answers — and so did the second `retro_controller_info` array and
   `has_cabinet_layout()`. The daytona row shipped exactly as this step scoped it (ABXY = gears 1–4 as
   `BUTTON2..5`, VR1–4 clockwise from Up on the free D-pad), and **the collision is dead, measured with a
   working negative control**. See [padmap-tool.md](../reference/padmap-tool.md).
3. **The synthesized gear index** and L/R. Both shifters live at once. ⚠️ **Still open**, and §2.1's open
   question is now half-answered: **daytona does not require leaving N to move** — the car reaches
   167 km/h with GEAR N latched — so the gear index may start at 0 safely.
4. ~~**Per-game input descriptors**~~ ✅ **DONE 2026-07-30, and NOT the way this step scoped it.** It said
   "driven off the loaded machine's `ioport_field` names", which cannot work: descriptors are sent from
   `retro_load_game()`, **before there is a machine**. They come from the layout row instead, whose labels
   are seeded from those same `PORT_NAME` strings at *authoring* time by the editor — and, critically,
   are **derived from the row's own button assignments by the generator**, so the labels and the mapping
   cannot disagree. That is what makes §5.1's inversion bug structurally impossible rather than merely
   fixed. daytona's remap screen now reads GEAR 1–4, VR1 (Red)…VR4 (Green), Brake, Accelerator, Steering.
5. ~~**The `srallyc` row**~~ 🛑 **NOT DONE, and it cannot be: `srallyc` is entirely `MACHINE_NOT_WORKING`
   — all five entries.** So it could never have been the "proof the table generalises", because nothing
   about it is verifiable in game. **`motoraid` took that role** (a working driving set), and the table
   generalised to **12 port sets covering all 28 working GAME entries** rather than the 13-of-90 this
   plan estimated.
6. ~~**Docs pass**~~ ✅ **DONE 2026-07-30** — worklog, this file, a new
   [padmap-tool.md](../reference/padmap-tool.md), `input-map.md`, `README.md` and CLAUDE.md.

---

## 5. Verification — `ab.sh` has nothing to say here

Input does not change rendering, so a green A/B table is not evidence. It is still the **no-op
guard**, and that is the one thing it is for.

0. 🚨 **BEFORE any of the below: prove the script reaches the machine.** A daytona run whose presses
   land on the attract screens produces perfectly clean, perfectly meaningless equalities — that is
   what happened on the first pass at step 1. **The check is that `Classic` and `Modern` DISAGREE**,
   which they must, because Classic puts button 5 on `R` and button 6 on `L` while Modern puts button
   6 on `R` and names `L` nowhere at all. If those two agree, nothing else in the run means anything.
   A coin at 600 and Start pulsed every 300 frames to 2400 puts daytona on the track at about frame
   **3400**; press from 3500 on.
1. **The no-op guard.** `ab.sh vf2 2500` must reproduce the `ab-baselines.md` digests byte-exactly
   after every step. Nothing in this work may touch a pixel. ⚠️ **The pair this line used to name —
   `16af05bb8d02a9a5` / `55da761fecca5c01` — is pre-startup-fix and dead**; at the current tree it is
   `9c20f1fac9d9fe92` (software) / `de94f44a06151f71` (vulkan), background `c3aaa56633c1c4f7`.
   **Read the baseline file, never this line.**
2. 🚨 **The collision test, and it is the exit criterion for step 2.** With the accelerator held to
   full, **VR3 must not fire**. ⚠️ **Measure it in daytona's own INPUT TEST screen, not from
   behaviour** — the lightgun phase's step 6 established this the hard way: neither obvious read-out
   can see a consumed input, a held coin registers *nothing at all*, and a probe scripted as a long
   hold reads exactly like a broken input. The INPUT TEST screen is reached through
   `model2_diagnostic_input`, which is what that option is for.
3. **The negative control is the device type, not the press.** The identical script under
   `RetroPad (Classic)` must still show VR3 firing with the accelerator. Without that, "VR3 stopped
   firing" is equally consistent with the button having been dropped on the floor — which is the
   failure mode `add_assignment()` produces silently when an assignment names an item the device never
   added.
4. **The gear state.** A scripted shift sequence (`R` × 4, `L` × 5, `X`, `B`) read off the in-game gear
   indicator. §2.1's open question — whether the index should start at N or at 1 — is answered here.
5. **VR4 on the new arrangement**, in a real race, because it is the binding this refactor is most
   able to break silently (§3.1).
6. **daytona under `Classic` must be byte-identical to today** at every step, since Classic's row is
   unchanged by construction. That is the cheap regression check and it should be run first.

---

## 6. Traps carried in from earlier phases — read before scripting a probe

- **Getting daytona into a race is not obvious**, and a run that fails to measures nothing while every
  log line looks healthy. The `vcop` lesson (lightgun step 2): a coin at 600 and Start pulsed at
  900/1200/1500/1800 is the shape that works; Start alone at 700 sits on the title with `CREDIT 1`.
  **Screenshot the last frame before believing a null result.**
- **daytona needs the second rompath** ([roms.md](../reference/roms.md), gotcha 2) — `devnotes/roms` alone does not
  have it. For `retrohost` that means `M2_SYSTEM_DIR=<dir>` where `<dir>/model2` reaches
  `Polydiver/roms`.
- **`retrohost`'s script format already covers this work** — digital buttons and half-axes with a
  deflection fraction (`lx+=0.35`), added at lightgun step 1. Nothing new is needed, unlike the gun,
  which had to grow a pointer first.
- 🚨 **…except a PARTIAL TRIGGER PULL, which it cannot express, and step 2's collision test needs one.**
  The control table has no analogue trigger, and `parse_script` refuses a value on a digital control
  (`control 'l2' takes no value`), so `l2`/`r2` always arrive saturated — `update()` substitutes 32767
  for a digital press. Measured at step 1. So "the accelerator is held to full" is scriptable and
  "the accelerator is at 40 %" is not, and the threshold's *position* is currently untestable from the
  harness. Adding an analogue trigger control to `CONTROLS[]` is the fix if step 2 needs it.
- **Give every run its own `M2_SAVE_DIR`.** daytona has NVRAM and a gearbox latch; two runs sharing a
  save directory start from different emulated state and it reads exactly like an input bug.
- **`env $e` does not word-split in zsh.** Pass env assignments as separate words, and read the core's
  own `[model2] options:` line before believing any result.

---

## 7. Out of scope, deliberately

- **The analog steering curve.** It lands on the same game and is the obvious thing to fold in, and it
  should not be: it is a change to the *value* on an axis, this is a change to *which control* feeds a
  button, and entangling them means neither has a clean no-op guard. It is the step after this one
  ([user-options.md](../reference/user-options.md) §2, §6 item 1).
- **The other 30 port sets.** Only `daytona` and `srallyc` get rows here. The table exists so the rest
  can be added one row at a time, with evidence, rather than in a sweep.
- **Force feedback, wheel passthrough.** A real wheel already presents as an absolute axis and wants
  none of this.
