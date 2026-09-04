> # 🚫 RETIRED — savestates are DISABLED core-wide (2026-09-04)
>
> `retro_serialize_size()` returns **0** for every family and `retro_serialize` /
> `retro_unserialize` return **false**, so RetroArch greys the save/load slots out. The feature was
> removed as a *guarantee*, not because the code stopped working: it was never uniformly trustworthy
> across the four families (`vcop2` never passed, the Model 1 TGP-copro / `gen_fifo` gap was never
> verified, the pipelined Android path dropped states outright), and gating every renderer change on
> a harness only half the cores could satisfy cost more than the feature returned.
>
> **This file is history from here down.** Do not gate work on it, do not run `state.sh`, do not add
> a savestate row to a new plan's exit criteria. The as-built code — `m2vk_savestate.cpp` with its
> `gen_fifo` trailer, `m2vk_snd::state_*`, `libretro_m2_osd_interface::state_*` — all still compiles
> and still works; only the three ABI bodies in `retro_entry.cpp` were emptied.
>
> Three findings here outlived the feature and are still worth reading: the `gen_fifo` **contents**
> being unsaved upstream (§9.1c), the two display caches that are never invalidated on load (a
> perfectly restored machine can still show a permanently wrong picture), and the reproducibility
> trap — a fixture whose own future is nondeterministic reads as a savestate FAIL.

# Savestates — the plan, and what was built

**Status (2026-07-29, fourth session): BUILT AND MOSTLY WORKING. 7 of 8 fixtures pass.**
**`vf2`, `daytona`, `vstriker`, `sgt24h`, `srallyc`, `desert`, `lastbrnx` pass; `vcop2` does not.**
§9 is the as-built record; **§9.1c is the current session and the one to read first**, then §9.6 for
what is left. §3 is the original plan and is now history.

🚨 **Before believing any FAIL here, check that the fixture's own future is reproducible.** For two
sessions it was not, and nobody had looked: two runs of the identical dirty command could land on
different emulated frames, so `C != D` said nothing about the savestate. Root-caused and fixed in
§9.1c; `state.sh` now runs the control itself and reports `NONDETERMINISTIC` rather than FAIL.

🚨 **Do not start by looking for a missing `save_item` — that question is answered.** Every fixture
loads *faithfully* (`M2VK_SAVE_VERIFY`), so the carrier is outside the registry. §9.1a. The thing that
was outside the registry turned out to be the **`gen_fifo` contents**, which upstream does not save and
which bite in both directions — §9.1c.

⚠️ **The single most important finding is not in the plan below, because the plan did not predict it:
the emulated state can be restored perfectly and the picture still be permanently wrong.** Two display
caches — the decoded character gfx and the tilemaps — are never invalidated on load, and MAME's own
post-load handling misses both on this driver. §9.3. That was daytona's entire failure and it cost
most of the session to find.

---

**Written 2026-07-29, after the lightgun phase closed.**

P1 dropped savestates and wrote down why
([p1-libretro-core.md](p1-libretro-core.md), "Savestates — a finding that changes the plan"), ending
with *"Revisit when the Vulkan path exists and there is a concrete reason to want scene-parking."*
The Vulkan path exists. This is the revisit.

⚠️ **The P1 reasoning was sound but its central fact does not gate what it was thought to gate.**
"No Model 2 set carries `MACHINE_SUPPORTS_SAVE`" is true and still true — but that flag does not
disable `save_manager`, and it is not evidence that registration is incomplete. It is evidence that
*nobody checked*. §1 replaces the inference with an audit, and the audit found **eight specific
missing items**, not an open-ended hazard. That is the finding that makes this tractable.

---

## 1. What is actually true, measured against the tree

### 1.1 `MACHINE_SUPPORTS_SAVE` is advisory. It gates three things, none of them serialisation.

`gamedrv.h:76` defines the flag; `gamedrv.h:119` turns its **absence** into
`device_t::flags::SAVE_UNSUPPORTED` on the driver's own device type. That flag is read in exactly
three places:

| Site | What it does |
| --- | --- |
| `save.cpp:105-112` | sets `save_manager::m_supported`, which drives a UI warning |
| `machine.cpp:250-259` | suppresses the `-autosave` load |
| `device.cpp:555-561` | **`fatalerror` if an execute-interface device registers nothing** — only when the flag is *present* |

**[measured] `do_write` (`save.cpp:409-445`) and `do_read` (`save.cpp:452-490`) contain no
`m_supported` check.** `write_buffer` / `read_buffer` run to completion on this driver today. So the
question was never "is the API available" — it is "is the registered set complete", and that is
answerable by audit.

🚨 **The third row is a free instrument and §3 step 1 uses it.** Adding `MACHINE_SUPPORTS_SAVE` to one
set turns every CPU/execute device that registers nothing into a hard `fatalerror` at start. That is a
one-character way to ask MAME which devices are silent, and the answer arrives before a single frame
runs.

### 1.2 The libretro glue is small, because MAME already has the exact three calls

```
save.h:308   save_error save_manager::write_buffer(void *buf, size_t size);
save.h:309   save_error save_manager::read_buffer(const void *buf, size_t size);
save.h:~349  static size_t ram_state::get_size(save_manager &save);     // sum of entries + HEADER_SIZE
```

`ram_state::get_size` (`save.cpp:598-606`) is `sum(typesize × typecount × blockcount) + HEADER_SIZE`,
and `HEADER_SIZE` is 32 (`save.cpp:50`). That is `retro_serialize_size()` exactly, with no guesswork.

⚠️ **`write_buffer`'s size check is an equality, not a minimum** — `check_space` is
`size == total_size` (`save.cpp:367-370`). A frontend handing us a larger buffer fails. Serialise into
an owned `std::vector<u8>` of the exact size and `memcpy` out; do not pass the frontend's pointer
straight through.

### 1.3 The header already refuses the states that would corrupt

`do_write` writes a 32-byte header carrying the magic, the save version, the endianness, **the
system's short name** (18 bytes at offset 0x0a, `save.cpp:423`) and **a CRC32 of the entire
registry's structure** — every entry's name, type size, count and block count (`signature()`,
`save.cpp:~495`). `do_read` validates all of it and returns `STATERR_INVALID_HEADER` before touching a
byte of machine state.

**That is the property that makes this safe to ship.** A state from a different game, a different
build, or a build where step 4 added registrations is *rejected*, not silently applied. The failure
mode P1 was avoiding — "a savestate that silently corrupts" — is structurally unavailable for
mismatched states. It remains available only for a state whose registry matches and whose *content*
is incomplete, which is §1.5.

### 1.4 Every RAM block is already registered, named or not

`memory_manager::allocate_memory` (`emumem.cpp:322-329`) calls `save().save_memory()` on **every**
block it allocates, so `map(…).ram()` is covered whether or not it carries `.share()`. For a Model 2A
set that is, at minimum:

| Block | Size |
| --- | --- |
| `workram` (`model2.cpp:1048`) | 1 MB |
| `textureram0` / `textureram1` (`model2.cpp:1284-1285`) | 2 MB each |
| `bufferram` (`model2.cpp:1057`) | 128 KB |
| `soundram` (`model2.cpp:2495`) | 512 KB |
| `backup1` (`model2.cpp:1082`) | 16 KB |
| anonymous `.ram()` at `0x200000`, `0x1d80000`, `0x00e00000` | ~320 KB |

plus `geo->polygon_ram0/1` (128 KB each), `raster->texture_ram` (128 KB), `log_ram` (32 KB), the
palette/`colorxlat`/`lumaram` tables, and each CPU's own register file.

**[inferred] ~6–8 MB per state, uncompressed.** Step 1's whole job is to replace that estimate with a
number.

### 1.5 The audit: what is registered, and the eight things that are not

Registration sites: `model2.cpp:161-181` (driver), `model2_v.cpp:285-300` (raster),
`model2_v.cpp:1011-1016` (geo), `model2_v.cpp:2417-2424` (video/palette).

**Devices are in good shape.** Save-item counts, measured:

| Device | Used by | `save_item`/`save_pointer` calls |
| --- | --- | --- |
| `i960` | all | 22 |
| `mb86233`/`mb86234` (TGP) | Model 2O, 2A | 32 |
| `adsp21062` (SHARC) | Model 2B | 85 |
| `scsp` | all | 55 |
| `315-5881_crypt` | protected sets | 11 |
| `315_5649` (I/O) | all | 4 |
| `eepromser` | all | 15 |
| `gen_fifo` | all | 2 |
| `segabill` | all | 1 |
| **`m2comm`** | all | **0** |
| **`mb86235` (TGPx4)** | **Model 2C only** | **1** |

🚨 **`mb86235` registers one item (`mb86235.cpp:222`, `m_core->pr` only) and is the worst gap in the
tree — and it does not matter, because no working set uses it.** All **28** sets in
`src/mame/sega/model2.cpp` without `MACHINE_NOT_WORKING` are `model2o_state` (6), `model2a_state`
(12) or `model2b_state` (10). **Zero are `model2c_state`.** Model 2C is `hotd`, `topskatr`, `stcc`,
`waverunr`, `overrev`, `skisuprg`, `segawski`, `bel`, `rascot2` — every one already
`MACHINE_NOT_WORKING`. ⚠️ **This is the scoping decision the whole plan rests on: savestates are for
the 28 working sets, and Model 2C is explicitly out.** If a 2C set is ever promoted to working, the
TGPx4's registration is a prerequisite and this document is where that is written down.

