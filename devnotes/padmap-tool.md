# The layout editor — `padmap.html`, and the pipeline either side of it

**Built 2026-07-30.** The per-game RetroPad layout for every Model 2 set is authored in a browser and
compiled into the core. This file is the record of how that works and of the traps found building it.

The vision it serves, stated by the user: **load a game and touch nothing, and the controls are already
right; open RetroArch's Controls menu and every entry says what the button actually does on that
cabinet.** Not "Button 4" — "GEAR 3".

**Extended to System 22 (2026-08-22, S3/Track C start).** The editor now carries a **family tab** at the
top of the Port sets list — `System 22` (default) and `Model 2` — and the whole pipeline is driver-aware:
`padmap-gen.py` reads **both** `model2.cpp` and `namcos22.cpp` for its set-existence check (a row naming
an S22 set is no longer refused as "not a GAME entry"), and `padmap-sweep.sh` reads both, boots the
matching subtarget core
(`model2_libretro.dylib` / `namcos22_libretro.dylib`) from the matching ROM dir (`devnotes/roms` +
Polydiver / `devnotes/roms/system22`), and tags every `drivers[]` row with a **`driver`** field. The tab
filters the library on that field (`familyOf(d) = d.driver || "model2"`, so a pre-tab data file reads as
all Model 2). `TIERS` is now family-keyed. **A third tab, `System 21`, was added 2026-08-25** — the sweep/gen backend
was already S21-aware (three driver files, `namcos21` core, shared `devnotes/roms/system22` dir); the tab,
its `TIERS` block, and `padmap-serve.py`'s FAMILIES entry were the missing front-end pieces. Current data:
143 GAME entries (90 M2 + 42 S22 + 11 S21), 57 port sets (32 + 18 + 7), 57 dumps. `padmap-test.sh` asserts
those totals and flattens `TIERS` across families. Two sets still can't be authored, for reasons that are
NOT fixable by moving ROMs: **`solvalou`** boots only far enough to demand `solvalou.nv` (a required
default-NVRAM file the local romset is missing), and **`driveyes`** lives in `namcos21_de.cpp`, which the
`namcos21` subtarget deliberately does not build (`namcos21.lua` scopes to the Star Blade + Winning Run
families; driveyes is also `MACHINE_NOT_WORKING`). `powsled` was a false alarm — a Model 2 set that had
been dropped into `devnotes/roms/system22` by mistake; moved to `devnotes/roms`, it dumps and is authored. Nothing
below changed in shape; the schema and label-derivation are unchanged. **All 18 S22 port sets are now
authored** (2026-08-25, shippable-plan.md R1) — see that file for what each row does and the two cabinet
corrections found along the way (`cybrcomm` twin-stick, not a wheel; `tokyowar` a tank sim, not a gun).

**Served mode is family-aware too (2026-08-22).** `padmap-serve.py` resolves each set's family from
`padmap-data.js` (`set_families()`) and keys a `FAMILIES` table off it: **Play** finds the zip in that
family's ROM dir (`devnotes/roms` vs `devnotes/roms/system22`) and launches that family's core
(`model2_libretro.dylib` vs `namcos22_libretro.dylib`); the stale-core check compares against the right
core; **Rebuild** builds the selected set's `SUBTARGET` (the editor sends `{set, family}` with the build
request, so an S22 set rebuilds `namcos22`). `/api/state` lists the zips of *both* dirs, which is what
re-enables the Play button for S22 sets. No ROMs were copied — the server just looks in both places.

---

## 1. The pipeline

