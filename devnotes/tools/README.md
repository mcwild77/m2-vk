# The layout editor — how to use it

**What this is for:** deciding which RetroPad control does what on each Model 2 game, and what the
frontend calls it. You do the deciding; the tool stops you from making the two mistakes that are
invisible until somebody plays the game, and fills in the names out of the driver so you never have to
look them up.

This file is the how-to. **[../padmap-tool.md](../padmap-tool.md) is the why** — the architecture and the
traps. Read this one to get work done, that one before changing how any of it works.

---

## The 60-second version

**Double-click `Model 2 Pad Editor` on the Desktop.** It starts the server, opens the editor, and you
author, click **Save to core**, click **Rebuild core**, click **▶ Play**. Or from a shell:

```sh
./devnotes/tools/padmap-serve.py     # opens the editor; then "Save to core", "Rebuild core", "▶ Play"
```

That's the whole loop, and the last button is the game itself — RetroArch, this port set's ROM, the core
you just built. The three buttons do what §4 used to ask you to do by hand.

Without the server (`open devnotes/tools/padmap.html`) it still works, one download and three commands:

```sh
mv ~/Downloads/input_layouts.json src/osd/libretro_m2/input_layouts.json
./devnotes/tools/padmap-gen.py
make SUBTARGET=model2 OSD=libretro_m2 NOWERROR=1 -j10
```

---

## 1. Opening it

Three ways in, all the same editor:

| | |
|---|---|
| **`Model 2 Pad Editor` on the Desktop** | double-click. Opens a Terminal window with the server in it, then the browser. |
| `./devnotes/tools/padmap.command` | the same thing from inside the repo — double-click it in Finder |
| `./devnotes/tools/padmap-serve.py` | from a shell |
| `open devnotes/tools/padmap.html` | no server: authoring works, the three buttons don't (§4) |

Either way it is offline: no network, nothing to install, any browser. The server is python3's own
`http.server` bound to `127.0.0.1`; it serves this directory and nothing else, and it exists only so the
page can reach two things a `file://` page cannot — the JSON on disk and `padmap-gen.py`.

**To stop it:** ctrl-c in the Terminal window, or the **Stop server** button in the editor's header.
**Starting it again while it is already running just reopens the tab** — any of the three ways in, since
the server checks the port itself. (It used to end in a Python traceback about the address being in use,
which reads as a broken tool rather than as "it is already open".)

⚠️ **The Desktop app deliberately goes through Terminal, and there is no tidier version of it.** The repo
is under `~/Documents`, which macOS protects: an app bundle reading it is refused with
`Operation not permitted` and **no permission prompt is ever offered**, so there is nothing to allow — ad-hoc
signing does not change it. Terminal already has that access. The window is the price, and it is also where
the build log is.

It loads `padmap-data.js` automatically, which is what each game declares. If the left pane is empty,
that file is missing or stale — see §6.

**Your work is saved in the browser as you go** (localStorage), so a refresh or an accidental close
doesn't lose it. But that is a scratchpad, not the file — nothing reaches the core until you save (§4).

## 1.1 Loading what's already authored

**Served, this is automatic**: the page loads `src/osd/libretro_m2/input_layouts.json` at startup, so you
start from the 12 rows that exist and get a "Changed since the file was loaded" panel — re-editing one game
later is a reviewable change instead of a rewrite. ⚠️ If the browser is holding unsaved scratch work it is
**not** overwritten; the file becomes the diff baseline instead and the header says
`editing local changes against …`.

From `file://`, click **Load input_layouts.json** and pick that file yourself.

---

## 2. The three panes

### Left — the games

One entry per **port set**, not per game. A port set is one `INPUT_PORTS_START` block in `model2.cpp`;
there are 32 of them across 90 game entries, so `daytona` covers all eight daytona variants and `vf2`
covers vf2, fvipers, lastbrnx and hpyagu98. Author once, and every clone gets it.

Badges:

| badge | means |
|---|---|
| *row* | already authored |
| *not working* | MAME flags every entry in this port set `MACHINE_NOT_WORKING` — see §7 |
| *no dump* | no ROMs locally, so nothing is known about what it declares. You can still author, but every label is a guess and no rule can be checked. |

### Centre — the pad

Click a control to assign whatever you've picked on the right. Assigned controls turn blue and show the
MAME button plus the label; **red means you've broken a rule** (§3).

**Select and Start can't be assigned** and are greyed out. They carry the arcade coin and start through
MAME's own defaults, so putting a game button there would take a credit every time you pressed it.