**`m2comm`'s zero** is the linked-cabinet board. Its shared RAM is unsaved. Networked cabinets are
already rejected in [user-options.md](user-options.md) §"Rejected", so this is a known, accepted gap —
not a bug to fix.

**The driver's own gaps.** Eight items of plain data are declared and never registered:

| Item | Where | Why it matters |
| --- | --- | --- |
| `geo->focus` | `model2.h:832` | `poly_vertex`, set by geo commands. Projection focus. |
| `geo->light` | `model2.h:833` | `poly_vertex`. The light vector every lit polygon reads. |
| `geo->texture_parameters[32]` | `model2.h:836` | diffuse/ambient/specular per texture slot — the table [model2_lighting.md](../../Polydiver/PDDocs/model2/model2_lighting.md) is about. |
| `raster->cur_window` | `model2.h:810` | current window index |
| `raster->clip_plane[4][4]` | `model2.h:811` | 16 `plane`s of float |
| `m_fbvramA` / `m_fbvramB` | `model2.h` | the render-test-mode framebuffers `lastbrnx` uses |
| `m_xoffs` / `m_yoffs` | `model2.h` | CRTC offsets |
| `raster->poly_list` + `poly_sorted_list` | `model2.h:801,804` | see §1.6 — **deliberately not saved, and the plan keeps it that way** |

⚠️ **Three of these are probably already covered and the audit cannot tell which.** `clip_plane` is
rebuilt from `viewport`/`center` whenever the viewport command arrives (`model2_v.cpp:900-917`), and
both of those *are* saved; `cur_window` is reset by `render_frame_start` (`model2_v.cpp:694`);
`m_xoffs`/`m_yoffs` look like configuration rather than state. **Do not decide this by reading —
decide it by §3 step 3's divergence test**, which names the item that actually diverges instead of
guessing at eight.

### 1.6 🚨 The big one: `poly_list` is per-frame, and the libretro save point is a frame boundary

`raster->poly_list` is `polygon[32768]` and `poly_sorted_list` is `polygon *[65536]` — several MB, and
**the second is an array of raw pointers, which cannot be serialised at all** without rewriting it as
indices. `model2_v.cpp:287` has the `save_item(NAME(m_raster->poly_list))` line **commented out**,
which is where upstream stopped.

**The lifetime, measured:**

- `screen_vblank` (`model2.cpp:2440-2456`) calls `geo_parse()` once per frame (or every other frame in
  30 Hz mode).
- `geo_parse` (`model2_v.cpp:2347`) calls `render_frame_start()` **first**, then walks the entire
  display list out of `m_bufferram` synchronously in one scheduler callback and returns.
- `render_frame_start` (`model2_v.cpp:675-697`) zeroes `poly_list_index`, fills `poly_sorted_list`
  with `nullptr`, and resets `min_z`, `max_z`, `polygon_z` and `cur_window`.
- `render_polygons` consumes the list during the screen update.

So the list is **built and consumed inside one emulated frame and rebuilt from scratch at the next
vblank**, from `m_bufferram`, which is a memory share and therefore already saved. The libretro baton
parks the emulation thread in `osd->update()` — *after* the screen update, *before* the next vblank.

⚠️ **This is the claim the plan depends on, it is [inferred] from the code, and step 3 is what
converts it to [measured].** If it holds, the multi-megabyte pointer array is simply not part of the
state, and MAME's own reason for leaving it out stops applying to us: MAME must serialise at an
arbitrary scheduler point, we serialise at exactly one point per frame. **We can make a guarantee MAME
cannot.**

### 1.7 Threading — the save point is already built

`retro_entry.cpp:1-25` describes the baton. `retro_serialize` is called on the frontend thread with
the emulation thread parked in `osd->update()`, which is the same discipline that already makes
`input->poll_frontend()` and `publish_reticles()` safe (`retro_entry.cpp:904-913`). Nothing is running;
`write_buffer` may be called directly from the frontend thread.

⚠️ **Two things to check rather than assume**, both in step 2:
- the CPU cores are between timeslices at that point, so their state should be at an instruction
  boundary — but `i960_stall()` and the `INPUT_LINE_HALT` juggling in `machine_start`
  (`model2.cpp:189-210`) mean the copro FIFOs can be mid-handshake. The FIFO device registers 2 items;
  whether that is its whole state is a step-3 question.
- `model2_renderer` is a `poly_manager` with worker threads. They write only `m_destmap` / `m_fillmap`,
  neither of which is saved, so an in-flight scanline job cannot corrupt a state — but a *load* that
  lands while they are running would be a race. Loading must happen at the same parked point.

---

## 2. What this does and does not buy

**Does:**
- Save/load in RetroArch, the thing a player expects and the reason to do this at all.
- Scene-parking for screenshots and for reaching gameplay without a 900-frame input script — which is
  what the lightgun phase burned time on twice (`vcop` needs coin at 600 and Start pulsed at
  900/1200/1500/1800 to reach a playable state).

**Does not, and these are decisions, not omissions:**

- 🚨 **The A/B harness stays keyed on frame number.** P1's reasoning is untouched and still correct: a
  fixture is `(rom, frame number)` because P0 proved runs are bit-repeatable. Rebuilding
  `ab-baselines.md` on savestate fixtures would put the *measuring instrument* downstream of the thing
  being measured. Savestates may be used to *find* an interesting frame; the fixture that gets recorded
  is still a frame number.
- **Rewind is not a goal.** At ~6–8 MB a state, RetroArch's default rewind buffer holds a couple of
  seconds. Step 6 decides whether to say anything about it; the honest answer is likely "works, costs
  a lot of RAM, not tuned".
- **Netplay is out.** It needs `retro_serialize` to be cheap and deterministic across hosts; ours is
  neither (host float, host endian, and a registry CRC tied to the build).
- **SRAM (`retro_get_memory_data`) is a separate feature and stays out of scope.** NVRAM already
  persists through MAME's own directories under the frontend save dir.

---

## 3. Order of work

Seven steps. Steps 1–3 build the instrument and prove the shape before any state is added, which is
the pattern the lightgun phase used (build the read-out first, because §1.5 removes every other way to
see anything).

### Step 1 — Measure the registry, before writing any entry point

No libretro code. Add `M2VK_SAVE_LOG=1` to the OSD: after the machine starts, print
`registration_count()`, `ram_state::get_size()`, `supported()`, and MAME's own `dump_registry()`
output (`save.cpp`, already exists behind `-verbose`) to the core log.

Then run the free instrument from §1.1: **temporarily add `MACHINE_SUPPORTS_SAVE` to `vf2` and boot
it.** Any execute device that registers nothing `fatalerror`s by name at start.

**Exit:** a size in bytes for `vf2`, `daytona`, `vstriker` (one per board variant), the same number on
two runs of the same game, and a list of silent devices — or the confirmation that there are none.
⚠️ Revert the `MACHINE_SUPPORTS_SAVE` edit; it is an instrument, not a change.

### Step 2 — The three entry points, and the round-trip-in-place guard

`retro_entry.cpp` only. `retro_serialize_size` returns `ram_state::get_size` cached at load;
`retro_serialize`/`retro_unserialize` go through an owned exact-size vector (§1.2). Map `save_error`
onto `false` and log which one.

**The guard this step exists for is not "does it save" — it is "does saving change anything".** Save at
frame N, immediately `retro_unserialize` the buffer just written, run to 2500, and compare the
whole-run digest against a plain run. `dispatch_presave()` / `dispatch_postload()` run real device
callbacks; a save that perturbs the machine shows up here and nowhere else.

**Exit:** `ab.sh vf2 2500` reproduces `16af05bb8d02a9a5` / `55da761fecca5c01` byte-exactly with the
entry points present, and the save-then-immediately-load run matches a clean run's digest.

### Step 3 — The divergence harness, and the answer to §1.5 and §1.6

`retrohost` gains `--save-at <frame> <file>` and `--load-at <frame> <file>`. The test:

```
run A:  0 ────────────────────────────────► 3000        digest A
run B:  0 ──► 1500 [save]                                (state file)
run C:  0 ──► 1500 [load state] ──────────► 3000        digest C
```

**`digest A == digest C` is the exit criterion**, and it is byte-exact rather than a tolerance because
both runs are the same binary on the same host. ⚠️ **Run C must reach frame 1500 by emulating, then
load** — a load into a freshly-booted machine tests less, because much of the state that matters is
identical to boot state and a missing item hides.

This is also what names the missing items from §1.5 individually: run C diverging tells you *that*
something is missing; bisecting by adding one `save_item` at a time tells you *which*. Two fixtures
matter here beyond the usual: **`lastbrnx`** (the only render-test-mode game, so the only one where
`m_fbvramA`/`B` are live — and ⚠️ **it is on the frame-parity bistable list**, so a one-off
disagreement must be re-run before it is believed) and **`vstriker`** (47 % of its frames are dupes,
so it exercises the empty/dupe record paths hardest).

**Exit:** for each of `vf2`, `daytona`, `vcop2`, `vstriker`, `lastbrnx`, `srallyc`, a save/load at
1500 produces a digest byte-equal to the straight run — or a named list of the items that must be
added for it to.

### Step 4 — Close the gaps step 3 named