```
   model2.cpp                     the machine, booted
   INPUT_PORTS  ──────────────►  M2VK_INPUT_DUMP  ──────────►  padmap-data.js
                (ROMs, retrohost)  m2vk_inputdump.h            (what each set declares)
                                   padmap-sweep.sh                    │
                                                                      ▼
                                                            devnotes/tools/padmap.html
                                                              (drag buttons onto a pad)
                                                                      │
                                                                      ▼
                                           src/osd/libretro_m2/input_layouts.json   ← data of record
                                                                      │  padmap-gen.py
                                                                      ▼
                                           src/osd/libretro_m2/input_layouts.ipp    ← generated
                                                                      │
                                                                      ▼
                              libretro_m2_input.cpp  ──►  the pad reads it   (sources)
                                                     └─►  the frontend reads it (labels)
```

Four commands:

```sh
./devnotes/tools/padmap-sweep.sh              # regenerate padmap-data.js (needs ROMs; ~3 min)
./devnotes/tools/padmap-serve.py              # author, and the middle two steps become a button (§3.2)
open devnotes/tools/padmap.html               # or from file://, then download and move the json by hand
./devnotes/tools/padmap-gen.py                # json -> ipp
./devnotes/tools/padmap-gen.py --check        # the ipp matches the json, and every row validates
./devnotes/tools/padmap-test.sh               # the editor's own logic, headless
```

## 1.1 Why a generator exists at all — the one thing to understand before changing any of this

The core needs **two views of one fact**: "MAME button *n* is produced by control *c*", to read the pad,
and "control *c* is called *L*", to tell the frontend. For as long as those were two hand-written tables
they disagreed — `INPUT_DESCRIPTORS[]` called `L` "Button 5" and `R` "Button 6" while
`BUTTON_LAYOUTS[LAYOUT_CLASSIC]` had them the other way round, so **daytona's remap screen named GEAR 4
and VR1 (Red) reversed for months** ([input-map.md](input-map.md) §5.1).

`padmap-gen.py` **derives the label array from the source array**, once. A derived table cannot drift
from what it is derived from, so that class of bug is now structurally impossible rather than fixed. The
collision check falls out of the same pass for free: if two MAME buttons name one control, the inversion
finds two labels for one slot and refuses instead of silently keeping the last.

🚨 **"Structurally impossible" was too strong, and the same bug shipped again on 2026-07-31 — through the
LOADER.** `adoptParsed()` marked every label read from `input_layouts.json` as a *manual override*, and a
manual override is by definition a second copy of the fact; `relabelFromRow` then skipped those slots, so
the labels froze at the file's values while the assignments moved. **doa was authored, saved, built and
played with BUTTON1 (Hold) bound to Y while the frontend called Y "Kick"** — all three of its buttons named
as something they do not press. Only reachable once served mode began loading the file at startup
(2026-07-30), which is why exactly one row of twelve was hit. Now: a loaded label is manual **only for a
control no button feeds**; typing a label for a control one button feeds edits **that button's** wording so
it travels with the button; and **the generator refuses the drift outright** (it had been printing the
feeding button beside every label and never comparing them). ⚠️ **`padmap-test.sh` missed it because its
round-trip test hand-rolled `adoptParsed`'s body** — the test's copy carried the same wrong line as the
tool's, so they agreed. It calls the real function now, and the invariant is tested in both directions.

**The lesson, in the form it generalises:** *derived* only holds while there is exactly one path to the
value. Every "override", "keep what the user typed", or "load what was there" is a second path, and it
will eventually be taken for the whole table.

**Corollary: do not add a second label table anywhere, for any reason.** If the frontend needs a string
the row does not carry, the row grows a field.

## 1.2 The steering-coverage check (2026-08-07, steering-curve.md step 4)

🚨 **Relaxed 2026-08-22 for the System 22 bring-up.** The check no longer refuses a paddle machine that
has **no** row — that "every racer must be authored" half made partial saves impossible for a new driver
family (you could not save the first S22 racer until all nine were done). A paddle cabinet on the generic
hedge is now a **non-blocking `note:` line** on stderr (collapsed per port set); **completeness accounting
moved to Track D's compatibility matrix**. The two *consistency* errors below are unchanged and still
fire, so a row that **is** authored can never disagree with its machine.

