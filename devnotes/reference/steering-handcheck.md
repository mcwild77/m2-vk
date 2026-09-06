# Steering hand-check — the thing to do with a pad in your hands

This is **step 5 of [steering-curve.md](steering-curve.md)** and it is the only step that is not
numbers. Everything else about the steering curve is measured and green; what is left is *does the
car feel right*, and no script can answer that.

**Fill in the blanks as you go.** At the end there are the starred answers I need. Nothing else in
the phase can be finished until they exist — they become the shipped defaults.

Written 2026-08-07. Tick the boxes; the whole thing is about 30 minutes.

---

## ✅ RESULT — run 2026-08-08, and three of the four decisions are made

| | Answer | What shipped |
|---|---|---|
| **Response** | **`Slight`** — `Medium` was "awful and twitchy and barely better than linear" | **default moved `Medium` → `Slight` (γ 1.30)** |
| **Deadzone** | **`5%`** — "fine, clear that part" | unchanged |
| **Range** | **`100%`** — "needs to be 100%, the end" | unchanged; settles the survey's contention against `80%` |
| **Test 6 (`vf2`)** | ✅ **PASS** — first reported as "no inputs work", withdrawn the same evening | nothing; the negative control holds |

Still open: Test 7 (`srallyc` with the new default) and Test 8 (`waverunr`'s handlebar, ANSWER 4).

🚨 **SUPERSEDED 2026-08-20 — Range is `80%` now, reversing 3b above.** The user came back with a
direct answer rather than a re-run of Test 5: **Range default → `80%`**, together with the two damping
knobs (below), which had not existed yet when this file's Test 5 was written. Nothing in this file's
5a/5b transcript was retaken — treat 3b's "100%, the end" as the mid-August read on this controller and
the 2026-08-20 instruction as the current one. `model2_steering_range`'s `DEFINITIONS[]` default and
`get_steering_range()`'s fallback are both `0.8f` now; `ab.sh srallyc 2500` and `ab.sh vf2 2500` still
reproduce their baselines byte-exactly (no fixture scripts an analog axis, so a default-only change
cannot move a digest).

🚨 **Also decided 2026-08-20, outside this file's tests: `model2_steering_damp_drive` = `4`,
`model2_steering_damp_return` = `8`.** This closes the open item at the bottom of
[worklog.md](../worklog.md)'s 2026-08-11 entry — the hand-check that was going to start from the measured
4/7 reference landed on **4/8** instead. Both were `Off` by default until now.

⚠️ **Amended 2026-08-08 after a survey of what other emulators ship and what their players tune** —
Tests 3 and 5 grew a head-to-head arm each, because that survey says the two decisions this check
exists to make are both closer-run than the original framing assumed. Read
"Where these arms come from" at the bottom if you want the reasoning; you do not need it to do the
tests. Now about 40 minutes.

---

## Part 0 — before you launch

### 🎮 The short version: double-click `devnotes/shortcuts/Model 2 Steering.command`

Added 2026-08-08, and it **does the whole of Part 0 for you** — §0.1 through §0.5. It lists only the
games below, one line per game saying which test it is for and what the buttons do; launches the
repo's own core **by path** (so §0.1's symlink cannot bite); forces Diagnostic Input to `None` (§0.3);
strips any leftover `M2VK_*` switch; offers `r` to put the three settings back to `Medium / 5% / 100%`
(§0.4); and prints the settings again when you quit the game, which is the row to write down. Quitting
RetroArch returns you to its menu, so the whole session is one Terminal window.

The rest of Part 0 is what it does, written out — read it if something looks wrong, skip it if not.

### 0.1 Check the core symlink

*(The `.command` above prints this for you and does not depend on it — it launches by path.)*

The installed core has silently reverted from a symlink to a stale copy **four times**. Paste this
into Terminal:

```sh
ls -la ~/Library/Application\ Support/RetroArch/cores/model2_libretro.dylib
```

