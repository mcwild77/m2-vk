# The per-game input map — what every Model 2 set declares, and which RetroPad control produces it

**Audited 2026-07-28** against the tree at HEAD (`bdee1aa5cb5` + the uncommitted transparency /
quit-crash work, neither of which touches input). Sources, all read rather than remembered:

- `src/mame/sega/model2.cpp` lines 1632–2431 — every `INPUT_PORTS_START` and every `PORT_NAME`.
- `src/osd/libretro_m2/libretro_m2_input.cpp` — `BUTTON_LAYOUTS[]`, `FIXED_BUTTONS[]`, `configure()`.
- `src/osd/modules/input/assignmenthelper.cpp` — `add_directional_assignments`,
  `choose_primary_stick`, `add_twin_stick_assignments` (where the analog types get their defaults).
- `src/osd/libretro_m2/retro_entry.cpp` — `INPUT_DESCRIPTORS[]`, `PORT_DEVICES[]`.

This is the *mapping* companion to [user-options.md](user-options.md), which is the *proposal*. That
file groups 32 port sets into 6 tiers and argues about what to build; this one says, for each game,
what button does what. **§5 lists four things the audit found that are not in any other doc**, one of
which is a live bug.

⚠️ **Three counts in [user-options.md](user-options.md) §1 are wrong and are corrected here.** It says
**83** GAME entries; the tree has **90** (`grep -cE '^GAME\(|^GAMEL\(' src/mame/sega/model2.cpp`), of
which **62** are `MACHINE_NOT_WORKING` and **28** are not. Its per-tier sizes and its "not flagged"
column are short by the same 7. The **32 port sets** and the **6 tiers** are right, and nothing that
was decided on the strength of that survey changes — the shape was correct, the arithmetic was not.

---

## 1. ~~The constant part — RetroPad → MAME, identical on every set~~ 🛑 NO LONGER CONSTANT (2026-07-30)

🚨 **There is no constant mapping layer any more, and the three device types this section describes are
gone.** Every port set has its own row in `src/osd/libretro_m2/input_layouts.json`, authored in
[devnotes/tools/padmap.html](../tools/padmap.html) and compiled into `input_layouts.ipp`; there is **one** pad
device type, `RetroPad`, and the row is what it does. `RetroPad (Classic)`, `RetroPad (Modern)` and the
never-shipped `RetroPad (Cabinet)` were all removed together. **[padmap-tool.md](padmap-tool.md) is the
current record.**

**§1 below is still worth reading for two reasons** and is kept unchanged for them: the `generic` fallback
row — what the 62 unauthored, all-`MACHINE_NOT_WORKING` entries still play as — is byte-for-byte §1.1's
Classic column, and §3's per-game transcription is where the authored labels came from. Read §1.1 as "the
fallback", not "the default".

This table is the whole of the mapping layer. Everything in §3 is just "which MAME button number is
this game's Punch", read off it.

### 1.1 `RetroPad (Classic)` — ~~the default~~ **now the `generic` fallback row**

| RetroPad | MAME | note |
|---|---|---|
| **B** | Button 1 | |
| **A** | Button 2 | |
| **Y** | Button 3 | |
| **X** | Button 4 | |
| **R** | Button 5 | 🚨 the frontend descriptor says L — see §5.1 |
| **L** | Button 6 | 🚨 the frontend descriptor says R — see §5.1 |
| **L2** (axis ≤ −16384) | Button 7 | *and* the L2 **axis** is `IPT_PEDAL2` (brake). Both fire. |
| **R2** (axis ≤ −16384) | Button 8 | *and* the R2 **axis** is `IPT_PEDAL` (accelerator). Both fire. |
| **Select** | `COIN1` / `COIN2` / `COIN3` / `COIN4` | by port index, via `JOYCODE_SELECT_INDEXED` |
| **Start** | `START1`…`START4` | by port index, via `JOYCODE_START_INDEXED` |
| **D-pad** | `IPT_JOYSTICK_UP/DOWN/LEFT/RIGHT` | OR'd with the left stick's switch codes |
| **L3** | `IPT_SERVICE1` (service coin) when `model2_diagnostic_input` ≠ `None`; `IPT_UI_MENU` when it is `None` | the UI menu is inert — this core draws no MAME UI |
| **R3** | Button 9 | exists for daytona's VR4 and nothing else |
| **the diagnostic combo** | `IPT_SERVICE` (the cabinet test switch) | synthetic `ITEM_ID_BUTTON11`; `None` by default |
| **Left stick X** | `IPT_PADDLE`, `IPT_AD_STICK_X`, `IPT_LIGHTGUN_X`, `IPT_DIAL`, `IPT_POSITIONAL`, `IPT_TRACKBALL_X` | one binding serves all six; only one type exists per set |
| **Left stick Y** | `IPT_AD_STICK_Y`, `IPT_LIGHTGUN_Y`, `IPT_PADDLE_V`, … | same |
| **Right stick** | `IPT_AD_STICK_Z`, and the twin sticks | **no Model 2 set declares `AD_STICK_Z`** — dead binding, see §5.4 |
| **R (Classic) / R2 (Modern)** | also `IPT_PEDAL3` as an *increment* | srallyc's hand brake only; ramps by `KEYDELTA` while held |