`--check` refuses **a paddle machine whose own row does not say so, and a layout that says so on a machine
that does not steer**. Both halves are one coupling and neither is visible from one side:
the analog steering curve applies **iff the machine declares an `IPT_PADDLE`** — the detector asks the
machine, nothing is authored per game ([steering-curve.md](steering-curve.md) §3.2) — while the
`LSTICK_X` label is the **only** place the frontend ever says what the stick does. So a steering set left
on the generic row is shaped while being offered the hedge `Steering / Stick X` instead of its cabinet's
own wording, and a row labelled `Steering` on a paddle-less machine promises a curve that will never run.
Neither shows up in a build, a digest or a screenshot.

The paddle half is read from **`padmap-data.js`**, the same sweep the editor already runs on — so the
check is against what the curve will actually do, not against a list somebody keeps. It is **silent when
that file is absent** (it is `devnotes/`, and a checkout without it must still generate), and it treats a
port set nobody dumped as *unknown* rather than paddle-free, which is what stops the reverse half being an
argument from silence.

⚠️ **A check that cannot fail is worth nothing, so both consistency arms were re-fired** (2026-08-22)
by mutating the JSON in memory and restoring it: `daytona`'s `LSTICK_X` set to `Stick X` → *"the one place
that says so does not"*; `vf2` labelled `Steering` → *"the label promises shaping the detector never
applies"*. The third arm — a paddle machine with **no** row — now yields the non-blocking `note:` line,
not an error (verified: `--check` is rc 0 and names the nine unauthored S22 paddle port sets in that
note). With the tree as it stands, `--check` passes.

---

## 2. `M2VK_INPUT_DUMP` — the machine describing itself

`src/osd/libretro_m2/m2vk_inputdump.h`, header-only, off unless the variable names a file.

```sh
M2VK_INPUT_DUMP=/tmp/daytona.json M2_SYSTEM_DIR=/tmp/m2sys M2_SAVE_DIR=/tmp/d \
  ./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/daytona.zip 20 /dev/null
```

Per field: the `IPT_` token, player, mask, **`name()` — the driver's own `PORT_NAME`** — and, for analog
fields, `PORT_MINMAX`/sensitivity/keydelta/centerdelta and the four analog flags. Per set: the player
count and three derived flags, which are the reason this is a dump and not a grep.

| flag | what a row may not do when it is true | why |
|---|---|---|
| `joystick` | point a numbered button at a D-pad control | the D-pad already feeds `IPT_JOYSTICK_*`; one press would fire both. per-game-input.md §2.3 |
| `pedals` | point a numbered button at `L2_AXIS`/`R2_AXIS` | flooring the pedal would press the button — **the daytona collision** |
| `lightgun` | (positive) it is what says gun descriptors may be sent at all | see §5.2 |

### 2.1 🚨 It cannot be taken from `input_init()`, and it was, and the output looked perfect

`osd().init()` is `machine.cpp:156`; `m_ioport.initialize()` is `machine.cpp:169`. Taken from
`input_init()` the dump reports **`"fields": []`** on a set with twenty of them — structurally valid JSON
that says the set has no controls. It is taken from `libretro_m2_osd_interface::update()` behind
`machine.ioport().safe_to_read()` instead.

⚠️ **`m2vk_gunlog.h` already carried this exact warning about this exact trap** and it was read
afterwards rather than before. Both the header and `padmap-sweep.sh` now shout when a dump has zero
fields, because nothing downstream would otherwise object.

### 2.2 What the sweep reaches

**31 of 33 attempted sets**, covering **11 of the 12 in-scope port sets**. `von` and `hotd` have no local
ROMs; `rchase2a` has none either and shares `rchase2`'s button row, differing only in analog range and
reverse. Also dumped and available for the follow-on rows: `srallyc`, `dynamcop`, `schamp`, `lastbrnx`,
`fvipers`, `indy500`, `sgt24h`, `overrev`, `manxtt`, `desert`, `waverunr`, `skytargt`, `bel`, `topskatr`,
`segawski`, `skisuprg`, `dynabb97`, `stcc`, `doa`, `vstriker`, `zerogun`, `pltkids`, `gunblade`,
`motoraid`, `rchase2`, `vcop`, `vcop2`, `vf2`, `daytona`.