- ✅ **Good** — the line ends with `-> /Users/…/mame-model2-vk/model2_libretro.dylib`
- ❌ **Bad** — the line starts with `-rwx` and has no `->`. **Stop and tell me.** You would be
  testing an old core and everything below would be meaningless.

- [ ] Checked, it is a symlink

*(Verified good on 2026-08-07 at 08:08. Check anyway.)*

### 0.2 Launch the core — the other way

`Model 2 Steering.command` is the shortest path. This one is equivalent and is what it was written
from; use it if you also want the button editor open. Paste into Terminal and leave the window open:

```sh
cd ~/Documents/GitHub/mame-model2-vk
./devnotes/tools/padmap-serve.py
```

A browser page opens. Pick the game from the dropdown, press the **▶ Play** button on that page.
RetroArch launches with that game.

**Use this, not the Desktop app**, for three reasons: it launches the core by its real path (so the
symlink above cannot bite you), it wipes every leftover `M2VK_*` environment switch (a leftover one
**beats the options menu**, so an option would silently do nothing), and it refuses to run a core
older than the button table.

- [ ] Launched, game is on screen

### 0.3 Turn OFF the test-menu combo — **do this first or daytona will misbehave**

⚠️ **Already done as of 2026-08-08** — the setting is `None` on this machine now, and
`Model 2 Steering.command` re-forces it before every launch. The original `.opt` is saved as
`m2-vk.opt.handcheck-backup`. Read on only if you launch some other way.

Your saved settings used to have **Diagnostic Input = "Hold Start"**. That means *holding Start
for about one second opens the cabinet's test menu*. In Daytona, Start is a real button you press
while driving. You will end up in a service menu mid-race and think the core crashed.

**In RetroArch:** press <kbd>F1</kbd> → **Core Options** → find **"Diagnostic Input (Test Menu)"** →
set it to **`None`** → press <kbd>F1</kbd> again to get back to the game.

- [ ] Diagnostic Input is set to `None`

### 0.4 Where the steering settings live

Same place, and you will be coming back here constantly:

> <kbd>F1</kbd> → **Core Options** → scroll down

The three you care about are the last three in the list:

| Menu name | What it does | Values you will see |
|---|---|---|
| **Steering Response** | how much of the fine control is near centre | `Linear`, `Slight`, `Medium`, `Strong`, `Very Strong` |
| **Steering Deadzone** | how far the stick moves before the wheel does | `0% (off)`, `2%`, `5%`, `8%`, `10%`, `15%`, `20%` |
| **Steering Range** | how much wheel you get at full stick | `100% (full lock)`, `90%`, `80%`, `70%`, `60%` |

**They apply instantly.** You do not need to reload the game after changing one. Change it, press
<kbd>F1</kbd>, keep driving.

Right now they should read **Medium / 5% / 100%** — those are the proposed defaults, and the whole
point of this check is to find out whether they are the right ones.

- [ ] Found the three settings, they say Medium / 5% / 100%

### 0.5 Turn ON the steering display — **new, 2026-08-08, and it changes how Tests 3 and 4 feel**

There is now a bar across the top of the screen that shows what the wheel is doing:

> <kbd>F1</kbd> → **Core Options** → **Steering Display** → **On**
> *(or press `d` in the `Model 2 Steering.command` menu before launching)*

- **Red** is wheel you are not using. All red means the car is going straight.
- **Green** grows out from the middle towards the side you are steering. It is the steering the game
  is **actually receiving** — not what your thumb is doing.
- The **white notch** is where your thumb actually is.

🚨 **The gap between the notch and the end of the green is the response curve.** That is the whole
thing Test 3 is asking you to judge, drawn. On `Medium` at half-stick the notch sits well outside the
green; on `Linear` they sit on top of each other at every position. Watch it change as you switch
settings — it is the fastest way to understand what the five values are actually doing.

⚠️ **It is drawn over the top of the game**, including over Daytona's lap counter, and there is
nowhere to put it that is not over something. Turn it off for Tests 5 to 8, and for playing.

- [x] Steering Display is On

### 0.6 Daytona controls, so you are not hunting