### 1.2 `RetroPad (Modern)` — the only difference is two buttons

| | Button 5 | Button 6 | Button on **L** |
|---|---|---|---|
| Classic | **R** | **L** | Button 6 |
| Modern | **R2** | **R** | *nothing* |

⚠️ Under Modern, **R2 is Button 5, Button 8 and the accelerator at once**. The code accepts this
deliberately (`libretro_m2_input.cpp:52-53`): the sets that *have* a button 5 are the driving sets, so
a player who picked Modern on one of those picked it knowing what is under that finger.

### 1.3 `Light Gun` — only `vcop`, `vcop2`, `hotd` declare anything for it

| libretro | MAME |
|---|---|
| Screen X / Y | `IPT_LIGHTGUN_X` / `_Y`, straight onto that port's own `PORT_MINMAX` — no scaling of ours |
| Trigger | Button 1 |
| Aux A | Button 2 |
| Aux B | Button 3 |
| Is Offscreen | both axes pinned to `ABSOLUTE_MIN`, which is inside the driver's own 5 % border |
| Reload | the above **plus** a synthetic trigger — the game reloads on a *shot fired* off screen, not on being pointed away |

A gun port keeps its RetroPad buttons: only the **two primary stick axes** are silenced, so coin,
start, L3 and the diagnostic combo all still work on a gun cabinet.

---

## 2. Tier sizes, corrected

| tier | port sets | GAME entries | not `MACHINE_NOT_WORKING` |
|---|---|---|---|
| buttons | 9 | 33 | 17 |
| driving | 7 | 29 | 5 |
| adstick | 7 | 7 | 3 |
| exotic | 5 | 11 | 0 |
| lightgun | 3 | 6 | 3 |
| twinstick | 1 | 4 | 0 |
| **total** | **32** | **90** | **28** |