🚨 **This is the step with a cost outside the code, and it needs a decision before it starts.** The
missing `save_item` calls belong in `model2.cpp:161` and `model2_v.cpp:285/1011`, which are **upstream
files**, and the mergeability golden rule (CLAUDE.md, "Repo / upstream conventions") says the only
edits to upstream files are `#ifdef`-guarded hook calls. **The diff against mame0288 has been 30 lines
since P3 step 8 and this would take it to roughly 42–45.**

🛑 **Decided 2026-07-29: nothing from this repo is ever pushed back to MAME.** That removes the option
this step would otherwise have led with — patch upstream, let the next release-tag merge take the diff
back to 30 — and it makes the added lines **permanent**. It does not change which option to pick, but
it changes the reasoning, so the reasoning is written down here rather than re-derived.

⚠️ **Re-read what the 30-line budget is actually for.** It is not a debt to upstream. It exists so the
monthly release-tag merge (`mame0289…`) stays cheap — conflict surface, nothing else. That reframes the
choice: what matters is not the line count but how likely upstream is to touch those lines.

Two ways out:

1. ✅ **Guarded inline** — `#ifdef M2VK` around the additions at the three existing registration sites
   (`model2.cpp:161`, `model2_v.cpp:285`, `model2_v.cpp:1011`). ~12 lines including guards. **This is
   the choice.** The lines are purely additive, and they sit inside functions that are already nothing
   but lists of `save_item(NAME(…))` — the cheapest conflict surface in the file. `git log
   --since=2023-01-01 mame0288 -- src/mame/sega/model2_v.cpp` is the check worth running once, but
   nothing about these functions invites upstream churn.
2. ❌ **Hook out to a new OSD file** — two guarded call sites, the list in `src/osd/libretro_m2/`.
   Fewer upstream lines (~6) but `geo_state` and `raster_state` are **private nested structs** of
   `model2_state` (`model2.h:40-45`), not nameable outside it, so it needs a `friend` declaration or an
   accessor — itself an upstream edit, in the *header*, which is a worse place to hold a conflict than
   the bottom of `machine_start()`. **Rejected: it trades cheap conflict surface for expensive.**

⚠️ **There is no zero-upstream-line option and it was checked rather than assumed.** Registering from
our side would need both the addresses of private members and a hook that runs before
`allow_registration(false)` (`save.cpp:82`, called after every device has started). We can have neither.

⚠️ **Every addition here changes the registry CRC, so it invalidates every state saved before it.**
That is correct behaviour (§1.3) but it means step 4 must land *before* anything ships, not after.

**Exit:** step 3's divergence test passes on all six fixtures; the upstream diff line count is stated
explicitly in the worklog rather than left to be discovered.

### Step 5 — The renderer and OSD side of a load

MAME's state is not the whole core's state. On a successful `retro_unserialize`:

- 🚨 **The frame record holds the pre-load polygon list, and the renderer redraws a stale list when a
  frame carries no new geometry** — that is P3 step 8's bug, fixed there for the empty-display-list
  case. A load lands in exactly the same shape: the next frame may not submit geometry, and the GPU
  would composite the *old* scene under the new one. **Call `m2vk::geometry_none()`** (the P3 step 8
  path — marks the record valid with `poly_count = 0` and bumps the serial) as part of the load. This
  is one line and it is the whole renderer-side change; everything else the renderer holds is derived
  per frame from state that was just restored.
- **Audio:** drop whatever is in the frame's audio buffer rather than emitting a batch from before the
  load.
- **Input:** nothing to do. The input module is written once per frame from `poll_frontend` before the
  baton is released, so the first post-load frame gets a fresh sample.
- ⚠️ **Nothing about the internal resolution, the pipelines or the texture upload needs touching.** The
  texture sheets are read live through the memory-share pointers (P3 step 4 — deliberately *not*
  snapshotted into the record), so a load that rewrites texture RAM is picked up on the next upload
  with no invalidation of ours.

**Exit:** save on a gameplay frame, load, and the next presented frame is the loaded scene with no
one-frame flash of the old one — checked by dumping the two frames, not by eye.

### Step 6 — The frontend contract

`RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS` (`libretro.h:2716`). The state contains host floats and is
host-endian, and its registry CRC ties it to the build, so
`RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT` (`libretro.h:3633`) and
`RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT` (`libretro.h:3626`) are both honest. **Do not set
`RETRO_SERIALIZATION_QUIRK_INCOMPLETE`** if step 3 passes — it means "known to be incomplete", and
after step 4 that is no longer the claim.

Then decide the rewind posture from step 1's measured size, and check the interactive path in
RetroArch: F2/F4, the state slot UI, and a load across a `context_destroy`/`context_reset` pair
(fullscreen toggle), which is the one lifecycle interaction savestates add.

⚠️ **`retro_serialize_size` must be stable for the whole session.** It is: registration closes at
machine start (`allow_registration(false)`, `save.cpp:82`) and `retro_load_game` blocks until the first
frame. Cache it at load and assert it never moves.

### Step 7 — Docs

The worklog entry, this file struck through as as-built, CLAUDE.md's "Where we are" and "Next step",
and — because this is the item that reopens a closed decision — **a correction in
[p1-libretro-core.md](p1-libretro-core.md)'s savestate section, marked in place rather than rewritten**,
the way `user-options.md` §7 was corrected by the lightgun phase. The P1 text is *why* savestates were
deferred and it should stay legible.

---

## 4. Exit criteria for the phase

1. **A save/load round trip at frame 1500 produces a whole-run digest byte-equal to a straight run**,
   on `vf2`, `daytona`, `vcop2`, `vstriker`, `lastbrnx` and `srallyc`. This is the criterion; the rest
   are guards.
2. `ab.sh vf2 2500` reproduces `16af05bb8d02a9a5` / `55da761fecca5c01` byte-exactly — savestates move
   no pixel.
3. The upstream diff line count is stated, with the decision from step 4 recorded.
4. A state saved before step 4's registrations is *rejected*, not applied (§1.3) — checked once, by
   keeping one old state file around.

## 5. Risks, ranked

1. **§1.6 is wrong** — the poly list turns out to be live across the libretro save point. Then either
   `poly_sorted_list` is rewritten as indices into `poly_list` (upstream-file surgery, and ~8 MB added
   to every state), or the save point moves to a defined boundary inside the emulated frame. **Step 3
   finds this on its first run**, which is why step 3 comes before step 4.
2. **The copro FIFO handshake** (§1.7). `gen_fifo` registers 2 items and the Model 2 setup wires seven
   callbacks per FIFO that assert and clear `INPUT_LINE_HALT` on both CPUs. A load that restores the
   FIFO contents but not the derived halt lines deadlocks the machine. Symptom: loads fine, then no
   geometry ever again. **Look here first if step 3 diverges on a Model 2B set and not on 2A.**
3. **Silent incompleteness in a device we did not check.** Mitigated by step 1's `MACHINE_SUPPORTS_SAVE`
   probe, which asks MAME instead of asking us.
4. **The 30-line budget** (step 4). A cost, not a risk — but it is a decision that belongs to the
   repo's owner and not to the step.

## 6. Open questions, deliberately not answered here

- Does `daytona`'s drive board / `sj25_0207_01` sub-board carry state? It is a Model 2O/2A extra that
  the six-fixture list covers only via `daytona` and `srallyc`.
- Should the core expose `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` for the backup SRAM? Adjacent,
  cheap, and out of scope here — NVRAM already persists through MAME's own directories.
- What happens to a state saved at one `model2_internal_res` and loaded at another? **[inferred]
  nothing** — the resolution lives in the renderer, not in MAME's state — but it costs one run to check
  and belongs in step 5.

---

# 9. As built (2026-07-29)