| You press | It does |
|---|---|
| **Select** | Insert coin |
| **Start** | Start |
| **R2** (right trigger) | Accelerator |
| **L2** (left trigger) | Brake |
| **Left stick, left/right** | **Steering ← this is the thing being tested** |
| B / A / Y / X | Gears 1 / 2 / 3 / 4 — **ignore these**, the car drives fine without shifting |

To get going: **Select** (coin), then **Start**, pick a course, then hold **R2**.

- [x] Driving

---

## Part 1 — the tests

> Do them in order. Tests 1–5 are all in **Daytona USA**, same session, no reloading.

---

### TEST 1 — feel the problem first

So you have something to compare against. This is what the core did before this work.

**Game:** `daytona`
**Set:** Response = **`Linear`**, Deadzone = **`0% (off)`**, Range = `100% (full lock)`
**Do:** drive one lap. Try to hold a steady line on the banking.

**What you should feel:** the car **darts** — a tiny thumb movement throws it across the track. Also
the *first* bit of stick movement does nothing, and the *last* bit does nothing (the wheel is already
at full lock before your thumb reaches the edge).

- [X] Done. Did it feel darty and hard to hold a line? **YES / NO** → `__YES it sucks and iis flying all over the place______`

*(If it felt fine, say so — it changes the recommendation, and it is a real answer, not a failure.)*

---

### TEST 2 — does the setting apply instantly?

**This is a pass/fail bug test, not a preference.**

**Game:** `daytona`, still driving, do **not** reload
**Do:** <kbd>F1</kbd> → Core Options → **Steering Response** → change `Linear` to **`Medium`** →
<kbd>F1</kbd> → drive into the very next corner.

- ✅ **PASS** — the steering feels different immediately, on that corner.
- ❌ **FAIL** — it feels identical until you quit and reload the game.

- [ ] Done. **PASS / FAIL** → `________`

> ❌ If this fails, **stop and tell me**. It is a bug in the code, not a matter of taste, and the
> rest of the tests would be measuring the wrong thing.

---

### TEST 3 — pick the Response default ⭐ **THIS IS THE MAIN ONE**

**Game:** `daytona`
**Do:** drive a lap at each setting, in this order. Change it live between laps (<kbd>F1</kbd> →
Core Options → Steering Response). Keep Deadzone at `5%` and Range at `100%`.

The question for every lap is: **can you hold a line through Daytona's banked turn without
constantly sawing the stick left-right to correct?**

⚠️ **Judge it by feel, and use the bar only to understand *why* it feels that way.** The setting that
makes the prettiest bar is not the answer; the setting you can hold a line with is.

| Setting | Lap done | How it felt (twitchy / good / vague / too slow) |
|---|---|---|
| `Linear` | [ ] | `________________________________` |
| `Slight` | [ ] | `________________________________` |
| `Medium` | [ ] | `________________________________` |
| `Strong` | [ ] | `________________________________` |
| `Very Strong` | [ ] | `________________________________` |

**Note:** every setting still gives you **full lock** at full stick deflection. Stronger settings do
not reduce how far you can turn — they just make the movement near centre finer. If a setting feels
like you *cannot turn far enough*, that is worth writing down, because it would be unexpected.

⭐ **Then do one extra thing: drive `Slight` and `Medium` back to back, twice each.** A survey of what
other emulators ship and what their players tune (see "Where these arms come from" at the bottom)
says the answer is very likely one of those two, and that the shipped default is currently one notch
*stronger* than the outside consensus. They are the closest pair on the list, so a straight sweep is
the worst way to tell them apart — alternating is the only way you will feel the difference.

#### ⭐ ANSWER 1 — which one was best? → **`Slight`** ✅ *(answered 2026-08-08; it is the shipped default now)*

#### ⭐ ANSWER 1b — `Slight` or `Medium`, head to head? → **`Slight`, and not narrowly** ✅