---

## 3. The editor

One self-contained file, offline, no network of any kind. Data arrives through a `<script src>` and not
`fetch`, because it runs from `file://` where `fetch` is blocked by CORS and a script tag is not.

It works per **port set** — what MAME's `INPUT_PORTS_START` blocks actually are, 32 of them across 90
`GAME` entries — and computes the `sets[]` a row must name from the driver table rather than asking for
it. A name is needed when the entry is a **parent**, and when it is a **clone whose parent uses a
different port set** (which is real: `rchase2a`). So `vf2`'s row names four sets —
`vf2, hpyagu98, fvipers, lastbrnx` — and eleven of the twelve name one.

**The validator is the reason this is a tool and not a text editor.** Every rule cites where it was
learned, and the two hard ones are the two that are invisible until somebody plays the game.

⚠️ **Both hard rules only apply to a button the set DECLARES.** A slot pointing somewhere for a button
that does not exist binds to nothing and cannot collide — without that gate `motoraid`, which has two
buttons and two pedals, failed on slots 7 and 8 that reach no part of the machine.

### 3.1 `padmap-test.sh` — and the two things it caught

Headless, under `jsc` (macOS's own JavaScriptCore; there is no node here), with a ~50-line DOM shim so
`render()` runs too and not only the pure functions. It checks all 32 port sets, every rule, the `sets[]`
coverage, that no name is claimed twice, and that export → load → export is stable.

It found two real problems on its first run:

- 🚨 **`relabelFromRow` was not idempotent.** It appended to the analog label slots without clearing
  them, so a second render turned daytona's steering label into
  `"Steering / Steering / Steering / Stick X"`. Invisible on the first paint. The test now asserts
  idempotence on **all 32** port sets.
- The seeded row's slot coverage was not what the plan assumed, which is what led to seeding only the
  **declared** buttons.

**Keep it passing.** A validator that silently stops firing is worse than no validator, which is the
whole argument for testing the rules rather than trusting them.

### 3.2 `padmap-serve.py` — making "get it into the core" a button (2026-07-30)

python3's own `http.server` on `127.0.0.1`, serving `devnotes/tools/` and five endpoints:
`GET /api/state` (the JSON on disk, plus whether the `.ipp` is in step with it), `POST /api/save` (write
it, then run `padmap-gen.py`), `POST /api/build` (run `make`, streamed line by line so the header shows
progress), **`POST /api/play`** (§3.2.2) and `POST /api/quit`. The editor probes `/api/state` at startup;
if nothing answers it is unchanged, and the Download-and-move loop is exactly as it was. **That fallback
is the requirement, not a courtesy** — the editor has to keep working from a bare checkout with nothing
running.

Four decisions worth not undoing:

- 🚨 **The save runs the generator, and that is the whole point of doing it server-side rather than with
  `showSaveFilePicker()`.** The File System Access API can write the JSON in place from a `file://` page,
  which looks like the cheaper answer — but it cannot run `padmap-gen.py`, and the generator is the half
  that matters: it is what refuses a broken table and what derives the labels from the assignments (§1.1).
  A button that writes the JSON and leaves the `.ipp` stale creates the exact disagreement `--check`
  exists to catch, and creates it silently.
- **A refused save writes nothing.** The old JSON is read into memory first and put back if the generator
  exits non-zero, so the two files cannot be left disagreeing by a failed click. Measured: a row with a
  numbered button on `SELECT` comes back 400 with the generator's own sentence, and both files hash
  identically to before.
- **What gets written is the POST body verbatim**, not a re-serialisation. That keeps the button
  byte-identical to the `mv` it replaces — round-tripping the current file through it reproduces both
  files to the hash.
- **`X-Padmap: 1` is required on every POST.** A cross-origin form post cannot set a custom header and a
  cross-origin `fetch` that sets one is preflighted, which this server answers for nothing. That plus
  binding `127.0.0.1` is the whole security story for a server that writes a file and runs `make`.

### 3.2.1 The one-button launcher, and the macOS trap under it

`~/Desktop/Model 2 Pad Editor.app` (LSUIElement, no Dock icon) probes the port; if the server is up it just
opens the tab, and if it is not it hands `devnotes/tools/padmap.command` to **Terminal**.

⚠️ **The port probe belongs in `padmap-serve.py`, and until 2026-07-31 it lived ONLY in the bundle** — so
`padmap.command`, which has no probe, met a server that was already running and died with a
`socketserver` traceback ending in `OSError: [Errno 48] Address already in use`. A stack trace out of the
Python runtime reads as a broken tool, not as *it is already open*, and a double-click is exactly what
starting a second one **is**. The server now answers for itself: if `/api/state` on the port says it is one
of these, it opens the tab and exits 0; if the port is held by anything else it says so in one line and
names `--port`. All three ways in behave the same, and the bundle's probe is now a nicety rather than the
only thing standing between a double-click and a traceback.

🚨 **Going through Terminal is not cosmetic — an app bundle cannot read this repo at all.** The checkout is
under `~/Documents`, which is TCC-protected, and a bundle launched from the Finder gets
`Operation not permitted` on `padmap-serve.py`. **Measured: no permission prompt is ever offered, unsigned
or ad-hoc signed**, so there is nothing for the user to allow and nothing to grant in System Settings
either. Terminal already holds that access. The visible window is the price and it is also where ctrl-c
and the build output live. **Do not "clean this up" by running the server from the bundle directly** — it
fails, and it fails the same way each time with a message that looks like a missing file.

Because the app exits immediately and leaves the server detached, the page carries **Stop server**
(`POST /api/quit`). `shutdown()` blocks until `serve_forever` returns, so it runs on its own thread and not
on the one handling the request.

⚠️ **The served path is not covered by `padmap-test.sh`** — it is `fetch` and streams, which `jsc` has
neither of. What the test does cover is that the new code does not break the `file://` path: `initServed()`
guards on `typeof location`, because a bare read of an undefined `location` at top level would take the
whole headless run down with it.

### 3.2.2 ▶ Play — the last step of the loop, added 2026-07-30

**The row's exit criterion is a person with a pad (§6.2), so the editor's last button is the game.** It
launches RetroArch on the selected port set's game and returns; nothing is measured and nothing is
pinned — it is `~/Desktop/Model 2.app` with the content already loaded and the core known to be this one.
Four decisions, each closing a failure that looks like a bad mapping:

- 🚨 **It launches the repo's dylib BY PATH (`-L <repo>/model2_libretro.dylib`), so the installed-core
  symlink is not in the loop at all.** `~/Library/Application Support/RetroArch/cores/` holds a symlink to
  the build that **has twice reverted to a plain copy on its own** (CLAUDE.md records both), and the
  reverted copy is byte-identical at the moment it happens — so it looks right until the next rebuild, at
  which point you check a mapping against a stale core. RetroArch loads a core from any path and keys the
  core-option file on the core's `library_name` ("Model 2"), not on where it was loaded from, so the
  options survive and the symlink is simply irrelevant. **Do not "fix" this to go through the installed
  core.**
- **The environment is stripped of `M2VK_*` and `M2OPT_*`, by prefix rather than by list.** A switch beats
  its core option by design and `M2OPT_` pins one outright, so either one left over from a harness run
  makes the options menu silently do nothing — the same trap CLAUDE.md's "playing and measuring must not
  share a command line" is about. By prefix, so a switch added later is covered without editing this.
- **It refuses a stale core, and the refusal is the feature.** `--check` must pass (else *Save to core*
  first) and `model2_libretro.dylib` must be newer than `input_layouts.ipp` (else *Rebuild core* first).
  Checking a layout against a core built before the layout is the one failure mode that produces a
  confident, wrong answer.
- **Every refusal that is a judgement call is overridable; the ones that are facts are not.** Stale core
  and "already running from here" come back `409` and the page offers *Launch anyway*; no ROM, no
  RetroArch, no selection come back `400` and are not offered as decisions.
- 🚨 **A launch that DIES is not a click that failed, and the first version reported one as success.**
  `doa` was clicked, RetroArch exited within seconds on a ROM audit failure, and the header said
  *playing doa* — because the POST had returned 200 long before, and the child's output went to
  `devnull`. **RetroArch exits with status 0 in that case**, so the return code says nothing either.
  Fixed by writing the child's output to `/tmp/m2vk-play.log` and adding **`GET /api/playing`**
  (`idle` / `running` / `exited` + the distilled reason); the page polls it for 25 s and reports the
  audit lines verbatim. Verified on `von`, which has no complete dump here: `exited`, `rc 0`,
  `epr-18832.15 NOT FOUND … Fatal error … failed to start`.
  ⚠️ **The discriminator is the core's own `started '<set>'` line, not the presence of scary text**, and
  the first cut got that wrong too: `doa` audits with a `WRONG CHECKSUMS` **warning**, runs perfectly, and
  came back *"doa would not start"* the moment the window was closed. A log that reached `started` means
  the machine ran and the exit is a person quitting.
  ⚠️ **The reason for `doa` was not the button and not the layout** — RetroArch's system directory holds
  the core's *second* rompath (`<system>/model2`) and it was an empty directory, so a set whose parent
  files live in `Polydiver/roms` could not audit under the frontend while every retrohost run of it
  passed. [roms.md](roms.md) carries the fix.

**Which zip:** the port set's own name first (`daytona` → `daytona.zip`), then its GAME entries with the
working ones ahead of the `MACHINE_NOT_WORKING` ones — so a port set named after an entry with no zip
still reaches a playable one (measured: **`dynabb` → `dynabb97.zip`**, the only set here that takes the
fallback), and a flagged parent does not shadow a sibling that runs. `~/Documents/ROMs/Model 2` (what the
Desktop app's playlist uses) is searched before `devnotes/roms`. The page sends the candidate list because
it is the side that holds the driver table; the server holds the ROM knowledge.

⚠️ **The button is hidden when RetroArch is not at `/Applications/RetroArch.app`**, and disabled with
`no ROM for <set>` when nothing playable exists — **4 of the 32 port sets** (`airwlkrs`, `rchase2a`,
`powsled`, `model2crx`), because a button that launches into a ROM-audit failure reads as a broken core.

---

## 4. The schema

```jsonc
{ "id": "daytona",
  "sets": ["daytona"],          // computed by the editor from the driver table; never typed
  "lightgun": false,            // does the set declare IPT_LIGHTGUN_X/Y — see §5.2
  "note": "...",                // becomes a comment in the .ipp
  "verified": "",               // free text; blank renders as "NOT YET MEASURED IN GAME"
  "buttons": [                  // exactly 9, MAME button 1..9
    { "source": "NONE", "label": "", "why": "GEAR N: the pad is out of buttons ..." },
    { "source": "B",    "label": "GEAR 1" } ],
  "labels": { "L2": "Brake", "LSTICK_X": "Steering", ... } }   // per control, only non-empty ones
}
```

`source` is a RetroPad id, `L2_AXIS`/`R2_AXIS`, or `NONE` — exactly the vocabulary
`read_source()` already understands, so **no new mechanism can enter the core through a row**.

The `generic` block is the fallback for every port set with no row, and is **byte-for-byte the old
Classic layout plus the old descriptor strings**. The regression check for this whole change is that
unauthored sets are unchanged; that only holds if this stays exactly what it is. Do not improve it.

---

## 5. What the core does with it

`layout_for(name, parent)` resolves a row once in `input_init()` and **never returns null** — a set with
no row of its own gets the generic one, so there is no "this set has no layout" case at any call site.

⚠️ `game_driver::parent` is the literal string **`"0"`** when a set has no parent, not the empty string.
An unguarded compare looks for a row named `"0"` on every parent set in the tree.

### 5.1 One device type

`RetroPad` and `Light Gun`. `RetroPad (Classic)`, `RetroPad (Modern)` and `RetroPad (Cabinet)` are all
gone, along with the second `retro_controller_info` array that existed only to offer Cabinet to the sets
that had a row. A config remembering a retired subclass id still plays: the pad treats any unrecognised
device value as "use this machine's row".

### 5.2 Descriptors, and the two gaps they close

Built at load from the resolved row. Two [input-map.md](input-map.md) §4 gaps close as a side effect:
**L3 gets a label** (suppressed when `model2_diagnostic_input` is `None`, because it is an inert
`IPT_UI_MENU` then), and the gun ports get **Reload**.

🚨 **The send has to sit BELOW the options read in `retro_load_game()`.** L3's label depends on
`diagnostic`, which is read after where descriptors used to go out. Sent from the old position it would
have labelled a dead control on every default run.

🚨 **The `lightgun` row flag is not optional and its absence was measured, not foreseen.** Without it the
gun descriptors went out on **every** set: daytona's remap screen listed a lightgun trigger called
"GEAR 1" and vf2's listed one called "Punch". `M2VK_HOST_DESCRIPTORS=1` is what showed it.

### 5.3 `M2VK_HOST_DESCRIPTORS=1` — the read-out for the whole feature

New in `retrohost.c`. Prints every descriptor the core sends. It exists because there was **no** read-out
at all: the labels are shown in RetroArch's Quick Menu → Controls, which is interactive and cannot be
checked from a shell. Same argument that built `M2VK_GUN_LOG` when MAME's crosshair turned out to be
undrawable here.

```sh
M2VK_HOST_DESCRIPTORS=1 M2_SYSTEM_DIR=/tmp/m2sys M2_SAVE_DIR=/tmp/d \
  ./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/daytona.zip 10 /dev/null \
  | grep '^descriptor: port=0'
```

Measured 2026-07-30 — this is the deliverable, in the core's own words:

| set | what port 0 is told |
|---|---|
| `daytona` | B GEAR 1 · A GEAR 2 · Y GEAR 3 · X GEAR 4 · Up VR1 (Red) · Right VR2 (Blue) · Down VR3 (Yellow) · Left VR4 (Green) · L2 Brake · R2 Accelerator · stick X Steering · Coin · Start |
| `vf2` | B Punch · A Kick · Y Guard · d-pad Up/Down/Left/Right · stick Move · Coin · Start |
| `vcop` | B Trigger · stick Aim X/Aim Y · gun Trigger · gun **Reload** · Coin · Start |
| `schamp` (generic) | the old generic strings verbatim — Button 1..9, Brake / Button 7, Steering / Stick X |

---

## 6. Verification, and what is left for a human

### 6.1 Done, statically

- **The A/B no-op guard passes on three fixtures with all nine digests byte-exact against
  [ab-baselines.md](ab-baselines.md)**: `vf2` (an authored row) `c3aaa56633c1c4f7` /
  `9c20f1fac9d9fe92` / `de94f44a06151f71`, and `schamp` and `dynamcop` (both unauthored, both on the
  generic row) likewise. Exit criterion 1 holds and real interior disagreements are 0 on all three.
  **That is the proof the 62 unauthored entries are unchanged.**
- `padmap-gen.py --check` — 12 rows naming 15 sets, `.ipp` matches `.json`.
- `padmap-test.sh` — 32 port sets, all rules.
- `M2VK_HOST_DESCRIPTORS` on four sets, §5.3.
- Upstream diff against mame0288 **unchanged** — no upstream file is touched by any of this.

### 6.2 🛑 The rest is the user's, by decision (CLAUDE.md, 2026-07-30)

**Automated button-press testing is banned as a verification method.** It is enormously expensive for
what it returns, and this session proved it: nine 4000-frame `daytona` runs to establish two facts a
human with a pad settles in a minute. What that sweep did establish, for the record:

- ✅ **The collision is fixed, and the negative control works.** With daytona in VR1's in-car view and
  the accelerator floored to 167 km/h, the camera **stays** in VR1's view
  (`screenshots/2026-07-30-input-daytona-collision-fixed.png`). Rebuilt with daytona forced onto the
  generic row, the identical script **snaps the camera back to the chase view**
  (`...-collision-negative-control.png`) — that is the collision, on demand.
- ✅ **VR1, VR2 and VR4 change the camera from the d-pad**, and **VR3 does too** — but only once the game
  is not already in VR3's view.
- 🚨 **And that is the trap: the first collision test was VACUOUS.** At frame 3500 daytona is *already*
  in VR3's view, so pressing VR3 changes nothing, and both arms of the test came back equal — which
  reads exactly like the collision still existing. It took a screenshot and four more runs to tell
  "the fix works" from "the test is blind". This is per-game-input.md §5 step 0 recurring in a new
  costume, and it is the strongest argument for the ban.
- ⚠️ **The gear buttons could not be settled this way and were not.** All four give an identical
  whole-run digest whether pressed or not, and **the car reaches 167 km/h with GEAR N latched**, so the
  transmission is behaving as automatic in this configuration and the game may simply be ignoring the
  gearbox. That is *not* evidence the buttons are unmapped. **It answers per-game-input.md §2.1's open
  question in passing — daytona does not require leaving N to move** — and it is exactly the case the
  lightgun phase said only the game's own INPUT TEST screen can decide.

---

## 7. Deliberately not done

- **Analog routing.** `desert`'s brake off the self-centring stick onto L2 (input-map.md §5.2), and
  `waverunr`/`topskatr`'s two-fields-on-one-axis pairs onto the idle right stick (§5.3, §5.4). The row
  and the editor are shaped for it; it resolves in `configure()` rather than per frame, so it verifies
  differently from the buttons and is its own step.
- **The synthesized gear index** — per-game-input.md §3.4's sequential shifter on L/R. Safe because
  `daytona_gearbox_r` latches: hold the bit, do not pulse.
- **The other 20 port sets.** The table exists to take them one row at a time, with evidence.
- **`srallyc`** — it is entirely `MACHINE_NOT_WORKING` (all five entries), so per-game-input.md step 5's
  "second customer" cannot be verified. `motoraid` took that role.

---

## 8. Two things found while checking the install, both of which would have hidden the feature

- 🚨 **`model2_libretro.info` said `input_descriptors = "false"`.** The core now sends a per-game
  descriptor array, so that flag was actively wrong — and it is the field RetroArch reads to decide what
  the core advertises about its own input. Set to `"true"` in both copies (`devnotes/shortcuts/` and
  `~/Library/Application Support/RetroArch/info/`). **`savestate = "false"` was stale too** — savestates
  landed 2026-07-29, 8 of 8 fixtures — and is now `"true"`.
  ⚠️ Whether that flag *gates* the Controls menu or merely describes the core was not established; it was
  fixed as correctness either way. If the labels do not appear in RetroArch, this is the first thing to
  re-check.
- 🚨 **The core symlink had reverted to a plain copy again, dated a day earlier.** CLAUDE.md records this
  happening twice on 2026-07-28 and it has now happened a third time. Restored. **Check it before
  concluding any change "does not work" in the app** — the copy is byte-valid, so nothing looks wrong:
  ```sh
  ls -la ~/Library/Application\ Support/RetroArch/cores/model2_libretro.dylib   # must print '-> …'
  ```