Everything below is measured. ✅ **Committed as `98b95a21917` ("start working on savestates")** and
`e53bfcc2f91` ("Save state hell", the fourth session's startup baton and FIFO trailer).

🚨 **THE CURRENT RESULT IS 8 OF 8 — read §9.1d first.** `vcop2` was the SCSP envelope phase
(`SCSP_EG_t::state`, unregistered upstream), and it is fixed. Every table and every "what is left"
below §9.1 predates that and is a dated record.

## 9.1 What works

⚠️ **This table is the SECOND session's and is kept as a dated record. The current result is 8 of 8
— see §9.1d.** Three of the four failures below were not what they looked like: `srallyc` was never
broken (the reference future was not reproducible), and `desert` and `lastbrnx` were the `gen_fifo`
contents. Point 2 below is the reasoning that this corrects, so it is left standing rather than
rewritten.

**Eight fixtures, swept sequentially 2026-07-29 at 2000 frames / save point 1500.** 4 pass, 4 fail.

| Fixture | Board | Divergence test |
| --- | --- | --- |
| **vf2** | 2A | ✅ PASS |
| **daytona** | 2O | ✅ PASS |
| **vstriker** | 2B | ✅ PASS — **fixed by `m_timerorig`**, §9.1a |
| **sgt24h** | 2B | ✅ PASS |
| **vcop2** | 2A | ❌ FAIL |
| **srallyc** | 2A | ❌ FAIL |
| **desert** | 2O | ❌ FAIL |
| **lastbrnx** | 2B | ❌ FAIL — ⚠️ and it is on CLAUDE.md's frame-parity **bistable** list, so one FAIL from it is not evidence |

State sizes measured earlier: vf2 8,826,324; daytona 9,058,489; vcop2 8,826,324; vstriker 6,974,280.

🚨 **Two things this table says that are worth more than the pass rate.**

1. **The failures are not board-specific.** Each of 2O, 2A and 2B has both a pass and a fail
   (`daytona`/`desert`, `vf2`/`vcop2`+`srallyc`, `vstriker`+`sgt24h`/`lastbrnx`). Any explanation that
   turns on the copro type is already refuted.
2. **All four failures share one signature: the load is faithful.** `M2VK_SAVE_VERIFY` on `srallyc`,
   `desert` and `lastbrnx` reports exactly what it reports on `vcop2` — nothing outside timer `m_index`
   and the Lua engine's timer. That is evidence for **one remaining bug affecting four fixtures**,
   not four separate missing registrations, and it is the reason §9.6 item 1 says to stop looking for
   another `save_item`.

   ⚠️ **The premise held and the conclusion did not, and the reason is worth carrying: "the four
   failures share a signature" was true, but the signature was shared because it is what a PASSING
   fixture looks like too.** A faithful load is normal. Grouping four fixtures on a property that
   `vf2` also has produced "one bug affecting four", where the truth was three different situations —
   one fixture that was never broken, two with words in the FIFO, and one still open. **A shared
   signature is only evidence of a shared cause if the passing fixtures do not share it**, and that
   control was available the whole time.

Also verified:
- **Saving does not perturb the machine.** `A == R` on every fixture — a run that serialises and
  immediately unserialises the same bytes is byte-identical to one that never saves. This matters
  because `dispatch_presave`/`dispatch_postload` run real device code.
- **Cross-game and cross-build states are refused, not half-applied.** A `vf2` state offered to
  `vcop2` returns `STATERR_INVALID_HEADER` before a byte is touched (§1.3).
- **The size is available before `retro_load_game` returns**, which needed explicit work — see 9.4.

## 9.1a The second session (2026-07-29, later): `vstriker` fixed, `vcop2` narrowed

§9.6's assigned next step was to bisect `vcop2` with `M2VK_SAVE_DIFF` at load N / diff N+1. It ran, and
the answer was that **one frame is already too coarse** — at N+1 all three CPUs have diverged, so no
first mover can be named that way. What came out of redirecting is below; the session log in
[worklog.md](worklog.md) has the full reasoning.

🚨 **The load is faithful, and that is the finding that reorders everything.** The new
`M2VK_SAVE_VERIFY=1` serialises straight back out after `read_buffer` and compares against the bytes it
was handed. On all four fixtures the **only** entries that move are timer `m_index` and the Lua
engine's own timer. So the carrier of the divergence is **outside the registry**, and every "find the
missing `save_item` on the CPU" instinct is aimed at the wrong place.

**The `m_index` churn is benign, proved by fixture rather than by argument** — `vf2`, which passes,
shows the identical churn. `device_scheduler::presave()` (`schedule.cpp:676`) renumbers `m_index` by
position in the active timer list on every write; `postload()` re-sorts by `(expire, m_index)`. The
numbering is regenerated, not restored.

**Two registry gaps found by audit** — cross-referencing every scalar member of `model2.h` against
every `save_item`/`save_pointer` in `model2.cpp` + `model2_v.cpp`:

- 🚨 **`m_timerorig[4]`**, the reload value behind the four hardware timers. `m_timervals` and
  `m_timerrun` are registered upstream and this is not — and it is the worst of the three to miss,
  because `timers_r` recomputes `m_timervals = m_timerorig - elapsed` on **every read**
  (`model2.cpp:107`). A machine that loads a state keeps its own reload values and hands the game a
  wrong countdown from the first poll on. **This is what fixed `vstriker`.**
- **`m_copro_atan_base[4]`**, the fourth TGP table base, missed when the other three went in. Not only
  a table pointer — `copro_atan_base_w` drives the TGP's `gpio0` line from a comparison of slots 0 and
  1 (`model2.cpp:594`).

⚠️ The audit's first pass grepped only `save_item` and produced a false positive on `m_gamma_table`,
which is registered by **`save_pointer`** (`model2_v.cpp:2458`). `m_xoffs`/`m_yoffs` are §9.5's
deliberate exclusion. `raster_state` and `geo_state` audit clean.

**Four hypotheses killed by measurement, each of which would have been plausible to "fix" on
argument.** They are written up in the worklog with their code references; the short form is that all
four are **real unregistered state and none of them is this bug**, so each stays a live hazard for some
other save point or some other set:

| Hypothesis | Why plausible | Measured |
| --- | --- | --- |
| Live anonymous timers | never registered (`schedule.cpp:96`), **deleted** by `postload` (`:705`); MAME *defers* saving rather than refusing (`machine.cpp:889`) and we cannot | none live at the save point on any fixture |
| FIFO contents | `gen_fifo.cpp:54`: *"This is not saving the fifo, let's hope it's empty..."* — and these carry the geometry stream | both FIFOs **empty** on all four; the parked frame boundary is structurally a good save point |
| i960 `m_stall_state.iswriteop` | genuinely unregistered, decides read-vs-write on stall resume (`i960.cpp:698`) | `burst_mode == 0`, `m_stalled == 0` everywhere |
| 315-5881 stream position | nine unregistered members | **vcop2 has no such device** |

**`vcop2` is now characterised as structural rather than transient**: it fails at save points 700, 900,
1100, 1300, 1500 and 1700, and **a coin alone** (`SCRIPT=600:select:20`) is enough — nothing about
starting a game is needed.

🚨 **The leading candidate is now the save POINT, not the save CONTENT, and it is characterised but not
demonstrated.** `osd().update()` runs inside `screen_device::vblank_begin`, a `TIMER_CALLBACK_MEMBER`
(`screen.cpp:1679`) — i.e. inside `device_scheduler::execute_timers()`. **MAME never saves or loads
there**: `handle_saveload()` is called from the scheduler loop *between* timeslices
(`machine.cpp:358`). So our `postload()` relinks and re-sorts the whole timer list while
`execute_timers()` (`schedule.cpp:963`) still holds a reference to the executing timer and may then
call `schedule_next_period()` on it. ⚠️ **Not proven** — the vblank timer re-`adjust`s itself at the end
of its own callback, which sets `m_callback_timer_modified` and would suppress exactly that. Proving or
killing this is the next session's job.

## 9.1b The third session (2026-07-29): the save-POINT hypothesis is dead, and the read-out that was going to confirm it does not discriminate

No code changed. The session was assigned one **measurement**, and it came back negative — which is the
useful outcome, because the fix it would have justified is a change to where the emulation thread parks.

**The mechanism is real and it is self-defending.** `vblank_begin` calls `machine().video().frame_update()`
— our load — **before** it re-`adjust`s its own timer, and `adjust()` sets `m_callback_timer_modified`
(`schedule.cpp:142`), so `execute_timers()` skips `schedule_next_period()` (`schedule.cpp:964`). The
predicted double-advance is suppressed by the very call that would have caused it. Two further hazards
found in the read, both then measured clean: **`m_basetime` is in the registry** (`schedule.cpp:305`), so
`read_buffer` rewrites `execute_timers()`'s own loop variable mid-loop; and **`adjust()` sets
`m_start = scheduler.time()`, which inside a callback returns the un-restored `m_callback_timer_expire_time`**
(`schedule.cpp:332`), re-arming the screen timers off the pre-load timeline.

**Three measurements, all negative:**

1. **No timer's `m_expire` or `m_period` moved** on `vcop2` one frame after a load — only `m_start` on
   three FIFO sync timers and the benign lua one. A relinked or spuriously rescheduled list carries its
   offset forward; nothing is offset.
2. **`m_basetime` is identical** one frame after the load.
3. **The histories are time-aligned at the save point** — the clean and dirty states both taken at frame
   1500 differ in 529 of 4321 entries and `m_basetime` is not among them, so the pre/post-load time mix
   has nothing to mix.

The instrument is **differential against the correct future** (load the dirty state at N, save at N+1,
`M2VK_SAVE_DIFF` against a dirty *reference* run's own N+1 state) rather than against what the state
said — and it needs **`M2VK_SAVE_DIFF_MAX=1000000`**, because timer entries sort late and are invisible
under the default cap. That cap is the reason nobody had looked at them before.

### 🚨 The entry count does not discriminate — the control caught it

`vcop2` (**FAIL**) differs from its reference in **73 of 4321** entries at N+1. So does `vf2` (**PASS**):
**73 of 4321** — and vf2 even has a timer `m_expire` that moved (`scsp_device::timerB_cb`) where vcop2
has none. **"Entries differ at N+1" is normal, passing behaviour.** Without the passing-fixture control,
vcop2's 73 and its three drifting `m_start`s would have read as confirmation.

**What discriminates is *which subsystem*:**

| | at N+1 | where |
| --- | --- | --- |
| `vf2` (PASS) | 73 | **audio only** — MC68000 `audiocpu`, SCSP, `soundram`, sound stream, SCSP timers. The picture digest never sees it, which is why it passes. |
| `vcop2` (FAIL) | 73 | **the main path** — i960 (`m_IP`, `m_PIP`, `m_r`, `m_rcache*`, `m_localtime`, `m_totalcycles`, `m_stalled`, `m_suspend`, `m_nextsuspend`, `m_input.m_curstate`), the TGP's ALU registers and data RAM, `copro_fifo_out/m_empty_triggered`, `workram`, `bufferram`, and the driver's `m_geo_*_start_address` / `m_copro_sincos_base` / `m_timervals[0]` |

**Use the subsystem, never the count.**

**The divergence grows monotonically and never reconverges** (`vcop2`, entries / bytes at N+k):

| k | 1 | 2 | 5 | 20 | 100 |
| --- | --- | --- | --- | --- | --- |
| entries | 73 | 82 | 88 | 125 | 194 |
| bytes | 3427 | 6680 | 15683 | 63489 | 515556 |

The clean and dirty machines differ in 529 entries at N, so the load transfers ~86 % of the gap and then
loses ground.

### Four more candidates closed

- 🚨 **`mb86233::m_stall` (`mb86233.h:107`) is genuinely unregistered** — every other member on lines
  101–105 is saved. **Not this bug**: a within-instruction transient, set by `stall()` from the FIFO
  read-empty callback and cleared in the same `execute_run` iteration at `do_stall:`
  (`mb86233.cpp:1223-1225`), and no device is inside `execute_run` at the save point. Same shape as the
  i960's `iswriteop`. **Added to §9.6 item 7.**
- **The driver's four hardware timers are registered** (`timer/timer_device::generic_tick/0..3`) and do
  not drift, so `elapsed()` agrees and `timers_r`'s countdown is right — **`m_timervals[0]` in the diff
  is a consequence.** The other half of the previous session's `m_timerorig` fix is already correct.
- **Texture RAM is registered** — `memory/:maincpu/0/:textureram0`/`1`, 2 MB each, the two largest
  entries in the registry.
- **The halt handshake is byte-identical in both fixtures at the save point**: TGP `m_suspend` /
  `m_nextsuspend` `0x01` (SUSPEND_REASON_HALT), maincpu and audiocpu `0x00`,
  `copro_fifo_in/m_empty_triggered` `01`, `copro_fifo_out` `00`. vcop2 is not saved in a special halt
  configuration.

### Where to start next

⚠️ **Answered in §9.1c, and the answer is the FIFO — but the route there went through a finding that
invalidates part of the reasoning above. Read §9.1c before acting on this section.** In particular,
the "73 of 4321 entries at N+1, and vcop2's are the main path" measurement was taken at a load point
whose *receiving* machine had six stale words in `copro_fifo_in`; at a clean load point the main path
is byte-correct one frame later.

The carrier is **unregistered state that differs between the clean and dirty machines at frame N and
reaches the i960/TGP path within one frame** — everything registered round-trips faithfully (§9.1a's
`M2VK_SAVE_VERIFY`) and the scheduler is now cleared. The most causally upstream entries in vcop2's set
are the i960's `m_suspend`/`m_nextsuspend`/`m_stalled`/`m_input.m_curstate` and
`copro_fifo_out/m_empty_triggered` — the maincpu↔copro **HALT handshake**. ⚠️ §9.6 records that
handshake as "tested and wrong on both counts", but what was tested was FIFO *emptiness* and live
anonymous timers — **not whether the halt line, the suspend mask and the FIFO's `m_*_triggered` edge
memory stay mutually consistent across a `postload` that re-runs none of the FIFO's edge callbacks.**
Different question, still open.

## 9.1c The fourth session (2026-07-29): the reference future was never reproducible, and the FIFO contents are the bug

Two code changes, both `src/osd/libretro_m2/`, **no upstream file** — the diff against `mame0288` is
unchanged at 123 lines. The worklog entry for this date has the full reasoning; this is the record.

### 🚨 First, the control that had never been run: `state.sh`'s `D` was not reproducible

**Two independent dirty `vcop2` runs, with no savestate activity at all, differ in 133 of 599 frames**,
and five runs partition into exactly two branches. So `C != D` was carrying **no information about the
savestate** — `C` was being asked to reproduce whichever of two futures `D` happened to take.
`vcop2` was not on CLAUDE.md's bistable list, which is the point: nobody had looked.

Diffing two branches' state files at frame 1500 named the carrier at once —
`Video Screen/:screen/0/m_frame_number` and the driver's `m_framenum`. **The two runs are at different
EMULATED frames at the same HOST frame.**

**The cause is a wall-clock measurement inside ROM loading.** `romload.cpp:649` calls
`set_startup_text(text, force=false)`, which (`ui.cpp:916`) pumps `video_manager::frame_update()`
whenever more than a **tenth of a wall-clock second** has passed. Each pump reached `osd().update()`,
parked on the baton, and cost `retro_run()` a frame — so the frontend got **5 or 6** duplicate startup
frames depending on how fast the ROMs loaded, and host frame *k* mapped to emulated frame *k−6* or
*k−7* thereafter. Measured as `frame − update_count`, fixed within a run, a coin flip between runs.

**Fixed in `libretro_m2_osd.cpp::update()`**: an update reached before `machine_phase::RUNNING`
returns without parking. After: **8 `vcop2` runs give offset −1 and one identical digest.**

⚠️ **Every whole-run digest in `ab-baselines.md` and `res-baselines.md` moves**, because a
fixed-length run now covers different emulated frames. Regenerate with `ab-table.py` / `res-table.py`;
never retype one. The committed polytap fixture is keyed on *rendered* frames, which is emulation
side, and is unaffected.

### `state.sh` had a second, independent design bug

`B` (which wrote the state) and `D` (which produced the reference digest) were **two separate runs**,
silently assumed to be the same machine history. **`D` now does both in one run**, and a new **`E`**
re-runs the dirty command and reports `🚨 NONDETERMINISTIC` — *not* FAIL — when `D != E`.

### The bug itself: `gen_fifo` contents, and it bites in BOTH directions

With the noise gone, the receiver probe correlates perfectly. One `vcop2` state loaded at eight
consecutive frames gives exactly two outcomes, partitioning **8 of 8** on the *receiving* machine's
`copro_fifo_in` occupancy (6 → one digest; 7 or 8 → the other). And across all eight fixtures,
**`desert` (4 words) and `lastbrnx` (6) are the only two whose FIFO is non-empty when the state is
taken — and both failed.**

So `gen_fifo.cpp:54`'s *"This is not saving the fifo, let's hope it's empty..."* costs us twice: words
**lost on the way out**, and the receiver's stale words **left behind on the way in**.

⚠️ **This corrects §9.1a's "FIFO contents: both FIFOs empty on all four".** True of what it looked at,
false in general — and the instrument that would have said so was already in `state_save` and had
never been read across the whole set.

**The fix is a trailer appended to MAME's buffer** (`m2vk_savestate.cpp`), not a registry entry, so it
costs no upstream line: `peek()`/`size()`/`clear()`/`push()` are all public. `state_size()` now
reports MAME's size plus a fixed slab per FIFO; `read_buffer`/`write_buffer` are still handed exactly
their own size. On load: **`clear()` first — that is the half that is easy to miss, and it is what
`vcop2` needs — then replay.**

🚨 **Order: the FIFO work happens AFTER `read_buffer`, deliberately.** Doing it before would leave the
`set_input_line` calls its callbacks make (`m_empty_cb`, `m_on_fifo_unfull`, `m_on_fifo_unempty`)
queued behind a `scheduler().synchronize()` **temporary** timer when `dispatch_postload` ran — and
postload *deletes* temporary timers (`schedule.cpp:705`) while `device_input::m_qindex` is in no
registry. That leaves a queue with a pending event and no timer to drain it, and `set_state_synced`
only arms a new timer when the queue was empty (`diexec.cpp:684`), so **the HALT line would stop
responding for the rest of the session.** After `read_buffer` there is no postload left to run.

### Result: 7 of 8, and every verdict is now trustworthy

`srallyc` flipped to PASS from the nondeterminism fix alone — it was never broken. `desert` and
`lastbrnx` from the trailer, with the logs naming the exact words carried and restored.

| | vf2 | daytona | vstriker | sgt24h | srallyc | desert | lastbrnx | vcop2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |

Confirmed by a full sequential sweep of all eight with both fixes in, 2000 frames / save point 1500,
**`D == E` on every one** — the first time the harness's own reference has been checked.

### ⚠️ `vcop2` is the one left, and what is now known about it

🛑 **ANSWERED IN §9.1d, and the halt-line lead this paragraph hands over is WRONG.** It was not the
halt line, the suspend mask or the FIFO edge memory. It was the **SCSP envelope phase**, and the
machine's whole non-sound half was byte-identical the entire time. Kept as the dated record of what
the fourth session knew.

Its own FIFO is **empty at save**, so nothing is carried; the load drops 6 stale receiver words and
the digest **does not move**. So the receiver-occupancy correlation above is real but the occupancy is
a *proxy* for something else on this fixture. What the next session should NOT redo: the registry is
complete by audit and round-trips faithfully (§9.1a), the save point is cleared (§9.1b), the reference
future is now reproducible, and the FIFO contents are now carried. The live lead is still the
**halt-line / suspend-mask / `m_*_triggered` consistency** question at the end of §9.1b — but now
against a machine whose FIFOs agree, which is a much sharper question than it was.

## 9.1d The fifth session (2026-07-29): 8 of 8 — `vcop2` was the SCSP envelope phase

**`SCSP_EG_t::state`, the envelope generator's attack/decay1/decay2/release phase, is not registered
by `scsp.cpp`.** Every other field of that struct is. A loaded state therefore keeps the *receiving*
machine's envelope phase for all 32 slots, and on `vcop2` that is the whole failure.

| | vf2 | daytona | vstriker | sgt24h | srallyc | desert | lastbrnx | vcop2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

### 🚨 The instrument that should have been first: the per-frame picture hash

`M2VK_HOST_FRAME_HASH` existed since the fourth session and had never been pointed at `vcop2`. Two
runs with it reframed the problem completely:

- **The load is byte-perfect for 486 frames.** 1501–1986 hash-identical to the dirty reference; first
  difference at **1987**, and it never recovers. Every earlier session was investigating a machine
  that loads wrong. It loads *almost* right and drifts.
- **The save point is not the variable — the dirtying script is.** At save point **1200 the fixture
  passes outright**: 0 of 999 frames differ, negative control 999 of 999. 🛑 **This retires §9.6
  item 1's "it fails at every save point from 700 to 1700"** — that was measured with one fixed script
  and a whole-run digest, and read a script property as a save-point property.
- **One press carries it, and it is the gun trigger.** `1300:a` alone passes; `1400:b` alone fails at
  1987. ⚠️ **The `a` arm was vacuous** — RetroPad A is `IPT_BUTTON2`, which `vcop2` does not declare
  (`model2.cpp:1994`, only `IPT_BUTTON1` per player). RetroPad B is `IPT_BUTTON1`, the trigger.
  **Firing the gun is what creates the state the save does not carry.**

### The registry diff, with the control run FIRST

Two independent dirty runs diffed against each other: **0 of 4321 entries, 0 bytes.** The machine is
fully deterministic, so every entry in the real diff is signal. §9.1c wrote that rule after it had
cost two sessions; this is the first time it was applied before the fact.

⚠️ **The first attempt at the diff was wrong, and it was CLAUDE.md gotcha 7.** Reusing an
`M2_SAVE_DIR` between the two runs put `:eeprom`, `global/m_coin_count`, `memory/:backup1`,
`memory/:workram` and `:copro_tgp/m_totalcycles` in the diff — five entries that read exactly like a
main-path divergence and are entirely NVRAM history. **Fresh save dirs on both sides, every time.**

With that fixed, the diff one frame after the load is **34 of 4321 entries and every one is sound**:
SCSP `DSP.TEMP`/`DSP.EFREG`/`RINGBUF`, eight slots' `EG.volume`/`active`/`cur_addr`/`nxt_addr`/`udata`,
the two sound-stream output buffers, one FIFO `sync_full` timer, and the known-benign lua timer.
**The i960, the TGP, workram, the video registers and the tilemaps are byte-identical.** Scanned
forward at 1550, 1700 and 1850 the shape never changes — the audio 68000 and 205430 of `soundram`'s
524288 bytes join in, the SCSP timers drift, and **nothing outside the sound subsystem ever differs**,
right up to the frame the picture changes.

### Why an unsaved envelope phase reaches the *picture*

- **`UpdateSlot` takes a different volume path in `SCSP_ATTACK`** (`scsp.cpp:1250`): attack applies
  `EG_Update` linearly, every other phase goes through `m_EG_TABLE`. `EG.volume` walks a different
  curve from the first sample — the eight slots in the N+1 diff.
- 🚨 **The phase is readable by the sound CPU.** `SGC` in the slot status register is
  `(slot->EG.state) & 3` (`scsp.cpp:924`). The sound driver polls it, branches, and writes different
  things to `soundram` — the 1550 divergence — and the main CPU eventually waits on the sound side,
  which is the 487-frame delay before the picture moves.

**Two neighbours ruled out rather than assumed.** `SCSP_SLOT::Prev` is also unregistered but is
written to zero in `StartSlot` and read nowhere in this tree — interpolation is dead code here. The
LFO `table`/`scale` pointers are unregistered too, and `device_post_load` already re-runs
`Compute_LFO` for all 32 slots; upstream handled that one.

### The fix, and why it is deliberately NOT `#ifdef M2VK`

`scsp.h`: `enum SCSP_STATE` moves `private` → `public` so `ALLOW_SAVE_TYPE(scsp_device::SCSP_STATE)`
after the class can name it (`ay31015.h:166` is the precedent). `scsp.cpp`: one
`save_item(NAME(m_Slots[slot].EG.state), slot)` plus its comment. **+11 lines, −2.**

⚠️ **A guard here would be inert.** `M2VK` is defined on the **`mame_model2` driver project only**
(`scripts/target/mame/model2.lua:82`); `scsp.cpp` compiles into `liboptional`, where it does not
exist, so an `#ifdef M2VK` fix would not be compiled at all. It is also a plain upstream bug —
correct for Model 2, Model 3, Saturn and ST-V alike — so it is unguarded by decision, not by omission.

### Verified

- **8 of 8**, one sequential sweep at 2000/1500, every fixture green on all three controls
  (`D == E`, `N != D`, `A == R`).
- `vcop2` also passes with a **1500-frame future** (3000/1500) and **through the Vulkan path**
  (`VK=1`) — not a 500-frame accident, and not renderer-dependent.
- **A/B no-op guard reproduces `ab-baselines.md` byte-exactly**: background `c3aaa56633c1c4f7`
  identical across renderers, software `9c20f1fac9d9fe92`, vulkan `de94f44a06151f71`, coverage
  agreement 1.0000, real interior disagreements 0, SSIM covered 0.996985.
- ⚠️ **The state file grew 128 bytes** (`vcop2` 8826884 → 8827012; 32 slots × 4), so **every state
  written before this change is refused** — measured, not assumed: the core prints
  `refusing to load, need 8827012 bytes and was offered 8826884`, `retro_unserialize` returns false,
  and the run continues. §1.3's header doing its job.

## 9.2 The instruments, which are most of the value

- **`M2VK_SAVE_VERIFY=1`** (2026-07-29) — after a load, serialise straight back out and name every entry
  that did not come back the way it went in. It answers the one question the N+1 diff cannot: *did the
  load restore what it was handed*, as opposed to *did the machine then run differently*. 🚨 It is not
  the tautology it looks like — `read_buffer` runs `dispatch_postload`, whose device callbacks recompute
  derived fields, so an entry can legitimately come back different.
- **`M2VK_SAVE_DUMP=<substr>`** (2026-07-29) — the serialised bytes of every entry whose name contains
  the substring. The companion to the diff: the diff says *which* entry, this says *what is in it*,
  which is what a question like "was the CPU mid-burst-stall when this was taken" needs.
- **`M2VK_SAVE_PROBE=<substr>`** (2026-07-29) — the machine's **live** condition rather than a
  recording: every FIFO's occupancy, every execute device's suspend mask and HALT line, the screen
  frame number, the scheduler time, the phase and the pause flag, plus the live bytes of any registry
  entry matching the substring. 🚨 It answers the one question no state-file comparison can — *is the
  RECEIVER ready to accept a load* — because the receiver's condition is by construction in no state
  file. It is what found the FIFO (§9.1c).
- **`M2VK_SAVE_PROBE_FROM/TO=<frame>`** (2026-07-29) — the same probe every frame over a window with
  no savestate activity at all, so the condition can be traced across the frames where a load works
  and the frames where it does not **in one run**, where two runs could differ for other reasons.
- **`M2VK_HOST_FRAME_HASH=<frame>`** (retrohost, 2026-07-29) — a per-frame picture hash. 🚨 The
  whole-run digest cannot distinguish "diverged at k and recovered" from "never recovered", and those
  are different bugs; it fails identically loudly for both. Align two runs by subtracting each one's
  load point.
- **`M2VK_SAVE_DIFF_MAX=<n>`** (2026-07-29) — raises the 40-entry print cap. §9.6 item 6, and it was
  needed within the hour: **timer entries sort late in the registry and are invisible under the
  default.**
- **`devnotes/state.sh`** — the divergence harness. Five runs; passes only if the loaded state
  reproduces a *different* machine history's future **and** that history really was different. 🚨 The
  negative control is the whole point: without it the obvious test passes with a `retro_unserialize`
  that restores nothing.
- **`M2VK_SAVE_DIFF=<file>`** (`m2vk_savestate.cpp`) — serialise, compare against a reference state
  file, and **name the registry entries whose bytes differ**. This is what turned the problem from
  guess-and-rebuild (a four-minute cycle per guess, unbounded guesses) into a direct read-out. It is
  the reason 9.3 was found at all.
- **`M2VK_SAVE_LOG=1`** — registry count, size, the ten largest entries, and whether registration has
  closed.
- **`retrohost`**: `M2VK_HOST_SAVE_AT`, `LOAD_AT`, `ROUNDTRIP_AT`, `DIGEST_FROM`.

## 9.3 🚨 The finding: state complete, caches stale

daytona's failure was **not** a missing `save_item`. `M2VK_SAVE_DIFF` between two states taken at the
same emulated frame — one from a straight run, one from a run that had loaded a state 20 frames
earlier — showed **5 of 5397 entries differing, 9 bytes total**, and none of it emulated hardware (two
`m_bank_count` bytes and the *Lua engine's* timer). The machine was restored. The picture never
reconverged.

Two display caches, both upstream gaps, both invisible to MAME because no Model 2 set claims
savestate support:

1. **`device_gfx_interface::interface_post_load()` (`digfx.cpp:106`) early-returns when
   `m_gfxdecodeinfo` is null.** `segas24_tile_device` has none — it calls `set_gfx()` directly
   (`segaic24.cpp:98`) with a `gfx_element` built over its own `char_ram`. So the decoded character
   cache is never marked dirty and restored `char_ram` is simply not looked at.
2. **`tilemap_t::postload()` (`tilemap.cpp:670`) calls `mappings_update()` and *not*
   `mark_all_dirty()`.** The device marks tiles dirty only from its write handler
   (`segaic24.cpp:533`), which a state load bypasses.

**Fixed from our side, at zero upstream cost**, because both APIs are public:
`machine.tilemap().mark_all_dirty()` plus a walk of `gfx_interface_enumerator` calling
`gfx_element::mark_all_dirty()`. In `m2vk_savestate.cpp`'s `state_load`.

⚠️ **Generalise the lesson, not the fix: "the state round-trips" and "the machine behaves the same"
are different claims, and only the second one matters.** Any derived cache rebuilt lazily off a dirty
flag is a candidate. The reason this is worth writing down at length is that every instinct says to
look for a missing `save_item`, and for daytona there wasn't one.

## 9.4 Other things measured that would otherwise be rediscovered

- 🚨 **`update()` is reached twice before the save registry closes** — the registry passes through 15
  and 26 entries before settling at 4319 on vf2. A `retro_serialize_size()` answered from the first
  frame returns **189 bytes** and a frontend caches it for the session. `state_size()` therefore
  refuses to cache while `save_manager::registration_allowed()` is true, and `retro_load_game` spins
  frames until it is false. `registration_allowed()` is the exact condition; a frame count or a
  machine phase is a proxy for it and would be a guess.
- **The `MACHINE_SUPPORTS_SAVE` probe came back clean.** Temporarily flagging `vf2`, `daytona` and
  `vstriker` turns `device.cpp:555` into a `fatalerror` naming any execute device that registers
  nothing. None fired on 2O, 2A or 2B — **every execute-interface device registers state**. The
  sizes were byte-identical with the flag set, confirming it adds nothing to the registry. The flag
  was reverted; this is a re-runnable probe, not a change.
- ⚠️ **`state.sh`'s save directories must be keyed on the GAME, not just the run tag.** They were not,
  and running three games into one output directory gave each game the previous one's NVRAM — **three
  false FAILs**, indistinguishable from an incomplete registry. That is CLAUDE.md gotcha 7; this file's
  own header quoted it and the script still got it wrong. Fixed.
- ⚠️ **`m2vk::geometry_none()` is called on a successful load**, so the renderer cannot composite the
  pre-load polygon list under the post-load frame. Same shape as P3 step 8's stale-3D bug.
- **Quirks declared: `PLATFORM_DEPENDENT` only.** Deliberately *not* `ENDIAN_DEPENDENT` (MAME records
  the writer's endianness and flips on read, `save.cpp:472`), not `MUST_INITIALIZE` (the size is ready
  before load returns), not `INCOMPLETE`.
- **macOS ships bash 3.2, where expanding an empty array under `set -u` is an error.** `VKFLAG=()` +
  `"${VKFLAG[@]}"` aborts every software-path run.
- 🚨 **Running `state.sh` on several games CONCURRENTLY is not trustworthy** (2026-07-29). Four
  backgrounded invocations reported `vf2` FAIL; run sequentially it passes twice with identical digests
  in all five slots. Each run already has its own `M2_SAVE_DIR`, so this is **not** gotcha 7 — something
  about wall-clock or contention leaks into the run. **Sweep sequentially.** A parallel sweep is how one
  session nearly recorded a regression in the fixture that was already known good.
- ⚠️ **`Sega 315-5338A I/O Controller/:billboard:io/0/m_cmd` is run-to-run nondeterministic on vcop2**
  (2026-07-29). Two clean runs with **no savestate activity at all** differ in that byte and nothing
  else, with identical digests. It appears in every vcop2 diff and means nothing. Establish the no-op
  control before reading a one-byte difference as a finding.

## 9.5 The upstream diff: 30 → 123 → 134 lines, and now FIVE files

⚠️ **Re-measured 2026-07-29 (fifth session).** The two driver files are **unchanged at 123
insertions** (`model2.cpp` 37 + `model2_v.cpp` 86, 0 deletions). §9.1d adds **two files that are not
the driver**: `src/devices/sound/scsp.cpp` **+6** and `src/devices/sound/scsp.h` **+5 −2**. Total
against `mame0288`: **134 insertions, 2 deletions across five files**, and the scsp.h hunk is the
first *modification* rather than a pure addition — the enum moves section.

🚨 **The count command in this section is now wrong on its own terms**, because it names only the two
driver files. Count all five:
`git diff --numstat mame0288 -- src/devices src/mame`.

⚠️ **And the sentence "the two files named are the only upstream files this fork touches" is dead.**
The reason it had to give way is in §9.1d: `M2VK` is defined on the driver project only, so a device
fix cannot be guarded into existence — see that section before proposing a guard for it.

The pre-existing measurement, kept: **`git diff --stat mame0288 -- src/mame/sega/model2.cpp
src/mame/sega/model2_v.cpp` reports 37 + 86 = **123 insertions**, 0 deletions.** The **93** that stood
here — itself a correction of an unreproducible "112" — was measured against the working tree before
the savestate work was committed at `98b95a21917`. Two successive numbers in this section have now gone
stale, which is the argument for the instruction rather than against it: **count it against the tag, do
not carry it forward.** The two files named are the only upstream files this fork touches.

22 registrations across three sites, all `#ifdef M2VK`, all additive:
`m_cmd_data`, `m_driveio_comm_data`, `m_gearsel`, `m_lightgun_mux`, `m_prot_a`, **`m_timerorig`**
(`model2.cpp` `machine_start`); the TGP bank register and **all four** table bases —
`m_copro_tgp_bank_reg`, `sincos`, `inv`, `isqrt` and, since 2026-07-29, **`atan`**
(`model2_tgp_state::machine_start`);
`m_fbvramA`/`m_fbvramB` — **512 KB each of ordinary CPU-mapped memory, the largest single omission** —
plus `m_crtc_xoffset`/`m_crtc_yoffset`/`m_palette_dirty` (`video_start`); `m_raster->cur_window`; and
`m_geo`'s `focus`, `light` and the 32 `texture_parameters` slots.

⚠️ **93 is more than it should be — about half is comment.** With no upstream push (decided
2026-07-29) the lines are permanent, and the budget's purpose is merge-conflict surface. **Trimming
the comments down to a pointer at this file is an open chore.**

Deliberately still absent, with reasons: `poly_list`/`poly_sorted_list` (§1.6 — per-frame, and the
second is an array of raw pointers); `clip_plane` (recomputed from saved viewport/center registers);
`model2_renderer`'s private *copy* of the CRTC offsets (re-pushed on every register write, and equal
across any two boots of the same set).

## 9.6 ⚠️ What is NOT done

🚨 **Item 1 is CLOSED — `vcop2` passes and the fixture set is 8 of 8 (§9.1d).** Its text is kept
below as a dated record, and two claims inside it are now known false: **"it fails at every save point
from 700 to 1700"** (it passes at save point 1200 — the variable was the dirtying script, not the save
point) and the halt-line framing the last paragraph hands over. Items 3 and 7 are still live.

1. 🛑 **CLOSED (fifth session). The carrier was `SCSP_EG_t::state`, the envelope phase**, unregistered
   upstream. "The carrier is outside the registry" was right; every guess about *which* carrier was
   wrong. Dated text follows.

   ⚠️ **`vstriker` is FIXED** (`m_timerorig`, §9.1a). **`vcop2` still fails**, and the framing in the
   line that used to sit here — "deterministic and not a save-point race", leading-suspect the copro
   FIFO's `INPUT_LINE_HALT` handshake — **has been tested and is wrong on both counts**. The FIFOs are
   empty at the save point and no anonymous timer is live; the registry round-trips faithfully
   (`M2VK_SAVE_VERIFY`); the driver's registry is complete by audit; and it fails at *every* save point
   from 700 to 1700 with **a coin alone** as the dirtying input. The carrier is outside the registry.

   ✅ **The save-POINT hypothesis is DEAD (2026-07-29, third session) — see §9.1b.** Measured three
   ways: no timer's `m_expire` or `m_period` drifts anywhere across a load, `m_basetime` (which *is*
   registered, `schedule.cpp:305`) is identical one frame later, and the two histories are time-aligned
   at the save point so the pre/post-load `machine().time()` mix in `vblank_begin`'s tail is a no-op.
   🛑 **Do not move the save point** — the pause/resume refactor sketched below is not justified, and it
   would move the load-bearing piece of the OSD threading model to buy a dead theory.
   ✅ **`desert` and `lastbrnx` are FIXED (fourth session, §9.1c) — the `gen_fifo` CONTENTS, which
   upstream does not save.** They are the only two fixtures whose `copro_fifo_in` is non-empty when the
   state is taken, and a trailer that carries the words fixes both. ⚠️ **`srallyc` was never broken** —
   it flipped to PASS from the nondeterminism fix alone. **Only `vcop2` is left**, and §9.1c's closing
   paragraph says what is now excluded and what the live lead is.