> In the player's words, `Medium` felt **"awful and twitchy and barely better than linear"**. That is
> worth reading carefully, because it is the *too strong* failure and it looks like the too-weak one:
> a strong curve buys a fine centre with a coarse outer travel, so a correction that leaves the middle
> overshoots, which reads as darty exactly as `Linear` does. The survey at the bottom of this file
> guessed `Slight` and was right, but the survey did not decide it — this did.

---

### TEST 4 — pick the Deadzone default ⭐

The deadzone is how far the stick has to move before anything happens. Too small and a worn stick
makes the car drift on its own; too big and the steering feels disconnected near centre.

**Game:** `daytona`
**Set:** Response = whatever you picked in Test 3
**Do:** on a **long straight**, take your thumb **completely off** the stick, and let it sit.

| Deadzone | Straight test done | Does the car wander off on its own? |
|---|---|---|
| `0% (off)` | [ ] | YES / NO → `______` |
| `2%` | [ ] | YES / NO → `______` |
| `5%` | [ ] | YES / NO → `______` |
| `8%` | [ ] | YES / NO → `______` |
| `10%` | [ ] | YES / NO → `______` |

Start at `0%` and go **up** until the car stops wandering. Then stop — do not go higher than you
need to, because the deadzone costs you fine control near centre.

**The bar makes this one easy**: with your thumb off the stick, the green should be completely gone.
If a sliver of green survives with nothing touching the stick, that is exactly the wander you are
hunting, and the deadzone is too small.

**Also check:** at the value you land on, does steering still feel responsive when you *do* move the
stick? It should not feel like there is a lag or a dead patch.

#### ⭐ ANSWER 2 — the smallest deadzone where the car stays straight → **`5%`, unchanged** ✅

> "Deadzone is fine, clear that part." Which agrees with both of the other two readings on it: the
> survey's 5–8 % band, and the stick tester's worst-case drift of 0.0103.

---

### TEST 5 — should Steering Range be `80%` by DEFAULT? ⭐ **reframed 2026-08-08**

🚨 **This test used to ask "is Range worth keeping at all?" and that was the wrong question.** The
survey at the bottom of this file found that *reducing maximum lock* is the single most commonly
recommended fix for driving an arcade wheel with a thumbstick — more commonly recommended than a
response curve. It is the same knob Supermodel players call "saturation above 100 %", written the
other way round: their 150 % is our `70%`, their 130 % is our `80%`. So Range may deserve to be a
shipped default rather than a setting to go and find, and this test decides that.

🚨 **MEASURED 2026-08-08, AND IT ARGUES THE OTHER WAY ON THIS PARTICULAR PAD — read before running 5a.**
The Switch Pro Controller used for this hand-check was put on a stick tester
(`devnotes/tools/sticktest.c`, worklog 2026-08-08 (4)) and **its analog sticks clip before their
physical gate in every direction**: minimum envelope radius 1.0038 over all 72 sectors, maximum 1.4142
(= √2, both axes railed at once), roundness 0.7097 against 1/√2 = 0.70711 — a perfect clipped square,
matched to rms 0.0299. **The stick is not uneven and does not drift** (worst 0.0103, so Deadzone `5%`
is confirmed from the other side); it simply reaches full scale early, and the last slice of travel is
already a plateau in the hardware.

Range below 100 % makes full lock arrive *earlier* in stick travel. The survey's recommendation assumes
a pad that **under**-reaches; this one **over**-reaches, so `80%` stacks a second plateau on a measured
one. ⚠️ **So a Range preference formed on this pad is partly measuring the clip rather than taste** —
which does not void the test, but means a "yes to 80 %" answer here is weaker evidence for a *shipped
default* than a "no" is. If 5b feels good anyway, that is worth knowing; treat it as this controller's
answer and not the general one.

**Game:** `daytona`
**Set:** Response and Deadzone at your picks from Tests 3 and 4

**Do two things, in this order.**

**5a — can you still finish the course?** Set **Range = `80%`**. Find the tightest corner on the
course and take it at speed with the stick pushed **fully** to one side.