(`desert` is counted under **adstick** to match user-options.md §1.1, though it declares `IPT_PADDLE`
+ `IPT_PEDAL` like a driving set and only its *brake* is an ad-stick. See §5.2 — that brake is the
audit's second finding.)

---

## 3. The per-game map

Grouped by port set. "sets" is every GAME entry sharing it. RetroPad controls are **Classic**; where
Modern differs the cell says so.

### 3.1 buttons — digital only

| port set | sets | control | MAME | RetroPad |
|---|---|---|---|---|
| **vf2** | vf2, vf2a, vf2b, vf2o, fvipers, fvipersa, fvipersb, lastbrnx, lastbrnxj, lastbrnxu, hpyagu98 | Punch / Kick / Guard | B1 / B2 / B3 | **B / A / Y** |
| | | movement | 4-way stick | D-pad or left stick |
| **doa** | doa, doab, doaa, doaab, doaae | Hold / Punch / Kick | B1 / B2 / B3 | **B / A / Y** |
| **dynamcop** | dynamcop, dynamcopb, dynamcopc, dyndeka2, dyndeka2b | Punch / Kick / Jump | B1 / B2 / B3 | **B / A / Y** |
| **schamp** | schamp, sfight | Punch / Kick / Barrier | B1 / B2 / B3 | **B / A / Y** |
| **vstriker** | vstriker, vstrikero | Short Pass / Long Pass / Shoot | B1 / B2 / B3 | **B / A / Y** |
| **zerogun** | zerogun, zerogunj, zeroguna, zerogunaj | two buttons only (B3/B4 cleared) | B1 / B2 | **B / A** |
| **pltkids** | pltkids, pltkidsa | two buttons only | B1 / B2 | **B / A** |
| **airwlkrs** | airwlkrs | 3 buttons + 4-way stick, **4 players** | B1–B3 per player | B / A / Y on **each of ports 0–3** |
| **model2crx** | rascot2 | generic B1–B4 + stick | B1–B4 | B / A / Y / X |

⚠️ **vstriker's bit order is not its button order** and the driver says so in a comment: IN1 bit 0 is
`IPT_BUTTON2` (Long Pass), bit 1 is `BUTTON3` (Shoot), bit 2 is `BUTTON1` (Short Pass). The table
above is by **button number**, which is what our layout keys on, so it is what the pad actually does.

`rascot2` (betting terminal) and `airwlkrs` are both out of any support commitment — see
user-options.md §1.4 and §3.1 gap 3.

### 3.2 driving

All of these share: **left stick X = steering** (`IPT_PADDLE`), **R2 = accelerator** (`IPT_PEDAL`),
**L2 = brake** (`IPT_PEDAL2`).

| port set | sets | control | MAME | RetroPad |
|---|---|---|---|---|
| **daytona** | daytona, daytona93, daytonas, daytonase, daytonat, daytonata, daytonam, daytonagtx | GEAR N / 1 / 2 / 3 / 4 | B1 / B2 / B3 / B4 / B5 | **B / A / Y / X / R** (Modern: R2) |
| | | **VR1 (Red)** | B6 | **L** (Modern: R) |
| | | **VR2 (Blue)** | B7 | **L2** — shares the brake axis |
| | | **VR3 (Yellow)** | B8 | **R2** — shares the accelerator axis |
| | | **VR4 (Green)** | B9 | **R3** |
| **srallyc** | srallyc, srallycb, srallycc, srallycdx, srallycdxa | GEAR N…4 | B1–B5 | **B / A / Y / X / R** |
| | | **VR** | B6 | **L** |
| | | **Hand Brake** | `IPT_PEDAL3` | **R** (Modern: R2) — shares the GEAR 4 slot, by design |
| **indy500** | indy500, indy500d, indy500to, stcc, stcca, stccb, stcco | Shift Up / Shift Down | B1 / B2 | **B / A** |
| | | View 1 (Zoom In) / View 2 (Zoom Out) | B3 / B4 | **Y / X** |
| **sgt24h** | sgt24h | as indy500, but **View 2 removed** and both pedals `PORT_REVERSE` | B1 / B2 / B3 | B / A / Y |
| **overrev** | overrev, overrevb, overrevba | as indy500, View 1 / View 2, both pedals `PORT_REVERSE` | B1–B4 | B / A / Y / X |
| **manxtt** | manxtt, manxttc, manxttdx | Shift Up / Shift Down | B1 / B2 | **B / A** |
| | | lean (**BANK**, `PORT_REVERSE`) | `IPT_PADDLE` | **left stick X** |
| | | throttle / brake | `PEDAL` / `PEDAL2` | **R2 / L2** |
| | | "Start / VR" | `START1` | **Start** |
| **motoraid** | motoraid, motoraiddx | Punch / Kick (same bits manxtt uses for shifting) | B1 / B2 | **B / A** |
| | | bank, throttle, brake | as manxtt | as manxtt |

**Daytona is the set the RetroPad genuinely cannot hold** — nine buttons, steering and two pedals —
and the VR2/VR3-on-the-pedals collision is accepted rather than scheduled (user-options.md §3.1 gap 2).
Flooring the accelerator presses VR3.

### 3.3 adstick

| port set | sets | control | MAME | RetroPad |
|---|---|---|---|---|
| **skytargt** | skytargt | flight stick (`STICKX` `PORT_REVERSE`, `STICKY`) | `AD_STICK_X` / `_Y` | **left stick** |
| | | Machine Gun / Missile / View Change | B1 / B2 / B3 | **B / A / Y** |
| **rchase2** | rchase2 | aim, per player, `PORT_REVERSE` + `CENTERDELTA(0)` | `AD_STICK_X/_Y` | **left stick**, each on its own port |
| | | trigger | B1 per player | **B** |
| **rchase2a** | rchase2a | as rchase2 but full `0x00..0xff` range and not reversed | | |
| **gunblade** | gunblade | as rchase2, different `PORT_MINMAX`, not reversed | | |
| **bel** | bel | gunblade + **Missile** per player | B2 | **A** |
| | | ⚠️ test switch and service coin are **swapped** in IN0 vs every other set | | |
| **waverunr** | waverunr | Handle Bar **and** Roll | both `AD_STICK_X` | **left stick X drives both** — §5.3 |
| | | Throttle Lever **and** Pitch (both `PORT_REVERSE`) | both `AD_STICK_Y` | **left stick Y drives both** — §5.3 |
| | | View | B1 | **B** |
| **desert** | desert | steering | `IPT_PADDLE` | **left stick X** |
| | | accelerator | `IPT_PEDAL` | **R2** |
| | | **brake** | `IPT_AD_STICK_Y` — *not* a pedal | **left stick Y** — §5.2 |
| | | Machine Gun / Cannon / Shift (`PORT_TOGGLE`) | B1 / B2 / B3 | **B / A / Y** |
| | | VR1 (Blue) / VR2 (Green) / VR3 (Red) | B4 / B5 / B6 | **X / R / L** |

### 3.4 lightgun

| port set | sets | `PORT_MINMAX` (X, Y — player 1) | controls |
|---|---|---|---|
| **vcop** | vcop, vcopa | `0x083–0x276`, `0x024–0x1a9` | trigger = B1 per player; DSW1:1 **Reloading: Normal / Auto Reload**; DSW1:2 Enemy Character |
| **vcop2** | vcop2 | `137–630`, `36–425` | trigger = B1 per player |
| **hotd** | hotd, hotdo, hotdp | `173–596`, `87–380` | `PORT_INCLUDE(vcop2)`; all three `MACHINE_NOT_WORKING` |

Player 2's window differs from player 1's on every set — it is per-cabinet calibration and lives in
the driver. **Nothing on our side scales it**, and nothing should (lightgun.md §5).

⚠️ On a port left as a **pad**, these still play: the trigger is `IPT_BUTTON1` → **B**, and aim is
`IPT_LIGHTGUN_X/Y` → **left stick**. That is the pre-lightgun behaviour and it is still reachable.

### 3.5 twinstick

| port set | sets | control | MAME | RetroPad |
|---|---|---|---|---|
| **von** | von, vonu, vonj, vonr | left stick 4-way | `JOYSTICKLEFT_*` | **left analog stick**, D-pad as digital fallback |
| | | right stick 4-way | `JOYSTICKRIGHT_*` | **right analog stick**, face diamond as digital fallback |
| | | Left Shot / Left Dash | B1 / B2 | **B / A** |
| | | Right Shot / Right Dash | B3 / B4 | **Y / X** |

🚨 **The face diamond is doing two jobs on `von`.** `add_twin_stick_assignments` is handed the four
face buttons as the *digital fallback* for the right stick, and the same four are Buttons 1–4, i.e.
all four shots and dashes. One press does both. [inferred from `configure()`; not measured — `von` is
`MACHINE_NOT_WORKING` and was never run.] Also: the driver declares **one** player's twin sticks
(IN1 = left stick, IN2 = right stick, both named "P1"), so **port 1's pad does nothing on von**.

### 3.6 exotic — every set here is `MACHINE_NOT_WORKING`

| port set | sets | the real cabinet | control | MAME | RetroPad |
|---|---|---|---|---|---|
| **dynabb** | dynabb, dynabb97 | bat you pull and snap | P1 Bat Swing | `IPT_PEDAL` (P1) | **R2** |
| | | | P2 Bat Swing | `IPT_PEDAL2` (P2) | **L2 of pad 2** — asymmetric, §5.5 |
| **topskatr** | topskatr, topskatru, topskatruo, topskatrj | skateboard deck | Curving **and** Slide | both `AD_STICK_X` | **left stick X drives both** — §5.3 |
| | | | Jump Front / Jump Tail | B1 / B2 | **B / A** |
| | | | Select Left / Select Right | B3 / B4 | **Y / X** |
| **segawski** | segawski | ski platform | Slide | `AD_STICK_X` | **left stick X** |
| | | | Pitch Left / Pitch Right | B1 / B2 | **B / A** |
| | | | Select (Up) / Set / Select (Down) | B3 / B4 / B5 | **Y / X / R** |
| **skisuprg** | skisuprg | ski platform + foot sensors | Inclining / Swing | `AD_STICK_X` / `_Y` | **left stick** |
| | | | Select 1 / 2 / 3 | B1 / B2 / B3 | **B / A / Y** |
| | | | Zoom Out / Zoom In | B4 / B5 | **X / R** |
| | | | Foot Sensor (R) / (L) — 4-bit nibbles | `JOYSTICK_RIGHT` / `_LEFT` | **D-pad right / left** |
| **powsled** | powsled, powsledr, powsledm | **4 linked sled cabinets** | Entry / Call per player, Cancel Error, Cancel Network Check, `SERVICE2` | B1 / B2 / B4 / B3 | — **out of scope**, user-options.md §1.4 |

---

## 4. ~~What the frontend's remap UI shows~~ ✅ FIXED 2026-07-30 — all three gaps closed

**The static `INPUT_DESCRIPTORS[]` is gone.** Labels are per game now, built at load from the same layout
row the pad reads, and seeded from exactly the `PORT_NAME` strings §3 transcribes — so daytona's remap
screen reads GEAR 1–4, VR1 (Red)…VR4 (Green), Brake, Accelerator, Steering, and vf2's reads Punch / Kick /
Guard. See [padmap-tool.md](padmap-tool.md); the read-out is `M2VK_HOST_DESCRIPTORS=1`.

The description below is kept as the record of what was there. All three gaps it names are closed:

- ~~**L3 has no descriptor at all.**~~ It has one — "Service Coin" — and it is **suppressed when
  `model2_diagnostic_input` is `None`**, because L3 is an inert `IPT_UI_MENU` then and labelling a dead
  control is the one thing worse than not labelling it. 🚨 That is why the descriptor send had to move
  *below* the options read in `retro_load_game()`.
- ~~**The gun ports do not describe Reload.**~~ They do.
- ~~**"View / Button 9" on R3 is only true for daytona.**~~ No set is told anything about a control it
  does not use: a null label emits no entry at all, so vf2 says nothing about X, L, R or the triggers.

⚠️ **One thing the fix did NOT do, and it is a limit of the source rather than of the mechanism.** Where
the driver never `PORT_NAME`d a button, the label falls back to "Button 1"/"Button 2" — which is what
`rchase2`, `gunblade`, `zerogun` and `pltkids` currently show, because `model2.cpp` does not say what
those buttons are. Improving them is an authoring change in the editor, not a code change.

The original text:

`INPUT_DESCRIPTORS[]` is generic on purpose — "Button 1" … "Button 9", "Steering / Stick X",
"Brake / Button 7", "Accelerator / Button 8", "Coin", "Start", "View / Button 9". None of the
`PORT_NAME` strings in §3 reach the frontend. Making them per-game is user-options.md §3 item 2 and is
unbuilt.

Three descriptor gaps, all cheap and none of them scheduled:

- **L3 has no descriptor at all**, so the service coin / UI-menu button is invisible in the remapper.
- The gun ports describe **Trigger, Button 2, Button 3** and not **Reload**, which is the control that
  makes `vcop` playable past the first magazine.
- **"View / Button 9" on R3 is only true for daytona.** Every other set's View button is `IPT_BUTTON3`
  (indy500, sgt24h, overrev, skytargt) or `IPT_BUTTON1` (waverunr), i.e. **Y** or **B**, not R3.

---

## 5. Findings — five, in order of how much they matter

### 5.1 ~~🚨 The shoulder-button descriptors are inverted, on both layouts. This is a live bug.~~ ✅ FIXED 2026-07-29

**Fixed in per-game-input step 1**, which is where this section said it belonged. The descriptors now
follow the layout table: `R` is "Button 5", `L` is "Button 6". The diagnosis below is unchanged and is
kept because it is the reasoning for *which* of the two tables was wrong. ⚠️ The Modern half is still
true and is not a bug: one set of labels describes Classic by design, since descriptors are sent once
at load and a layout is a per-port choice the player can change at any moment.

`retro_entry.cpp:236-237` tells the frontend **L = "Button 5"** and **R = "Button 6"**.
`BUTTON_LAYOUTS[LAYOUT_CLASSIC]` (`libretro_m2_input.cpp:61-69`) puts MAME button 5 on
`RETRO_DEVICE_ID_JOYPAD_R` and button 6 on `..._L`. **The two tables disagree, and the layout table is
the one that runs.**

Concretely on daytona Classic: **R** is GEAR 4 and **L** is VR1 (Red); the remap UI says the opposite.
Under **Modern** the descriptor is wrong in a second way — button 5 moves to R2 and **L does nothing
at all**, while the descriptor still claims it is button 5.

Nothing else keys off `INPUT_DESCRIPTORS`, so this is cosmetic in the sense that no input is lost —
but it is a label lying about a control, on the one screen a player consults when a control is
missing, and it is a two-line fix. Costs no measurement: descriptors touch no pixel and no digest.

### 5.2 ⚠️ `desert`'s brake is an ad-stick, so it very likely rests half-applied

`BRAKE` is `PORT_BIT(0xff, 0x00, IPT_AD_STICK_Y)` (`model2.cpp:1775`) — an absolute stick axis whose
*driver default* is `0x00`, mapped to **left stick Y**. An absolute analog field takes the middle of
the input range to the middle of the port range, so a centred stick should present ≈ `0x80`, i.e. the
brake half on, with no way to release it below centre except pushing the stick up. It is also on the
same stick as the steering, which is the X axis of the same physical control.

[inferred from `model2.cpp` + `assignmenthelper.cpp`; **not measured**.] `desert` is
`MACHINE_NOT_WORKING`, so this is low priority — but it is the one place in 90 sets where a *brake* is
on a self-centring stick axis, and if `desert` is ever driven and feels wrong, this is why. It is also
the cleanest argument for the per-set override table (user-options.md §3 item 3): the right answer is
to put `desert`'s `AD_STICK_Y` on **L2** with the other brakes, which only a per-set map can express.

### 5.3 ⚠️ Three sets declare two ports of the same analog type, so one stick axis drives both

- **waverunr** — `HANDLE` and `ROLL` are both `IPT_AD_STICK_X`; `THROTTLE` and `PITCH` are both
  `IPT_AD_STICK_Y`.
- **topskatr** — `CURVING` and `SLIDE` are both `IPT_AD_STICK_X`.

MAME's default assignment is per *(type, player)*, so both fields of a pair receive the same binding
and move together. **This is upstream's, not ours** — the driver's own comment on both sets says
`// TODO: requires LEFT/RIGHT_AD_STICK in framework`. There is nothing to fix at this seam; a per-set
override could at most move one of each pair onto the **right** stick, which is otherwise idle (§5.4).
[inferred from the assignment code; not measured. Both sets are `MACHINE_NOT_WORKING`, and `waverunr`
is an `ab.sh` fixture only for its *rendering*.]

### 5.4 The right analog stick is idle on 89 of 90 sets

`configure()` gives the right stick to `IPT_AD_STICK_Z` (`libretro_m2_input.cpp:421-429`) and **no
Model 2 set declares `AD_STICK_Z`** — confirmed by grep over the whole driver. Its only real use is
`von`'s right twin stick. So on every driving, ad-stick and exotic set there is a whole analog stick
free, which is the resource §5.2 and §5.3 both want and the per-set table would spend.

The dead branch also has a fallback that combines the two triggers into an `AD_STICK_Z`, which no set
can ever reach. Harmless; noted because it reads as if some game needs it.

### 5.5 ⚠️ `dynabb`'s two bats land on different shoulders

`BAT1` is `IPT_PEDAL` player 1 and `BAT2` is `IPT_PEDAL2` player 2. `apply_device_defaults()` matches
on player == device index, so P1's bat lands on **pad 1's R2** (the accelerator slot) and P2's on
**pad 2's L2** (the brake slot). Two players, two different triggers, same physical action. This is
user-options.md §1.3's suspicion, re-derived here from the same two files and still **unverified** —
`dynabb` is `MACHINE_NOT_WORKING` and has never been run.

---

## 6. What this audit did *not* do

- **Nothing was run.** No `retrohost`, no RetroArch, no digest. Every claim is a read of
  `model2.cpp`, `libretro_m2_input.cpp` and `assignmenthelper.cpp`, and the ones that are inferences
  rather than transcriptions say so in place. This matches user-options.md §3.1's finding that the
  fault class "a control with no mapping" is answerable statically and a play-through is worst at it.
- **The analog *feel* is out of scope** — that is the steering-curve work (user-options.md §2 and §6
  item 1), which is the next queued step and needs a scripted sweep, not a table.
- **`MACHINE_NOT_WORKING` sets were mapped but not judged.** 62 of 90 entries are flagged, including
  every set in the exotic and twinstick tiers, so most of §3 documents what *would* happen.