2. ~~**`lastbrnx` and `srallyc` were never measured.**~~ Measured 2026-07-29 — both now **PASS**.
   ⚠️ **The "`lastbrnx` is bistable so one FAIL is not evidence" caution was right for the wrong
   reason.** The bistability was not `lastbrnx`'s and not frame parity in `draw_framebuffer` — it was
   ours, a variable number of startup frames handed to the frontend, and it affected every set. §9.1c.
3. **No RetroArch interactive check** — F2/F4, the state-slot UI, or a load across a
   `context_destroy`/`context_reset` pair.
4. ⚠️ **The A/B no-op guard no longer reproduces the recorded digests, and that is the fourth
   session's fix rather than a regression.** Not parking on the baton before `machine_phase::RUNNING`
   means a fixed-length run covers different emulated frames, so **every whole-run digest in
   `ab-baselines.md` and `res-baselines.md` moves**. What must still hold is the *relation*: software
   and vulkan agreeing with each other, and the background reference identical across renderers.
   **Regenerate both baseline files with `ab-table.py` / `res-table.py`; never retype a number.** The
   committed polytap fixture keys on *rendered* frames, which is emulation side, and is unaffected.
   (The values recorded here before that change were `16af05bb8d02a9a5` / `55da761fecca5c01`,
   background `6b831e519ff46d42`, SSIM covered 0.9963 — kept as a dated record, not as a target.)
   ✅ **Run at this tree and the relation holds**: background `c3aaa56633c1c4f7` **identical across
   renderers**, software `9c20f1fac9d9fe92`, vulkan `de94f44a06151f71`, coverage agreement 1.0000,
   real interior disagreements 0, exit criterion 1 holds, SSIM covered 0.996985. 🚨 The first of those
   is the one to read — an identical `M2VK_NO_3D=1` background across renderers is what makes every
   coverage number in `ppmdiff.py` mean anything.