- ✅ **Fine** — you get round it.
- ❌ **Too far** — you physically cannot make the corner even at full stick.

- [ ] Done.

**5b — if 5a was fine, which do you actually prefer?** Drive `100%` and `80%` back to back, a lap
each, twice. The thing to judge is *not* the tightest corner — it is Daytona's long banked turn and
the straights, where a smaller range means a given thumb movement asks for less wheel.

| Range | Laps done | How it felt |
|---|---|---|
| `100%` (today's default) | [ ] | `________________________________` |
| `80%` | [ ] | `________________________________` |

⚠️ **The bar tells you what to expect here and is worth a glance**: at `80%`, pushing the stick all
the way over leaves the green **short of the end of the bar**. That is the setting working, not a
bug — you are trading top-end lock for finer control everywhere else. If the green stops short and
the car *also* will not turn far enough for the course, that is 5a failing and it beats 5b.

#### ⭐ ANSWER 3a — tightest corner still makeable at `80%`? → **not the deciding question** — see 3b

#### ⭐ ANSWER 3b — better to drive, `100%` or `80%`? → **`100%`** ✅ *("Steering range needs to be 100%, the end.")*

> **So the survey's contention on Range is settled against it and `100%` stays the default.** The
> pad measurement above predicted this: this controller already clips before its gate, so `80%` was
> stacking a second plateau on a measured one. `90%`…`60%` stay in the list as settings to find, and
> nothing is dropped.

*(3a NO → I drop `70%` and `60%` from the list; offering a setting that makes the game unfinishable
is a trap. 3a YES and 3b says `80%` → it becomes the shipped default and `100%` stays available.)*

---

### TEST 6 — the negative control ⭐ **do not skip this**

This checks the curve is **not** being applied to games that have no steering wheel. If it is, the
fighting games get mushy movement and none of the numeric tests can see it.

**Game:** `vf2` (Virtua Fighter 2) — quit Daytona, launch `vf2` from the ▶ Play page
**Set:** Steering Display **On** for a moment first — on vf2 the bar should **not appear at all**,
which is the same claim this test makes, for free and before you have pressed anything. Then
Response = **`Very Strong`**, Deadzone = **`20%`** (deliberately extreme — if any of it
leaks through, this is where you would feel it)
**Controls:** Select = coin (**press it twice**, vf2 wants 2 credits), Start, then left stick to
walk, B = punch, A = kick, Y = guard
**Do:** walk your fighter forward and back, step left and right, in a real round.

- ✅ **PASS** — movement feels **completely normal**. Exactly like it always has. The extreme
  settings do nothing at all.
- ❌ **FAIL** — walking feels sluggish, or there is a dead patch before your fighter moves.

- [x] Done. **PASS** ✅ *(2026-08-08)*

⚠️ **It was reported as a FAIL first — "vf2, no inputs work" — and withdrawn the same evening once the
pad was tried again.** Kept here rather than deleted, for the one lesson and the one measurement:

- **The reported symptom did not match the failure this test watches for.** Test 6 fails as *mushy or
  sluggish* movement; "no input at all" is something a chain that multiplies one axis cannot produce,
  and `vf2` declares no `IPT_PADDLE` so `steer_shape()` is the identity there anyway. **When a bug test
  fails with the wrong symptom, check whether the named mechanism can even express it before
  investigating it as that bug.**
- **The static evidence is worth keeping as the negative control's numbers.** On the binary that session
  played, under its exact options (`vulkan / 1440x1080 / blended / steering_display=on / Response=Linear`),
  `retrohost --vk` on `vf2` over 2600 frames gives **three distinct whole-run digests** — no input
  `a3854c40fd484bbe`, coin ×2 + Start `cc5a5af7f6deb4de`, plus B/A/d-pad-left `4a1c70243d1974e3`. Coin,
  Start, the face buttons and the d-pad each reach the machine and change the picture. Same three-way
  split on the software path (`203a2c657145fca3` / `ba3b7180e8d6c86a`). That is Test 6's claim measured
  from the code's side, and it is now confirmed from the player's.

> ❌ If this ever fails **in the way the test describes** — sluggish walking, a dead patch — the
> game-detection is wrong and that is a real bug.
> *(The code says vf2 has zero steering-wheel controls, so this should pass — but a felt check is
> the point.)*

---

### TEST 7 — a second steering game

Confirms the curve suits a different wheel, not just the one it was tuned on. Sega Rally's wheel
uses a different internal number range to Daytona's, so this is a genuine second case.

**Game:** `srallyc` (Sega Rally Championship)
**Set:** Response and Deadzone at your picks from Tests 3 and 4
**Controls:** Select = coin, Start, R2 = accelerate, L2 = brake, left stick = steering
**Do:** drive a stage.

- [ ] Done. Does your Daytona setting feel right here too? **YES / NO** → `________`
- If NO, what does Sega Rally want instead? → `________________`

---

### TEST 8 — waverunr, the odd one out ⭐

WaveRunner is a jet ski. Its handlebar is wired up as a different kind of control, so **the curve
does not apply to it at all** — deliberately. This test asks whether that is a mistake.

**Game:** `waverunr`
**Set:** anything — the steering settings have **no effect on this game**, that is the point
**Do:** race. Pay attention to how the handlebar responds to the left stick.

*(The bar does not appear here either, for the same reason the curve does not reach it. That is the
detector being consistent, not a second bug.)*

**The question:** does steering the jet ski have the **same darty, twitchy problem** that Daytona had
back in Test 1?

- [ ] Done. **YES, it is twitchy too / NO, it feels fine** → `________________`

#### ⭐ ANSWER 4 → `________________`

- **YES** → I add a per-game override so waverunr gets the curve too.
- **NO** → leave it alone, it is fine as it is.

---

## Part 2 — what to send me

Copy this block, fill it in, paste it back:

```
TEST 1  Linear/0% felt darty?        ...........
TEST 2  Setting applied live?        PASS / FAIL      <-- bug test
TEST 3  Best Steering Response:      ...........      <-- ANSWER 1, becomes the default
TEST 3b Slight or Medium, head to head?  ...........  <-- ANSWER 1b, breaks the likely tie
TEST 4  Smallest good Deadzone:      ...........      <-- ANSWER 2, becomes the default
TEST 5a 80% Range still makes the tightest corner?   YES / NO
TEST 5b Better to drive, 100% or 80%?    ...........  <-- ANSWER 3b, may become the default
TEST 6  vf2 movement normal?         PASS / FAIL      <-- bug test
TEST 7  srallyc same settings OK?    YES / NO
TEST 8  waverunr twitchy too?        YES / NO         <-- ANSWER 4

Anything that felt weird, in your own words:
...................................................
```

---

## Notes and gotchas, if something goes strange

- **A setting seems to do nothing.** Most likely a leftover `M2VK_STEER_*` environment switch — it
  overrides the menu **by design**. The ▶ Play button strips these, so this should not happen; if it
  does, tell me and quote the `[model2] options:` line from the Terminal window.
- **RetroArch saves your option choices when you quit**, even though it is told not to save the rest
  of its config. Whatever you leave the settings on is what the next launch starts from. That is
  fine — just know it, so a later test is not silently starting from Test 6's extreme values.
- **Daytona does not need gear changes.** The gearbox behaves as an automatic; the car reaches
  167 km/h with the gear indicator still on N. Ignore the gear buttons entirely.
- **Do not try to get to a game's test menu** during any of this — Part 0.3 turned that combo off on
  purpose.
- **`indy500` is an acceptable substitute for `srallyc`** in Test 7 if you prefer it; it has the same
  kind of wheel.

## What the code already knows, for reference

Measured statically 2026-08-07 — you do not need to check any of this, it is here so you know what
"correct" looks like:

| Game | Has a steering wheel? | Curve applies? |
|---|---|---|
| `daytona` | yes — 1 wheel control, range `0x020..0x0e0` | **YES** |
| `srallyc` | yes — 1 wheel control, range `0x000..0x0ff` | **YES** |
| `indy500` | yes — 1 wheel control, range `0x000..0x0ff` | **YES** |
| `waverunr` | **no** — handlebar is a different control type | **NO** ← Test 8 asks if this is wrong |
| `vf2` | **no** | **NO** ← Test 6 confirms |

The settings resolve as `deadzone=0.050 gamma=1.30 range=1.000`, on frame 0, before the first
controller read. *(γ was 1.70 until this check moved the default on 2026-08-08.)*

---

## Where these arms come from — the survey, 2026-08-08

You do not need this to do the tests. It is here so that a later reader knows why Tests 3 and 5 are
shaped the way they are, and does not "tidy" the head-to-head arms back into straight sweeps.

A survey of racing-game and arcade-emulator practice — Supermodel (Model 3, the closest sibling),
MAME, Cannonball, Flycast, and what players of those actually hand-tune — agrees with this core's
design on the big things and disagrees with two of its **defaults**.

**Confirmed, and already built:**
- A curve is the right instrument, and **no neighbouring emulator has one**
  ([steering-curve.md](steering-curve.md) §2 found the same by reading their source). It is a
  console-racer technique, which is why it is the highest-value thing available here.
- Non-linear by **default** is right. The strongest independent evidence is someone who built a real
  Model 2 Daytona I/O board, drove it from a PS4 pad, and added an exponential curve after comparing
  against what a real wheel produced — the same conclusion as §3.5, reached from the hardware side.
- Deadzone in the 5–8 % band, and larger than instinct suggests, because worn sticks self-steer.
- Linearity is genuinely **contested** between players, which is why `Linear` stays on the list
  rather than being designed out.

**The two disagreements, which are what Tests 3 and 5 now measure:**
1. The consensus expo is *mild*. Our `Medium` (γ 1.70) is one notch above the outside recommendation,
   which lands on `Slight` (γ 1.30). → Test 3's head-to-head.
2. **Reduced maximum lock is the most commonly recommended fix of all** — more than the curve — and
   ours is off by default. Supermodel players call it "saturation above 100 %"; algebraically that is
   the reciprocal of our Range, so their recommended 130–150 % is our `80%`/`70%`. → Test 5 reframed.

🚨 **And the one thing the survey cannot transfer, which matters if a number from it is ever quoted
here directly:** none of those emulators sits *inside* MAME, so none has MAME's own
`joystick_deadzone 0.15` / `joystick_saturation 0.85` applied downstream of its shaping. Our §3.3
pre-compensation inverts exactly that, and **that inversion is the only reason their deadzone numbers
are comparable to ours at all** — without it, our 5 % would stack to roughly 19 % effective with the
top 15 % of travel a flat plateau, which is the defect step 2 measured and removed. A figure lifted
from a Supermodel or Flycast config is only meaningful here because that stage exists.

**Two things the survey names that this core does not have**, both deliberately left out of this
hand-check because they are code and not feel:
- **An output slew limit** ("steering filter"). Cheap, but it puts per-frame state into
  `steer_shape()`, which is currently a pure function — and the byte-exactness argument for every
  `ab.sh` fixture rests on *a centred stick returning a hard zero at every setting*. A limiter still
  converges to zero, just not in one frame. No baseline actually moves (no accuracy fixture scripts an
  analog axis) but the guard's wording would have to be redone, so it wants its own step.
- **A rate-based path for digital input** (d-pad / keyboard), which is what the consensus says rate
  mode is genuinely *for* — not for thumbsticks. 🚨 On `daytona` there is nowhere to put it:
  [input_layouts.json](../../src/osd/libretro_m2/input_layouts.json) spends the whole d-pad on VR1–4.
  `srallyc`'s d-pad is free; `daytona`'s is not. So that is a **layout** decision before it is an
  input one, and it re-frames [steering-curve.md](steering-curve.md) step 6: that step was deferred
  for needing per-port accumulators, and the better reason is that the positional model is the right
  one for a stick and rate only earns its place for digital controls.