Underneath is the label table — every control, what it feeds, and the string the frontend shows. **Blank
means no label is sent at all**, which is the right answer for a control the game doesn't use. Don't put
"unused" in there; leave it empty.

### Right — what this game declares

Every input the machine actually has, **in the driver's own words**. daytona's buttons come up as
`GEAR N`, `GEAR 1`…`GEAR 4`, `VR1 (Red)`…`VR4 (Green)`; vf2's as `Punch`, `Kick`, `Guard`. Those strings
are from `model2.cpp`, read off the running machine — which is the point of the tool: you don't have to
know what button 6 is on a game you've never played.

Two collapsed sections below:

- **analog fields** — steering, pedals, ad-sticks. Shown so you can use their names as labels; where they
  *land* is not editable yet (that's the follow-on step).
- **handled automatically** — coin, start, the service switches, the drive-board switches. Listed so they
  don't read as missing. Nothing to do with them.

---

## 3. Assigning

Two ways, both fine:

- **Drag** a button chip from the right onto a pad control.
- **Click** the chip (it highlights), then click the control. Escape cancels.

`✕` on a chip unassigns it. A button with no control shows a **"why" box** — type a reason and the warning
clears. That's not busywork: daytona's `GEAR N` genuinely has no control (the pad runs out, and you reach
neutral by shifting down out of gear 1), and the note is what tells the next person it was a decision.

**Labels** are filled in from the driver and are editable — click and type. Once you edit one it stops
being auto-filled, so moving buttons around won't overwrite your wording.

### 3.1 The two rules it enforces, and why you want it to

| it refuses | because |
|---|---|
| a game button on the **D-pad**, when the game has a joystick | the D-pad already moves the character. One press would do both. |
| a game button on the **L2/R2 trigger**, when the game has pedals | flooring the accelerator would press that button too |

That second one is the bug this whole thing was built to kill. On daytona, VR2 and VR3 used to sit on the
pedal axes, so **accelerating changed the camera**. If you point something there on a game with pedals,
the tool goes red and tells you.

It also flags two buttons on one control, a game button left unreachable with no note, and labels long
enough that RetroArch truncates them.

### 3.2 One trap worth knowing about, in MAME rather than in the tool

**Gear numbering is off by one.** `GEAR N` is MAME's button 1, so the player's "gear 1" is button **2**.
The chips on the right are in MAME's numbering and carry the driver's own names, so **just follow the
names** — drag the chip that says `GEAR 1` and you're correct. It only bites if you count slots.

---

## 4. Getting it into the core

**Three buttons, if you started it with `padmap-serve.py`.**

| button | does |
|---|---|
| **Save to core** | writes `src/osd/libretro_m2/input_layouts.json` and runs `padmap-gen.py` |
| **Rebuild core** | runs `make SUBTARGET=model2 OSD=libretro_m2 NOWERROR=1 -jN`, progress in the header |
| **▶ Play `<game>`** | launches RetroArch on this port set's ROM, with the dylib in this repo (§4.1) |

The status line next to them is the generator's own message — `wrote … 12 row(s) naming 15 set(s)`, or the
refusal in red. **Nothing is written when it refuses**: the previous JSON is put back, so the JSON and the
`.ipp` can never be left disagreeing, which is the one state `--check` exists to catch and the one a save
button could plausibly create silently.

Otherwise, by hand:

```sh
# 1. Download in the browser, then:
mv ~/Downloads/input_layouts.json src/osd/libretro_m2/input_layouts.json

# 2. Compile the table
./devnotes/tools/padmap-gen.py

# 3. Rebuild
make SUBTARGET=model2 OSD=libretro_m2 NOWERROR=1 -j10
```

⚠️ **That download step is the one that goes wrong quietly** — a browser that has kept
`input_layouts(2).json` leaves you moving a stale file, and the change simply does not take. The button
does not have that failure mode; it is the reason it exists.

`padmap-gen.py` will **refuse** and tell you what's wrong rather than emit a broken table. It re-checks
everything the editor checks, plus that every game name in a row is a real `model2.cpp` entry and that no
name is claimed by two rows.

```sh
./devnotes/tools/padmap-gen.py --check     # is the compiled table in step with the JSON?
```

Run that if you're ever unsure whether a rebuild picked up your edits. It's also the thing to run before
committing.

⚠️ **Never hand-edit `input_layouts.ipp`.** It's generated; the next `padmap-gen.py` overwrites it. Edit
the JSON through the tool.

## 4.1 ▶ Play — going straight into the game

Pick a port set, click **▶ Play `<game>`**. RetroArch opens with that game running on the core in this
repo. Play it, check the mapping, come back and fill in **Verified**.

It is your ordinary play session, not a test rig: nothing is pinned, no config is written, the options
menu is in charge — same as `~/Desktop/Model 2.app`, with the content already loaded. **Set
`model2_diagnostic_input` from the options menu** if you want the game's INPUT TEST screen (§5.3).

Three things it does for you, each of which has burned a session when done by hand:

- **It plays the dylib in this repo, by path.** The core RetroArch has *installed* is a symlink here that
  has twice turned back into a stale copy on its own — silently, and it looks fine until the next rebuild.
  This button cannot pick up that copy, because it never looks at it.
- **It refuses to launch a core older than your layout** and says which button to press first — *Save to
  core* if the table hasn't been compiled, *Rebuild core* if it hasn't been built. That's the mistake that
  gives you a confident wrong answer about a mapping. You can override it if you meant it.
- **It clears `M2VK_*` / `M2OPT_*` from the environment.** A harness switch left in your shell beats the
  options menu by design, so it makes the menu silently do nothing.

⚠️ **If RetroArch opens and vanishes, the editor tells you why.** It keeps watching for 25 seconds, and
if RetroArch quits it shows the reason out of its log — almost always a ROM audit failure, which is a
fact about the ROM set and not about your layout. (RetroArch exits with status **0** when content fails
to load, so nothing else would notice.) Full log: `/tmp/m2vk-play.log`.

If the button says **no ROM for `<set>`** there is no zip for that set or any of its clones in
`~/Documents/ROMs/Model 2` or `devnotes/roms` — four port sets are in that position (`airwlkrs`,
`rchase2a`, `powsled`, `model2crx`). If it's missing entirely, RetroArch isn't at
`/Applications/RetroArch.app`.

⚠️ **Already running?** It says so rather than stacking a second RetroArch on top of the first — quit the
one you have, or say *Launch anyway*.

---

## 5. Checking your work

### 5.1 What the frontend will say — without launching RetroArch

```sh
M2VK_HOST_DESCRIPTORS=1 M2_SYSTEM_DIR=/tmp/m2sys M2_SAVE_DIR=/tmp/d \
  ./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/daytona.zip 10 /dev/null \
  | grep '^descriptor: port=0'
```

Prints exactly the labels RetroArch's **Quick Menu → Controls** will show. `device=1` is the pad,
`device=5` the analog sticks, `device=4` a lightgun.

(`M2_SYSTEM_DIR` only matters for games needing the second ROM path — `mkdir -p /tmp/m2sys && ln -s
/path/to/Polydiver/roms /tmp/m2sys/model2`.)

### 5.2 That nothing else broke

```sh
./devnotes/tools/padmap-test.sh     # the editor's own logic, all 32 port sets
./devnotes/ab.sh vf2 2500 /tmp/ab   # must reproduce devnotes/ab-baselines.md exactly
```

Input changes no pixel, so `ab.sh` is a *no-op guard* — a green table means you didn't break rendering,
not that the mapping is right.

### 5.3 🛑 And then play it

**One click: ▶ Play (§4.1).**

**Checking a mapping is your job, by decision** (CLAUDE.md, 2026-07-30). Scripted button-press testing is
banned here — it costs a fortune in tokens and it lies: the first automated check of the daytona collision
came back clean on *both* arms simply because the game was already in the camera view being tested, which
reads exactly like a failure.

For anything a game *consumes* rather than obviously reacts to, use the game's own **INPUT TEST** screen —
set `model2_diagnostic_input` in the core options to reach it. A held coin button, for instance, registers
nothing at all to the emulated hardware, so "nothing happened" tells you nothing.

---

## 6. Regenerating the game data

Only needed after an upstream MAME merge, or if you add ROMs.

```sh
./devnotes/tools/padmap-sweep.sh            # every reachable set, ~3 min
./devnotes/tools/padmap-sweep.sh vf2 doa    # just these
```

It boots each game for 20 frames, asks the machine what inputs it has, and writes `padmap-data.js`. It
prints a field count per game and lists what it skipped.

⚠️ If it ever reports **ZERO fields** for a game, that's a bug in the dump timing, not a game with no
controls — `../padmap-tool.md` §2.1.

---

## 7. "A lot of games say not working — do I need better dumps?"

**No. Your ROMs are fine, and better dumps would change nothing.**

`MACHINE_NOT_WORKING` is a flag **MAME's own developers** put on a driver to mean *"our emulation of this
board isn't finished"*. It is a statement about MAME's code, not about your files. MAME won't audit or
reject anything differently because of it.

**The proof is in your own sweep:** of the 62 flagged entries, **20 booted from the ROMs you already
have** and built their complete input list — `srallyc`, `dynamcop`, `schamp`, `lastbrnx`, `fvipers`,
`indy500`, `sgt24h`, `overrev`, `manxtt`, `manxttc`, `manxttdx`, `desert`, `waverunr`, `skytargt`, `bel`,
`topskatr`, `segawski`, `skisuprg`, `dynabb97`, `stcc`. ROMs that load and a machine that runs is not what
a bad dump looks like.

What's actually incomplete is the **coprocessor emulation on the later boards**. Model 2 came in variants
with different geometry hardware — the original's 5× TGP, the 2B's 2× SHARC, the 2C's 2× TGPx4 — and the
flags cluster exactly there:

| board | entries | flagged |
|---|---|---|
| `model2a` (+ variants) | 11 | 3 |
| **`model2b`** (2× SHARC) | 14 | **12** |
| **`model2c`** (2× TGPx4) | 2 | **2** |

The driver's own header lists what's known-broken per game — `doa` sound decays to silence, `lastbrnx`'s
SHARC program upload may not be right, `manxtt` can't escape its tutorial without analog input, `sgt24h`'s
steering doesn't centre. Those are emulation bugs upstream. **They get fixed by merging a newer MAME
release** (the repo tracks release tags — `mame0289` and on), not by re-dumping.

~~**Two games are genuinely missing files**, and they're the only ones where finding ROMs helps:~~

| game | ~~needs~~ |
|---|---|
| ~~`von` (Virtual On)~~ | ~~`epr-18832.15`, `epr-18833.16`~~ |
| ~~`hotd` (House of the Dead)~~ | ~~`epr-19696a.15`, `epr-19697a.16`~~ |

🚨 **STRUCK 2026-08-02 — neither was missing anything, and both play.** The zips were named after the
**parent** while holding a **clone**: `von.zip` is a `vonj` set (Japan, Rev B) and `hotd.zip` is a
`hotdo` set. Loading them under the clone's name is the whole fix, and both render — `vonj` 633 3D
frames, `hotdo` 823. They are copied to `vonj.zip` / `hotdo.zip` in `devnotes/roms/`, and the old names
are still there and still fail. **No game in the library is currently blocked on a ROM we do not have.**
See [compatibility.md](../compatibility.md) §4, which is now the reference for this question.

⚠️ The general point this paragraph was making still stands: a `MACHINE_NOT_WORKING` game that starts
loading becomes *authorable*, not necessarily playable.

### 7.1 What this means for authoring

You can still write a row for a flagged game and it will be correct — the input ports are real and the
driver names them, whether or not the graphics or sound work yet. What you **can't** do is verify it by
playing. So:

- **The 28 working entries (12 port sets) are what to get right**, and they're all authored.
- For a flagged game, author from the names, leave **Verified** blank, and it renders in the generated
  table as `NOT YET MEASURED IN GAME` — which is honest and is exactly what a future session needs to see.
- If an upstream merge un-flags a game, its row is already waiting.

---

## 8. Files

| file | |
|---|---|
| `padmap.html` | the editor |
| `padmap-serve.py` | serves it on `127.0.0.1` so the editor can write the JSON, run the generator, build, and launch the game |
| `padmap.command` | double-clickable launcher; `~/Desktop/Model 2 Pad Editor.app` is a wrapper round it |
| `padmap-data.js` | what each game declares — **generated**, by the sweep |
| `padmap-sweep.sh` | boots the games and writes the above |
| `padmap-gen.py` | `input_layouts.json` → `input_layouts.ipp`, and `--check` |
| `padmap-test.sh`, `padmap-test.js` | the editor's logic, headless |
| `../padmap-tool.md` | the architecture and the traps |
| `src/osd/libretro_m2/input_layouts.json` | **the data of record — this is the file that matters** |
| `src/osd/libretro_m2/input_layouts.ipp` | generated; never hand-edit |

`devnotes/` is gitignored, so the editor and its data never ship. The two files under `src/` do.