5. ~~**Nothing is committed.**~~ ✅ **Committed as `98b95a21917`**, and the fourth session's startup
   baton + FIFO trailer as **`e53bfcc2f91` ("Save state hell")**. ⚠️ **The fifth session's SCSP fix
   (§9.1d) is UNCOMMITTED** — `src/devices/sound/scsp.{h,cpp}`, two files this fork had never touched.
6. ~~**The `M2VK_SAVE_DIFF` cap of 40 printed entries** is fine for diagnosis and useless for a full
   audit.~~ ✅ **DONE 2026-07-29 — `M2VK_SAVE_DIFF_MAX=<n>`.** It was needed within the hour of being
   written: **timer entries sort late in the registry and are invisible under the default cap**, which
   is exactly the class of entry the remaining `vcop2` question turns on.
7. **Four unregistered fields are known, real, and deliberately left alone** because measurement says
   none of them is the current bug — ⚠️ **and a fifth was on this list's blind side and turned out to
   BE the bug: `SCSP_EG_t::state` (§9.1d), now fixed.** The list was built by auditing the *driver* and
   the devices the driver talks to directly; the SCSP was never audited because nothing about a sound
   chip looks like it can change a picture. **It can, whenever the sound CPU can read the field back**
   — which is exactly what `SGC` in the slot status register does. Two more of the same species are
   named and dismissed in §9.1d (`SCSP_SLOT::Prev`, the LFO table pointers).

   The original four stand, and each is a live hazard for some other save point or set that would
   otherwise be rediscovered from scratch: the i960's **`m_stall_state.iswriteop`**
   (`i960.cpp:2323-2327` saves the other five; it decides read-vs-write on stall resume at `:698`), the
   TGP's **`mb86233::m_stall`** (`mb86233.h:107` — every other member on lines 101–105 is saved; added
   2026-07-29, §9.1b), the **generic FIFO contents** (`gen_fifo.cpp:54`, upstream's own *"let's hope
   it's empty"*), and the **315-5881** protection chip's nine stream-position members. ⚠️ **The first
   two are the same shape and are safe for the same reason** — both are within-instruction transients,
   cleared before `execute_run` returns, and no device is inside `execute_run` at our save point. They
   become live the moment the save point moves. Fixing the first three needs upstream lines; the FIFO
   one could be done from our side by serialising the queues into a side-channel.

---

# 10. System 21 (T5, 2026-08-24)

The savestate module is **driver-agnostic** — MAME's registry via `write_buffer`/`read_buffer` plus the
generic FIFO trailer (`m2vk_savestate.cpp`, which enumerates every `generic_fifo_u32_device` by type, so
it covers whatever a driver has, or nothing). No System 21 code was needed to make it apply; the work
was to run `state.sh` against the family and see what falls out.

`state.sh` gained a `CORE=` override and a fallback to `devnotes/roms/system22/` (the S21 sets live there
alongside the S22 ones and their C67/C68 BIOS), so:

```sh
CORE=$PWD/namcos21_libretro.dylib ./devnotes/state.sh winrun 3000 1500 /tmp/s21state
```

**Result: all 4 fixtures PASS with zero code changes.**

| Fixture | Driver | Save size | Divergence test |
| --- | --- | --- | --- |
| **winrun** | `namcos21.cpp` | 5,909,875 | ✅ PASS |
| **winrungp** | `namcos21.cpp` | 5,909,875 | ✅ PASS |
| **cybsled** | `namcos21_c67.cpp` | 4,111,509 | ✅ PASS |
| **starblad** | `namcos21_c67.cpp` | 4,111,509 | ✅ PASS — but only after §10.1 |

None of these drivers uses a `generic_fifo`, so the trailer is a no-op here and the coverage comes
entirely from the registry. `A == R` (saving does not perturb) and `D == E` (the dirty future is
reproducible) on all four — no nondeterminism, unlike the two sessions §9.1c burned on that for Model 2.

## 10.1 `starblad` — two FALSE failures, and the measurement discipline that cleared them

starblad FAILed twice before it passed, and **both FAILs were artefacts of the test, not the savestate.**
Written up because each is a trap the next carrier-hunt will otherwise re-walk.

**False FAIL 1 — the dirtying script held input past the save point.** The script had
`2300:b:600` / `2400:lx+:400`, whose holds run *through* the save frame 2600. The dirty run keeps
pressing after 2600; the loaded clean run (C) has no script to replay, so C and D diverge on **input**,
not on state. The fix is the same rule state.sh's default script already obeys: **every press must END
before the save point.** N != D still holds (the game diverged), so state.sh cannot catch this on its own.

**False FAIL 2 — CPU contention.** The re-run with the corrected short script was launched while a
*different* starblad run was still going. Two emulations fighting for cores land on different results —
CLAUDE.md's contention gotcha — and it reads exactly like a savestate bug. **Run carrier tests one at a
time.**

**Cleared by the contamination-free measurement, which is the real lesson:**

- **`M2VK_SAVE_VERIFY` on the load: only the benign timer `m_index` / lua-engine churn** (the identical
  signature a *passing* Model 2 fixture shows). Everything real round-trips.
- **A single-history diff at N+1 AND N+300 is byte-identical** (only the benign lua timer). One dirty run
  saves both the load-state (2600) and the reference future (2601 / 2900) — `M2VK_HOST_SAVE_AT` +
  **`M2VK_HOST_SAVE_AT2`**, added to retrohost for exactly this. A byte-identical state on a deterministic
  machine cannot diverge later, which is proof the registry is complete.
- **The direct C-vs-D digest passes:** dirty save@2600 / digest-from-2601 == clean load@2600 /
  digest-from-2601 (`f9fb66e77ac84df7`), uncontended.

🚨 **Why the FIRST diagnostic attempts pointed at phantom carriers — the finding worth keeping.** Diffing
two states from **separate runs** named `memory/:c68mcu:mcu/0/0-bf` and then
`Namco C140/:c140/0/m_bank_count` as the "carrier" on successive tries. Both are **contamination**, and
`m_bank_count` explains it: **`device_rom_interface::m_bank_count` is `save_item`'d
(`src/emu/dirom.ipp:147`) but never initialised** when a device uses a configured address map instead of
a ROM region (`interface_pre_start` only calls `set_rom()` — which sets it — in the ROM-region branch,
`:123-125`). So it is saved *garbage*, different every boot (0 vs `0x66690909` seen), and a two-run diff
shows it drifting. It is **dead** here (read only by `set_rom_bank`, which throws without banking), so it
never affects a load — but it makes a two-separate-run state diff lie. This is a genuine upstream MAME
quirk (an uninitialised member in the registry); left unpatched because it is dead and `emu/dirom.ipp` is
a high-traffic core file — **the workaround is the single-history diff above, not a patch.**

⚠️ **A registry diff can only see registered drift, and with a single history there was none** — so the
earlier "the carrier is unregistered, audit the C68" reasoning was chasing a contaminant. The C68
(`m37450_device` / M3745X / m740 / m6502) audits clean, but that audit answered a question that turned out
not to need asking. 🛑 **A speculative `m37710` DMA-register fix was tried and reverted** — wrong CPU (the
C68 is an M37450). The M37710's unsaved DMA/`dmac`/`rto`/`dram` control registers are a real latent bug
for *its* users, but M37710 is not in this machine. Do not re-propose it for starblad.

**Two diagnostics were added and kept** (both env-gated, off by default): retrohost `M2VK_HOST_SAVE_AT2`
(a second save point in one run — the single-history enabler) and `M2VK_SAVE_DIFF_HEX=1`
(`m2vk_savestate.cpp` — prints the differing byte offsets + values within each entry).
