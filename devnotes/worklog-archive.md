# Worklog archive — Model 2 (P0 → steering block), closed

Closed-phase history for the Model 2 core, all committed by HEAD `aabcd8d5cac` (2026-08-20). Covers
P0–P5, the performance/A-B harness, the lightgun phase, the twelve core options, savestates,
per-game input, the Android test core, the repo rename, and the whole steering block. Newest at the
bottom. The live worklog (`worklog.md`) continues from the System 22 port.

---

## 2026-07-25 — docs directory established

Created `devnotes/` as the local-only home for port documentation; gitignored via `/devnotes/` in
`.gitignore` (that file is `skip-worktree`'d) and mirrored in `.git/info/exclude`. `CLAUDE.md`
now points here. (Landed via `claudedocs/` → `notes/` → `devnotes/`: the first name carried AI
nomenclature, the second was generic enough to risk colliding with a future upstream directory.)

Repo state: branch `main`, clean, at `27a8d9e85b5` (version bump to 0.288).

**Next:** P0 spike — locate `model2_renderer::model2_3d_render` in
`src/mame/sega/model2_v.cpp` at mame0288 (the ~line 611 figure in the Polydiver notes is from an
older tree and needs re-confirming), then add the `#ifdef`-guarded poly-tap that logs polys/frame
and dumps one frame of `m2_poly_extra_data`. Record findings in [seam.md](seam.md).

---

## 2026-07-25 — P0 spike: seam proven

Did the whole spike in this tree at mame0288 rather than in Polydiver's `pdmame` build — the seam
is what we keep, so it may as well be verified where it will live. Full findings in
[seam.md](seam.md); the short version:

`model2_renderer::model2_3d_render` is at `model2_v.cpp:565` (the old ~611 figure was from the
pre-0288 reference tree). Instrumented with a new header-only file
`src/mame/sega/model2_polytap.h` plus **four `#ifdef M2VK_POLYTAP` blocks** in `model2_v.cpp`
(include, `frame_begin`, `submit`, `frame_end`) — no build-script changes at all, because a
header-only tap needs no new compilation unit. The mergeability golden rule holds trivially.

Build and run (~35 min from cold on 10 cores, ~1 min incremental):

```sh
make SOURCES=src/mame/sega/model2.cpp SUBTARGET=model2 ARCHOPTS_CXX=-DM2VK_POLYTAP NOWERROR=1 -j10
M2VK_POLYTAP_EVERY=100 M2VK_POLYTAP_DUMP=500 \
  ./model2 vf2 -rompath devnotes/roms -video none -window -nomaximize -nothrottle -skip_gameinfo -str 30
```

What it settled:

- **The tap is complete and single-threaded.** Tapped polys == `raster->poly_list_index` on every
  frame; the submitting-thread check never tripped. Buffer building at the seam needs no locking.
- **VF2 attract mode is 550–1450 polys/frame**, ~80 % quads, ~98 % textured. Nothing about this is
  throughput-limited.
- **Runs are bit-repeatable** frame-for-frame — the A/B harness can key fixtures on frame number.
- **Headless needs no recording flag**, but it does need `-window` and a long enough run. Runs at
  420–460 % with zero disk writes. Two traps: `window` defaults to off so `-video none` blanks the
  whole display (`renderer_none` is still a renderer *for* a window), and VF2 does not render its
  first 3D frame until ~16 emulated seconds — fixtures shorter than that see nothing. An earlier
  version of this entry claimed `-video none` rendered nothing without `-aviwrite`; that was wrong,
  from two too-short runs with duration and flag changed together.
- **The P4 problem is bigger than expected, and the plan for it is right.** In the dumped frame,
  **476 of 552 polys share a sort bucket with an adjacent poly** in draw order (213 distinct buckets
  for 552 polys, runs up to 11 long). Coplanar ties are the norm, not the exception. Also found:
  the sort-bucket lists are built by *prepend*, so within a bucket polys draw in **reverse**
  submission order — display-list index is not draw order. Depth bias must key on actual draw order.
- Vertices arrive **already projected to screen space** (`model2_3d_project` runs immediately
  before), and for textured polys `p[0]` has already been reciprocated to `1/z` while untextured
  polys keep raw `z`. Two depth conventions in one stream; normalise at submit time.

Reference dump kept at `devnotes/fixtures/vf2-frame500-polytap.txt` (552 polys, frame 500,
regenerable in 30 s with the command above).

Also read `src/mame/sega/model2rd.ipp` end to end — it is the complete shading model (texel →
`lumaram` → `colorxlat` → gamma) and matches Polydiver's `model2_lighting.md`. That file is the
spec for the P3 GLSL port; nothing extra needs deriving.

Committed as `27eb79f23c2` on `main` (246 lines: 230 in the new header, 16 guarded lines in
`model2_v.cpp`). Committed rather than kept as a throwaway for two reasons: the tap is the
diagnostic instrument P3/P4 will need when Vulkan output diverges from the software renderer, and
getting the upstream footprint into history now means the first `mame0289` merge exercises it while
the diff is still trivially small.

**The hook shape is the permanent one.** `frame_begin` / `submit` / `frame_end` is not
tap-specific — it is exactly what the Vulkan renderer needs (begin frame, append poly, flush). So
P1 evolves what sits *behind* those three calls, not the calls themselves:

- rename the guard `M2VK_POLYTAP` → `M2VK`, defined properly by the subtarget's genie script rather
  than by `ARCHOPTS_CXX` (which sprays the define across every translation unit)
- point the three calls at a sink in `src/osd/libretro_m2/`; the poly tap becomes one debug consumer
  behind that sink instead of being the sink
- move `model2_polytap.h` out of `src/mame/sega/` at the same time — once it is a real submit path it
  belongs with the rest of our code, so upstream directories hold nothing of ours but the guarded
  calls. Deferred to P1 so the include line only changes once.

Goal is to freeze the upstream diff at ~16 lines permanently; that is the whole mergeability bet.

---

## 2026-07-25 — feature-survey sweep over 29 games

Full write-up in [feature-survey.md](feature-survey.md); ROM set notes in [roms.md](roms.md). Ran the
tap over 29 working sets, 65 emulated seconds each, and **checked every game's screenshots by eye**
before believing any of it. That last part was the important part: a Model 2 service screen or
attract text card is itself hundreds of textured translucent quads, so it passes as plausible
geometry in the statistics. `skisuprg` turned out to be sitting on "DRIVE BOARD TROUBLE" for its
entire run, and `manxttc`'s first 60 s are pure text cards.

Extended the tap with run-level aggregates (destructor-emitted `key=value` summary via
`M2VK_POLYTAP_SUMMARY`) and added [snap.lua](snap.lua) for timed PNG snapshots.

Decisions this settles, all of which contradict something inferred from VF2 alone:

- **Microtextures are in, not deferred.** 5 games use them; `waverunr` 448k and `segawski` 391k
  polys. VF2/Daytona/Sega Rally use none — a VF2-only look gets this exactly wrong.
- **`checker` stipple is mandatory and moves into P3.** All 29 games use it, up to 381k polys.
- **Depth must be clamped.** `stcc` emits 292k non-finite/absurd `1/z` vertices; `vstriker` and
  `motoraid` emit *negative* `1/z`. The SW rasterizer absorbs this silently.
- **The solid (untextured) path matters** — up to 39.7 % (`vonj`). VF2's 0.2 % was the outlier.
- **For `overrev` and `sgt24h` the translucent cutout path is the *only* textured path** — zero
  opaque textured polys.
- **Sizing:** 4137 polys/frame worst case, 228 texture pages in one frame, buckets to full 65535,
  up to 12 windows (not 8).
- **Ties are pervasive everywhere:** 25–68 % across the library, not a VF2 quirk.

Two games are not usable A/B targets: `skisuprg` (drive-board error screen) and `topskatr` (renders
~87 frames of degenerate geometry then stops, and **aborts MAME** when snapshotted — `model2_v.cpp:32`
already documents that MAME doesn't implement its geometry code properly).

Caveat recorded prominently in the survey: ratio columns (`trans%`, `tie%`, `solid%`) are inflated by
attract-mode UI screens by an unknown amount. Presence/maximum columns are trustworthy. Clean ratios
need gameplay-parked savestate fixtures — a P1 job.

**Next:** P1 — Model-2-only libretro core with the stock software renderer, evolved from
Polydiver's `pdmame_osd.cpp`.

---

## 2026-07-25 — P1 planned, and steps 1–4 landed: the core boots VF2 and draws

Plan written up first in [p1-libretro-core.md](p1-libretro-core.md), because the wiring was the
part expected to be fiddly. It was, but the headline is better than hoped:

**Zero edits to upstream files outside `model2_v.cpp`.** All five of pdmame's upstream patches
(`PDTooling/mame_patches/0002`–`0006`) turn out to be avoidable at mame0288 — the visibility patch,
the `register_options()` patch, the `main.lua` patch, the makefile target, and the `osdlib_unix`
SDL patch. How each is dodged is in the plan doc; the four short versions:

- **the M2VK define** → a real subtarget (`scripts/target/mame/model2.lua` + `src/mame/model2.flt`),
  generated by `makedep.py`, carrying `defines{"M2VK"}` in the driver project only
- **SharedLib** → `maintargetosdoptions()` re-issues `kind`, since `mainProject()` calls it from
  inside its own project scope
- **the ~50 module symbols `register_options()` names** → `module_stubs.cpp`, a file we own, rather
  than a patch to `osdobj_common.cpp` that would conflict on every merge
- **module visibility** → never touch the private module manager; `module_type` is a plain factory
  pointer, and every field we set is already protected

Two decisions settled (see the plan doc for reasoning): input goes through a real MAME
`input_module`, not per-game ioport injection; and **savestates are deferred past P1**, because
`src/mame/sega/model2.cpp` carries *zero* `MACHINE_SUPPORTS_SAVE` flags. That last one also changes
the A/B plan — the survey's ask for "gameplay-parked savestate fixtures" should become frame-number
fixtures plus `.inp` playback, which is what P0 actually proved reliable.

### What now works

`model2_libretro.dylib`, 25 `retro_*` exports, no SDL/bgfx/OpenGL linkage. Driven by a new
[retrohost.c](retrohost.c) (a ~180-line dlopen host, no RetroArch needed — and the seed of the A/B
harness, since it can dump any frame by number):

```
core: Model 2 0.288 (api 1) exts=zip|7z
av: 496x384 (max 496x384) aspect 1.2917  fps 57.5242  rate 48000
ran 1200 frames, 1200 with video, 999361 audio sample frames (832.8/frame)
```

Frame 1200 of VF2 attract mode was checked **by eye**, not just by the numbers: Shun Di on the
temple stage, correct colours and geometry, `INSERT COIN(S)` overlay. 48000/57.5242 = 834.4 expected
samples per frame against 832.8 measured, the gap being startup frames. The failure path was tested
too — a nonexistent set returns `false` cleanly instead of hanging.

The poly tap still fires in this build (`tie_pct=57.9`, `multithreaded=0`), so `M2VK` is reaching
the driver through the new subtarget.

### Four things that bit, all worth remembering

1. **`REGENIE=1` is mandatory** after touching a subtarget script — the makefile's `$(SCRIPTS)` list
   doesn't include it (`makefile:908` is commented out). The first "successful" build proved nothing:
   it silently reused P0's project files, still carrying `-DM2VK_POLYTAP`.
2. **genie's gmake output never rebuilds on flag changes**, only on header deps. After a define
   change, `touch` the sources or you link a mix of old and new objects.
3. **A static-archive libretro core exports nothing.** Nothing inside the core references the
   `retro_*` entry points — the frontend looks them up after `dlopen()` — so as archive members they
   were never pulled into the link. `nm` showed 0 exports on a library that built fine.
   `retro_entry.cpp` now compiles into the main target instead.
4. **A genuine deadlock on shutdown**, found by `sample`, not by guessing: `schedule_exit()` only
   takes effect at end of timeslice, so `update()` runs several more times on the way out and parked
   on a baton nobody would release again — `retro_unload_game` had already moved on to `join()`.
   Fixed with a sticky `m_exiting` flag that stops the parking.

Also fixed two things the numbers exposed: the framebuffer was the full 656×424 raster rather than
the 496×384 visible area (which would render as garbage in RetroArch), and the refresh rate read
60 Hz because it was sampled at `osd->init()`, before the screen device applies the driver's
`set_raw()`. Both now taken from `visible_area()` / `frame_period()` at capture time.

**Next:** step 5, the libretro `input_module`; then the seam move (`model2_polytap.h` →
`src/osd/libretro_m2/m2vk_sink.h` + the tap behind it, and `no_3d=1` for games that render no 3D),
then core options. Not yet committed.

---

## 2026-07-25 — step 5: input. The core is now playable

`src/osd/libretro_m2/libretro_m2_input.{h,cpp}` — a real MAME `input_module` registering two
RetroPad joystick devices, as decided earlier today. Built clean first try; the interesting part was
all in reading how MAME 0288 wires device defaults, and that is now written up in
[p1-libretro-core.md](p1-libretro-core.md).

Three things that shaped the implementation:

- **`configure()` is `sdl_game_controller_device::configure()` with the probing deleted.** A
  RetroPad always has every control, so all the `SDL_GameControllerHasAxis`/`HasButton` branching
  collapses to a straight list. What survives is the part that matters: one
  `add_directional_assignments()` call covers `IPT_PADDLE`, `IPT_AD_STICK_X/Y` and
  `IPT_LIGHTGUN_X/Y` — between them every analogue type in `model2.cpp` bar the pedals.
- **The device index is the player number, and nothing else does that pairing.** `START1..8` and
  `COIN1..8` default to `JOYCODE_START_INDEXED(n)` / `JOYCODE_SELECT_INDEXED(n)` in
  `inpttype.ipp`, so coin and start need no assignment at all — only that RetroPad port *n* is the
  *n*th joystick device MAME sees.
- **Polling runs backwards.** The item pointers address the device object's state directly and
  `retro_run()` writes it while the emulation thread is parked on the baton, so `poll()` is a no-op
  and `poll_if_necessary()`'s wall-clock throttle never matters. One input sample per frame, taken
  at a fixed point — which is what the A/B harness needs.

Also landed with it: `retro_reset()` (a flag picked up on the emulation thread in `update()`, same
shape as `request_exit()`), input descriptors for the frontend's remapping UI, and an `osd_exit()`
override so the input module and the render_target are released at `MACHINE_NOTIFY_EXIT` rather than
left pointing into a dead machine.

### Verified by playing it

[retrohost.c](retrohost.c) grew a control script — `frame:control[:held][:port]`, where a control is
a RetroPad button or a half-axis held at full deflection — so the input path can be exercised
without RetroArch:

```
./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/vf2.zip 1400 /tmp/f.ppm \
  "300:select:5,340:select:5,1000:start:30,1150:right:5,1250:right:5"
```

- **vf2** — two coins read as exactly two credits, start reaches PLAYER SELECT, two D-pad rights
  move the cursor two characters. Repeated on **port 1**: P2's panel, so `COIN2`/`START2` are
  reaching device index 1.
- **srallyc** — coined and started into a race, R2 held: 202 km/h in 4th. Then `lx-` for 800
  frames: off the road to the left and down to 39 km/h. Pedal *and* steering, i.e. both the digital
  trigger fallback and the analogue stick.

One scare worth recording so it is not re-debugged: credit counts looked non-deterministic across
runs (1 coin → 4 credits). It is NVRAM — MAME persists Model 2 battery RAM into `nvram/<set>/`, so
credits carry over between runs. `rm -rf nvram/<set>` before a fixture run. This will matter to the
A/B harness: **a frame-number fixture is only reproducible if the NVRAM state is too.**

**Next:** step 6, the seam move — `src/mame/sega/model2_polytap.h` → `src/osd/libretro_m2/m2vk_sink.h`
plus the tap behind it, guard rename `M2VK_POLYTAP` → `M2VK`, and `no_3d=1` for games that render no
3D. Then core options. Still not committed.

---

## 2026-07-25 — step 6: the seam moves out of `src/mame/sega/`

`model2_polytap.h` is gone. In its place, in `src/osd/libretro_m2/`:

- **`m2vk_sink.h`** — the snapshot type `m2vk::poly`, the `consumer` interface, and the conversion
  from `model2_state::polygon` + `m2_poly_extra_data` + clipped viewport into the snapshot.
- **`m2vk_sink.cpp`** — the sink object and the dispatch, ~110 lines.
- **`m2vk_polytap.h`** — the tap, now one consumer that reads `m2vk::poly` and knows nothing about
  the driver.

`model2_v.cpp` is still four `#ifdef M2VK` blocks / 16 lines; only the include line and the
namespace changed (`m2vk::`, not `model2_polytap::`). The include is
`"libretro_m2/m2vk_sink.h"` — `src/osd` is on the driver project's include path, `src/` is not, so
the `osd/libretro_m2/…` spelling the plan doc used would not have compiled.

Three decisions behind it, written up in [p1-libretro-core.md](p1-libretro-core.md):

- **What crosses the seam is a snapshot with no MAME types in it.** Consumers — the tap now, the
  Vulkan renderer later — compile without the driver's headers, so an upstream change to `model2.h`
  breaks one conversion rather than every consumer, and the snapshot is close to what a vertex
  buffer upload wants anyway. The conversion is a template purely so the header can also be compiled
  where `model2_state` does not exist. ~230 bytes per polygon, skipped when nothing is attached.
- **The sink has static storage duration; the OSD brackets the run.** `sink_open()` in `init()`,
  `sink_close()` in `osd_exit()`, consumers built and destroyed with the run. So the sink is there
  even for a game that renders no 3D, which is the `no_3d=1` fix. The plain `OSD=sdl3` binary calls
  neither, so the first `frame_begin()` opens implicitly and `~sink` closes at process exit — P0's
  behaviour unchanged.
- **The tap is off unless an `M2VK_POLYTAP*` variable is set.** It used to be on in every `M2VK`
  build, which is wrong for a core people play. The sweep scripts set `_SUMMARY`/`_TAG` so they are
  unaffected; `M2VK_POLYTAP=1` is the bare "on".

One thing the snapshot fixed on the way past: `model2_v.cpp` resolves the texture fields of
`m2_poly_extra_data` only for textured polygons, and the `object_data()` slot is recycled, so for a
solid polygon they hold the *previous* polygon's values. The tap always guarded its own printing; the
conversion now zeroes them, making it a property of the data.

### Verified — the stream is unchanged

- `vf2` frames 100/300/400/500/600 off the standalone binary reproduce [seam.md](seam.md)'s recorded
  lines exactly.
- The full per-polygon dump of `vf2` rendered frame 800 is **byte-identical** to the P0 fixture
  `devnotes/fixtures/vf2-frame800-polytap.txt` — every field of the conversion, including all five
  vertex floats, is proved by that one diff.
- Through the core: same frame-100 line, summary written at `retro_unload_game`. 200 frames of `vf2`
  (short of its first 3D frame at ~16 s) writes `frames=0` / `no_3d=1`, which is the case that used
  to write nothing at all.
- No `M2VK_POLYTAP*` set → the core attaches nothing and says nothing.

### Both build shapes still link, and that shaped the wiring

The sink lives in the OSD directory because that is where the Vulkan renderer will live. The seam
that calls it is in the driver project, which is linked *before* the OSD archive — that direction is
fine, the reverse is not, which is why nothing in the OSD reaches into the driver. `OSD=sdl3` does
not compile the sink at all, so a HAND-ADDED `if _OPTIONS["osd"] ~= "libretro_m2"` block in
`scripts/target/mame/model2.lua` compiles it into the driver project for any other OSD. Both
`OSD=libretro_m2` and `OSD=sdl3` build and run.

Also: **the two OSD configurations share one object directory** (`build/osx_clang/obj/…`). The sdl3
build after the libretro one recompiled nothing but the sink, because `model2_v.o` was already
current — correct here since the driver's flags do not depend on the OSD, but worth knowing before
trusting a build that "did nothing".

**Next:** step 7, core options (savestates are deliberately out of P1 — see
[p1-libretro-core.md](p1-libretro-core.md)). Still not committed.

---

## 2026-07-25 — step 7: core options, and where MAME's files go

P1's last step. `src/osd/libretro_m2/retro_options.{h,cpp}` — two core options, declared through
`SET_CORE_OPTIONS_V2` with the v1 and pre-options forms derived from the same table so an older
frontend gets them too. Full write-up in [p1-libretro-core.md](p1-libretro-core.md).

| key | values | default |
| --- | --- | --- |
| `model2_renderer` | `vulkan`, `software` | `vulkan` |
| `model2_service_buttons` | `disabled`, `enabled` | `disabled` |

Both are read once, in `retro_load_game`, because both are settled before the machine starts.
`retro_run` checks `GET_VARIABLE_UPDATE` and logs one line per change rather than half-applying it.

- **`renderer` defaults to `vulkan` in a build with no Vulkan renderer.** Nothing ships until the
  Vulkan path works, so the shipping default is the default now and no saved config ever needs
  migrating. Until P3, a `vulkan` selection logs that it is falling back and runs software.
- **`service_buttons` puts the service coin on L3 and the test switch on R3.** This core draws none
  of MAME's menus, so with it off there is no way at all into a game's test mode. Off by default —
  an accidental stick click should not drop a service coin.

Rejected while scoping: a `model2_polytap` option (it stays an `M2VK_POLYTAP*` environment switch;
it is a developer tool and the sweep scripts already drive it that way), `model2_samplerate`, and a
`model2_bios` knob for the driving sets' drive-board ROM, where `ROM_DEFAULT_BIOS` is already right
for all eight.

### The part that was not an option at all

MAME resolves `nvram_directory`, `cfg_directory` and the rest relative to the working directory —
which for a core is the frontend's, and not ours to litter. All six now go under
`<save dir>/model2/`, and `<system dir>/model2` joins the rompath so a clone whose parent lives
elsewhere still loads. `-noreadconfig` went in with them: MAME's default inipath includes `.` and
`$HOME/.mame`, so without it a stray `mame.ini` from someone's standalone MAME silently changes how
the core runs. It gates ini files only; per-game cfg input remaps still load.

This is reproducibility work as much as tidiness. Credits live in battery RAM — the input session
already lost time to NVRAM carrying over between runs — so a `(rom, frame)` fixture is only
reproducible if the NVRAM state is. **`rm -rf retrohost-save` before a fixture run.**

### A binding that had been doing nothing since step 5

`buttonitems[L3]`/`[R3]` were `ITEM_ID_INVALID`: the stick clicks were named in
`RETROPAD_BUTTON_NAMES` but never added as device items, and `add_assignment()` silently skips an
assignment naming an invalid item. So the `IPT_UI_MENU` binding on L3 had never worked, and the
first cut of the service option didn't either. They are now `ITEM_ID_BUTTON9`/`BUTTON10` in
`FIXED_BUTTONS` — not in `NUMBERED_BUTTONS`, since no Model 2 game has nine buttons.

### Verified

retrohost grew the frontend side of all this: option declaration and `GET_VARIABLE`, with a value
overridable per run by `M2OPT_<key>` in the environment, and a save directory defaulting to
`./retrohost-save`.

- `model2_service_buttons=enabled`: two L3 presses in `vf2` attract read as **CREDITS 2/2**, and R3
  held brings up the **TEST MENU**. With the option off the same script changes nothing.
- `renderer=software` loads without the fallback warning; the default `vulkan` logs it.
- Input regression after the L3/R3 change: the step-5 script still reaches PLAYER SELECT with the
  cursor two characters across.
- Seam regression: `vf2` rendered frame 100 through the core reproduces [seam.md](seam.md)'s line
  field for field.
- `OSD=libretro_m2` and `OSD=sdl3` both build.

**P1 is done.** Savestates stay deliberately out (no Model 2 set carries `MACHINE_SUPPORTS_SAVE`;
reasoning in [p1-libretro-core.md](p1-libretro-core.md)). Next: commit the branch, then P2.

## 2026-07-25 — P2 planned: Vulkan HW context, passthrough

P1 is committed (`00a245ac219`, "Software core ready"). P2 is planned in
[p2-vulkan-passthrough.md](p2-vulkan-passthrough.md); nothing implemented yet.

**Frontend settled: RetroArch**, after checking what is actually on this machine rather than
assuming. `/Applications/RetroArch.app` is 1.22.2 (git 69a4f0ea, Nov 2025), arm64,
`video_driver = "vulkan"` already in its config, and it **bundles MoltenVK 1.2.7** in
`Contents/Frameworks` — so Vulkan-over-Metal works with no system Vulkan install. The binary
carries the HW-render paths (`GET_PREFERRED_HW_RENDER: RETRO_HW_CONTEXT_VULKAN`, `VK_KHR_swapchain`).
RetroArch is the compatibility target, so P2 is developed against it and only then ported into our
own headless host.

Three findings that shaped the plan:

- **Nothing Vulkan is present to build against.** No Vulkan headers in MAME's `3rdparty/`, no SDK,
  no `glslc`. Needs `brew install vulkan-headers shaderc` (and `molten-vk` for the headless host
  only). `libretro_vulkan.h` is not vendored either — our `libretro.h` has the enums
  (`RETRO_HW_CONTEXT_VULKAN = 6` at `:5251`) but not the interface struct.
- **The core will link no Vulkan library at all** — every entry point comes from the frontend's
  `get_instance_proc_addr`. macOS has no loader unless someone ships one, and RetroArch is already
  talking to its own bundled MoltenVK; linking our own would load a second implementation. It also
  keeps the dylib loadable where there is no Vulkan, which is what makes the software fallback real.
- **`retrohost` cannot follow us.** It is software-`video_cb` only, and the A/B harness needs
  headless Vulkan captures from P3 on. So P2 ends with a `retrohost-vk` that owns MoltenVK directly
  and reads the image back to a PPM — which gives P2 a hard exit criterion: same ROM, same frame,
  `software` vs `vulkan`, **PPMs bit-identical under `cmp`**. Passthrough is the one phase where
  that is achievable, so it calibrates the A/B rig for free.

Deferred on purpose: the context **negotiation** interface. Without it RetroArch picks the GPU and
creates the device, which is fine for passthrough and removes a failure surface; `vk_present` takes
an externally supplied device from day one so adding negotiation later changes only who creates it.

## 2026-07-25 — P2 step 1: toolchain

The build can now see Vulkan. Nothing Vulkan runs yet.

Installed: `vulkan-headers 1.4.350.1`, `shaderc 2026.3` (gives `/opt/homebrew/bin/glslc`),
`molten-vk 1.4.2` (for step 7's headless host only — the core never links it).

**Headers are newer than the implementation and that is fine.** Homebrew's headers say
1.4.350; RetroArch bundles MoltenVK 1.2.7. We target core 1.0, resolve every entry point at run
time, and only opt into anything above that after the step-2 log says the device supports it. Worth
remembering when a symbol exists in the headers and not in the loader.

`libretro_vulkan.h` vendored to `src/osd/libretro_m2/` from libretro-common
(`23d82a25841350e7b7db93905ee1fc3ec09ac9d2`, master `52193838ab22` as of today), byte-for-byte
upstream — same treatment as `libretro.h`, which is also pristine. The plan text asked for a
"vendored, do not edit" banner; `libretro.h` carries no such banner, and keeping both files
literally upstream is worth more than the comment, so provenance is recorded here instead.

**The include path is an environment variable, not a genie `--option`.** `M2VK_VULKAN_INCLUDEDIR`,
defaulting to `/opt/homebrew/include` on macOS and `/usr/include` elsewhere, read at the top of
`scripts/src/osd/libretro_m2.lua`. A genie option would have been the nicer interface, but options
reach genie only through the `PARAMS` list in the top-level `makefile` — an upstream file, and a
command-line `PARAMS=` would clobber all 190 of the entries already there. So: env var, zero
upstream edits. A missing header errors out at generate time with the `brew install` line in the
message (verified against `M2VK_VULKAN_INCLUDEDIR=/nowhere`).

First `renderer_vk` TU: `vk_funcs.{h,cpp}` — the eventual home of the function-pointer table,
holding for now the static_asserts on the vendored interface version and `vk_build_info()`, one line
naming the headers we compiled against. `retro_init()` logs it, which is also what proves the TU is
linked into the dylib rather than merely compiled.

Verified:
- `make SUBTARGET=model2 OSD=libretro_m2 REGENIE=1` builds clean, with the default path and with
  `M2VK_VULKAN_INCLUDEDIR` set explicitly.
- `otool -L model2_libretro.dylib` names **no Vulkan library** — the rule holds.
- `retrohost` + `vf2` + 1100 frames: `[model2] vulkan headers 1.4.350, libretro vulkan interface v5
  (negotiation v2)` first in the log, then the software path runs exactly as before — 1100/1100
  frames with video at 562 % of real time, poly-tap numbers unchanged, and the dumped PPM is the
  attract-mode Akira in front of the temple, checked as a picture and not just as statistics.
- Upstream diff untouched: `model2_v.cpp` is still the 16 `#ifdef M2VK` lines and nothing else.

Next: step 2 — declare HW render, fetch the interface at `context_reset`, log everything MoltenVK
will tell us. Build nothing on top of it until that log exists.

## 2026-07-25 — P2 step 2: the Vulkan context, and the log that was the point

The core declares `RETRO_HW_CONTEXT_VULKAN`, takes delivery of
`RETRO_HW_RENDER_INTERFACE_VULKAN` at `context_reset`, and probes what it was handed. **Nothing is
drawn through it yet** — that is steps 3 and 4. `renderer=vulkan` in RetroArch therefore shows no
picture at all, which is the expected state of this step and not a regression.

New: `renderer_vk/vk_context.{h,cpp}` (the whole lifecycle, plus the probe) and, in `vk_funcs`, the
function-pointer table and a `vk_log()` shim that prefixes `[model2] vk:` and formats through a
buffer rather than forwarding a caller's format string — device names come from drivers, and a stray
`%` in one should not be someone else's problem. Edits to existing files: four lines and a
presentation branch in `retro_entry.cpp`, two in the genie script. Upstream diff still zero.

**MoltenVK + this core work.** The whole log:

```
vk: interface v5 (this core built against v5)
vk: handles: instance 0x9e99a9818 gpu 0x9e9a8d818 device 0x9eaeae018 queue 0x9e9abc658 (family 0), frontend handle 0x9eadaa800
vk: entry points: +set_image +get_sync_index +get_sync_index_mask +wait_sync_index +set_command_buffers +lock_queue +unlock_queue +set_signal_semaphore
vk: instance api 1.3.313
vk: device 'Apple M5' (integrated GPU), api 1.1.0, driver 0x0000283c (0.2.2108), vendor 0x106b device 0x1a050209
vk: limits: max 2D image 16384, array layers 2048, viewports 16, colour attachments 8, samplers/stage 16, sampled images/stage 128
vk: limits: push constants 4096 B, bound descriptor sets 8, vertex attributes 31, max anisotropy 16.0, max sampler LOD bias 4.0
vk: limits: optimal buffer copy offset 16, row pitch 1, non-coherent atom 16, buffer-image granularity 16, map alignment 64
vk: limits: max allocations 1073741824, line width 1.0-1.0, point size 1.0-511.0, timestamps yes (1.0 ns)
vk: device supports (not necessarily enabled — the frontend created the device): +depthBiasClamp
    +depthClamp +fillModeNonSolid +samplerAnisotropy +independentBlend +dualSrcBlend -logicOp
    +alphaToOne -wideLines +largePoints -geometryShader +multiViewport +imageCubeArray
    +textureCompressionBC +occlusionQueryPrecise +fragmentStoresAndAtomics +shaderClipDistance
    +shaderSampledImageArrayDynamicIndexing
vk: queue family 0: [ours] 1 queue, graphics|compute|transfer, 64 timestamp bits
vk: queue family 1..3: 1 queue each, graphics|compute|transfer, 64 timestamp bits
vk: memory heap 0: 32768 MiB, device-local
vk: memory type 0: heap 0, device-local
vk: memory type 1: heap 0, device-local|host-visible|host-coherent|host-cached
vk: memory type 2: heap 0, device-local|lazily-allocated
vk: format B8G8R8A8_UNORM: optimal sampled|storage|colour-attachment|blend|blit-src|blit-dst|transfer-src|transfer-dst|filter-linear|0x80000000
vk: format R8G8B8A8_UNORM: optimal sampled|storage|colour-attachment|blend|blit-src|blit-dst|transfer-src|transfer-dst|filter-linear|0x80000000
vk: format D32_SFLOAT: optimal sampled|depth-stencil|blit-src|blit-dst|transfer-src|transfer-dst|filter-linear
vk: format D24_UNORM_S8_UINT: optimal none
vk: format D32_SFLOAT_S8_UINT: optimal sampled|depth-stencil|blit-src|blit-dst|transfer-src|transfer-dst|filter-linear
vk: device offers 110 extensions (availability, not enablement): +VK_KHR_portability_subset
    +VK_KHR_swapchain +VK_KHR_maintenance1 +VK_KHR_dynamic_rendering +VK_KHR_synchronization2
    +VK_KHR_timeline_semaphore +VK_KHR_push_descriptor +VK_KHR_image_format_list
    +VK_EXT_descriptor_indexing +VK_EXT_memory_budget +VK_EXT_metal_objects
```

Six things in there change what later steps do:

1. **The ceiling is core 1.1, not 1.2 or 1.3.** The MoltenVK version in the P2 plan (1.2.7, read off
   the bundle) was wrong: `driverVersion` 0x283c is 10300 decimal, and MoltenVK encodes
   major·10000 + minor·100 + patch — so **MoltenVK 1.3.0**, built against headers 1.3.313, which is
   also what the instance reports. But the *device* reports `apiVersion` **1.1.0**, because MoltenVK
   clamps a device's reported API version to the instance's requested version and RetroArch asks for
   1.1 (we asked for 1.0; we got 1.1, which is the frontend's call to make). Target 1.1.
2. **`D24_UNORM_S8_UINT` does not exist here** — `optimalTilingFeatures` is literally zero. P3's depth
   buffer is `D32_SFLOAT`, or `D32_SFLOAT_S8_UINT` if stencil is ever wanted. This is exactly the
   class of thing that would have been found in P3 with the polygon renderer in the way.
3. **`depthBiasClamp` is available**, which matters because submission-order depth bias is the
   z-fight/decal fix ported from the Unity work. Available, not necessarily *enabled* — see 6.
4. **Step 4's format chain is confirmed.** `B8G8R8A8_UNORM` optimal-tiled supports sampled +
   transfer-dst + linear filter; `optimalBufferCopyRowPitchAlignment` is 1 and the offset alignment 16,
   so a tightly-packed 496×384 upload needs no padding at all. Memory type 1 is device-local *and*
   host-visible/coherent (unified memory), so the staging buffer is nearly free.
5. **The image ring should be 3, not 2.** RetroArch logged "Got 3 swapchain images", so
   `get_sync_index_mask` will report three. Size the ring off the mask.
6. **What is enabled on the device is unknowable from here.** We did not create it, and Vulkan offers
   no "which features did you enable" query. `dynamic_rendering`, `synchronization2` and
   `timeline_semaphore` are all present on the device and all off-limits until the negotiation
   interface lands. The probe log says so on the line itself, so a future reader cannot misread it.

Also worth keeping: MoltenVK reports `0x80000000` in `optimalTilingFeatures` for both 8-bit colour
formats. No such bit exists in the 32-bit `VkFormatFeatureFlagBits`; it is
`VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT` from the *flags2* enum leaking into a flags1
field. Harmless. The probe prints unnamed bits as hex rather than dropping them, which is how it was
noticed at all.

One correction to the plan, found by reading `libretro.h` rather than by debugging: a core with HW
render declared may pass **only** `RETRO_HW_FRAME_BUFFER_VALID` or null to `video_cb`
(`libretro.h:946`). Showing the software picture while the Vulkan renderer is unfinished was never an
option, so `renderer=vulkan` dupes every frame for now. Verified harmless — 400 frames in RetroArch,
audio running, clean exit, no warnings.

Verified:
- RetroArch 1.22.2, `video_driver=vulkan`, windowed via `--appendconfig`, `--max-frames=400`:
  `SET_HW_RENDER, context type: vulkan` accepted, context up, log above, `context destroyed` on
  unload, exit 0. (`--appendconfig` with `video_fullscreen=false` matters — the machine's config has
  fullscreen on, and a fullscreen RetroArch owns the display for the length of the run.)
- The device loader is exercised, not assumed: `context_reset` resolves `vkQueueSubmit` through
  `get_device_proc_addr` and bails if it cannot.
- `retrohost` (no HW render at all) with the default `renderer=vulkan`: declaration refused → the
  warning fires → software path, 1100/1100 frames with video at 559 % of real time, PPM still the
  attract-mode temple scene.
- **The software path is bit-identical to the committed P1 core**, not merely "looks the same":
  `git stash -u`, rebuild, run vf2 for 1100 frames, `git stash pop`, rebuild — and `cmp` on the two
  PPMs is silent. Worth the four minutes, because it is a rehearsal of the step-7 exit criterion with
  one side of the comparison already known-good, and because it is the only thing that can prove the
  presentation branch left the software path alone.
- `otool -L` still names no Vulkan library. `model2_v.cpp` diff still zero lines added this step.

Next: step 3 — the image ring, command pools, sync-index bookkeeping, `set_image`, and a flat colour
clear. Ring size 3 off the mask, submits bracketed by `lock_queue`/`unlock_queue`, and leave it
running for minutes before believing it, because sync bugs show up as drift rather than a crash.

Committed as `cf822015bfc`, "Vulkan context and device probe".

## 2026-07-25 — P2 step 3: the image ring, and a flat colour through it

The core now owns a ring of images, draws into one every frame, and hands it to RetroArch through
`set_image`. What it draws is a flat orange clear — the picture is not the point of this step; the
ring, the sync-index bookkeeping and the handover are.

New: `renderer_vk/vk_present.{h,cpp}`. `vk_funcs` grew its device-level half (22 entry points,
resolved through `get_device_proc_addr` against the frontend's `VkDevice`, not through the instance
loader — the instance loader would answer for names the device dispatches through a trampoline, and
that has no business in a per-frame path). `load_funcs` now takes the device and resolves both halves
in one go, which subsumes the standalone `vkQueueSubmit` probe step 2 used to prove the device
loader. Edits to existing files: the presentation branch in `retro_entry.cpp`, three `present_shutdown()`
calls in `vk_context.cpp`, two lines in the genie script. **Upstream diff still zero.**

**The ring is 3**, exactly as step 2's `get_sync_index_mask` predicted — but it is read from the mask
every frame rather than baked in, because the mask is documented to change (a fullscreen toggle
changes swapchain length) and the spec's promise is that the device is idle when it does.

Per slot: `VkImage` + memory + view + `VkCommandPool` + `VkCommandBuffer` + `VkFence`. Nothing is
shared between slots; that is the entire point of indexing by `get_sync_index()`. The frame is:

```
get_sync_index_mask  →  rebuild if it changed
get_sync_index       →  which slot is ours
wait_sync_index      →  the frontend is done with it, and has released ownership back
vkWaitForFences      →  our own submit from three frames ago has retired
vkResetCommandPool   →  barrier UNDEFINED→TRANSFER_DST
vkCmdClearColorImage →  barrier TRANSFER_DST→SHADER_READ_ONLY_OPTIMAL
lock_queue / vkQueueSubmit(fence) / unlock_queue
set_image(handle, &slot.handover, 0, nullptr, VK_QUEUE_FAMILY_IGNORED)
video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0)
```

Four decisions in there that should survive step 4 replacing the clear:

1. **No semaphores.** The closing layout transition *is* the synchronisation, and `libretro_vulkan.h`
   says so outright: "The use of pipeline barriers instead of semaphores is encouraged as it is
   simpler and more fine-grained." `src_queue_family` is `VK_QUEUE_FAMILY_IGNORED` because we submit
   on the frontend's own family, so there is no ownership transfer to make.
2. **`oldLayout` is `UNDEFINED` every frame**, never the layout we left the image in. The frontend is
   explicitly allowed to transition an image while it holds it, so its current layout is not ours to
   know — and discarding contents is free when the frame overwrites every pixel anyway.
3. **`vkDeviceWaitIdle` is bracketed by `lock_queue`/`unlock_queue` too.** It is a wait on every
   queue, and the queue is shared. Easy to forget because it is not a submit.
4. **The `retro_vulkan_image` lives in the slot**, at a stable address, never as a temporary — the
   interface requires it to stay valid until `retro_video_refresh_t` returns, and permits the
   frontend to reuse the older pointer if a later frame is duped.

**The clear colour is deliberately not static.** A single unchanging colour cannot tell "the ring is
advancing and each slot is being presented" apart from "the frontend is showing one stale image
forever", which is the exact failure this step exists to rule out. Brightness walks a triangle over
120 frames. Orange, so that a red/blue swizzle would read as blue and be unmissable.

Verified:
- **Soak: 16000 frames (~4m40s) in RetroArch**, five screenshots at 60 s intervals. `ring of 3
  496x384 B8G8R8A8_UNORM images, sync index mask 0x7, queue family 0` **once** — no rebuild, no
  drift, no stall. Zero `[ERROR]`/`[WARN]` lines in the entire frontend log, zero validation
  messages, clean `ring of 3 destroyed` → `context destroyed` → exit 0.
- **Frames are actually advancing**, which took a second run to establish honestly. The five
  60-second samples all came back at nearly the same brightness (R = 103–111), which looks exactly
  like a frozen image. It is not: 60 s at the ~60 fps RetroArch actually drives the core is 3600
  frames, and 3600 mod 120 = 0, so the sampling interval aliased perfectly against the triangle.
  Re-sampled at 0.45 s: R = 235 → 115 → 252, i.e. brightness swinging across most of its range.
  **A near-constant reading at a round sampling interval is not evidence of a stall** — pick an
  interval coprime with whatever is being animated.
- Software path **bit-identical to HEAD**: `git stash`, rebuild, `retrohost` vf2 1100 frames,
  `git stash pop`, rebuild, run again — `cmp` silent. Same rehearsal as step 2, and worth repeating
  every time `retro_run`'s presentation branch is touched.
- `otool -L` still names no Vulkan library.

**One finding that changes what step 7 can claim.** RetroArch's presented output on this machine is
not the core's pixel values. The clear writes UNORM bytes `(252, 113, 13)`; a RetroArch GPU
screenshot of the presented window reads `(252, 131, 43)` — red exact, green and blue lifted. That is
the signature of a colour-space/gamut conversion on presentation (this display is P3), not of
anything the core did. The P2 plan already listed this as a risk; it is now measured rather than
suspected. The consequence: **a RetroArch screenshot can never be the A/B ground truth**, and the
step-7 exit criterion has to be `retrohost-vk`'s own read-back, which bypasses the frontend's
presentation entirely. That was already the plan — this is why.

Also worth keeping: **`screencapture` cannot be used to screenshot RetroArch from this shell** (it
fails with `could not create image from display`, wanting a Screen Recording permission). RetroArch's
network command interface works and is the better tool for a HW-render core anyway —
`network_cmd_enable`, `video_gpu_screenshot`, `screenshot_directory` in the `--appendconfig` file,
then `printf 'SCREENSHOT' | nc -u -w0 127.0.0.1 55355`. Recorded in
[p2-vulkan-passthrough.md](p2-vulkan-passthrough.md).

Next: step 4 — the picture. Staging buffer, sampler, pipeline, fullscreen triangle from
`gl_VertexIndex`, and the clear goes away. The format chain is already confirmed
(`B8G8R8A8_UNORM`, no swizzle, `optimalBufferCopyRowPitchAlignment` 1 so a tightly-packed 496×384
upload needs no padding).

---

## 2026-07-25 — P2 step 4: the picture

`renderer=vulkan` now draws MAME's frame. Staging buffer → optimal-tiled texture → fullscreen
triangle → the ring image the frontend presents. The clear is gone. vf2's attract renders in
RetroArch on the Vulkan path, and **the read-back is bit-identical to the software frame that went
in**. Plan of record updated as-built in [p2-vulkan-passthrough.md](p2-vulkan-passthrough.md).

**Say this out loud before reading further, because the sentence above invites the wrong reading:
nothing is GPU-rasterised yet.** MAME's software rasteriser draws every polygon on the CPU exactly as
it did in P1; the GPU's entire contribution is one textured triangle. The bit-exact read-back is the
*proof of that*, not evidence of acceleration — if the GPU were rasterising the polygons, matching
MAME bit-for-bit would be impossible, which is why the plan calls passthrough "the one phase where
'identical to the software renderer' is actually achievable". The polygon stream is intercepted and
does reach `m2vk_sink.cpp`; it is counted and logged there rather than drawn. P3 is where it becomes
vertex buffers, a depth buffer, texture decode and real pipelines, and where the software rasteriser
leaves the path. (This question was asked directly at the end of the session, which is why it is
written down here rather than left implicit in the plan doc.)

What was built:
- `renderer_vk/shaders/` — `fullscreen.vert` (three positions from `gl_VertexIndex`, no vertex
  buffer), `passthrough.frag` (sample, alpha forced to 1 because MAME's high byte is X, not A), and
  `build_shaders.sh`, run by hand with the SPIR-V committed. The headers emit **`uint32_t` words,
  not `xxd -i` bytes**: `vkCreateShaderModule` takes a `const uint32_t*` and an `unsigned char[]`
  carries no 4-byte alignment guarantee. That is undefined behaviour that happens to work.
- `vk_present.cpp` grew the shared objects (render pass, sampler, descriptor layout + pool, pipeline
  layout, pipeline) and four more per-slot ones (framebuffer, persistently mapped staging buffer,
  texture + view, descriptor set). All of it is built and destroyed **with the ring**: the pool is
  sized off the ring and the framebuffers need the render pass, so one lifetime means one build path
  and one teardown path instead of two that have to agree.
- `vk_funcs` roughly tripled. Still no Vulkan library on the link line; `otool -L` names none.
- The render pass's `finalLayout` now does what the clear's closing barrier did, with **both**
  subpass dependencies stated explicitly. The implicit ones are `TOP_OF_PIPE` in and
  `BOTTOM_OF_PIPE` with no access mask out, and the latter is no dependency at all as far as the
  frontend's fragment shader read is concerned. Still no semaphores.
- `loadOp` is `CLEAR` to black rather than `DONT_CARE` even though the triangle covers every pixel.
  Diagnostic, not correctness: a draw that fails to happen reads as black instead of as whatever was
  last in that memory, which would read as a picture.

Verified:
- `ring of 3 496x384 B8G8R8A8_UNORM images, sync index mask 0x7, queue family 0; 2232 KiB of
  staging`, then `first frame presented: 496x384 through slot 1`. No errors, no validation messages,
  clean teardown, exit 0.
- **`cmp /tmp/m2dump-src.ppm /tmp/m2dump-vk.ppm` is silent** at presented frame 1500 of vf2. The
  picture is Pai in her stage — real textured 3D, not an attract text card, screenshotted before
  being believed as the P1 gotchas demand.
- Software path unchanged: `retrohost` vf2 1500 frames, 1500 with video, 461% of real time.
- MoltenVK logs `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart`
  once per pipeline, from `primitiveRestartEnable = VK_FALSE`. A warning; creation succeeds and the
  draw is not indexed.

### Two things that cost real time, both recorded in the plan's gotchas

**`pause_nonactive` defaults to true, and it silently ate most of the afternoon.** RetroArch pauses
the core whenever its window is not focused, and a run launched from a shell never gets focus. The
symptom is not an error: RetroArch spins through `--max-frames` in milliseconds, reports
`Content ran for a total of: 00 hours, 00 minutes, 00 seconds`, exits 0, and every log line looks
healthy — while the core has advanced about three frames. A read-back armed for frame 1500 never
fires, and the obvious conclusion (the read-back logic is broken) is wrong. **Read
`Content ran for a total of` first**: if it does not roughly match `max-frames / 57.5`, nothing else
in the run means anything. `pause_nonactive = "false"` goes in the `--appendconfig` file.

**There is no working way to screenshot RetroArch from here, step 3's note notwithstanding.**
`screencapture` still fails on the Screen Recording permission. The network command interface is up
and commands demonstrably arrive — `GET_STATUS` **segfaults RetroArch 1.22.2** in its own
`command_get_status` (`strlen(NULL)`, reached from `command_network_poll` during input polling;
nothing to do with the core, it merely unwinds through `retro_run`) — but `SCREENSHOT` writes no
file, with `video_gpu_screenshot` either way, with the directory existing, with the core unpaused,
and with nothing logged. Step 3's recipe does not reproduce; treat it as unexplained, not as a
method.

So step 4 grew a read-back instead: **`M2VK_VK_DUMP=<prefix>`**, with optional
`M2VK_VK_DUMP_FRAME=<n>` (default 600), writes `<prefix>-src.ppm` (what went into staging) and
`<prefix>-vk.ppm` (the ring image copied straight back in the same command buffer, after the draw).
One extra entry point, one buffer that exists only when the variable is set, and it stalls only the
frame it is taken on. It is better evidence than the screenshot would have been — the core's own
pixels, with the frontend's presentation nowhere near them — but it does **not** retire step 7: this
compares source against read-back inside one Vulkan run, and the exit criterion is software against
Vulkan across two renderers. It does make that result predictable.

```sh
M2VK_VK_DUMP=/tmp/m2dump M2VK_VK_DUMP_FRAME=1500 \
  /Applications/RetroArch.app/Contents/MacOS/RetroArch -v --log-file /tmp/ra.log \
  --appendconfig /tmp/ra.cfg --max-frames=1700 \
  -L ./model2_libretro.dylib devnotes/roms/vf2.zip &
wait                       # backgrounded: see pause_nonactive above
cmp /tmp/m2dump-src.ppm /tmp/m2dump-vk.ppm
```

### Three observations from an interactive vf2 session, after the commit

Launched windowed for a play-test rather than a measurement, so RetroArch ran without `-v` and the
core's own log lines are absent. All three are inputs to step 5 rather than conclusions:

- **A window resize made MoltenVK create swapchain images twice** — `(2940, 1846)` then
  `(2790, 1790)`. That is the shape of a `context_destroy`/`context_reset` pair, i.e. plausibly the
  first time the ring-rebuild path has ever executed, and there is no log to confirm it either way.
  Step 5 should reproduce this deliberately, with `-v`.
- **`Destroyed VkPhysicalDevice for GPU Apple M5 with 59 MB of GPU memory still allocated`** at
  instance teardown. This does *not* isolate a leak to us: RetroArch's own resources are in that
  number and the line fires after our `context_destroy` has already run. Worth accounting for at
  step 5 rather than waving past.
- **72.88 % of full speed** over 54 seconds. Expected — the software rasteriser is still doing all
  the drawing and the GPU is blitting one triangle — and the contrast with `retrohost`'s 461 %
  headless says the cost is presentation, not emulation. Performance is not scoped until later; noted
  so that the number is not mistaken for a regression when it changes in P3.

Next: step 5 — lifecycle. Force `context_destroy`/`context_reset` (fullscreen toggle, video driver
change and back, resize) and confirm no leak, no crash, the picture returns, and the emulator has
not lost a beat. The ring rebuild path is written but has never knowingly run.

## 2026-07-25 — P2 step 5: lifecycle

Forced `context_destroy`/`context_reset` and confirmed all four of the step's claims. The lever is
**`FULLSCREEN_TOGGLE` sent to RetroArch's network command port**, which is a complete video-driver
teardown — new `VkInstance`, new `VkDevice`, new swapchain, new interface pointer — so it covers the
"change the video driver and back" and "resize" cases at the same time. It is also the only one of
the three network commands tried here that works; `SCREENSHOT` still writes nothing and `GET_STATUS`
still segfaults RetroArch.

Six forced cycles in one 3400-frame vf2 run, exit 0, no error or warning from the core:

- **Picture returns.** `picture resumed at frame N` after every reset, and an `M2VK_VK_DUMP`
  read-back taken *in the fourth context* is still bit-identical to the software frame that went in.
- **The emulator has not lost a beat.** Frame accounting is continuous across every handover —
  1078 → 1416 → 1765 → 2058 → 2409 → 2745, each context resuming at exactly the frame the previous
  one ended on — and the polygon tap, which is not the Vulkan side, counts straight through.
- **No leak.** MoltenVK's `still allocated` at each device teardown tracks the *window* (20–21 MB
  windowed, 58–63 MB fullscreen) and is identical at cycles 1, 3 and 5 after intervening teardowns.
  RSS settles flat. **This explains the 59 MB line from the step-4 play session**: it is RetroArch's
  swapchain, not us. Our ring is ~7 MB, which is a useful number — it is the step this figure moves
  by when the ring really is leaked.

### Three defects found by reading, none reachable through RetroArch's own ordering

- **The slots were a `std::vector` and could not be.** `vk_present.h` already stated the rule — the
  `retro_vulkan_image` given to `set_image` must stay at a stable address, because the frontend keeps
  the pointer and reads it again on a duped frame — and a ring rebuild frees the old storage before
  the new ring is handed over. Now a fixed `std::array`, zeroed in place on teardown.
- **`M2VK_VK_DUMP_FRAME` counted from the wrong zero.** The counter restarted at every context reset,
  so a read-back armed for frame 1500 fired 1500 frames after the *last* reset — a different frame,
  silently, in a phase whose premise is that a fixture is `(rom, frame)`. It now survives context
  loss. Not cosmetic: this verification needed a dump landing after a reset, and under the old
  counter no context in these runs lasted long enough to reach one.
- **A `context_reset` with no `context_destroy` before it would have destroyed the ring against a
  dead device.** The header called that ordering legal and the code claimed to handle it, but it ran
  `vkDeviceWaitIdle` and `vkDestroyImage` on dead handles. The ring is now *abandoned* instead —
  dropped without a Vulkan call, leaking ~7 MB — which is the cheap side of the trade.

**The obvious fix for the third one is wrong, and finding that out was the useful part of the day.**
The first attempt compared the incoming `VkDevice` against the one the ring was built on: same device
means the old objects are still real, different device means they are not. It does not work.
**MoltenVK recycles the handle** — RetroArch destroys the device and creates a new one, MoltenVK logs
both, and the new handle compares *equal* to the destroyed one. So that guard did precisely what it
was written to prevent, and silently: no crash, just the old ring's handles destroyed against a
device that never owned them, with `still allocated` going 20 → 27 MB as the tell. A handle is a
value, not a reference; the only sound signal that a device is gone is `context_destroy` having been
called, which is what the code tests now.

### Two paths that cannot be reached through RetroArch, and how they were run anyway

"It compiles" is not evidence, so both were exercised with throwaway one-line patches and then
reverted — deliberately not left in as test hooks, since their job is to break the lifecycle:

- **the in-place ring rebuild** (written at step 3, never executed until now): `|| (s_frames == 900)`
  and friends bolted onto the rebuild condition in `present_frame`. Three rebuilds, picture
  uninterrupted, frame count continuous.
- **the abandon path**: an early `return` in `context_destroy_cb` behind an env var, so the ring
  survived into the next reset. `the context was replaced without being destroyed; 3 slots
  abandoned`, picture resumes, no crash.

Still not exercised by any route: a rebuild triggered by a *genuine* mask or geometry change. The
mask was `0x7` in every context of every run and 496×384 never varies. `retrohost-vk` owns the mask
and can force it properly at step 7.

### Two harness facts that invalidate earlier advice

- **`--max-frames` restarts at every video-driver reinit**, so a run with N toggles lasts about
  (N+1) × `max-frames`. Confirmed exactly: the last context of a `--max-frames=3400` run reported
  `destroyed after 3400 frames in this context` at 6145 frames since load. The
  `Content ran for a total of` heuristic still catches the `pause_nonactive` failure (far too short)
  but "longer than expected" is now normal. The core's own frame counts are the better measure.
- **`config_save_on_exit` defaults to true, so `--appendconfig` values are written into the user's
  real `retroarch.cfg` on exit.** That is how `video_fullscreen` became `"false"` on this machine
  while CLAUDE.md still described it as on: a previous session's harness edited the user's
  configuration as a side effect. `config_save_on_exit = "false"` now goes in the appendconfig.
- **RetroArch remembers core options per core** in `config/Model 2/Model 2.opt`, and a shell run
  inherits them. The first step-5 run measured the *software* path end to end because an earlier
  interactive session had left `model2_renderer = "software"` there, and nothing looked wrong. Pin
  that file in the harness, and read `[model2] options:` before believing a result.

Next: step 6 — `renderer=software` regression. Flip the option and confirm identical behaviour to the
P1 build, including under `retrohost`, which never sees HW render at all.

## 2026-07-26 — P2 step 6: the software path is unchanged

**No code changed.** The software renderer is the A/B ground truth, so the requirement is that P2 did
not move it by a bit — and it did not. Measured against a real P1 binary rather than by reading the
diff.

**Building "the P1 build" cost two minutes, not a worktree.** P2 modified exactly two files the build
compiles — `retro_entry.cpp` and `scripts/src/osd/libretro_m2.lua` — and everything else it added is
new files, which drop out of the project when the lua reverts. So `git checkout 00a245ac219 --` those
two, `REGENIE=1`, build, keep the dylib, check the two files back out, rebuild. The object directory
is shared, so each direction relinks in about two seconds. `nm` on the result finds no
`declare_hw_render` and no `vk_build_info`, and it is 37 KB smaller: it really is P1.

Three configurations through `retrohost`, each with a private empty save directory — credits live in
battery RAM and a stale one changes the emulated timeline:

| | |
|---|---|
| `p1` | the P1 build at its own default (`vulkan`; P1 answers "not built into this core yet") |
| `head-sw` | HEAD with `model2_renderer=software` |
| `head-vkdefault` | HEAD at its default `vulkan` — declared, refused by retrohost, falls back |

Over `vf2` (4500 frames), `vcop2` and `srallyc` (2500 each) and a `vf2` run driven by the input
script `600:select,660:start,900:b:30,1200:lx+:200`, **every artifact is byte-identical**: the last
frame as a PPM, the per-polygon dump, the run summary, the whole per-frame polytap stream (3191
rendered frames for vf2), retrohost's frame and audio accounting, and the NVRAM and `.cfg` trees MAME
wrote. The vf2 rendered-frame-800 dump also matches the **committed P0 fixture** byte for byte, which
turns this from a HEAD-vs-P1 comparison into an absolute anchor: the stream is what it was at P0.

The one difference from P1 worth stating: `retro_init` logs the `vulkan headers 1.4.350 …` line on
every path, software included. It is a log line; nothing downstream of it differs.

Under RetroArch, flipped both ways at 1200 frames: with `software` the core emits 3 log lines, none
of them `vk:`, and **RetroArch never logs `SET_HW_RENDER` at all**; with `vulkan` it emits 31, 28 of
them `vk:`, and RetroArch logs `SET_HW_RENDER, context type: vulkan`. Both ran 20 s of content and
exited 0. `M2VK_VK_DUMP` is inert on the software path — no file, no log line — which matters because
it is armed from an environment variable a user could have left set.

### The half-hour this actually cost: an idle Mac stalls RetroArch, and it looks like a core hang

A `--max-frames=1200` RetroArch run that should take 21 s was still running at five minutes, with
**zero** rendered frames in 75 s of `M2VK_POLYTAP` output — stopped, not slow. The machine had been
user-idle for ~40 minutes (`pmset -g assertions`: `UserIsActive 0`, `HIDIdleTime` ~2400 s) and macOS
throttles the unfocused window's presentation; RetroArch's main loop stops calling `retro_run` with
it. Worse, **short runs still pass** — 300 frames finished in 5 s, 600 did not — so it presents as
"the software path hangs after a few hundred frames", which is exactly the regression this step was
looking for.

What told it apart was **running the other renderer**: the Vulkan path stalled identically, and it
had run 3400 frames the evening before. A failure both renderers share belongs to the environment,
not to the path under test — that is the check to reach for first, not last. `caffeinate -dsu` in
front of the command fixes it: the same 1200-frame run then finishes in 21.9 s. Every RetroArch
invocation should carry it. It is a different failure from `pause_nonactive`, which runs far too
*fast* rather than never finishing.

### Correction to step 4: the primitive-restart warnings are not ours

Step 4 recorded MoltenVK's `Metal does not support disabling primitive restart` as coming from our
pipeline, once per creation. A 300-frame vf2 run emits **36 of them on both paths** — including
`renderer=software`, where the core declares no context and creates no pipeline at all — and the
count does not move by one when our ring, and with it our pipeline, is built. They are RetroArch's
own stock shaders. Nothing to fix, but they would still be there with the core removed.

Next: step 7 — `retrohost-vk`, the headless MoltenVK host with image read-back to PPM, and with it
the phase's exit criterion: same ROM, same frame, `renderer=software` vs `renderer=vulkan`,
PPMs bit-identical under `cmp`.

## 2026-07-26 — P2 step 7: the exit criterion, met

**`renderer=software` and `renderer=vulkan` produce byte-identical pictures.** Four cases, each run
twice through the same host — vf2 (4500 frames), vcop2 (2500), srallyc (2500), and a scripted vf2
run (`600:select,660:start,900:b:30,1200:lx+:200`, 2500) — and in every one the last frame's PPM,
the whole-run picture digest, the per-frame polytap stream, the frame and audio accounting and the
NVRAM/`.cfg` tree MAME wrote are all identical. The Vulkan run's rendered-frame-800 dump is also
byte-identical to the **committed P0 fixture** `fixtures/vf2-frame800-polytap.txt`, so this anchors
to P0 directly rather than only to the software path of the same build.

| | digest over the run |
|---|---|
| vf2, 4500 | `8699deecfd9062b7` both paths |
| vcop2, 2500 | `0d0fd33179b795c4` both paths |
| srallyc, 2500 | `0d29e6de5aad4354` both paths |
| vf2 + input script, 2500 | `58df60d4601455e0` both paths |

### `retrohost --vk`, not a second binary

One host, one flag. The plan left the choice open; a flag wins because every non-video behaviour —
options, saves, input script, frame counting, the PPM writer — is then not merely "kept in sync"
but *literally the same code*, which is the only thing that makes an A/B comparison mean anything.
Without `--vk` the host refuses `SET_HW_RENDER`, which is exactly what it did before it could do
Vulkan at all, so the software side of the comparison is untouched by construction. Build with
`devnotes/build-retrohost.sh` (two include paths, no libraries).

**Two decisions inside it are load-bearing:**

- **It includes the core's own `libretro.h` and `libretro_vulkan.h`** instead of hand-rolling the
  structs, as the host had done since P1. `retro_hw_render_interface_vulkan` is far too big to
  retype, and a field at the wrong offset would be silent corruption across a `dlopen` boundary, not
  a compile error. The hand-rolled block is gone; the host and the core can no longer disagree about
  a layout. Verified rather than assumed: the software path through the new binary is byte-identical
  to the pre-step-7 binary — PPM, the entire polytap stream, the save tree — with only the new
  `digest:` line added to stdout.
- **The read-back lands in the same buffer the software path fills, in MAME's own pixel form.**
  `B8G8R8A8_UNORM` read back is B,G,R,A in memory, which little-endian reads as `0xAARRGGBB` — so
  the host points its frame pointer at the read-back and every downstream step (PPM writer, digest,
  frame counter) is one code path for both renderers. There is no conversion anywhere to be wrong.

The read-back happens inside `video_cb`, per frame, not once at the end: it is when a real frontend
would sample the image, it makes the digest cover every frame instead of the last one, and by the
time the final PPM is written that same path has already run three thousand times. It costs about
20 % of throughput (534 % of real time software, 406 % under Vulkan with a per-frame GPU stall).

**The digest is worth more than the PPM.** FNV-1a over every frame's visible RGB, in the PPM
writer's own byte order, printed by both paths. A last-frame `cmp` proves one frame out of 4500;
this proves all of them for the price of a pass over a buffer that is already hot, and it is what
the SSIM harness in P5 will be calibrated against.

### The two paths RetroArch cannot reach, finally exercised

Both were called out at step 5 as unreachable, and both are now scripted from environment variables
because the host owns what RetroArch does not:

- **A genuine sync-mask change** (`M2VK_HOST_MASK_AT=800:0x1,1600:0xf`). RetroArch's mask is `0x7`
  in every context of every run, so the core's rebuild-on-mask-change had never run against a real
  change — only against a throwaway patch. It now runs both directions, **3 → 1 → 4 slots**, mid-run,
  with the picture uninterrupted and the digest still equal to the software reference.
- **Context loss** (`M2VK_HOST_RESET_AT=800,1600`), which tears down the device and builds a new one
  — what a RetroArch fullscreen toggle does, without needing a window. Three contexts, frame count
  continuous across both handovers, bit-exact. With `M2VK_HOST_SKIP_DESTROY=1` the same thing
  happens *without* `context_destroy` first, and the core takes its abandon path
  (`the context was replaced without being destroyed; 3 slots abandoned`) and stays bit-exact.

**The mask change found a bug — in the host, not the core.** The first run segfaulted at the
`0x1 → 0xf` step: the host built command pools and fences for as many slots as the *initial* mask
needed, so slot 3 was a set of null handles when the mask grew. The core's own log had already done
its half correctly (`ring of 1 destroyed … ring of 4 … mask 0xf`) before the host fell over on the
frame it produced. Slot resources belong to the host and not to any particular mask, so it builds
all eight up front now, and clears every cached image on a mask change since the ring those images
belonged to is about to be destroyed. The core needed no change: it survived every ordering thrown
at it exactly as designed.

### Homebrew's MoltenVK is not RetroArch's, and the differences are small but real

The host dlopens `/opt/homebrew/lib/libMoltenVK.dylib`; RetroArch bundles its own inside the
`.app`. They are different builds, so the step-2 probe log is not reproduced verbatim under
`retrohost --vk`:

| | RetroArch (step 2) | `retrohost --vk` |
|---|---|---|
| MoltenVK | 1.3.0 (`driverVersion` 0x283c) | **1.4.2** (0x28a2) |
| instance api | 1.3.313 | 1.4.357 |
| device `apiVersion` | **1.1.0** | **1.1.357** |

Both clamp the device to the 1.1 the instance asks for — the host requests `VK_API_VERSION_1_1`
deliberately, to mirror RetroArch, because asking for more would hand the core a device with a
different ceiling than the one it sees in the real frontend and quietly make the two hosts
non-comparable. Everything P3 depends on is the same on both: **`D24_UNORM_S8_UINT` still does not
exist**, `D32_SFLOAT` and `D32_SFLOAT_S8_UINT` do, `depthBiasClamp` is present, `geometryShader` and
`logicOp` and `wideLines` are not, copy alignments are 16/1/16, and the unnamed `0x80000000`
format-feature bit is still there (with `0x10000` alongside it now). The host creates its device
with **no features enabled at all** — the passthrough needs none, and a feature turned on here but
not by RetroArch would be a difference that only surfaces once a later phase depends on it.

### Notes for whoever reads this next

- **Nothing in step 7 is committable.** The entire deliverable is `devnotes/retrohost.c` and
  `devnotes/build-retrohost.sh`, and `devnotes/` is local-only. The core is unchanged — HEAD is
  still `c18e41c536b`, and P2 has now been verified end to end without touching it.
- **`/dev/null` is a valid PPM destination** when only the polytap dump is wanted.
- **`M2VK_POLYTAP_DUMP=N` counts *rendered* frames, not host frames.** vf2 renders nothing for the
  first ~990 frames, so rendered frame 800 is around host frame 1790 — a 1500-frame run produces no
  dump file at all and looks like a broken flag.
- The host implements `set_command_buffers` and `set_image`-with-semaphores as hard failures rather
  than no-ops. The core uses neither; if that ever changes, this host would silently read an
  unfinished frame, and a bit-exact comparison that quietly stopped being bit-exact is the worst
  possible failure mode for the thing the whole A/B harness rests on.

Next: step 8 — docs. Update `p2-vulkan-passthrough.md` to as-built in full, add the P2 row to the
README index, and fold the MoltenVK numbers above into whatever P3 reads first.

## 2026-07-26 — P2 step 8: docs, and P2 is closed

No code, and none was expected. Three things happened.

**`p2-vulkan-passthrough.md` is now as-built rather than a plan.** The "Order of work" had already
been kept current step by step; the *body* had not, and was still describing the ring, the format
chain and the headless host in the future tense in places its own step notes had long since settled.
Each of the five problem sections now carries an as-built block under the original text. **The
original text stays** — where a decision came out differently from the plan, the reasoning that
turned out to be wrong is usually the part worth reading, and deleting it would leave the correction
looking like it had been obvious all along. The Risks section had two entries step 7 answered, struck
through with what actually happened, and two entries were *added*, because closing a phase is when
the risks it hands forward are clearest:

- the two hosts permanently run different MoltenVK builds, and the failure mode to watch for in P3 is
  a capability present on one and not the other — the harness would report that as a renderer bug;
- **nothing has been rendered yet.** P2's bit-exactness is the bit-exactness of a `memcpy` through a
  fullscreen triangle. Reading it as a stronger result than that is the main hazard the phase leaves
  behind, and it has already had to be written down twice.

**`devnotes/vulkan-target.md` is new** — the measured device, extracted from two probe logs buried in
this file, in the form P3 will actually want it: formats (no `D24_UNORM_S8_UINT`, use `D32_SFLOAT`),
features (`depthBiasClamp` yes, `geometryShader` and `wideLines` no), a limits table with **both**
hosts side by side, the copy-alignment numbers, and the extensions that are available on the device
but unreachable until context negotiation lands. It exists because the alternative was P3 starting
with "read the worklog bottom-up until you find the probe log", and because the two logs are not
interchangeable — a fact that is invisible if you only ever read one of them.

**Two limits differ between the hosts, and both in the dangerous direction:** `maxImageDimension2D`
16384 (RetroArch) vs 32768 (Homebrew), `maxSamplerLodBias` **4.0 vs 16.0**, plus 128 vs 256 sampled
images per stage. A renderer written against the host's numbers would work headless and fail in the
frontend. `vulkan-target.md` says take the smaller of each column; the LOD-bias one is the likely
biter, since microtexture LOD is a real Model 2 feature and 4.0 is not much headroom.

Also settled while writing it: the unnamed `0x10000` format-feature bit the newer MoltenVK reports is
`VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT` — checked against the header rather than guessed
— and `0x80000000` is the flags2 leak already recorded at step 2.

**The end-of-step ritual is now written down**, in CLAUDE.md and in `devnotes/README.md`'s
conventions: a step is not done when the code works, it is done when the worklog, the phase's plan
file and CLAUDE.md's "Where we are" + "Next step" have all been updated. A direct response to
CLAUDE.md having gone two steps stale by the end of step 7 and pointing the next session at work that
was already finished.

**P2 is closed.** Five commits, HEAD `c18e41c536b`, `git status` clean, the upstream diff still 16
lines in `model2_v.cpp`. Steps 6, 7 and 8 changed no core code at all.

Next: **P3 — HW geometry.** First task is a `devnotes/p3-*.md` plan file in the shape of the P1 and P2
ones, not code. Read `vulkan-target.md` first, and note that decals and z-fighting are deliberately
P4: P3's job is to get the tapped polygon stream on screen at native 1× with nearest filtering, so the
A/B harness built at step 7 can still compare it against the software renderer.

---

## 2026-07-26 — P3 planned, and step 1 (the layer sandwich) built and verified

### The plan file — `devnotes/p3-hw-geometry.md`

Written first, as CLAUDE.md's "Next step" asked. Same shape as the P1 and P2 files: what the phase
delivers, an exit criterion, the problems and how each is handled, where the code goes, the order of
work, gotchas known in advance, risks. The four decisions in it that were not in the plan of record,
and why:

- **The frame is a sandwich and only the middle is ours.** `screen_update` draws background tilemaps,
  then the 3D, then foreground tilemaps. There is no compositing a GPU 3D layer onto MAME's finished
  frame — the foreground has to go over it. So the two 2D layers get captured separately and the
  compositing moves to the GPU. Rejected the obvious alternative (read the GPU's 3D back into
  `m_destmap` and let MAME composite as usual, which would have kept the bit-exact harness working
  unchanged): the seam is on the emulation thread, Vulkan is only ever touched on the frontend's, and
  a readback would be on the wrong thread and one frame early. It also dies at P5 — a 4× frame does
  not fit in a 512×512 `bitmap_rgb32`.
- **Depth is draw order, not z.** The software renderer's whole hidden-surface algorithm is
  `m_fillmap`: front-to-back, first writer wins the pixel. A depth buffer reproduces that exactly with
  a flat per-poly key of `1 - n/65536`, test GREATER, depth write on. Interpolated 1/z is *more*
  correct 3D and would z-fight across most of the frame — 86 % of polys share a bucket with an
  adjacent one — so it is P4's, together with the decal fix, deliberately. Also settled while working
  this out: **nothing blends.** Model 2 translucency is a cutout (< 50 % discarded) and `checker` is a
  stipple; both are per-pixel discards and every surviving fragment is opaque. P3 needs no sorted
  transparent pass, contrary to `model2_libretro_core.md` §4.
- **Texture RAM is 2 MB in total and wants no atlas.** Each sheet is one 1 MB memory share = 1024×2048
  at 4 bpp. Upload both as raw `uint32` words and transliterate `get_texel` in GLSL: no decode, no
  cache, no eviction, no packing. And the mip chain needs no generation — `fetch_bilinear_texel`
  reaches level *n* by shifting `texx`/`texy` and reading `texsheet[level & 1]`, so **the mips are
  already in texture RAM at other addresses, alternating between the two sheets**. Both sheets have to
  be addressable from every draw.
- **Filtering must be done by hand.** Three independent reasons, each fatal on its own: MAME
  interpolates in *index* space before the luma/colour LUTs; the translucent filter makes a
  transparent texel take its neighbour's luma; and the non-wrap edge case snaps `ufrac` rather than
  clamping. No sampler state does any of that. `model2rd.ipp` gets transliterated, not reinvented.

**Two things in `model2_libretro_core.md` §4 do not survive contact with this seam**, and the plan
file says so rather than dropping them quietly:

- **`det(3×3)≈0` shadow detection cannot be done here.** It is a *Model 1* technique operating on
  per-object model→view matrices in Unity's playback path (`model1_sortorder.md` §9.5). At this seam
  there are no matrices — polygons arrive transformed, projected and clipped. It is also unnecessary:
  `model2_shadows.md` proved VF2's shadows are real flattened silhouette geometry (15 meshes, 32
  polys, rigidly attached to the fighter, camera-independent over a 168° orbit), so they arrive as
  ordinary polygons and become an instance of P4's coplanar problem, not a special pipeline.
- **Most of `model2_lighting.md` is already done for us.** §1's first block — dot products, diffuse,
  ambient, the Phong specular — is the geometry engine's work and MAME's copro emulation has already
  performed it by the time the tap fires. The result is the single `poly.luma` byte. What P3 ports is
  §1's *second* block, the raster tail (`lumaram` → `colorxlat` → gamma). Porting the first would be
  recomputing an answer we were handed.

The exit criterion changes shape too, and this is the phase transition P3 represents: a GPU rasterizer
and MAME's scanline loop will never agree on coverage, so **bit-exact is gone for good**. What
replaces it is two-part — every pixel the 3D layer does *not* touch stays bit-exact (that part is
still `cmp`, and it catches compositing/crop/palette regressions), plus SSIM with the residual
*attributed*. "0.97 and we don't know why" fails.

### Step 1 — the sandwich, with MAME's own 3D still in the middle

Built and verified. The trick that made this cheap: **the under-layer hook goes *after* the
render-polygons branch, not before it.** While the software rasteriser still owns the 3D it is
already in the captured layer, so the GPU composite is under + over and must equal MAME's own frame
exactly — a `cmp`-able test of the layer plumbing with no polygon renderer in existence yet. When
`m2vk::rasterize()` goes false at step 3 the identical hook yields background alone and the 3D goes
in the gap. One hook, both renderers, no third capture of `m_destmap`.

What was written:

- `src/osd/libretro_m2/m2vk_frame.{h,cpp}` — the frame record. Two layers, cropped to the visible
  rectangle at capture, one record and no lock (the OSD's baton already parks the writer for the whole
  of `retro_run`; the assumption is stated in the file rather than left to be rediscovered).
- `m2vk_sink.h` — `capture_layer()`, templated on the bitmap and rectangle types for the same reason
  `submit()` is, plus `rasterize()` / `set_rasterize()`.
- `renderer_vk/shaders/overlay.frag` — the foreground layer. This is `copybitmap_trans(..., 0)`:
  discard when the texel is exactly zero. It tests **all four channels**, not just alpha — every pen
  the tilemap draws carries alpha 0xff so the two agree today, and testing alpha alone would be a
  silent trap the first time something writes a pen that does not.
- `renderer_vk/vk_present.cpp` — `frame_slot` grew a `layer_tex[2]`, the descriptor pool doubled, and
  a second pipeline (`s_pipeline_over`) built from the *same* structs as the first with only the
  fragment module swapped. Two near-identical 60-line pipeline descriptions that have to stay in step
  is how the second one ends up subtly different.
- `retro_entry.cpp` — `frame_enable(s_hw_render)`, so the software path pays a predicate and nothing
  more; `frame_end_run()` on unload.

**Upstream diff against mame0288: 16 → 26 lines** (20 of them inside the six `#ifdef M2VK` blocks; the rest are blank separators). Three new `#ifdef M2VK` sites: `capture_layer(LAYER_UNDER)` after
the render-polygons branch, `capture_layer(LAYER_OVER)` before the last `copybitmap_trans`, and
`if (!m2vk::rasterize()) return;` inside the existing `model2_3d_render` block.

**Verification — everything below is a `cmp`, not an eyeball:**

| | |
|---|---|
| vf2 1500, vcop2 2500, srallyc 2500 | `renderer=software` vs `renderer=vulkan` **byte-identical PPM and identical whole-run digest** (vf2 `cf043ff583370663`, vcop2 `0d0fd33179b795c4`, srallyc `0d29e6de5aad4354`) |
| two context losses + two mask changes (0x7→0x1→0xf) mid-run | digest `cf043ff583370663` — **identical to the undisturbed run** |
| device loss with no `context_destroy` (the abandon path) | PPM identical to the undisturbed run |
| `M2VK_VK_DUMP` in-core read-back | `-src.ppm` == `-vk.ppm`, under `retrohost --vk` *and* under RetroArch |
| committed P0 fixture `vf2-frame800-polytap.txt` | still `cmp`-identical |
| RetroArch 1.22.2, 1400 frames | ran 24 s (1400/57.5 = 24.3 — the honest number), no errors, dump bit-exact |

**The sanity check that mattered more than any of them:** `M2VK_NO_SW_3D=1` (new, and it becomes the
normal path at step 3) drops the software rasteriser's per-poly dispatch. The picture comes back with
the sky, clouds and horizon intact, every polygon gone — Pai, the lantern, the arena floor, the
banners — and **`CREDIT 0/2` and `© SEGA 1994` still drawn on top of the hole**. That is the whole
claim of the sandwich in one image: the two 2D slices are genuinely separate, and the foreground is
genuinely composited last. 37073 of 190464 pixels differ, i.e. the 3D covers 19 % of this frame.

Two things noticed and worth having written down:

- MoltenVK emits `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart`
  once per pipeline creation. Pre-existing (P2's single pipeline did it too), harmless for
  non-indexed draws — but **step 3 introduces indexed draws**, so index values must never be
  `0xffff`/`0xffffffff`, because primitive restart cannot be turned off on this implementation.
- The ring's staging log line was counting one layer's worth. Now `4464 KiB` for 3 slots × 2 layers,
  which is the truth.

One thing the build nearly got away with: `m2vk_frame.cpp` had to be added to
`scripts/target/mame/model2.lua` as well as to the OSD's own script. That file compiles the sink into
the **standalone `mamemodel2`** binary, which no other OSD links `src/osd/libretro_m2/` for — so the
new `capture_layer()` call in `model2_v.cpp` would have left `mamemodel2` with undefined symbols.
Both builds and the polytap sweep were re-run afterwards.

Next: **step 2 — record the geometry and hand it across the baton, draw nothing.** Output must still
be bit-exact against this step, poly counts must match the polytap frame for frame, and RSS must be
flat.

---

## 2026-07-26 — P3 step 1 committed, step 2 (the geometry record) built and verified

### Step 1 is now a commit

`d05bdda8f31`, "Composite the 2D tilemap layers on the GPU". No code changed from what the previous
entry describes; it had simply never been committed.

### Step 2 — record the geometry and the tables, draw nothing

The frame record grows the other two thirds of what the hardware renderer will need. Nothing is
drawn from either, and the picture is still MAME's software rasteriser composited between the two 2D
layers — which is the point, because it means the whole step is checkable by `cmp`.

**Where the structs went, and why it is not where the plan said.** `m2vk::vertex` and `m2vk::poly`
moved out of `m2vk_sink.h` and into `m2vk_frame.h`. The record has to hold a `std::vector<poly>` and
`m2vk_sink.h` already includes `m2vk_frame.h`, so leaving `poly` in the sink meant either a circular
include, a third header, or an extra include line in `model2_v.cpp`. Moving the data shapes to the
file that owns the data costs nothing and the division reads better afterwards: **`m2vk_frame.h` is
what crosses, `m2vk_sink.h` is how it is intercepted.** `model2_v.cpp` did not have to change at all.

**What crosses now:**

| | how | why there |
|---|---|---|
| the poly stream | `std::vector<poly>` + a count, appended in submission order | that order *is* the hidden-surface algorithm — first writer wins the pixel — so it has to come from the seam and can never be reconstructed from the display list |
| `palcolor` | one `u16` per polygon, `m_palram[colorbase + 0x1000]`, resolved at submit | exactly what both scanline renderers do, and it turns a 16 KB table into 2 bytes per polygon |
| `colorxlat` | 0x6000 bytes, **gamma already folded in** — `gamma[colorxlat[i] & 0xff]` | every reader of one is a reader of the other in `model2rd.ipp`, so the fold is lossless and it halves the bytes *and* the shader's dependent lookups |
| `lumaram` | 0x8000 bytes, as-is | — |

Doing the palette resolve and the table snapshot on the emulation thread is safe for a reason worth
writing down rather than re-deriving: the scanline callbacks read these same tables during
`render_polygons`, which runs **on this thread inside `screen_update`**, so no CPU write can land
between the record and the raster it describes.

`frame_begin` grew the `frame_tables` parameter as planned, and it is passed braced at the seam —
`m2vk::frame_begin(raster->poly_list_index, { m_colorxlat.get(), m_lumaram.get(), m_gamma_table })`.
One existing line changed, none added: **the upstream diff is still 26 lines.**

**The tables carry their own serial, bumped only when the bytes actually changed.** The compare rides
along with the copy rather than being a separate `memcmp` — both tables are read start to finish
either way. vf2 changes them **six times in nine hundred frames**, so the upload at step 4 can skip
almost every frame.

**`active()` is now `g_active || capturing()`.** The polygon tap used to be the only thing that could
turn the stream on, which meant the record would have been dead unless a diagnostic environment
variable happened to be set. `g_want_layers` was renamed `g_capturing` for the same reason — it
gates geometry as well as layers now, and the old name would have lied.

The frame record is deliberately **not** a consumer behind the sink: it is the renderer's half of the
seam rather than something watching it, it is the only thing that needs the tables, and it must not
depend on a consumer list that an environment variable can empty.

### Verification — all of it mechanical

| | |
|---|---|
| vf2 1500, vcop2 2500, srallyc 2500 | `renderer=software` vs `renderer=vulkan` **byte-identical PPMs**, and all six whole-run digests equal the values step 1 recorded (`cf043ff583370663`, `0d0fd33179b795c4`, `0d29e6de5aad4354`) |
| the record vs the polygon tap | **poly counts and submitted counts identical frame for frame**: 511/512 on vf2, 1804/1805 on vcop2, 1142/1143 on srallyc |
| two context losses + two mask changes (0x7→0x1→0xf) | digest `cf043ff583370663`, and the PPM `cmp`s clean against the undisturbed run |
| the abandon path (reset with no `context_destroy`) | same |
| committed fixture `vf2-frame800-polytap.txt` | still `cmp`-identical |
| the standalone `mamemodel2` | still runs; the seam's new signature compiles in a build with no libretro OSD |
| RetroArch 1.22.2, 1400 frames | ran 24 s (1400/57.5 = 24.3), read-back `src == vk`, and the tables hash to **the same values as under retrohost** — `a11d2aa4a6212d40` then `23d44e76b54c7dbc` |

**The off-by-one in every poly-count comparison is real and benign, and it is worth knowing before it
looks like a dropped frame.** The tap always reports exactly one more rendered frame than the record
delivers. It is the shutdown timeslice: `retro_unload_game` sets the exit flag, `schedule_exit()`
only takes effect at the end of the timeslice, and the OSD's `update()` runs several more times on
the way out without parking on the baton. One of those does a full `screen_update`. Confirmed by
ordering in the log — **the extra `[polytap] frame 512` is emitted after `ring of 3 destroyed`**, so
it was rendered after the renderer had already been torn down and could not have been presented by
anything.

### RSS — the criterion as written cannot be met, and it is not this step's fault

`retrohost` gained `M2VK_HOST_RSS=<n>`, which prints resident size every n frames and the peak at the
end (`task_info`/`MACH_TASK_BASIC_INFO`; `getrusage`'s `ru_maxrss` is a peak and a peak cannot show a
slow climb flattening out).

RSS over vf2 2500 frames is **not flat on any path**:

| | frame 0 | peak | final |
|---|---|---|---|
| `renderer=software` | 160.4 | 183.4 | 178.4 |
| `renderer=vulkan`, step 1 | 175.6 | 211.1 | 205.3 |
| `renderer=vulkan`, step 2 | 175.6 | 205.1 | **202.7** |

So step 2 ends **2.6 MiB below step 1** on the same run, i.e. the record costs nothing measurable —
its polygon vector settles at a capacity of 1598 and stops, and the two table snapshots are fixed
size. What is not flat is a ~25 MiB drift shared by all three columns, **including
`renderer=software`, where not one line of this step's code runs.** Same shape, same inflection at
frame 1500, same drop back at 2000. It predates step 2 and it predates step 1; it is the
emulator/host side. Left as an open observation rather than chased, since nothing in P3 is going to
move it, but it should not be rediscovered at step 6 and blamed on the geometry path.

The persistent ~15 MiB gap between the software and Vulkan columns is accounted for: the ring is 3
slots x (1 image + 2 layer textures + 2 staging buffers) at 762 KB each ≈ 11.4 MB, plus retrohost's
own instance and device.

### Two things worth carrying forward

- **The table digest is the check that the bytes crossed, not just a serial.** It caught nothing this
  time, but "the serial incremented" and "the right 56 KB arrived" are different claims, and the
  second one is the one that matters at step 4. Both hosts agreeing on it is the stronger result.
- **`M2VK_GEOM_LOG=1` logs one line per frame with new geometry, not per presented frame.** That is
  deliberate: it makes the sequence line up one for one with the tap's own per-frame line, which is
  the whole comparison. A duped frame is silent, which is also how the "keep last frame's 3D" case at
  step 6 will show up.

Next: **step 3 — untextured polygons and the real depth buffer.** Vertex and index buffers, fan every
polygon to n-2 triangles, `D32_SFLOAT`, draw-order depth with `GREATER` and depth write on, the solid
shading path and the `checker` stipple. This is where the GPU first draws, and where the
single-polygon A/B mode gets built. Two things already written down that bite here: index values must
never be `0xffff`/`0xffffffff`, because **primitive restart cannot be disabled on MoltenVK**; and
untextured *translucent* polygons must draw **nothing**, as in the software renderer.

---

## 2026-07-26 — P3 step 3 of 8: untextured polygons and the real depth buffer

**The GPU draws.** This is the phase transition the plan has been building towards, and it is where
`cmp` stops being the exit criterion for the 3D layer for good.

New: `renderer_vk/vk_geom.{h,cpp}`, `shaders/poly.vert`, `shaders/poly.frag`, `devnotes/ppmdiff.py`.
Changed: the render pass gained a `D32_SFLOAT` depth attachment and every slot a depth image;
`m2vk_sink.h` gained the debug filter; `vk_funcs` gained four entry points and `find_memory_type`
moved into it so the ring and the geometry buffers share one copy. **Upstream diff 26 → 28 lines**,
and the two extra lines are the object-data arena reset described below — not the drawing.

### What is drawn

The record's polygon stream, in draw order, fanned to `n-2` triangles, as **one indexed draw per
frame** into the ring image between the under and over layer draws. Depth is the draw-order key
`1 - n/65536` with `GREATER` and depth writes on against a buffer cleared to 0 — `m_fillmap` in
hardware, first writer wins. Shading is `draw_scanline_solid` transliterated: `palcolor` → the baked
`colorxlat` ramps → out, plus the `checker` stipple as a literal `(x ^ y) & 1` discard.

Textured polygons are **counted and skipped** (steps 4 and 5); untextured translucent ones draw
nothing, as in the software renderer, and are dropped at upload rather than discarded in the shader.
So the default `renderer=vulkan` picture is still mostly a hole — VF2 has one or two solid polygons
a frame. `M2VK_FORCE_SOLID=2` is what makes the whole frame visible, and it is the harness.

Four decisions worth recording:

- **The vertex shader maps to the *visible* extent, not to a 512-wide target.** `gl_Position =
  pos.xy / half_size - 1` with the viewport at 496x384 makes `gl_FragCoord.xy` equal the software
  renderer's `x` and `scanline` exactly, which the checker stipple depends on — a half-pixel or
  origin disagreement inverts the entire pattern. It is also scale-invariant, so P5's internal-res
  scaling is a viewport change and nothing else. The plan's "render into 512x512 and crop later"
  turned out to be a step nobody needs.
- **32-bit indices**, because primitive restart cannot be disabled on MoltenVK and 0xffff is
  reachable in principle with 16-bit ones. 0xffffffff is not: the capacity check refuses anything
  near it.
- **The parameter buffer is indexed by the polygon's position in the record, not among the drawn.**
  Skipped polygons leave a dead 16-byte entry. That is the right trade — the depth key comes from
  the same index, so the textured paths will slot in at the depth they always had.
- **`m2vk::submit()` now returns the renderer class and the seam assigns it back.** That is what
  keeps `M2VK_FORCE_SOLID` acting on *both* renderers without costing an upstream line: the seam
  already had the value in its hands.

### The one real bug, and it was not in the drawing

**Turning the software rasteriser off leaks `poly_manager`'s object-data arena.** RSS went from
175 MiB to **398 MiB** over 2500 frames of VF2 — and it did so with `M2VK_NO_3D=1` too, which draws
no geometry at all, which is what identified it.

`model2_3d_render` takes a slot from `object_data()` for **every** polygon, before the seam is
reached. `poly_manager` recycles that arena in `wait()` — and `wait()` early-outs when
`m_unit.count() == 0` ([poly.h:659](../src/devices/video/poly.h#L659)). With the scanline dispatch
skipped there are never any work units, so nothing is ever recycled and the arena spills into its
overflow chain for the length of the run.

Fixed with two lines in the existing `frame_end` hook block: `if (!m2vk::sw_owns_3d())
m_renderer->object_data().reset();`. That is the whole of the diff growth this step. Afterwards:

| vf2, 2500 frames | final RSS |
|---|---|
| `renderer=software` | 180.6 MiB |
| `renderer=vulkan` | **187.5** MiB |
| `renderer=vulkan`, `M2VK_SW_3D=1` | 195.0 MiB |
| `renderer=vulkan`, `M2VK_FORCE_SOLID=2` | 188.1 MiB |

which is *below* step 2's 202.7, because the arena no longer holds a frame's chained overflow
either. The ~20 MiB whole-run drift that all columns share is the one documented at step 2 and it
still predates all of this.

### The new switches

All four act on **both** renderers, which is the entire point — a difference between the two paths
is only attributable if the two paths were given the same polygons.

| | |
|---|---|
| `M2VK_FORCE_SOLID=1` | clear the textured bit; translucent still means "draw nothing". The faithful one. |
| `M2VK_FORCE_SOLID=2` | force renderer 0 — everything opaque and untextured, nothing skipped. What the coverage comparison uses. |
| `M2VK_ONLY_POLY=<n>` | draw polygon n of a frame and nothing else. n counts polygons *arriving at the seam*, so it names the same polygon in both renderers. |
| `M2VK_ONLY_FRAME=<m>` | narrows the above to frame m. Without it, polygon n of every frame. |
| `M2VK_SW_3D=1` | put MAME's rasteriser back in charge and take the GPU out of it — the step-1/2 arrangement, still bit-exact against `renderer=software`. The isolation tool for "rendering problem or timing problem". |
| `M2VK_NO_3D=1` | neither draws. The background-only reference the coverage diff differences against. |

`M2VK_NO_SW_3D` is gone; `M2VK_SW_3D` is its inverse and the default flipped.

### Verified — by measurement, not by eye

`devnotes/ppmdiff.py` is the new tool. `coverage` calls a pixel drawn when it differs from the
`M2VK_NO_3D=1` reference — which both renderers produce **bit-identically**, since neither touches
those pixels, and that is what makes the whole method valid. `exact` is exit criterion 1: mask off
the union of the two coverages and `cmp` the remainder.

`M2VK_FORCE_SOLID=2`, last frame of the run, both renderers:

| game | pixels covered | coverage disagreement | colour disagreement | criterion 1 |
|---|---|---|---|---|
| vf2 (1500) | 37448 | **0 sw-only, 1 vk-only**, on an edge | 8 of 37448 (0.021 %) | passes |
| vcop2 (2500) | 154203 | **none at all** | 1 | passes |
| srallyc (2500) | 136116 | **none at all** | 1 | passes |
| dynamcop (2500) | 187571 | **none at all** | 1 | passes |

All nine of vf2's disagreements are **isolated single pixels**, and for seven of them the other
renderer's colour appears in its own 8-neighbourhood — i.e. the two disagree about which of two
adjacent polygons owns a boundary pixel. That is the fill-rule rim the plan predicted, and there is
no filled region of disagreement anywhere.

**Single-polygon A/B is exact.** vf2 polygons 0, 100 and 300 of every frame, and vcop2's `seq=432`
(a `checker=1` quad, 697 pixels): every one is **pixel-perfect, 100 % same colour, zero coverage
disagreement**. So the rasterizers agree exactly on an isolated polygon and the residual really is
only at shared edges.

The checker stipple is genuinely exercised rather than assumed: the survey counts 2995 checker
polygons over a vf2 run, 25259 over srallyc and 123171 over vcop2, and under `M2VK_FORCE_SOLID` they
all take the solid path. An inverted parity would have shown as roughly half of those pixels
disagreeing; vcop2 disagrees on one.

Everything else still holds:

| | |
|---|---|
| `M2VK_SW_3D=1` vs `renderer=software` | **byte-identical** PPM and whole-run digest (`cf043ff583370663`) — steps 1 and 2 are not disturbed |
| `M2VK_NO_3D=1`, software vs vulkan | byte-identical on vf2, vcop2 and srallyc |
| two context losses, mask 0x7→0x1→0xf, the abandon path | digest `396860d7ccf65935` on all three, PPM `cmp` clean against the undisturbed run |
| the growth path (dynamcop, indy500 — 3127 and 2250 polygons) | each slot grows 2048 → 4096 once and never again; dynamcop's coverage is still exact |
| committed `vf2-frame800-polytap.txt` | still `cmp`-identical under both renderers |
| the standalone `mamemodel2` | still builds and runs |
| RetroArch 1.22.2, 1450 frames | 25 s (1450/57.5 = 25.2), clean, `D32_SFLOAT` confirmed present, **104.55 % of full speed** — up from 72.88 % at P2, which is the software rasteriser no longer running |

### One gotcha this creates

**`M2VK_VK_DUMP`'s `cmp /tmp/x-src.ppm /tmp/x-vk.ppm` is no longer an equality test.** `-src` is
MAME's finished software frame, which now has a hole where the 3D is; `-vk` is the composite with the
GPU's 3D in it. They are *supposed* to differ from here on, and the measured 97 % on a full-screen
frame is right rather than alarming. Use `ppmdiff.py` against `retrohost --vk`'s read-back instead —
which was always the ground truth anyway.

Next: **step 4 — texture RAM and the textured opaque path.** Both 1 MB sheets as one storage buffer,
`get_texel` and `fetch_bilinear_texel<false>` transliterated, `fast_log2`, the mip level and
trilinear blend, microtexture, and the `lumaram` → `colorxlat` tail. The single-polygon A/B mode
built here is the tool for it: whole-frame SSIM will say "0.91" and nothing about which of a dozen
things in `fetch_bilinear_texel` is wrong.

## 2026-07-26 — P3 step 4 of 8: texture RAM and the textured opaque path (`f3aa8614856`)

**The picture is the game.** Everything up to here drew a hole with a handful of flat polygons in it;
this is `draw_scanline_tex` on the GPU, so VF2's attract renders as VF2's attract.

New: nothing. Changed: `shaders/poly.frag` grew from the untextured colour chain to the whole raster
tail; `vk_geom.cpp` grew a fourth storage buffer and a sixteen-word parameter block; `m2vk_frame.h`
grew the two texture-RAM pointers; `m2vk_sink.{h,cpp}` gained `M2VK_OPAQUE_ONLY`. **Upstream diff
still 28 lines** — the `frame_begin` call site grew, it did not multiply.

### What is drawn

`draw_scanline_tex<false>` from `src/mame/sega/model2rd.ipp`, transliterated: `get_texel`,
`fetch_bilinear_texel<false>`, `fast_log2`, the mip level, the trilinear blend, the microtexture
blend, and the `lumaram` → `colorxlat` tail. It is transliteration, not design, and the file says so
in the places where a line is gratuitously literal. Textured **translucent** polygons are still
counted and skipped — that is step 5 — and untextured translucent ones are still dropped at upload,
as in the software renderer.

The plan's four load-bearing decisions all survived contact and none of them needed revisiting: no
atlas, no cache, no `VkSampler`, no perspective-correct interpolation. What follows is the two things
the plan did not say.

### Texture RAM is not snapshotted, and that is the one design change

Problem 5's table has three colour tables crossing the seam **as copies**, snapshotted on the
emulation thread once a frame. Texture RAM is 2 MB and does not belong in that list: copying it there
would be the largest single cost in the frame and it buys nothing, because the pointers are memory
shares that live as long as the machine does.

So `frame_tables` grew `texram[2]` and `texram_words[2]`, indexed the way `texheader[2]`'s bit 12
names the sheets rather than the way any one polygon sees them, and the record holds **the live
pointers**. The frontend thread reads through them inside `retro_run`, while the emulation thread is
parked on the baton — which is the same guarantee the snapshots were resting on all along, stated
once instead of paid for twice.

One `memcpy` of 2 MB per frame into the slot's own buffer, and **only on a frame that actually drew a
textured polygon**, which is what keeps the `M2VK_FORCE_SOLID` harness runs free of it. It is not
serial-checked: a dirty check on 2 MB costs a compare against a shadow copy, which is the memcpy
again plus the shadow. If it ever needs to be cheaper the answer is a dirty range on the `tex0_w` /
`tex1_w` write handlers, and the plan's instruction not to build that before measuring it stands —
RetroArch runs at 104.45 % of full speed with it in.

Per slot: 2 MB of texture RAM, 128 KB of parameters, 448 KB of vertices, 144 KB of indices, 56 KB of
tables — **2824 KiB, three slots, 8.3 MB over the ring.** RSS over 2500 vf2 frames went 187.5 →
**194.7 MiB**, which is the 6 MB of sheets plus the wider parameter block and nothing else; the
software column moved the same way it always does (160.4 → 172.8 over the same run).

### `M2VK_OPAQUE_ONLY=1`, and why the step is not measurable without it

The first whole-frame coverage diff of this step said **1102 interior pixels of disagreement on vf2**,
which is precisely the shape the diff is meant to flag as a real bug: a filled region, not an edge
rim. It was not a bug. The software renderer draws the 72 textured translucent polygons of that frame
and the GPU does not, because their cutout is step 5.

The fix is a switch, not an exception in the tool: `M2VK_OPAQUE_ONLY=1` rewrites every translucent
polygon to renderer **class 1 — the one class neither renderer draws.** `draw_scanline_solid<true>`
returns before it writes a pixel and `vk_geom` drops it at upload, so the same set leaves both paths
and what remains is comparable. It sits with `M2VK_FORCE_SOLID` in `submit()`, after it, and both its
modes subsume it.

This is the third switch built on the principle that a difference between two renderers is only
attributable if the two were given the same polygons, and it is worth stating as the general rule:
**when a step lands one renderer's feature ahead of the other's, the harness gets a switch that
removes the feature from both, not a comparison that tolerates its absence.**

### Verified — by measurement

Whole frame, last frame of the run, `M2VK_OPAQUE_ONLY=1`, both renderers, through `retrohost --vk`:

| game | pixels covered | coverage disagreement | same colour | criterion 1 |
|---|---|---|---|---|
| vf2 (2500) | 105009 | 1 + 1, both on an edge | 99781 (**95.02 %**) | passes |
| vcop2 (2500) | 154168 | **none at all** | 153665 (**99.67 %**) | passes |
| srallyc (2500) | 136116 | **none at all** | 114837 (**84.37 %**) | passes |
| dynamcop (2500) | 140175 | 72, but see below | 136407 (**97.34 %**) | passes |
| desert (2500) | 138402 | **none at all** | 134671 (**97.30 %**) | passes |
| waverunr (2500) | 188898 | **none at all** | 171428 (**90.75 %**) | passes |

**Coverage agreement is 1.0000 on five of the six and effectively on the sixth.** The rasterizers
agree about which pixels a textured triangle covers to within two pixels of a hundred thousand, and
there is no filled region of coverage difference anywhere in the set.

dynamcop's 72 are worth the paragraph, because the tool flags three of them as *interior* and its own
help says "any of these is a real bug, not a fill rule". They are not. **Every one of the 72 is a
pixel whose background is `(0,0,0)` and where one renderer produced `(0,0,0)`** while the other
produced `(2,0,0)` or `(2,2,0)` — both renderers drew it, they disagree by two levels at the black end
of a ramp, and a coverage diff defined as "differs from the background" cannot tell "drew black" from
"did not draw". That is the caveat `ppmdiff.py coverage` documents, seen for the first time; dynamcop
has a large black background and the others do not. One pixel of the 72 is a genuine difference
(`(77,75)`, sw black against vk `(42,42,76)`) and it has a both-covered neighbour, so it is the
fill-rule rim. Under `M2VK_FORCE_SOLID=2`, where the shading is flat and cannot land on black by
accident, dynamcop's coverage is **1.0000 with no disagreement at all**.

**Single-polygon A/B on textured polygons is pixel-perfect.** vf2 polygons 100, 200, 300, 400 and 500
of every frame — 34, 92, 2, 4 and 1 pixels — are 100 % same colour with zero coverage disagreement,
and the run's largest textured polygon (a 5-vertex floor quad, **35910 pixels**) is 99.54 % with
coverage exactly 1.0000. So the whole chain — `get_texel`'s address fold, the bilinear filter, the
edge fudge, the mip arithmetic, the LUT tail — is exact on an isolated polygon.

**Microtexture is genuinely exercised rather than assumed.** `desert` taps 71582 microtextured
polygons over a 896-frame run, and `waverunr` is the heaviest user in the whole 29-game survey. Both
come out with coverage exactly 1.0000 and no disagreement at all — waverunr over **188898 pixels, 99.2
% of the picture**. Level -1 reads the *other* sheet, so a wrong sheet index there would have shown
as garbage over most of the water rather than as nothing.

### The residual, attributed

Exit criterion 2 asks for a number **and an explanation**, so:

- **It is not the shading chain.** Isolated polygons are bit-identical; a wrong LUT index, a wrong
  shift or a wrong mip level would show there first and largest.
- **It is not the rasterizer.** Coverage is 1.0000 with at most two edge pixels, and dynamcop's
  apparent exception is a black-on-black artefact of the measurement.
- **It is not the fan triangulation.** A single 5-vertex polygon rendered as three triangles agrees
  with MAME's edge-walk over 35910 pixels, and its 166 differing pixels are scattered — 1 to 9 per
  496-pixel row, with no growth along the scanline, which is what would appear if MAME's incremental
  `uoz += duoz` accumulation were the cause.
- **It is float rounding in the perspective divide, amplified by a lookup table with a step per
  index.** `z = 1/ooz` and `u = int(uoz * z * 256)` are evaluated by MoltenVK's Metal, not by MAME's
  x86-64 float, and where a sample lands within an ULP of a texel or blend boundary the two pick
  adjacent entries. The evidence is that the differing pixels are **isolated** (934 of 1349 on vf2
  have no differing 4-neighbour), that the mean signed difference is **[+0.08, +0.06, −0.04] of 255**
  — symmetric noise, no bias — and that the deltas are one or two steps along the ramp the pixel is
  already on.
- **Which is why srallyc is 84 % and vcop2 is 99.7 %.** The difference is not renderer quality, it is
  how hard the scene minifies. srallyc's differences band by row: rows 211–215 and 240–243 — the
  horizon, where a pixel spans many texels — disagree on 57–65 % of their covered pixels, while rows
  224 and 270–280, the near foreground, are **pixel-identical**. The two pictures are
  indistinguishable side by side; the dirt and cliff textures are noise, and ±1 texel of noise is
  still noise.

### Everything else still holds

| | |
|---|---|
| `M2VK_SW_3D=1` vs `renderer=software` | **byte-identical** PPM and whole-run digest (`cf043ff583370663`, the same value as at step 3) |
| `M2VK_FORCE_SOLID=2`, step 3's headline | vcop2 154203, srallyc 136116, dynamcop 187571 pixels — **the same counts to the pixel**, coverage 1.0000 with no disagreement at all, one colour difference each; vf2 1 edge pixel of 73093 |
| `M2VK_NO_3D=1`, software vs vulkan | byte-identical on all six games — which is what makes every number above mean anything |
| ring rebuild, mask 0x7→0x1→0xf, the abandon path | clean; the geometry buffers and their descriptors are rebuilt with the ring each time |
| committed `vf2-frame800-polytap.txt` | still `cmp`-identical |
| the standalone `mamemodel2` | still builds |
| RetroArch 1.22.2, 1700 frames | 29 s (1700/57.5 = 29.6), clean, **104.45 % of full speed** — unchanged from step 3, so the 2 MB upload costs nothing measurable |

### Two gotchas this created

**`M2VK_ONLY_POLY=<n>` names a polygon in the run's LAST rendered frame.** That is the frame
retrohost writes to the PPM. Picking a large polygon out of a `M2VK_POLYTAP_DUMP` of frame 800 and
asking for that seq over a 1900-frame run selects a *different* polygon — the frame-800 dump's
`seq=568` covers 52783 pixels and the same seq at the last frame covered six, which reads as "the
single-polygon mode is broken". Dump the run's last rendered frame instead: the polytap summary's
`frames=` minus one.

**Two harness scripts running at once fight over `./retrohost-save`.** A background A/B and a
background single-polygon sweep were in flight together, and one run's `renderer=software`
reference came out **17127 pixels different** from four other runs of the identical command — the
clouds at a different phase, because the NVRAM had crossed between processes and the emulated state
diverged. It reads exactly like a renderer bug. `M2_SAVE_DIR` has been honoured by retrohost since
P1; **give every run its own** rather than relying on runs not overlapping. (Re-run serially, all the
concurrent numbers reproduced to the pixel — but that was luck, not design.)

Next: **step 5 — the translucent cutout.** `fetch_bilinear_texel<true>`: the alpha bit packed into
the high lane at bit 23, the transparent-texel-takes-neighbour-luma rule at all three interpolation
stages, and the `t < 0x00400000` discard. The packed-lane `LERP` and the flag word are already in
place; what changes is one templated function and the `cls == 3` skip in `vk_geom.cpp`.
`M2VK_OPAQUE_ONLY=1` is what step 5 makes unnecessary, so the measurement to beat is the table above
**without** it.

## 2026-07-26 — P3 step 5 of 8: the translucent cutout

**The faces have eyes.** Step 4 drew every *opaque* polygon and skipped 109 of VF2's 654, so Pai and
Lau rendered with blank faces and `sgt24h`'s trees, crowd and bushes did not exist at all. This is
`fetch_bilinear_texel<true>` and the `< 50 %` discard, and it is the last piece of the raster tail.

New files: none. Changed: `shaders/poly.frag` (the packed alpha lane, the neighbour rule at all three
interpolation stages, the cutout), `vk_geom.cpp` (the `cls == 3` skip deleted, one log line reworded),
`vk_geom.h` (the header comment is no longer describing a gap). **Upstream diff still 28 lines** — this
step touches no upstream file at all.

### What it is

The whole of it is the `Translucent` specialisation of `fetch_bilinear_texel` in `model2rd.ipp`,
which MAME reaches by template parameter and the shader reaches by an ordinary `bool` that is uniform
across the polygon. Two copies of a hundred lines that must stay identical is the more expensive
mistake, and the driver either hoists the branch or it does not.

Three things happen, and the first exists only to make the second possible:

1. **Pack.** `if (tex != 0xf0) tex |= 0x00800000` — texel index 15 is the transparent one, so the
   alpha flag is *absence* of that index, parked at bit 23 in the high lane of the `0x00ff00ff`
   packing that `LERP` was already written for. The lane rides through both horizontal `LERP`s and the
   vertical one, so the filter interpolates coverage and luma in one expression.
2. **The neighbour rule, at all three stages.** A fully transparent texel takes the luma of its
   neighbour before each interpolation — before the two horizontal `LERP`s, and again between them.
   Without it the filter drags colour out of the transparent region and every cutout edge gets a
   fringe of whatever the unused texels happen to hold. The four tests are sequential and each reads
   what the one before it may have written, exactly as upstream; that agrees with a parallel reading
   in every case, but the software renderer is the reference and its order is not ours to tidy.
3. **The cutout.** `if (t < 0x00400000) discard; t &= 0xff;` — the alpha lane sits in bits 16..23, so
   `0x00400000` is 50 %. `discard` rather than a blend, because nothing in this renderer blends.

One property that had to be true and is: **a discarded fragment does not write depth.** The shader
has no `EarlyFragmentTests` execution mode, so depth is written at late fragment tests, after the
discard — which is what keeps the draw-order key equal to `m_fillmap`, where a skipped pixel likewise
leaves `fill[x]` at zero for a later polygon to claim. The `checker` stipple has depended on this
since step 3; the cutout is the same mechanism on a much larger fraction of the frame.

### Verified — by measurement

Whole frame, last frame of a 2500-frame run, both renderers through `retrohost --vk`, **no
`M2VK_OPAQUE_ONLY`** — which is the measurement this step exists to make possible.

| game | pixels covered | coverage disagreement | same colour | criterion 1 |
|---|---|---|---|---|
| vcop2 | 154195 | 1, on an edge | 153490 (**99.54 %**) | passes |
| overrev | 183505 | **none at all** | 179970 (**98.07 %**) | passes |
| dynamcop | 138698 | 72, the black-on-black artefact again | 134954 (**97.33 %**) | passes |
| desert | 138914 | **none at all** | 135193 (**97.32 %**) | passes |
| vf2 | 106397 | 1, on an edge | 101139 (**95.06 %**) | passes |
| waverunr | 188898 | **none at all** | 170201 (**90.10 %**) | passes |
| srallyc | 136116 | **none at all** | 118354 (**86.95 %**) | passes |
| sgt24h | 187983 | **none at all** | 159433 (**84.81 %**) | passes |

**`sgt24h` and `overrev` are the result that matters, and they were chosen for it.** They are the two
heaviest translucency users in the 29-game survey — 1436 of 1742 polygons in sgt24h's sampled frame
are `renderer=3`, and 1094 of 1145 in overrev's — and both come out with coverage agreement 1.0000
and **zero disagreeing pixels**. The cutout is a hard binary decision per fragment, so agreeing about
it on 188 thousand pixels means the packing, the neighbour rule at three stages, the trilinear blend
of two packed lanes and the threshold all produce the same alpha as MAME does. A whole-frame colour
percentage could hide a systematic error; a coverage count of exactly zero cannot.

**Step 4's 1102 interior disagreements on vf2 are gone**, which is the specific thing the step was
for: that number was the missing translucency reading as a filled region of rasterizer disagreement,
and vf2 now reports **0 interior, 1 edge** without the switch that was papering over it.

dynamcop's 72 are the same 72 as at step 4 and the same artefact; printed rather than assumed this
time. All three "interior" ones are pixels over a `(0,0,0)` background where one renderer produced
`(0,0,0)` and the other `(2,2,0)` or `(6,6,6)` — both drew them, and `ppmdiff.py coverage` cannot tell
"drew black" from "did not draw". Its own help says so.

**Single-polygon A/B on a translucent textured polygon.** vf2 `seq=608` of the run's last rendered
frame, a 4-vertex quad whose vertices enclose **3600 pixels**: covered 2044 / 2043, coverage agreement
0.9995 with the single disagreement on an edge, **96.08 % same colour**. The covered count being 57 %
of the polygon's area is the cutout doing its job — the other 43 % is texel 15.

The colour percentages move in both directions against step 4's opaque-only table (vf2 95.02 →
95.06, desert 97.30 → 97.32, srallyc 84.37 → **86.95**, dynamcop 97.34 → 97.33, vcop2 99.67 → 99.54,
waverunr 90.75 → 90.10) and the largest move is an improvement. That is the signature of the same
residual — the float-rounding attribution written up under step 4 — now measured over a slightly
different set of pixels, not of a new source of error.

### Everything else still holds

| | |
|---|---|
| `M2VK_OPAQUE_ONLY=1`, vf2 | **105009 covered, 95.022 % same colour, 2 edge pixels** — step 4's numbers to the pixel. The opaque path is untouched, which is the regression guard that matters most here |
| `M2VK_FORCE_SOLID=2`, vf2 | 106792 / 106791 covered, coverage 1.0000, 1 edge pixel, 99.98 % same colour |
| `M2VK_SW_3D=1` vs `renderer=software` | **byte-identical** PPM and whole-run digest `cf043ff583370663` over vf2 1500 — the step-3 value, unchanged |
| `M2VK_NO_3D=1`, software vs vulkan | byte-identical on all eight games, which is what makes the table above mean anything |
| ring rebuild 0x7→0x1→0xf, two context resets, the abandon path | clean; all four digests equal to the base run's `7aa3c7c7bdfd2be6` over vf2 2500 |
| RSS, vf2 2500 | 182.66 → **194.92 MiB**, peak 194.92 (step 4: 194.7). The cutout allocates nothing |
| committed `vf2-frame800-polytap.txt` | still matches |
| the standalone `mamemodel2` | still builds and runs |
| RetroArch 1.22.2, 1700 frames | 29 s (1700/57.5 = 29.6), clean, **104.38 % of full speed** — step 4 was 104.45, so within noise |

### One gotcha, and it is about the harness rather than the core

**`env $e` does not word-split in zsh.** A check script built its environment as a single string
(`e="M2VK_SW_3D=1 M2OPT_model2_renderer=vulkan"`) and passed it unquoted to `env`. In bash that is two
assignments; in zsh — which is this machine's shell — it is **one**, so `env` set
`M2VK_SW_3D="1 M2OPT_model2_renderer=vulkan"` and left the core option at its default. The run that
was labelled `renderer=software` therefore ran `vulkan`, and produced a digest that looked like a
`M2VK_SW_3D` regression. Two things caught it: the "software" digest was byte-equal to a *vulkan*
digest from earlier in the session, and the core's own `options:` line printed
`model2_renderer=vulkan M2VK_SW_3D=1` — the value with the space still in it. **Read the
`[model2] options:` line, exactly as gotcha 6 says for RetroArch; it applies to `retrohost` too.**
`ab.sh` was unaffected because it is `#!/usr/bin/env bash` and passes `"$@"` through.

Screenshots in `devnotes/screenshots/`, named `2026-07-26-step5-*.png` because step 4 landed the same
day and its files should not be overwritten — the point of keeping them is the before-and-after.

Next: **step 6 — scissor, windows and the dupe path.** Per-polygon scissor from `poly.clip[]`, grouped
so the common case stays one draw; window ordering (descending, later windows drawn first); and the
`m_render_done` case, which step 3 already gets right for free and which step 6 has to *prove* rather
than assume. vcop2's inset panel and srallyc's viewport are the fixtures; VF2's single constant
viewport never exercises any of it.

---

## 2026-07-26 — P3 step 6: scissor, windows and the dupe path

**All three parts are done, and the headline is that the picture does not change anywhere in a
25-game survey.** That is not a disappointing result, it is the result: two of the three items were
already correct for structural reasons and step 6's job was to find out *whether* rather than to
build. What is new is the code for the one part that was genuinely missing (per-polygon scissor) and,
much more valuably, four measurements that turn "we think this is handled" into numbers.

**Only `vk_geom.{h,cpp}` changed. No shader, no new file, and no upstream file — the diff against
mame0288 is still 28 lines.**

### 1. Per-polygon scissor from `poly.clip[]` — built, and it never fires

`geom_upload` now groups consecutive polygons that share a clipped viewport into a `draw_batch`
(`left/top/right/bottom` inclusive, `first_index`, `index_count`) and `geom_draw` records one
`vkCmdSetScissor` + `vkCmdDrawIndexed` per batch, restoring the full extent before it returns —
without that last line the OVER tilemap layer would be clipped to the last polygon's window, which is
a whole-frame corruption from one omission. Scissor was already a dynamic state, so the pipeline did
not change. A polygon whose `vp` came out empty (`right < left`) is **dropped**, which is what
`render_triangle` against an empty cliprect does.

**The survey — 25 games, `retrohost --vk`, 1800 frames each:**

| | games |
|---|---|
| more than one scissor draw in a frame | **`schamp` (up to 8)** and **`dynabb97` (up to 3)**. Nothing else. |
| exactly one scissor draw, every frame | the other 23, and the rectangle is always the full `0,0..495,383` |
| polygons cut by a viewport tighter than the screen | **`dynabb97` 3421 in 311 frames, `schamp` 35 in 26. Zero in all 23 others.** |
| **the largest amount by which any of those exceeded its viewport** | **`3.05176e-05` px — exactly 1/32768, one float ULP** |

So the scissor is correct to have and it changes nothing, and the reason is upstream of the seam:
**`model2_3d_render`'s polygons have already been clipped against four frustum planes built from the
same `viewport`/`center` registers `clip[]` is derived from** (`model2_v.cpp:894-912`,
`clip_polygon`). The screen-space scissor is an exactly redundant second cut. The residual ULP is
projection rounding.

Confirmed by picture, not just by counting: whole-run `digest:` with the scissor on and with
`M2VK_NO_SCISSOR=1` (the pre-step-6 behaviour) is **the same value** on `dynabb97`, `schamp`, `vcop2`
and `lastbrnx`, and the last-frame A/B tables are identical to the pixel either way.

⚠️ **One anomalous digest, unexplained, and it is written down here so it is not rediscovered as a
scissor bug.** The first on/off comparison returned `b3c2896438f248d0` for `schamp` scissor-on against
`8f1abfef0c4f9bed` for scissor-off, which read as "the scissor changes schamp". It **did not
reproduce**: ten subsequent `schamp` runs — four consecutive, then three interleaved with
`renderer=software`, then two more on/off passes — all gave `8f1abfef0c4f9bed` for vulkan and
`d5bb0f74d1fa6965` for software, with no variation. One-in-eleven, once, on one game. If it recurs,
it is a `retrohost --vk` read-back or emulated-timing question and not a renderer one; the scissor is
ruled out because on and off both land on the same value when re-measured.

**`vcop2`'s inset demo panel is a *window*, not a viewport,** which corrects a guess carried since
step 3. CLAUDE.md and the step-5 screenshot notes said the panel was unscissored and the geometry
merely happened to stay inside. It is the second half that is wrong: vcop2 reports 4 window runs, **1
scissor draw and 0 polygons cut**, so the panel is a priority group and its contents are built to
fit. Nothing was ever going to scissor it. **`schamp` and `dynabb97` are step 6's real fixtures; the
plan's guess of vcop2 and srallyc tests none of it** — srallyc has one window and one viewport for
every frame of a run.

### 2. Window ordering — already correct, and now asserted rather than assumed

`render_polygons` walks `for (window = cur_window; window >= 0; window--)`, so higher-numbered
windows reach the seam *first*. The record is in seam order and the draw-order depth key **is** the
record index, so window priority is already in the key: there is nothing to sort and nothing to group.
Rather than trust that, `geom_upload` now checks the record really is window-descending and the run
summary says so. **All 25 games: descending, every frame.** Up to 7 window runs (`lastbrnx`), 8 in
`schamp` over a longer run. A run that ever prints `ASCENDING` has window priority inverted, which is
invisible in a single-window game — which is most of them.

### 3. The dupe path — proven, and it is exercised much harder than expected

`render_polygons` takes its `m_render_done` early return without touching the record, so what is in
there is last frame's stream; `geom_upload` re-runs on it and `geom_draw` redraws it. Step 3 got that
for free and step 6's job was to prove it, so the counter now distinguishes *dupes* from *drops* —
a drop being a frame that had 3D and then did not, which is the 3D layer flickering.

| game, 2500 frames | frames redrawing last frame's list | **3D dropped after first being drawn** |
|---|---|---|
| `vstriker` | **1165 of 2500 — 47 % of the run** | **0** |
| `lastbrnx` | 529 | **0** |
| `vf2` | 259 | **0** |
| `schamp` | 197 | **0** |
| `vcop2`, `srallyc`, `dynabb97`, `daytona` | 0 | **0** |

Virtua Striker's geometrizer is behind on nearly half its frames and the 3D layer never once drops
out. That is the measurement the step wanted and it needed no code to achieve.

### Regression — nothing moved

The eight-game step-5 table reproduces **to three decimal places**, which is as close as the
measurement resolves:

| game | covered | same colour | coverage |
|---|---|---|---|
| `vcop2` | 154195 | **99.543 %** (step 5: 99.54) | 1.0000, 1 edge |
| `overrev` | 183505 | **98.074 %** (98.07) | 1.0000, **zero disagreement** |
| `dynamcop` | 138698 | **97.328 %** (97.33) | 0.9995, the same 72 black-on-black artefacts |
| `desert` | 138914 | **97.321 %** (97.32) | 1.0000, **zero** |
| `vf2` | 106397 | **95.059 %** (95.06) | 1.0000, 1 edge |
| `waverunr` | 188898 | **90.102 %** (90.10) | 1.0000, **zero** |
| `srallyc` | 136116 | **86.951 %** (86.95) | 1.0000, **zero** |
| `sgt24h` | 187983 | **84.812 %** (84.81) | 1.0000, **zero** |

Plus the two new fixtures: **`dynabb97` 156086 covered, 92.111 %, coverage 1.0000 with zero
disagreement**, and **`schamp` 50691 covered, 99.791 %, 0.9993 with 35 disagreements all on an edge
and 0 interior**. schamp's 35 is the highest edge count in the set and it is 0.07 % of its coverage on
a scene that only covers 26.6 % of the frame; exit criterion 1 passes on all ten.

| | |
|---|---|
| `M2VK_OPAQUE_ONLY=1`, vf2 | **105009 covered, 95.022 %, 2 edge** — step 4's numbers to the pixel |
| `M2VK_FORCE_SOLID=2` | vcop2 **154203** and srallyc **136116**, coverage 1.0000, **zero** disagreement, 1 colour difference each — step 3's headline |
| whole-run digest, vf2 2500 | `software 16af05bb8d02a9a5`, `vulkan 7aa3c7c7bdfd2be6` — **both byte-equal to a build of HEAD without step 6**, which is the strongest form of "changed nothing" |
| `M2VK_SW_3D=1` vs `renderer=software` | byte-identical PPM and digest |
| `M2VK_NO_3D=1`, software vs vulkan | byte-identical on all ten games |
| two context resets / mask 0x7→0x1→0xf / the abandon path | clean; all three digests equal the base run's `7aa3c7c7bdfd2be6`; the per-slot batch vector survives a rebuild |
| RSS, vf2 2500 | 182.86 → **195.20 MiB**, peak 195.20 (step 5: 194.92). The batch vectors are the +0.3 |
| committed `vf2-frame800-polytap.txt` | still matches |
| the standalone `mamemodel2` | still builds |
| RetroArch 1.22.2, 2500 frames | 43 s (2500/57.52 = 43.5), clean, **104.50 % of full speed** (step 5: 104.38) |

### A stale number, corrected

**The documented `M2VK_SW_3D` digest `cf043ff583370663` is wrong and has been for at least a step.**
The correct value on vf2 2500 is **`16af05bb8d02a9a5`**, and a build of HEAD *without* step 6 gives
the same thing — so it was already stale before this step, and `renderer=software` cannot be affected
by a change confined to `vk_geom.cpp` in any case (`geom_upload` returns before its first line when
`sw_owns_3d()`). It is stable over repeated runs, with a shared save dir or a private one. The
invariant that matters — `M2VK_SW_3D=1` byte-identical to `renderer=software` — holds.

### The new switch and the new log lines

- **`M2VK_NO_SCISSOR=1`** goes back to one unscissored draw for the whole frame. It is deliberately
  *not* a symmetric harness switch in the sense the others are: MAME always clips to `vp` and cannot
  be asked not to, so this makes the two paths differ on purpose. Its use is answering "did the
  scissor move these pixels" in one run, which is exactly how the survey above was done.
- The run summary is now unconditional and is what a survey reads:
  `geometry: over the run, at most N window runs and M scissor draws in a frame; P polygons in Q
  frames cut by a viewport tighter than the screen, worst by X px`, followed by
  `geometry: D frames redrew last frame's list (geometrizer behind), 3D dropped from E frames after
  first being drawn`. **E must be 0.**
- The first-geometry-frame line now always prints, with the scissor rectangle, and carries the
  window-order assertion.

Screenshots in `devnotes/screenshots/`, `2026-07-26-step6-*.png`. `schamp`'s character-select screen
is the one to look at: eight circular portraits, which is literally the 8-window / 8-scissor-draw
frame the numbers above are about.

Next: **step 7 — the A/B harness, rebuilt.** SSIM and the heatmap, neither of which exists;
`ppmdiff.py` has `coverage` and `exact` only. Baselines recorded for vf2, vcop2 and srallyc.

---

## 2026-07-26 — P3 step 7: the A/B harness, rebuilt

**The harness found two things on its first full run, and that is the entry's headline rather than
the SSIM number.** A **real stale-3D bug** in the renderer (`vstriker`), present since step 3 and
invisible to step 6's own drop counter; and a **bistable, renderer-independent digest** on
`lastbrnx` that retro-explains the anomalous `schamp` digest step 6 wrote down and could not
reproduce. Neither was reachable with `coverage` and `exact` alone.

No core code changed. Everything is in `devnotes/`: `ppmdiff.py` rebuilt, `ab.sh` and `ab-table.py`
new, `ab-baselines.md` new. **HEAD is still `3c8632ce4d3` and the upstream diff is still 28 lines.**

### What was built

`ppmdiff.py` is now numpy-backed and has four modes plus a `report` that runs them all. The rewrite
of the two existing modes was verified before being trusted: **`coverage` and `exact` produce output
byte-identical to the old pure-Python implementation on real fixture data, exit status included.**

- **`ssim`** — standard SSIM (11×11 Gaussian, σ 1.5, C1/C2 as Wang et al.), computed **per RGB
  channel** rather than on luma, because the `colorxlat` tail produces chroma-only differences a
  luma SSIM scores as perfect. Validated independently: exactly 1.0 on identity, and equal to a
  brute-force 2D-window implementation to **1.9e-15**.

  **It is reduced over the covered region, not the frame**, and three means are printed so the gap
  is visible rather than assumed: `whole frame`, `covered`, and `interior` (covered pixels whose
  entire 11×11 window is also covered, so no identical background leaks into the window).
  Percentiles come with it, because a mean of 0.97 from a uniformly slightly-wrong picture and one
  from a perfect picture with a broken object are the same number and not the same bug — `sgt24h`
  is 0.9414 with a **p1 of 0.137**, and that spread is the whole story of the fixture.
- **`heatmap`** — a native-1× PNG. Dimmed background for bearing, heat ramp on both-covered
  differences, cyan/magenta for one-sided coverage, white for exit-criterion-1 violations. Written
  with stdlib `zlib`, so numpy is the only dependency.
- **`coverage` gained a verdict that can be trusted.** Interior disagreements are now split by how
  much the two renderings actually differ: `<= 8` is the documented "cannot tell drew-black from
  did-not-draw" artefact and does not fail the run; more than that is a polygon somebody is missing.
  **dynamcop's three interior disagreements are `(0,0,0)` vs `(2,2,0)` and `(6,6,6)`** — printed and
  checked, per the standing instruction — so it now passes, while `vstriker`'s 190464 still fail.
  A harness that returns non-zero on a known-good fixture is one people learn to ignore.

`ab.sh` is the four runs and one report in a single command, `bash` with every env assignment its own
word (gotcha 7), own `M2_SAVE_DIR` per run. `ab-table.py` regenerates the baseline tables from the
reports — it exists specifically so no number in `ab-baselines.md` is ever retyped.

### The stale-3D bug — `vstriker`

**The GPU keeps redrawing the last polygon list after the game stops submitting geometry.** MAME
draws no 3D; the GPU composites a stale football pitch under the copyright card's 2D text. The two
frames are `screenshots/2026-07-26-step7-vstriker-{correct,stale3d}.png` and the difference is not
subtle.

One branch causes it, at `model2_v.cpp:711`, `render_polygons`:

```c
	/* if we have nothing to render, bail */
	if (raster->poly_list_index == 0)
		return;
```

It returns **before** `m2vk::frame_begin`, so a new-but-empty display list leaves the record exactly
as untouched as the `m_render_done` dupe case eight lines above at line 705 — and the two want
opposite behaviour. MAME re-copies the previous `destmap` for the dupe and draws nothing for the
empty list; the renderer has one response to an untouched record and it is right for only one of
them.

**Step 6's `3D dropped from 0 frames` counter cannot see this**, because it reads the record, which
by construction never reports a drop. It printed 0 for vstriker while vstriker was doing it.

Ruled out rather than assumed, in this order:

| check | result | rules out |
|---|---|---|
| polytap stream, software vs vulkan, 2500 frames | **identical, frame for frame** | any emulated-state divergence |
| repeat runs of each renderer | identical digests both times | non-determinism |
| `M2VK_SW_3D=1` vs `renderer=software` | byte-identical | the capture / record / composite plumbing |
| `vstriker` at **1500** frames | coverage **1.0000, zero disagreement**, 96.10 %, SSIM 0.9969 | anything else about the game |
| core's own summary | `1172 frames redrew last frame's list … 3D dropped from 0 frames` | — (this is the counter that is blind) |

Not fixed: step 7 is scoped to `devnotes/` and the fix is core — a third case in `render_polygons`,
which would take the upstream diff from 28 lines to about 30.

### `lastbrnx` is bistable, and it is not ours

Dropped from the fixture set. Identical commands give `d71e61d5538b7cdf` or `76b26a1ecc2148d8`, in
runs of two or three rather than at random, with **11.26 % of the last frame differing** between the
two outcomes. **Both values occur under `renderer=software` with `M2VK_NO_3D=1` — a configuration in
which none of the P3 renderer code runs at all**, which is what takes the Vulkan path out of it.

Two stable outcomes rather than noise points at frame parity, and lastbrnx is *the* render-test-mode
game: `draw_framebuffer` picks its source with `m_screen->frame_number() & 1 ? m_fbvramB : m_fbvramA`
(`model2_v.cpp:765`), so a one-frame phase difference at startup selects the other framebuffer for
the whole run. Not chased further — the conclusion the harness needed is that it is upstream of
everything P3 touches.

**This explains step 6's anomalous `schamp` digest** (`b3c2896438f248d0` against `8f1abfef0c4f9bed`,
recorded as not reproducing in eleven runs). Same shape, now with a mechanism. `ab.sh` therefore
compares the background **digest as well as the last frame** — the last-frame `cmp` passed on a
lastbrnx run whose digests differed — and its failure message says to re-run before believing it.

### Baselines

Twelve fixtures in `ab-baselines.md`, recorded 2026-07-26 at HEAD `3c8632ce4d3`. **Exit criterion 1
passes on every one. Exit criterion 2 passes with room on the three the plan names: vf2 0.9963,
vcop2 0.9999, srallyc 0.9896.**

| game | covered | same colour | SSIM covered | p1 |
|---|---|---|---|---|
| `vcop2` | 154195 | 99.54 % | **0.9999** | 0.997 |
| `desert` | 138914 | 97.32 % | **0.9994** | 0.996 |
| `schamp` | 50691 | 99.79 % | **0.9984** | 0.971 |
| `dynamcop` | 138698 | 97.33 % | **0.9977** | 0.947 |
| `vstriker@1500` | 183982 | 96.10 % | **0.9969** | 0.949 |
| `overrev` | 183505 | 98.07 % | **0.9968** | 0.946 |
| `vf2` | 106397 | 95.06 % | **0.9963** | 0.929 |
| `srallyc` | 136116 | 86.95 % | **0.9896** | 0.825 |
| `dynabb97` | 156086 | 92.11 % | **0.9862** | 0.689 |
| `waverunr` | 188898 | 90.10 % | **0.9664** | 0.219 |
| `sgt24h` | 187983 | 84.81 % | **0.9414** | 0.137 |

Every `same colour` figure reproduces step 5's and step 6's to two decimals. The guards:

| | |
|---|---|
| `M2VK_OPAQUE_ONLY=1`, vf2 | **105009 covered, 95.022 %, 2 edge** — step 4's numbers to the pixel |
| `M2VK_FORCE_SOLID=2` | vcop2 **154203**, srallyc **136116**, dynamcop **187571**, coverage 1.0000, zero disagreement — step 3's counts exactly, and **SSIM 1.0000 on all three**, vf2 0.9996 |
| `M2VK_SW_3D=1` vs `renderer=software` | byte-identical PPM and digest, `16af05bb8d02a9a5` — **read off this run**, not copied forward |

**`M2VK_FORCE_SOLID=2` scoring SSIM 1.0000 is the most informative single number here.** With flat
shading the two renderers are pixel-identical, which puts the entire residual in the texture and
shading chain and none of it in the rasterizer, the depth key, the scissor or the composite — the
same conclusion the coverage diff reaches, reached by an independent route.

### What the heatmap showed that the aggregates did not

`srallyc`'s differences are 97 % of them **≤ 15 of 255**, but **33 pixels of 136116 differ by up to
255**, scattered along hard texture edges in the ranking-screen text. Same root cause as the
attributed residual — float rounding in `z = 1/ooz` and `int(uoz*z*256)` on Metal — but where the
texture has a hard boundary rather than a smooth ramp, one texel of `u` error is a large colour delta
instead of one LUT step. This **refines** step 4's attribution rather than contradicting it: the
residual's *magnitude* distribution is bimodal even though its *cause* is single.

Screenshots: `2026-07-26-step7-{vf2,srallyc,sgt24h}-heatmap.png` and the two vstriker frames.

Next: **step 8 — docs, and P3 closes.** The stale-3D bug is the one open item and it is a core fix,
so it wants to be its own step rather than being folded into a docs pass.

---

## 2026-07-26 — the deployment target is written down, and the performance picture was wrong

**No code changed. HEAD is still `3c8632ce4d3` and the upstream diff is still 28 lines.** This entry
is analysis, one new doc, and three corrections — two of them to claims this file has been repeating
since step 3.

### The target, which had never been recorded anywhere

**Quest 3 (Snapdragon XR2 Gen 2, Adreno 740), embedded binary, Vulkan output piped into a Unity
session.** Already in production in that shape for Dreamcast and Model 3, tight but holding 60 FPS.
The Mac is the development and A/B host, not the target.

Before this entry the word "Quest" appeared in **no file in the repo** — the only `Unity` hits were
Polydiver the research project. Every performance judgement in P2 and P3 was made without it, which is
why two of them turn out to be wrong. It is now at the top of `CLAUDE.md` and the analysis is in the
new **[performance.md](performance.md)**.

### Correction 1 — "104.5 % of full speed" is a throttle artifact, not headroom

This file and `CLAUDE.md` have quoted RetroArch's `Average speed:` as evidence of spare capacity since
step 3, most recently as *"at 104.50 % there is nothing asking to be [scoped]"*. **That reading is
wrong.** RetroArch runs the core at its declared 57.52 Hz, so the number means "keeps up with
realtime". The tell was sitting in our own table the whole time:

| step | what landed | speed |
|---|---|---|
| 3 | untextured polygons | 104.45 % |
| 4 | **the entire textured path** — 2 MB sheets, mip chain, microtexture | 104.45 % |
| 5 | the translucent cutout | 104.38 % |
| 6 | per-polygon scissor | 104.50 % |

Landing ~16 texel fetches per fragment moved it by less than the noise. If the GPU were near its limit
that is impossible. **The real headroom number is `retrohost`'s unthrottled `Average speed:`, and it
has never been run for `--vk`** — the only figure ever captured is **356.72 % on the software path**
(vf2, 2000 frames). Taking it was deliberately deferred: an unthrottled figure is pure wall-clock and
another session was running `ab.sh` loops in this tree at the time (see below).

### Correction 2 — "filtering must be done by hand" is right about colour and too strong in general

P3 settled that hardware samplers are unusable because MAME filters in **index space before the
LUTs**. True, and it correctly rules out sampling a decoded *colour* texture. It does **not** rule out
sampling a texture whose texels *are* the indices, which is a different and much better idea. Read off
`poly.frag` rather than inferred:

- `get_texel` returns `texel & 0x0f` — a **4-bit index**, promoted to 8 bits (`15 << 4 = 0xf0`);
- `LERP(x,y,a) = x + (((y-x)*a) >> 8)` — an 8-bit fractional weight;
- the tail is `t → lumaram[lumabase + (t>>1)] → × poly.luma/256 → clamp 0x3f → colorxlat → RGB`.

**The texture is pure luminance; all colour arrives afterwards from `palcolor`.** So storing the
indices in an R8 image and letting the sampler bilinear *those* IS index-space filtering — the same
arithmetic, done by the texture unit — with `lumaram`, the luma multiply, the clamp and `colorxlat`
still per-fragment and bit-exact.

This also **retires the hardest part of the texture-cache idea**: with no LUT applied at decode time
the cache key needs neither `colorbase`/`palcolor` nor `lumabase`, so the atlas-plus-cache becomes a
format conversion (unswizzle 4bpp → linear R8 on dirty). The one thing a sampler cannot do is the
transparency neighbour rule — and that lives **only** in `fetch_bilinear_texel<true>`, so opaque
textured polygons can take the hardware path essentially exactly while translucent ones keep the
hand-written one. That is the same specialisation boundary §4.1 wants for early-Z.

**Unverified and load-bearing:** MAME's LERP uses 8 fractional bits, so this matches only if
`subTexelPrecisionBits` ≥ 8. The Vulkan minimum is 4 and it is not in `vulkan-target.md`'s limits.
**Probe it before building on any of the above.**

### Correction 3 — step 6's `3D dropped from 0 frames` counter is blind to step 7's bug, confirmed

Step 7's stale-3D bug (`render_polygons` bails at `poly_list_index == 0`,
[model2_v.cpp:712](../src/mame/sega/model2_v.cpp#L712), *before* `m2vk::frame_begin` at 719) was
re-derived from the source here rather than taken on trust, and the mechanism is sharper than the
step-7 note says:

- **line 705** (`m_render_done`) does `copybitmap_trans` of the previous `m_destmap`, so the software
  renderer genuinely keeps last frame's 3D and the GPU redrawing the stale list **agrees**;
- **line 712** (empty list) returns with **no** copy, so software draws **no 3D at all** — but it also
  returns before `frame_begin`, so the record is untouched and the GPU redraws anyway. **Divergent.**

Two paths that are *indistinguishable from the renderer's side* wanting opposite behaviour. Step 6's
counter cannot see it because `geom_upload` returns **true** in that case — the record is valid, just
stale — and `s_dropped_frames` only fires on a false-after-true. It also **cannot be fixed
renderer-side**: both cases leave `geometry_serial` identical, so core has to signal the difference
first. Step 7's "third case in core" plan is the right shape and the counter fix has to ride with it.

### Also settled: the schamp digest anomaly was not ours, and not concurrency

Step 6 recorded an anomalous non-reproducing `schamp` digest. An early draft of `performance.md`
blamed concurrent harness activity. **Both were wrong** — step 7 reproduced the phenomenon on
`lastbrnx`, bistable between two digests 11.26 % of a frame apart, under `renderer=software` with
`M2VK_NO_3D=1`, where none of our code runs. Almost certainly `draw_framebuffer`'s
`frame_number() & 1`. The rule that generalises: **re-run a one-off disagreement before believing it,
and before theorising about it.**

### One new gotcha that is real: concurrent sessions corrupt wall-clock measurements

Two sessions worked in this tree simultaneously on 2026-07-26. Everything the harness measures —
coverage, colour, SSIM, digests — is deterministic pixel output and is **immune**. A timing number is
not, and there is no way to tell a contended result from a real one afterwards. Related: both sessions
run `./model2_libretro.dylib`, so a `make` in one swaps the binary under the other's in-flight loop and
**nothing in either log says so**. `ps -Ao args | grep -E "retrohost|make"` before building or timing.
Any timing figure wants **3 repeats and a reported spread**. Full version in performance.md §7.

Next: unchanged — **the stale-3D fix, then step 8 and P3 closes.** Nothing in this entry is on that
path; performance.md §8 says where each optimisation lands (and the Quest 3 port itself has no phase
yet, which wants raising in the Polydiver plan).

---

## 2026-07-26 — the stale-3D fix: built, headline-verified, NOT committed

> **Superseded by the next entry** (P3 step 8): all three verifications listed at the end of this one
> pass, and the fix is committed as `c38dbbefffe`. The entry is left as written because it is the
> record of what was true when the work stopped, and the reasoning in it is still the reasoning.

**HEAD is still `3c8632ce4d3` and six files are modified.** The headline measurement passes; three
secondary verifications are outstanding and are listed at the end. Do not read this entry as "done".

### What the bug was, re-derived rather than inherited

`render_polygons` ([model2_v.cpp](../src/mame/sega/model2_v.cpp)) had two early returns that are
**indistinguishable from the renderer's side and want opposite behaviour**:

- **line ~705**, `if (m_render_done)` — `copybitmap_trans`es the previous `m_destmap`, so the software
  renderer genuinely keeps last frame's 3D. The GPU redrawing its stale list **agrees**. Correct.
- **line ~712**, `if (raster->poly_list_index == 0)` — returns with **no** copy, so MAME's 3D layer is
  blank for that frame. But it also returned *before* the `#ifdef M2VK m2vk::frame_begin(...)` at ~719,
  leaving `geometry_serial` unchanged — identical to the dupe case — so the GPU redrew the last list it
  was handed for the rest of the run.

### The fix

`frame_begin` is hoisted above the empty-list bail (**upstream diff 28 → 30 insertions**, within the
stated budget). `m2vk_sink.cpp` routes `submitted == 0` to a new **`m2vk::geometry_none()`**, which sets
`geometry_valid = true` with `poly_count = 0` and bumps the serial: *"a new frame that is empty"* rather
than *"an abandoned capture"*. It skips the 56 KB table snapshot deliberately — nothing reads the tables
while `poly_count` is zero, and vstriker would have paid for it on 47 % of its frames.

**Two decisions not to undo.** The sink deliberately does **not** notify consumers on an empty frame:
"rendered frame" in the polytap means a frame *carrying polygons*, which is what `M2VK_POLYTAP_DUMP=N`
counts, what the committed vf2 frame-800 fixture keys on, and what `M2VK_ONLY_FRAME`'s numbering must
stay in step with — and VF2 queues nothing for its first ~990 frames, so counting them would renumber
every fixture. And step 6's single `dropped` counter became **three**:

- `dupes` — the `m_render_done` path. Redrawing agrees with MAME, which re-copies its own destmap.
- `empty` — a new, empty list. Drawing nothing agrees with MAME. **Not a fault, and inverted into a
  regression check: every game boots through empty frames, so a run reporting zero of them means the
  core has stopped notifying the record and the bug is back.**
- `dropped` — 3D lost from a frame that should have had it. Must stay 0, and now excludes frames made
  only of polygons neither renderer draws (`poly_count > skipped_translucent + skipped_offscreen`).

### Verified

**The headline: `./devnotes/ab.sh vstriker 2500`, 190464 interior disagreements → 0.** 190464 is the
whole 496×384 picture: vstriker is on the copyright card at 2500, software correctly draws no 3D, and
the bug had the GPU painting the stale football pitch over every pixel. Now `covered by A = 0`,
`covered by B = 0`, no coverage disagreement, exit criterion 1 holds, heatmap `white 0`.

**It is not a vacuous zero, and that mattered** — a fixture whose last frame has no 3D reads as
`covered by A = 0` with a plausible SSIM, which is exactly what this run looks like at a glance. Three
things rule it out: the background reference is identical across renderers on the last frame *and* the
whole-run digest; the two 3D digests still differ from each other (`dffa9344450bc43a` vs
`9becd9ba1e6a702f`) by the expected float residual, so geometry is being drawn on the frames that have
it; and the counter below shows the notification arriving.

| game, 2500 frames | dupes | empty (after 3D drawn) | dropped |
|---|---|---|---|
| `vstriker` | 898 | **703 (267)** | **0** |
| `daytona` | 0 | 502 (0) | **0** |
| `vf2` | 0 | **987 (259)** | **0** |

**vf2's 987 is the strongest single number here:** it matches the independently documented "~990 frames
with no geometry at boot", so the empty-list notification is demonstrably arriving. vstriker's 267 is
where the football pitch used to live. daytona's `0 after` says it never stops submitting once started.

**`renderer=software` is untouched:** vf2 2500 digest still `16af05bb8d02a9a5`.

### ⚠️ A recorded digest is now stale — the third time this has bitten

**vf2 2500 `renderer=vulkan`: `7aa3c7c7bdfd2be6` → `55da761fecca5c01`.** The change is *correct* — 259
vf2 frames were drawing a stale list and now draw nothing. The old value is quoted as a regression check
in the step-6 entry above and in CLAUDE.md's step-6 section; **those are dated records of what was true
at `3c8632ce4d3` and are not being retro-edited**, but they are not current. This is the same failure
mode as `cf043ff583370663`: a number recorded once and carried forward. It is why `ab-baselines.md`'s
first rule is "regenerate, never retype".

**vf2 being affected at all is worth noting** — the bug was reported as a vstriker problem, but vf2 had
259 stale frames too. It went unnoticed because vf2's *last* frame at 2500 has 3D, so last-frame A/B
never saw it. Only the whole-run digest could, and nothing was comparing digests across the fix.

### Not verified — do these first

1. **The 12-fixture `ab-baselines.md` table.** **Expect whole-run digests to move on any fixture with
   empty-frames-after-draw; that is correct.** Last-frame metrics should not move where the last frame
   has 3D.
2. **The standalone build**, `make SUBTARGET=model2 OSD=sdl3 REGENIE=1 NOWERROR=1 -j10`.
3. **The polytap fixture** `vf2-frame800-polytap.txt` — the consumers-not-notified decision is
   specifically what protects it, so this check is what proves that reasoning rather than assuming it.

Then commit, then step 8.

### How this went, since it is a process note worth having

The fix was written by a delegated session that died twice on transient API errors (529), the second
time producing nothing, so the verification was finished by hand in the parent session. The code was
reviewed against the source rather than accepted — which is how the `frame_begin`-without-`frame_end`
question got asked, and the answer (`geometry_none`) turned out to be already handled. Then the Bash
safety classifier went unavailable, which is why three verifications are outstanding rather than done.
None of that is a problem with the change; it is why this entry stops where it does.

## 2026-07-26 — P3 step 8: the stale-3D fix verified and committed, and P3 closes

**HEAD is `c38dbbefffe`, "Stop redrawing the 3D after the display list goes empty", and `git status`
is clean.** This entry finishes the one above it, which stopped with three verifications outstanding.
All three pass. Then the docs, which is what step 8 was actually scoped as.

### The three outstanding verifications

1. **The 12-fixture table, regenerated.** `ab.sh` over every fixture at 2500 frames (plus
   `vstriker@1500`), then `ab-table.py`. **`vstriker@2500` goes from 190464 real interior
   disagreements to 0** — 190464 being the entire 496×384 picture. Exit criterion 1 passes on all 12,
   no interior disagreement anywhere, and **every last-frame metric reproduces the pre-fix table to
   the digit** on the eleven fixtures whose last frame carries 3D. That is the shape a correct fix
   has: it touches only frames with no display list, and none of those is a fixture's last frame
   except vstriker's.
2. **The standalone build.** `make SUBTARGET=model2 OSD=sdl3 REGENIE=1 NOWERROR=1 -j10` links. This
   is the check that the new `#ifdef M2VK` placement still compiles with `M2VK` undefined.
3. **The polytap fixture.** `devnotes/fixtures/vf2-frame800-polytap.txt` is byte-identical under
   **both** renderers over a 2000-frame run (`frames=1012` rendered). This is the check that proves
   the not-notifying-consumers decision rather than assuming it: 987 empty frames now reach the
   record and the rendered-frame numbering did not move by one.

### The digests moved, and which ones is the interesting part

Whole-run 3D digests move on **exactly** the six fixtures with empty display lists after the 3D had
been drawn — `vf2` (259 such frames), `sgt24h` (6), `overrev` (13), `desert` (8), `dynamcop` (18),
`vstriker` (267) — and stand still on the four with none — `vcop2`, `srallyc`, `waverunr`,
`dynabb97`. Every `bg` digest and every `3D software` digest is unchanged. The movement **is** the
fix: those frames were drawing a stale list and now draw nothing.

**`schamp` broke the pattern and was chased rather than explained away.** 197 empty-after-draw frames
and its digest did *not* move. `3c8632ce4d3` was rebuilt and both sides measured directly:

| | pre-fix digest | post-fix digest | pre-fix `dupes` | post-fix `empty after draw` |
|---|---|---|---|---|
| `vf2` | `7aa3c7c7bdfd2be6` | `55da761fecca5c01` | 259 | 259 |
| `schamp` | `8f1abfef0c4f9bed` | `8f1abfef0c4f9bed` | 197 | 197 |

Two things fall out of that rebuild, and the second is worth more than the first. **schamp's 197
stale redraws were landing on pixels indistinguishable from drawing nothing**, so the game never
visibly showed the bug — plausible for the smallest 3D layer in the set (26.6 % of the picture, eight
character portraits each in its own window). And **the counter accounting is exactly right**: the old
`dupes` figure was counting precisely the frames the new `empty after the 3D had been drawn` figure
counts, on both games, which have no true dupes at all. One reclassification, no frame gained or
lost. That is a stronger check on the new counters than any number they produce on their own.

⚠️ **`vstriker@2500` now measures nothing about 3D and the table cannot say so.** Its frame is a
uniform white flash — 190464 pixels of `(255, 255, 255)`, byte-identical under both renderers and to
the `M2VK_NO_3D=1` reference — so `covered px` is 0 and SSIM prints `—` because it is undefined over
an empty region. The row is kept because the *interior* count is the guard. **Read that column and
nothing else on that row**, and use `vstriker@1500` for anything about the game's rendering. This is
the same trap `ab-baselines.md` already warns about for a fixture whose last frame has no 3D, now
sitting inside the fixture set itself.

### The screenshots

`devnotes/screenshots/2026-07-26-step8-{vstriker,sgt24h,schamp}.png` — vstriker at frame **2200**
(not 2500), where the game is still submitting geometry, so the composite is visible; sgt24h as the
"what is still imperfect" frame (SSIM 0.9414, p1 0.137, and nothing wrong to the eye); schamp as the
scissor fixture and the digest curiosity above. **There is deliberately no screenshot of the frame
the headline is measured on** — it is a white rectangle, and the README says why.

### The regression guards, re-measured

Both switches reproduce their recorded numbers **to the digit** at `c38dbbefffe`, which is the useful
outcome for a guard — nothing to reprint. `M2VK_OPAQUE_ONLY=1` on vf2: 105009 covered, agreement
1.0000, 1/1 edge pixels, 95.02 % same colour, SSIM 0.9964 — step 4's numbers. `M2VK_FORCE_SOLID=2`:
vf2 106792 / 99.98 % / 0.9996, and vcop2 154203, srallyc 136116, dynamcop 187571 all at **100.00 %
same colour and SSIM 1.0000** — step 3's numbers, and the standing proof that with flat shading the
two renderers are pixel-identical, so the whole residual is in the shading chain and none of it is in
the rasterizer, depth key, scissor or composite.

### performance.md §6 is answered, and the answer is the bad one

§6 said one grep should happen before any renderer optimisation is funded: **does MAME recompile or
interpret the i960?** It **interprets**. [measured] `src/devices/cpu/i960/` holds exactly `i960.cpp`
and `i960dis.cpp` — the core and a disassembler. There is **no `i960fe.cpp`**, the frontend file every
MAME DRC core has, and `i960.cpp` contains **zero** `drcuml` references against four in `sh/sh2.cpp`,
a DRC core in the same tree. `execute_run()` ([i960.cpp:2204](../src/devices/cpu/i960/i960.cpp#L2204))
is a plain `while (m_icount > 0)` fetch-decode-execute loop with a `debugger_instruction_hook` per
instruction.

Two i960s interpreted, plus the copro DSPs and a 68000, all on the emulation thread — **and nothing
in performance.md §4 touches any of it.** This does not reorder §4; it changes how much §4 is worth,
and it makes the *first* measurement to take the emulation thread alone, which `M2VK_NO_3D=1` already
isolates on both renderers. The good news from §6 still stands: the software rasterizer was the
largest single CPU cost available here and P3 switched it off at the seam.

### ⚠️ One assigned step-8 item did NOT land: the performance harness

performance.md §8 puts "a perf mode for the harness" in **P3 step 8, alongside the docs pass**:
unthrottled `Average speed:` per fixture, 3 repeats with the spread, GPU timestamps for a per-stage
breakdown. **It is not built.** P3 therefore closes with a rigorous *accuracy* harness and **no
performance harness at all** — there is no timing measurement of any kind in `ab.sh`, `ppmdiff.py` or
`ab-baselines.md`, and `retrohost --vk`'s unthrottled `Average speed:` has still never been taken.
It is recorded here, in performance.md §8, in CLAUDE.md's "Next" and in the next-session prompt,
because a step that quietly drops an assigned item is how a plan stops being a plan.

### What P3 ends as

Eight steps, seven commits, **upstream diff 30 lines** against mame0288 — the budget was 28 and the
two extra are a comment explaining why `frame_begin` sits above a bail, which is exactly what a
future merge wants to find there. The whole raster tail is on the GPU: untextured, textured, mipped,
microtextured, trilinear-by-hand, translucent-cutout, stippled, scissored, depth-keyed by draw order.
Exit criterion 1 passes on all 12 fixtures; exit criterion 2 passes with room on the three named
(vf2 0.9963, vcop2 0.9999, srallyc 0.9896) and the residual is attributed to float rounding rather
than left as "0.97 and we don't know why".

**What P3 does not do, and P4 inherits:** interpolated z, decals and coplanar ordering. The depth key
is draw order, which reproduces `m_fillmap` exactly and cannot z-fight — and cannot express a decal
either.

### Process note

The pre-fix rebuild was the only part of this that was not planned, and it is the part that produced
the two best facts in the entry. The prompt for it was a single fixture disagreeing with a prediction
the other eleven confirmed. Predicting *which* numbers should move before regenerating a table is
worth doing for exactly this reason: an unexplained agreement is as informative as a disagreement,
but only if you wrote the prediction down first.

---

## 2026-07-26 — The performance harness, and the answer reorders the plan

**`devnotes/perf.sh` + `M2VK_HOST_PERF` in `retrohost.c`.** This is performance.md §8's row — the item
P3 step 8 was assigned and shipped without. It changes no pixels: `ab.sh vf2 2500` reproduces all
three digests exactly (`6b831e519ff46d42` / `16af05bb8d02a9a5` / `55da761fecca5c01`), 95.059 % same
colour, coverage 1.0000, 1 edge pixel, 0 interior. No screenshots this entry, for the same reason.

### What was built

`retrohost` gained three timers around the existing loop, and the split between them is the whole
design:

- **`core`** — wall time inside `retro_run` minus everything below. The emulation thread plus, on the
  Vulkan path, the renderer's own recording and submit. What an optimisation has to move.
- **`gpuwait`** — the read-back's `vkWaitForFences` and nothing else. The core ends its frame with a
  submit and no fence, so this is where its GPU work is observed to finish.
- **`host`** — the read-back's command recording and 762 KB memcpy, plus the whole-run digest. Pure
  harness overhead that no frontend pays, measured so it can be *taken out* rather than assumed small.
  It is **0.54–0.58 ms/frame**, which is not small.

Plus `M2VK_HOST_PERF_SKIP=<n>`, and a **per-bucket table** printed regardless of the skip. That table
is the part worth keeping: every game runs a stretch that submits no geometry, and it shows up as two
plateaus rather than as a quietly lower average. Calibrated across six fixtures — vf2's boot ends at
~frame 1027, vcop2 ~711, srallyc ~1343, sgt24h ~1027, desert ~1106, waverunr ~1343 — so **skip 1500 of
2500** covers all six with 1000 frames left to measure.

### ⚠️ The first version of the report was wrong, and the way it was wrong is the useful part

The first sweep quoted one speed number, `core + gpuwait`, and it said the Vulkan path was **slower
than the software rasterizer on vcop2** (388.2 % against 412.9 %) and on srallyc. That is not a
renderer result. **This harness reads every frame back off the GPU and waits on a fence to do it,
immediately after the core's submit, so CPU and GPU never run at once** — which no real frontend does.

The fix is to print a bracket and refuse to collapse it:

- `serial%` = `core + gpuwait`, the lower bound this harness achieves
- `pipe%` = `max(core, gpuwait)`, the upper bound a pipelined frontend reaches

and the truth is nearer `pipe%`, because `gpuwait` *over*-estimates the core's GPU time — the wait
covers our own whole-frame image→buffer copy as well as the core's rendering. The lesson generalises
past this harness: **an instrument that serialises two things cannot report their sum as a cost**, and
the tell was a result (a GPU renderer losing to a software rasterizer) that contradicted a thing
already known to be true.

`renderer=software` under `--vk` turned out to be a fourth config not worth running: with that option
the core never declares `RETRO_HW_CONTEXT_VULKAN` at all, so the frontend's device is never created
and the run is identical to plain `sw` — same three timings, `gpuwait` exactly 0. Convenient rather
than limiting: `sw` carries no frontend cost, so `sw`→`vk` needs no correction term.

### The numbers — 6 fixtures × 3 configs × 3 repeats, spread ≤ 4.0 % everywhere

| game | emul | sw | vk (pipe) | vk core ms | vk gpuwait ms |
|---|---|---|---|---|---|
| `vf2` | 416.3 | 311.7 | **399.0** | 4.357 | 0.720 |
| `vcop2` | 512.4 | 412.9 | **488.1** | 3.562 | 0.916 |
| `srallyc` | 410.5 | 332.5 | **394.6** | 4.406 | 0.925 |
| `sgt24h` | 377.7 | 306.4 | **364.6** | 4.767 | 0.897 |
| `desert` | 578.2 | 513.8 | **550.8** | 3.157 | 0.709 |
| `waverunr` | 422.9 | 327.0 | **403.8** | 4.305 | 0.936 |

**P3 is worth +7 % to +28 %.** Real, and an order of magnitude less than "the rasterizer was the
largest single CPU cost" implied — it was never the largest cost, only the largest one we could
remove.

**⚠️ And the Vulkan path is already within 3.5–5 % of the emulation-only ceiling on every fixture.**
Driving the entire renderer to zero cost moves vf2 from 399 % to 416 %. That is §6's i960 finding
turned into a budget: **everything in §4 is bidding for ~4.5 % of the frame.**

The renderer's CPU side is **0.15–0.20 ms/frame** (`vk core` − `emul core`), recording, buffer fills
and the 2 MB texture memcpy included. §3.3's dirty-range fix is measured at ~0 on this machine.

### GPU timestamps were not built, deliberately, and here is what replaced them

The assigned row also wanted "GPU timestamps for a per-stage breakdown". **Differential switching gives
the breakdown with no core change, no query pool and no risk to the accuracy harness** — run the same
fixture with `M2VK_NO_3D=1`, then `M2VK_FORCE_SOLID=2`, then whole:

| `gpuwait` ms/frame | waverunr | desert |
|---|---|---|
| 2D composite + our read-back copy | 0.492 | 0.480 |
| + untextured 3D | 0.548 | 0.509 |
| + the whole texture chain | **0.936** | **0.709** |

So on `waverunr` — 99.18 % 3D coverage, the largest 3D layer in the fixture set — **rasterisation is
0.056 ms and the hand-written index-space filtering is 0.388 ms, 87 % of the 3D layer's GPU cost.**

**§3.1's ranking is confirmed and its stakes are refuted.** Filtering does dominate the GPU. The GPU is
0.94 ms of a 4.31 ms frame, and over half of that 0.94 is the harness's own read-back — so the
"dominant cost" is **9 % of the frame on the fixture that stresses it hardest**, 5 % on the
microtexture extreme. Timestamps would refine below 0.05 ms on a quantity that is 10 % of the frame.
Recorded as **deferred with a reason** rather than dropped, and worth revisiting on Quest 3, where the
GPU's share is expected to be the interesting one. This is the item P3 step 8 dropped silently; not
doing that again is the point of writing the reason down.

### What this does to the plan

- **Do not fund §4 on desktop.** The budget it was competing for does not exist. The early-Z pipeline
  split keeps its P4 slot on the *risk* argument (P4 opens the depth path anyway, so verify it once) —
  the speed argument for it is dead.
- **The Quest 3 port is promoted from "unphased sequel" to "the blocker".** It is now the only place
  the remaining optimisation questions can be answered. Still needs a phase in the Polydiver plan.
- **A new first question:** what does the *emulation thread* cost on an XR2 Gen 2? Here it is
  3.0–4.8 ms/frame with two interpreted i960s. That is what the port lives or dies by, and no renderer
  work moves it.

### Process note

The bracket bug was found by a result that contradicted something already known — not by re-reading
the code. Worth keeping as a habit: when a new instrument's first output disagrees with an established
fact, suspect the instrument before the fact, and check specifically whether it *serialises* something
the real system overlaps.

---

## 2026-07-26 — Input survey, and a P6 options proposal

**`devnotes/user-options.md`.** Prompted by a question about Dynamite Baseball's bat controller. No
code changed. The survey is parsed out of `src/mame/sega/model2.cpp` at mame0288: **83 GAME entries,
32 input-port sets, and they collapse to 6 tiers.**

**The structural finding, which is the useful one: MAME has already normalised every exotic controller
into a standard analog type.** The OSD never sees a bat or a ski platform — it sees `IPT_PEDAL` and
`IPT_AD_STICK_X`. dynabb's bat is literally an 8-bit `0x00→0xff` pedal axis, and our generic layer
already maps it to a trigger. **So no tier needs new plumbing, only better mapping** — which retires
the worry that 29 sets of weird cabinets each need bespoke work. The P1 decision to reject a per-game
ioport table was right when the shape was unknown; the shape is now known and it is 6 tiers, so a small
per-tier table with a few per-set exceptions is tractable.

| tier | sets | not `NOT_WORKING` | today |
|---|---|---|---|
| buttons | 29 | 12 | fine |
| driving | 26 | 5 | mapped but bad |
| exotic | 11 | 0 | partly, by accident |
| adstick | 7 | 3 | probably fine |
| lightgun | 6 | 3 | on an analog stick |
| twinstick | 4 | 0 | dedicated path exists |

**Steering is the sharpest real problem and it is not a mapping problem.** Daytona declares a plain
linear `IPT_PADDLE`; the cabinet wheel is ~270° and a thumbstick is millimetres with a hard self-centre.
`PORT_SENSITIVITY`/`PORT_KEYDELTA` do **not** help — they govern MAME's digital-increment emulation of
an analog control, not the shaping of a real absolute axis. The fix has to be ours: a response curve,
deadzone and saturation applied in the OSD's axis read, scoped to `IPT_PADDLE` and deliberately **not**
to `IPT_LIGHTGUN_X/Y`, where a curve would make aim non-linear with position.

**Out of scope, decided: networked / multi-cabinet sets.** `powsled` (3 sets, four linked cabinets via
`m2comm`, has a "Cancel Network Check" input) and `rascot2` (Royal Ascot 2 betting terminal). All four
are `MACHINE_NOT_WORKING` upstream. No option, mapping or fixture gets spent on them.

**One suspected bug, recorded unverified.** dynabb's BAT1/BAT2 are `IPT_PEDAL`/`IPT_PEDAL2` *and*
PLAYER(1)/PLAYER(2). `apply_device_defaults()` matches `entry.player() == device->devindex()`, so P1's
bat should land on pad 1's right trigger and P2's on pad 2's **left** — asymmetric. A code reading, not
a measurement; dynabb is `NOT_WORKING`. It is the cleanest argument for a per-game override table.

**Two things checked rather than assumed.** Input descriptors *are* sent
(`retro_entry.cpp:287`) — they are just deliberately generic, which is a different and much smaller
problem than "not wired". And `retro_set_controller_port_device` *is* an empty stub
(`retro_entry.cpp:268`) with no `RETRO_DEVICE_LIGHTGUN`/`POINTER` ever offered, so the lightgun gap is
real: `vcop`, `vcop2` and `hotd` aim with a thumbstick.

**Three things cut on the spot, and the doc records them as rejected so they are not re-proposed.**
**Widescreen** — Model 2 projects inside the copro emulation, upstream of our seam, so FOV widening
means patching MAME's copro code and breaking the 30-line upstream-diff budget the whole mergeability
strategy rests on. **Wireframe** — no `wideLines` here and `fillModeNonSolid` unverified on MoltenVK,
i.e. a probe plus a second pipeline for a debug view `ppmdiff.py`'s coverage heatmap already does
better. **"No lighting" is renamed `model2_flat_luma`**, because there is no lighting stage at this
seam to switch off: lighting is baked into `poly.luma` by the copro emulation before the seam, so
flattening the per-polygon luma is the whole of the feature. The rename is the honest name, not a
smaller version of the request.

Most of the rendering options already exist as P3's `M2VK_*` switches and just need promoting. The
standing rule survives the promotion and is restated in the doc: **an option must act on both
renderers**, or it silently invalidates every comparison in `ab-baselines.md`.

Suggested order, none of it a P4 blocker: lightgun device → analog curve → promote the switches →
per-game descriptors and the override table → internal resolution (P5).

---

## 2026-07-26 — P4 planned, and the measurement voids its premise

**`devnotes/p4-depth-and-decals.md`.** No code changed; HEAD is still `c38dbbefffe` and `git status`
is clean. The phase was scoped as "Correctness: decals / z-fight / sort — the hard part". **Measured,
the hard part is not there**, and adopting interpolated z would be a regression rather than a fix.

**Six polytap dumps of a late frame each** (vf2 f1200, srallyc f900, vcop2 f1400, sgt24h f1600,
overrev f1600, desert f700), analysed over every bounding-box-overlapping polygon pair. Two results.

**Draw order does not disagree with real depth.** Model 2 walks `z` from `min_z` to `max_z` and
`m_fillmap` gives the pixel to the first writer, so the *correct* case is "the polygon drawn later is
farther" — front-to-back, not painter's. Pairs where the later polygon is strictly **nearer**, i.e. a
genuine sort error: **0** on vf2, 6 of 42720 on srallyc, 23 of 9479 on vcop2, 4, 7 and 2 on the rest.
Bounding-box overlap is a superset of real pixel overlap, so those are **upper bounds**.

**And the coplanar population has no tie to break.** 43–366 coplanar pairs per frame, and the
**median pair is exactly 0 float32 ULPs apart** on five of the six fixtures — bit-identical
interpolated depth, not merely close. 336 of srallyc's 366 are sub-ULP. `vkCmdSetDepthBias` and
`depthBiasClamp` exist to nudge values that are *nearly* equal; there is no difference here to
amplify, so the bias would have to key on draw order — reconstructing the draw-order depth key by a
longer and lossier route.

**The decisive evidence needed no new run and it was already in `ab-baselines.md`:
`M2VK_FORCE_SOLID=2` A/B is pixel-identical, SSIM 1.0000, on vcop2/srallyc/dynamcop.** Flat shading
strips the texture chain and leaves the rasterizer, the depth key, the scissor and the composite. A
mis-ordered polygon under flat shading is a solid patch of the wrong colour — the most visible failure
available — so 1.0000 closes the ordering question outright.

**Also checked rather than assumed: the depth key cannot saturate.** `vk_geom.cpp:957` clamps at
`DEPTH_MAX_INDEX = 65535`; MAME `fatalerror`s above `MAX_POLYGONS = 32768` (`model2.h:776`), so the
clamp can never fire and every polygon is ~256 float32 ULP from its neighbours. The comment at
`vk_geom.cpp:84` justifies the width with "ample for 1450 polygons"; the `MAX_POLYGONS` bound is the
real and much stronger reason. Queued as a comment fix.

**One methodological correction, recorded because it cost the first pass.** The first run of the
analysis counted "later polygon is farther" as the error and reported 53–79 % of all pairs, which
looked like a catastrophe. It is the *normal* case — the direction was mine, not the data's, and
performance.md §3.2 already says "Model 2 draws front-to-back with first-writer-wins". Same shape as
the `perf.sh` bracket bug: a new instrument's first output contradicted something already written
down, and the instrument was wrong.

**What survives of P4:** the opaque-pipeline split (performance.md §4 item 1, whose *speed* argument
§2a already killed and whose *risk* argument is now the only one left), a resolution-invariance check
on the depth path that P5 is built on and that has so far only been argued in a comment, and a docs
pass. **This voids the first two bullets of the Polydiver plan's §4 at this seam**, the way P3 voided
its `det(3x3)` shadow bullet — the plan named the hazard, P3 took a design that avoids it, and the
phase closes because the plan worked.

**Open and deliberately unassigned:** 446–2065 pairs per frame interleave in depth over a substantial
shared area, in different buckets and far apart in draw order — plausible interpenetration, where a
per-object sort cannot be right at every pixel. We reproduce MAME's order exactly. Whether the arcade
hardware agreed is **not answerable with the ground truth this project has**, because the ground truth
is MAME. Not a reason to build interpolated z: a z-buffer would not reproduce the hardware either,
just be wrong differently.

**Consequence for the phase order, for the Polydiver plan to decide:** P4 gated P5 because "enhanced
res + z-buffer exposes decal cases". There is no z-buffer and there will not be one, so that gate is
not load-bearing — which, with §2a promoting the Quest 3 port from sequel to blocker, argues for
taking the port before P5.

---

## 2026-07-26 — Licence audit for a public binary release (`devnotes/legalstuff.md`)

**No code changed.** HEAD is still `c38dbbefffe` and `git status` is clean. The question was "assuming
the repo stays public, can a binary be released?" and the answer is **yes**, with one decision
attached that is not about copyright.

**Method: audited the tree, did not answer from memory.** Every figure comes from a command, and the
commands are in `legalstuff.md` §8 so the audit is re-runnable — which matters, because an upstream
tag merge can pull GPL-tagged files into the driver's dependency set without anything visibly
changing.

**The copyright result is clean and slightly surprising.** Of the 606 `src/` objects in `build/` that
resolve to a file on disk, **585 are BSD-3-Clause and zero are GPL-tagged**. The remainder: 15
generated m68000 files (`m68kmake.py` output from BSD-3 Musashi), `disasmintf.cpp` + `nanosvg.cpp`
untagged, 2 **LGPL-2.1+** (`imagedev/mfmhd.cpp`, `lib/formats/rpk.cpp` — TI-99 hardware arriving via
the generic device/format libs), `md5.cpp` public domain, `path_to_regex.cpp` MIT. MAME as a whole is
GPL-2.0+ because the union of all drivers contains GPL files; **this subtarget links none of them.**
All 26 third-party libs with objects are permissive — none GPL. `otool -L` on the dylib shows system
frameworks only. Our own 25 files are already `BSD-3-Clause` / `copyright-holders:mcwild77` in MAME's
header format, so nothing had to be fixed.

⚠️ **The tally is a superset and that is deliberate.** `build/` holds objects from *both* the
`libretro_m2` and `sdl3` builds — they share the `src/` and `3rdparty/` object trees, so bgfx, lua and
portaudio appear despite `module_stubs.cpp` stubbing them out of the core. For an audit that is the
right direction to be wrong in: everything in the superset is clear, and the core links less than it.

⚠️ **Two files write `// license: BSD-3-Clause` with a space after the colon** (Dirk Best's), so a
naive `grep -o 'license:[^ ]*'` buckets six files as untagged. §8's script uses `'license: ?[^ ]*'`.
Recording it because the first pass reported six mystery files that were nothing of the kind.

**The live decision is trademark, and it is a separate body of law the licences say nothing about.**
[COPYING](../COPYING) and `README.md:83` both state MAME is a registered trademark of Gregory Ember
and permission is required to use the name, logo or wordmark. A *source fork* named `mame-model2-vk`
is nominative use and ordinary practice; a *downloadable binary* with MAME in the product name is the
wordmark on a distributed product. **Half of this was already right**:
`retro_entry.cpp:220` sets `library_name = "Model 2"`, so nothing MAME-branded reaches RetroArch's UI
or `config/Model 2/Model 2.opt`. What is left is naming the release asset to match, and the real gap —
**`README.md` is upstream's, completely unmodified**, last touched by the upstream commit
`d0231349f75`. Anyone landing on the public repo reads a page presenting itself *as MAME*, with MAME's
forums, bug tracker and download links. That is the worst item here and it is a documentation problem,
not a legal one.

**Four release chores, none of them blockers:** release as **GPL-2.0-or-later** (nothing forces it —
§2 found no GPL source — but it is what COPYING declares the whole to be and BSD-3 + LGPL-2.1+ are
both compatible; claiming BSD is defensible and not worth the argument); **tag** the commit that built
the artifact (`m2vk-v0.1`, the prefix convention) because GPL/LGPL want source *corresponding* to the
binary and a drifting `main` is not that; ship **`COPYING` + `docs/legal/`** in the archive, which
COPYING explicitly requires and which discharges the libjpeg/IJG notice at the same time.

**LGPL §6 is satisfied by the public repo** — anyone can rebuild — so there is nothing to do now.
⚠️ **But the obligation survives a move to a closed binary**: a Quest 3 / Unity build shipped without
corresponding source makes those two files a real problem. Nothing Model 2 touches TI-99 floppy or
cartridge formats, so the fix that day is to cut them from the build, not to argue about §6. Worth
knowing before that build exists.

⚠️ **`3rdparty/astc-encoder` is Apache-2.0 — the only GPL-2.0-*only*-incompatible licence in the
tree.** Non-issue twice over (it arrives via bimg/bgfx, which the core stubs out, and MAME is GPL-2.0
*or later* so the combination resolves at GPL-3.0), but it becomes live **if the standalone
`mamemodel2` is ever shipped as a release artifact**, which it currently is not. In `legalstuff.md`
§6 and on the §9 checklist so it is not rediscovered.

**ROMs: clean.** `git ls-files roms/` is `roms/dir.txt` and nothing else; no `.zip`/`.7z`/`.chd`/`.rom`
in the index outside bgfx's own example assets. Standing rule added: no ROM bundled **and none linked
from the release page** — a link is its own distribution question.

⚠️ **`CLAUDE.md`'s account of the gitignore hack is out of date, and the audit is where it was
caught.** It says `.gitignore` is edited and then `git update-index --skip-worktree`'d. **`git ls-files
-v` shows no skip-worktree flag on any file**, and the working `.gitignore` is byte-identical to the
committed one — that arrangement is gone. The ignore now rests entirely on `.git/info/exclude`
(`/CLAUDE.md`, `/devnotes/`), which is never committed at all. The committed `.gitignore` is pristine
either way, so a GitHub source tarball of the tag leaks nothing. **This is the better arrangement and
should be left alone** — it removes the caveat about an upstream merge touching `.gitignore` and
needing the `--no-skip-worktree` → merge → re-add → re-`--skip-worktree` dance; one mechanism instead
of two. ⚠️ **It is also a single point of failure no clone carries**: a fresh checkout has neither
line and would show `CLAUDE.md` and `devnotes/` as untracked, so a second working copy has to re-add
both by hand before anything else.

**New file: `devnotes/legalstuff.md`** (§9 is a release checklist), row added to the devnotes index.
The one piece of new public-facing prose the release needs is the fork README — and the
commit-hygiene rules apply to it in full, along with the release notes.

---

## 2026-07-26 — P4 step 1: the opaque pipeline split, built and verified

**Commit: the early-Z pipeline split.** `poly.frag` now compiles twice, `vk_geom.cpp` owns two
pipelines instead of one, and **no pixel moves anywhere in the 12-fixture set**. Upstream diff is
still **30 lines** — nothing in this step is core-side. Files: `poly.frag`, `build_shaders.sh`,
`vk_geom.{h,cpp}`, plus the generated `poly_early_frag_spv.h`.

### What was built

The general fragment shader has two `discard` sites — the `checker` stipple and the translucent
cutout — and a `discard` anywhere in a module forces depth writes to late fragment tests for *every*
polygon drawn with it. A polygon that is neither translucent nor checkered cannot reach either site,
so it can take a variant that declares `EarlyFragmentTests` and has its depth resolved before the
shader runs.

**`EarlyFragmentTests` is an execution mode on the entry point, not a value, so a specialisation
constant cannot reach it** — the plan's phrasing ("a specialisation constant plus a second pipeline")
is half right and the half that matters is the second module. `poly.frag` is therefore compiled twice
from one source, `-DEARLY_Z=1` for the second, and `build_shaders.sh`'s `emit` grew a pass-through for
extra glslc arguments. One source rather than two files was the whole point: **the two discard sites
are gated on exactly the two flag bits `vk_geom.cpp` tests to choose the pipeline**, so the shader's
predicate and the renderer's are the same line of code and cannot drift.

Under `EARLY_Z` the two discards are removed **textually**, not left to the optimiser to fold away on
a constant — a discard that is merely unreachable is still a discard in the module. Verified in the
SPIR-V rather than assumed: the general blob has **2 `OpKill` and one `OpExecutionMode`**, the early
blob **0 `OpKill` and two** (`OriginUpperLeft` + `EarlyFragmentTests`). 30232 bytes against 25188 —
the packed alpha lane and its four neighbour tests are genuinely gone from the early variant.

⚠️ **`poly_frag_spv.h` came out byte-identical to the committed one**, so the general path is
provably untouched rather than argued to be. That is worth keeping as the check on any future edit to
this file: if the general blob moves when it should not, `git status` says so.

### The predicate, which is the load-bearing line

`(gp.flags & (FLAG_TRANSLUCENT | FLAG_CHECKER)) == 0`, read out of the same word just written for the
shader. Not `p.renderer`, not `p.checker`, not `cls` — each of those is a second copy that can fall out
of step. Getting it wrong is invisible on most frames and catastrophic on one: an early-Z polygon that
*does* discard claims the pixel in the depth buffer and never writes a colour to it, so the background
shows through a hole nothing later can fill. A stipple over a decal is where it would first show.

### Batches follow submission order, and the alternative is written down rather than taken

A `draw_batch` now breaks on the pipeline as well as on the viewport. **The depth key makes the final
picture order-independent** — the winner of a pixel is the lowest record index covering it that does
not discard, whatever order the draws arrive in, because the key decreases monotonically with the index
and the test is `GREATER` — so all the early polygons could legally be swept into one draw and all the
late ones into another. **Deliberately not done:** it would rest the picture on that argument being
airtight to buy back draw calls in a phase where performance.md §2a says the whole optimisation list
is bidding for 4.5 % of a frame. The argument is recorded in `vk_geom.cpp`'s header as the escape hatch
if the draw count ever does become the problem — see the cost below, because it might on Quest 3.

### ⚠️ The cost is draw calls, and it is much larger than the plan expected

`M2VK_NO_EARLY_Z=1` is the new switch: it sends every polygon to the general pipeline, i.e. the
pre-split behaviour. **Unlike `M2VK_NO_SCISSOR` it is a pure no-op switch** — it must not move a pixel
on either renderer, so "digests equal with it on and off" is the entire verification of the split.

| | early-Z share of drawn polygons | scissor draws in a frame, worst |
|---|---:|---:|
| `schamp` | 93.1 % | 351 |
| `dynamcop` | 87.1 % | 781 |
| `dynabb97` | 84.7 % | 144 |
| `vf2` | 84.4 % | 205 |
| `vcop2` | 84.1 % | 140 |
| `desert` | 80.8 % | 483 |
| `vstriker` | 72.2 % | 455 |
| `waverunr` | 66.0 % | 597 |
| `srallyc` | 57.7 % | 626 |
| `sgt24h` | 41.6 % | 831 |
| `overrev` | 13.4 % | 92 |

**Before this step 23 of 25 games were one draw a frame and the worst anywhere was schamp's 8.** Now
the range is 92–831. The split is what did it: the pipeline alternates with the polygon stream where
the viewport essentially never does. `overrev` at 13.4 % is the case the plan predicted — 1094 of 1145
polygons translucent, so it pays the batching and gets almost nothing.

The redundant `vkCmdSetScissor` is skipped when only the pipeline changed, so a batch boundary costs
one command rather than two. Without that the step would have doubled the command count as well.

### [measured] Wall clock unchanged; GPU time down where there is overdraw

`perf.sh`, 2500 frames, skip 1500, 3 repeats, split on against `MODE=M2VK_NO_EARLY_Z=1`:

| | `gpuwait_ms` off → on | `core_ms` off → on | `pipe%` off → on |
|---|---|---|---|
| `waverunr` | 0.892 → **0.759** (−15 %) | 4.309 → 4.351 | 403.4 → 399.5 |
| `desert` | 0.714 → **0.699** (−2 %) | 3.204 → 3.187 | 542.5 → 545.4 |

**The GPU win is real and larger than expected on waverunr, and it is invisible in the wall clock** —
`pipe%` is `max(core, gpuwait)` and `core` is 4.3 ms against a `gpuwait` of 0.8, so the GPU is nowhere
near the bottleneck here. Both `pipe%` movements are inside the run-to-run spread (1.2–1.8 %). This is
§2a restated: the renderer's whole GPU side is ~10 % of the frame on this host, so cutting 15 % off it
moves nothing measurable.

**Why waverunr gains and desert barely does: overdraw.** Model 2 draws front-to-back with
first-writer-wins, which is exactly the pattern early-Z pays off on — an occluded fragment under late
tests runs the entire filtering chain (0.388 ms of the frame, 87 % of the 3D layer's GPU cost) and is
then thrown away by the depth test. waverunr's large overlapping water and scenery quads generate a lot
of those; desert's scene has less of it. **The plan said "expect it not to [move anything], on this
machine" and that is right about the wall clock and wrong about the GPU.**

⚠️ **This is now the open question for the Quest 3 port and it cuts both ways.** On an Adreno 740 the
GPU is a much larger share of the frame, so −15 % of GPU time is worth having — and mobile draw-call
and pipeline-switch costs are far higher than here, so 831 draws a frame is a real risk. Both halves of
the trade get bigger and neither can be measured from a Mac. The sweep-into-two-draws escape hatch
above is the lever if the draw count turns out to be the binding half.

### Verified

- **All 12 fixtures: every metric and every digest byte-identical to `ab-baselines.md`**, compared
  mechanically rather than by eye (the table was regenerated with `ab-table.py` and diffed
  cell-by-cell against the committed one). Exit criterion 1 passes on all 12; `ab.sh` exits 0 on all.
- **`M2VK_NO_EARLY_Z=1` on vf2: digest `55da761fecca5c01` and the last-frame PPM byte-identical to the
  split-on run**, with the report confirming 1 scissor draw against 205 and 0 % early-Z against 84.4 %.
  Same picture from two genuinely different command streams.
- Both regression guards to the digit: `M2VK_OPAQUE_ONLY=1` vf2 **105009 covered, 95.022 %, 2 edge, 0
  interior, SSIM covered 0.996364**; `M2VK_FORCE_SOLID=2` **0.999999 / 0.999981 / 0.999979** on
  vcop2 / srallyc / dynamcop — 1.0000 to four places. Both guards drive the early share to ~100 %, so
  they exercise the early pipeline hard; the unswitched sgt24h and overrev runs are what exercise the
  late one.
- `M2VK_SW_3D=1` under `renderer=vulkan` still **`16af05bb8d02a9a5`**, byte-identical to
  `renderer=software`.
- **Lifecycle: both pipelines survive a ring rebuild.** A scripted run with the sync mask going
  3 → 1 → 4 and two `context_destroy`/`context_reset` pairs ends on the same digest
  `55da761fecca5c01` and the same early-Z count to the polygon (1163273 of 1378969); the abandon path
  (`M2VK_HOST_SKIP_DESTROY=1`, two resets) likewise. No errors, no leak reported.
- The committed `vf2-frame800` polytap fixture is byte-identical under **both** renderers.
- Standalone `OSD=sdl3` still builds.

**No screenshots for this step** — it changes no pixel by construction, and the 12-fixture digest
comparison is a stronger statement than a picture would be.

### Process note

The order-independence property of the depth key was noticed while writing the batching code, not
before, and it is the kind of thing that would have changed the design if it had come first. It is
written into `vk_geom.cpp`'s header rather than acted on, because acting on it and the split at once
would have made a failure ambiguous between the two — and the split's own verification is what proves
the property is not needed yet.

### ⚠️ Side finding: the licence audit's gitignore paragraph was wrong on every factual claim

Caught while checking that the step-1 commit had not picked up a local file. `git check-ignore -v
CLAUDE.md devnotes/worklog.md` reports **`.gitignore:52` / `.gitignore:53`**, not `.git/info/exclude`.
Against the three claims the audit put in CLAUDE.md on 2026-07-26:

| claim | measured |
|---|---|
| "hidden by `.git/info/exclude` **alone**" | both mechanisms are live and redundant |
| "`git ls-files -v` shows no skip-worktree flag on any file" | `S .gitignore` |
| "the working `.gitignore` is byte-identical to the committed one" | differs by exactly those two lines |

**No leak, and that is the part the audit got right**: `git show HEAD:.gitignore` has neither line, so
a source tarball of a release tag is clean.

### 🚨 …and then the "cleanup" was recommended, executed, and broke it

**The recommendation was wrong and the reason is a fact that was never checked.** Having found both
mechanisms live, the conclusion drawn was that the redundancy bought nothing — "both are local-only, a
fresh clone carries neither, and **either one alone already blocks `git add -A`**" — and that dropping
the skip-worktree half was therefore strictly better. That last clause was an assertion, one
`git add -A --dry-run` away from being tested, and it is false.

Executed with a backup taken first:
`git update-index --no-skip-worktree .gitignore && git checkout -- .gitignore`. **`git status`
immediately reported `?? CLAUDE.md`** — untracked, unignored, one `git add -A` from the public repo.
Restored from the backup in the same minute and re-verified: `S .gitignore` back,
`check-ignore -v CLAUDE.md` naming `.gitignore:52`, `git add -A --dry-run` clean.

**Root cause, and this is the fact the whole arrangement turns on: upstream's `.gitignore` line 41 is
`!/*.md`.** A negation that un-ignores every root-level `.md` file — upstream's way of keeping
`README.md` visible under a broad ignore. **`.gitignore` takes precedence over `$GIT_DIR/info/exclude`**,
so the exclude file's `/CLAUDE.md` loses to it. The only place a pattern can win is **inside
`.gitignore`, below line 41**, which is precisely what lines 52–53 are — and skip-worktree is what keeps
that working-tree edit out of the index so the committed file stays pristine. **The exclude file is a
partial backstop, not a substitute.** So the arrangement two write-ups called redundant is the only one
that works, and the "one mechanism is cleaner" instinct was right about aesthetics and wrong about git.

⚠️ **Why a half-broken state looks healthy: `devnotes/` is double-covered** by upstream's `/*/` at line
13, which catches every root directory. Break the mechanism and `devnotes/` stays hidden while only
`CLAUDE.md` pops out — so a glance at `git status` looks nearly fine. **Check `CLAUDE.md` by name.**

The three checks now written into CLAUDE.md and legalstuff.md §7, because one of them alone would have
prevented this:

```sh
git check-ignore -v CLAUDE.md          # must name .gitignore:52, NOT .git/info/exclude
git ls-files -v | grep -v '^H '        # must print: S .gitignore
git add -A --dry-run | grep -iE 'CLAUDE|devnotes'   # must print NOTHING
```

The merge dance is therefore unavoidable and is the price of the arrangement. It is also *rare* —
`git log --since=2023-01-01 mame0288 -- .gitignore` is empty, upstream has not touched the file in over
two years — and the earlier framing of it as a pending hazard was itself overweighted. ⚠️ **The dangerous
moment is the recovery from the merge, not the merge**: committing `.gitignore` with the two local lines
still in it publishes `/CLAUDE.md`.

**Process note. This is the fourth documented conclusion here to outlive its measurement** (with the
`cf043ff583370663` digest, `perf.sh`'s bracket and step 6's `dropped` counter), but it failed in a new
and worse way that is worth naming on its own: **the previous three were stale facts; this one was a
recommendation built on an untested assertion and then acted on.** The project's own rule — "when a new
instrument's first output contradicts something already written down, suspect the instrument" — has a
sibling: *when a simplification argument rests on a claim about tooling behaviour, run the tooling.* The
backup is the only reason this cost a minute instead of a disclosure.

---

## 2026-07-27 — P4 step 2: the depth path is resolution-invariant, and `checker` is not

**Built `M2VK_SS=<n>`, ran it on 10 fixtures at 2×, 3× and 4×, and the answer is yes.** The claim in
`poly.vert`'s header — that the draw-order key carries no screen-space term so the depth path cannot
depend on the resolution — was an argument until today and is now a measurement. P5 is built on it.

Committed as **`83491ca0fa3`**, "Draw the frame at an internal scale and resolve it back down".
No upstream file touched, so **the diff against mame0288 is still 30 lines**. New:
`renderer_vk/shaders/downsample.frag`, `devnotes/res.sh`, `devnotes/res-table.py`,
`devnotes/res-baselines.md`. Changed: `vk_present.cpp`, `vk_geom.{h,cpp}`, `poly.vert` (comment only —
`poly_vert_spv.h` regenerated byte-identical), `build_shaders.sh`, `devnotes/README.md`,
`screenshots/README.md`.

### What it is

`M2VK_SS=n` draws the whole frame — both 2D layers *and* the polygon pass — into an n× oversized
colour and depth attachment sharing the ring's render pass (a render pass says nothing about extent),
then resolves it back into the image the frontend is handed with a second pass over the same render
pass. Everything downstream still sees 496×384, so `ppmdiff.py`, `ab.sh` and `retrohost --vk`'s
read-back measure a supersampled run without knowing it is one. `M2VK_SS_POINT=1` resolves by the
centre subpixel instead of the box mean.

**`M2VK_SS` unset is a proven no-op**: vf2 2500 frames is `55da761fecca5c01`, the documented baseline,
to the digest.

### 🚨 Two things this step got wrong before measuring them, and both are the interesting part

**1. The vertex shader's half-extent must NOT be scaled with the viewport.** First attempt passed
`geom_draw` the attachment extent for everything. `poly.vert` turns m_destmap pixels into NDC and
**NDC is the resolution-independent quantity**, so the visible half-extent is right at every scale;
scaling it put the entire frame in a 1/n corner of the attachment. The symptom was a 4× run with
**zero** coverage overlap against 1× — which reads as a catastrophic ordering failure, not a wrong
constant, and would have been a very believable "the depth key is not invariant after all". The
header comment said "nothing here changes" and was correct; the misreading was mine. `geom_draw` now
takes `scale` as its own argument, applied to the scissor rectangles and nothing else.

**2. "Supersampling can only add coverage" is false, and it was already written into the plan file
before it was checked.** The tempting rule — an `A only` pixel (covered at 1×, not at n×) must be a
polygon that stopped winning — holds only if the 1× sample point is still sampled. **It is not, at an
even scale**: 2× subpixel centres sit at ±0.25 and the 1× centre at +0.5 is not among them. So a
sliver narrower than half a pixel that contains the 1× sample point can miss every 2× sample point
and vanish. **desert does exactly that** — 7 `A only` pixels at 2×, a one-pixel-wide grey mast
against the sky; **1 at 4×** as the sampling gets finer; **0 at 3× point**. Gaining *and losing*
sub-sample-width geometry is what point-sampled supersampling does and it says nothing about depth.

That is why `M2VK_SS_POINT` exists and why it is **refused on an even scale**: only an odd n has a
subpixel whose centre coincides with the 1× pixel's (`n*x + (n-1)/2` has centre `x + 0.5`), and with
one, the fragment shader runs at the *same screen positions* as the 1× render. That run is the one
that carries the claim. `res.sh` enforces `ppmdiff`'s interior verdict only on point runs and says
so in the report; on box runs it prints it as information and judges on the background reference and
exit criterion 1.

### The result — [res-baselines.md](res-baselines.md), regenerate with `res-table.py`

3× point, 10 fixtures: **`A only` 0 on 8 of 10** (dynamcop 12, schamp 2, every one of them on a
silhouette edge with 2–7 both-covered neighbours and 10 of the 14 the `(0,0,0)`-background drew-black
artefact); **coverage agreement 1.0000 on 8 of 10**; 98.97–99.97 % of covered pixels bit-identical;
SSIM 0.9994–1.0000; **0 interior coverage disagreements anywhere**.

**The measurement that actually rules out an ordering change is that the residual has no shape** — a
depth change takes a *region*. Connected components of the 3× point residual: 19–1426 differing pixels
in almost exactly as many clusters, **largest connected run 1–7 px on nine of ten fixtures**.

⚠️ **vcop2's largest cluster is 129 and it was chased rather than waved through.** It is a
one-pixel-wide vertical line at x=485, 129 px tall, going `(125,125,125)` → `(133,133,133)`: one step
along the `colorxlat` ramp on a sliver seen edge-on, i.e. the float-rounding-amplified-by-a-LUT
residual `ab-baselines.md` already attributes. Shading, not depth. The flat-shaded vcop2 run has
**one** differing pixel in the entire frame.

**Flat-shaded 3× point (`MODE=M2VK_FORCE_SOLID=2`) is the strongest form** — it removes the texture
chain and leaves the rasterizer, the depth key, the scissor and the composite, where a mis-ordered
polygon is a solid patch of the wrong colour. vcop2 / srallyc / dynamcop: coverage **identical to the
pixel** (`A only` 0, `B only` 0), 1 / 3 / 1 differing pixels, SSIM 0.999999 / 0.999972 / 0.999980.
Each of the five sits on a boundary and takes the colour of the polygon immediately next to it — a
rasterisation edge tie.

### The `checker` hand-off to P5, measured on one polygon

One vcop2 checkered quad (`M2VK_ONLY_POLY=114`, frame 1804), drawn alone: 1× draws **78968** pixels —
half the hull, the screen door — and 2× box draws **157945, exactly twice, the whole hull, with 0.000 %
of the overlap the same colour**. The 50 % screen door has become a uniform 50 % blend. This is a
*shading* problem; `p3-hw-geometry.md` already lists it beside the resolution-blind mip selection, and
S× is what forces the decision.

⚠️ **The 3× point row of that table is pixel-identical and is NOT evidence the stipple is invariant.**
The stipple is `(x ^ y) & 1`, and for odd n, `(n*x + (n-1)/2) + (n*y + (n-1)/2) ≡ x + y (mod 2)` — the
parity survives *by accident of the odd scale*. What is on the oversized attachment before the resolve
is a stipple n times finer than the hardware's. **Use a box resolve to see the stipple problem; the
point resolve hides it**, which is why both modes exist.

### Verification

- 1× no-op: vf2 2500 `renderer=vulkan` digest `55da761fecca5c01`, unchanged.
- `res.sh`'s own precondition passed on every one of the 30 supersampled runs: the `M2VK_NO_3D=1`
  background comes back **bit-identical at every scale**, last frame and whole-run digest. That is the
  check on the plumbing — the 2D layers are uploaded at 1× and NEAREST-magnified, so every subpixel of
  a pixel holds the same texel and an exact resolve returns it. If the resolve, the viewport or the
  upscale were wrong, the 2D would move and every 3D number would be measuring that instead.
- Exit criterion 1 (`ppmdiff exact`) holds on all 30.
- **The 12-fixture A/B table reproduces `ab-baselines.md` cell-for-cell and digest-for-digest** —
  every metric on all 12, and all three whole-run digests on each. That is the check that
  `M2VK_SS` unset is a no-op, and it is the second consecutive step to reproduce the table in full.
- **Both regression guards reproduce**: `M2VK_OPAQUE_ONLY=1` on vf2 gives 105009 covered / 95.022 % /
  2 edge pixels / SSIM 0.996364, and `M2VK_FORCE_SOLID=2` gives coverage agreement 1.0000 with SSIM
  0.999999 / 0.999981 / 0.999979 on vcop2 / srallyc / dynamcop.
- `poly_vert_spv.h` byte-identical after the comment edit; the other five SPIR-V blobs regenerated
  byte-identical too, which is also the check that `glslc` has not moved under us.
- Standalone `OSD=sdl3` still builds.
- The three ignore-hygiene checks pass (`.gitignore:52` names `CLAUDE.md`, `S .gitignore` is flagged,
  `git add -A --dry-run` mentions nothing local).

⚠️ **`schamp`'s background digest flipped on one run of three, and it is the documented bistability
rather than this step.** The run that moved was **`renderer=software` with `M2VK_NO_3D=1`** — the path
where none of the code under test executes — while schamp's `vulkan` background and **both** its 3D
digests matched `ab-baselines.md` exactly. Re-ran twice: `8a04ae573b2b31b2` (the table's value) then
`964db6922c299090`. `lastbrnx` disagreed on both re-runs, which is why it is in the fixture prose and
not in the measured tables. `ab.sh`'s own message is right and was followed rather than argued
around: **re-run before believing a one-off background disagreement.**

⚠️ **No Vulkan validation layers are installed on this machine**, so the two-pass arrangement is
verified by the spec and by results rather than by a validator. Reusing one render pass across two
framebuffer *extents* is legal — render-pass/framebuffer compatibility covers formats and sample
counts, not extent — and the empirical side is 30 supersampled runs with a bit-identical background
at every scale. Worth one run under validation if it ever becomes available here.
- Screenshots in `screenshots/`, including the checker pair — whose numbers (coverage agreement
  0.9763, `A only` 0) read as a clean pass and whose *picture* is where "the sky stopped being a
  screen door" is obvious. That is the case the screenshot rule exists for.

### Process note

Both mistakes above were the same mistake: **a statement about geometry that was easy to phrase and
was never sampled.** The first one at least announced itself (zero overlap is not a subtle wrong
answer); the second would have shipped as a false claim in a baselines file, because desert's 7 pixels
look exactly like noise until you ask which direction they point in and notice that 2× has *more* of
them than 4×. The habit that caught it was the project's own: when the instrument and the belief
disagree, take the instrument apart first.

---

## 2026-07-27 — P4 step 3: the docs pass, and P4 closes

**No behaviour changed.** Committed as **`eaa1355f451`**, "Correct the comments on the depth key's
width and the bucket lists". Two code comments, four docs in this tree, and the Polydiver plan. The core
was rebuilt to confirm the comment edits compile and nothing else was touched, so there is nothing to
re-measure: step 2 reproduced the 12-fixture table cell-for-cell *and* all three whole-run digests per
fixture at this HEAD, and step 3 does not touch executable code. **The upstream diff is still 30 lines**
(`git diff mame0288 --stat -- src/mame/` says `model2_v.cpp | 30 ++++`).

### The two comment corrections, and why both grew

Both were queued as one-line fixes and both came out longer, because in each case the sentence that
was wrong had no *reason* attached to it and the reason is the part a refactor will need.

**`vk_geom.cpp`'s depth-key width** justified 16 bits with "ample for 1450 polygons" — an observation
about one VF2 frame, i.e. precisely the kind of justification that stops holding without announcing
it. The real bound is structural: **MAME `fatalerror`s above `MAX_POLYGONS = 32768`** (`model2.h`), so
the key has 2× headroom over a limit the emulation enforces before the stream reaches the seam, and
`DEPTH_MAX_INDEX`'s clamp is unreachable rather than merely untriggered. §1.4's ULP arithmetic went in
with it — keys in (0.5, 1.0], a float32 ULP of ~6e-8 against a key step of 1.5e-5, so ~256 ULP between
neighbouring polygons — because "D32_SFLOAT represents these exactly" was already in the comment
without the numbers that make it checkable.

**`m2vk_frame.h`'s `bucket` field** said "within one bucket, draw order is submission order". The
lists are built by **prepend** (`model2_v.cpp:520-522` — `zpoly = poly_sorted_list[z]`, then the new
polygon becomes the head with `next = zpoly`), so `render_polygons` walks the **newest first**:
*reverse* submission order. Corrected with the reason it has never mattered in the same breath — the
record is taken at the seam, i.e. in traversal order, so the reversal is already baked into the stream
and into the draw-order key, and nothing downstream undoes it. The case it exists to catch is a future
refactor that rebuilds the record from the bucket lists rather than from the seam.

### The Polydiver plan correction

Raised in `../Polydiver/PDDocs/model2/model2_libretro_core.md`, not forked here.

- **§4 gets a correction box.** Three of its four bullets do not apply at this seam and the box says
  which and why: submission-order depth bias (voided by P4 — the draw-order key *is* submission-order
  priority, applied exactly and for free, and the coplanar population it was meant to rescue is a
  median of **zero** float32 ULPs apart, so there is no tie to break); `det(3×3)` shadow detection
  (voided at P3 — needs matrices that do not exist here, and Model 2 shadows are real geometry);
  translucency ordering (voided at P3 — nothing blends, translucency is a cutout). **Precision, the
  fourth, survived intact and is the one that came true.** The original text is kept *below* the box
  rather than deleted: it is why P3 chose the design it did, and deleting it would make the phase look
  like a mistake instead of like the plan working.
- **§3 gets the as-built phase statuses** — P1–P4 done, each with the settled decisions that should
  not be re-derived — **plus a box on the phase order**: P4 does not gate P5 (there is no z-buffer to
  amplify), the desktop has no remaining performance question (within 3.5–5 % of the emulation-only
  ceiling), and the Quest 3 port still has no phase. Written as a suggestion — *take the port before
  P5* — and explicitly left undecided, because that is a Polydiver decision.
- **Smaller stale statements fixed while in there**: the seam's line number (611 → **565 at
  mame0288**), §5's A/B harness as built (four *forced* departures — no savestates because no Model 2
  set carries `MACHINE_SUPPORTS_SAVE`, `retrohost` rather than RetroArch because only a host that
  reads the GPU back into the software path's own buffer makes the comparison mean anything, coverage
  before SSIM, no CI), and §7's four open questions, **all four of which are now closed** (threading,
  microtexture, variants, licence).

⚠️ **The numbers deliberately did not go into the Polydiver plan.** It carries findings and decisions;
the measurements stay in `devnotes/`, next to the harness that regenerates them. A number copied into
a second document disagrees with the first one within two steps — the same reason `ab-baselines.md`
opens by saying never to retype one. The plan cites `devnotes/` paths instead.

### P4 closes

- **Exit criterion 1 — no unaccounted pixel.** Step 1 reproduced `ab-baselines.md` cell-for-cell,
  step 2 reproduced it again plus every whole-run digest, step 3 changed no executable code.
- **Exit criterion 2 — the ordering decision is written down with its measurement**, and has now
  reached CLAUDE.md and the Polydiver plan, which is what stops it being re-litigated a third time.
- **Exit criterion 3 — the upstream diff did not grow.** 30 lines, unchanged since P3 step 8.

**What P4 turned out to be**: one free optimisation, one invariance check, and a correction to the
plan. It was scoped as "the hard part". The reason it was not is the whole content of §1–2 of
[p4-depth-and-decals.md](p4-depth-and-decals.md) — P3 picked a design that made the phase's problem
not exist, *because* the plan had named the hazard.

### Process note

Three of the four documents touched here were corrections of things this project itself wrote down
confidently. The pattern across P4's three steps is the same one every time — **a claim that was easy
to phrase and was never sampled**: "ample for 1450 polygons", "draw order within a bucket is
submission order", "supersampling can only add coverage", "the half-extent scales with the viewport".
None was careless; each was the obvious reading. The habit that catches them is cheap and is the only
one that has worked here: **when the statement is about geometry or about a bound, measure it before
writing it down, and when a new instrument disagrees with the record, take the instrument apart
first.**

---

## 2026-07-27 — Next work decided: the lightgun. Quest 3 shelved, README is the user's

**No code.** Two decisions recorded and one piece of research done, so the next session starts from
facts rather than from the survey's one-paragraph entry.

**The Quest 3 port is SHELVED** — not cancelled and not disproved. Everything `performance.md` §2a
and §8 say about it stands, including that it is the only place the remaining optimisation questions
can be answered. It is simply not being worked on, and the consequence worth stating plainly is that
**there is now no live performance work at all**: every item in §4 is measured dead on desktop and
alive only on Adreno. The phase-order box raised in the Polydiver plan at P4 step 3 stands as raised —
it was a suggestion, and the answer for now is "later". **The fork README stays the user's to write**,
so `legalstuff.md` §5.2 and §9 keep their open box.

**Next is the lightgun** (`user-options.md` §3.1 — biggest user-visible win, self-contained). Wrote
**§7 of `user-options.md`** as the scoping, read out of the tree rather than assumed. What it found,
in the order it changes the job:

- **6 sets, only 3 verifiable.** `vcop` + `vcopa` (port set `vcop`), `vcop2`, and `hotd`/`hotdo`/
  `hotdp` — **all three House of the Dead sets are `MACHINE_NOT_WORKING`**. Wire all three port sets;
  accept on the first three. A hotd that does not respond proves nothing.
- **The calibration is already MAME's.** Four analog ports per set, `PORT_MINMAX` differing per set
  *and* per axis (vcop X `0x083`–`0x276`, vcop2 X 137–630, hotd X 173–596), because it is the
  cabinet's on-screen window. 🚨 Do not duplicate it in the OSD and do not "correct" an aim offset
  with a second scale factor on our side.
- **MAME may bind the gun with no assignment code at all**: `inpttype.ipp:831/845` already defaults
  `IPT_LIGHTGUN_X/Y` to `GUNCODE_X_INDEXED(n) | MOUSECODE_X_INDEXED(n)`, which is what a
  `DEVICE_CLASS_LIGHTGUN` device with absolute `ITEM_ID_XAXIS/YAXIS` satisfies. ⚠️ **But our pad
  device supplies those defaults itself today** (`libretro_m2_input.cpp:208`), so whether a per-device
  default replaces or adds to the core default is the first question to settle — the two failure modes
  are "stick and gun fight over the port" and "gun never binds, looks like a dead device".
- **Two unset MAME options are probably load-bearing.** `ioport.cpp:1869` auto-selects the device
  class for these types from `-lightgun_device`, and `-lightgun` gates lightgun input; neither is in
  the argument vector at `retro_entry.cpp:349`, and **`-nomouse` is set** while the core defaults name
  `MOUSECODE` as the alternative.
- 🚨 **Offscreen reload is the real design question and it is written up as §7.6.** vcop's DSW1:1 is
  "Reloading: Normal / Auto Reload", so Normal reloads by shooting off screen — but `analog_field`
  clamps to `PORT_MINMAX`, the *on-screen* window (`ioport.cpp:3744`), and marks lightguns
  non-autocentering and non-interpolating (`:3619-3622`), while libretro states offscreen explicitly
  (`IS_OFFSCREEN`, `RELOAD`). The two models disagree. **This is the part where the obvious
  implementation looks completely correct — trigger fires, crosshair tracks — and the game is
  unplayable past the first magazine.**
- **There is no headless test for this today.** `retrohost`'s control script knows only RetroPad
  digital buttons and half-axes (`retrohost.c:996-1010`), so it needs an absolute pointer control.
  That is a `devnotes/` change and never ships, but without it the feature rests on eyeballing
  RetroArch, which this project distrusts for exactly this class of question.

⚠️ **The A/B harness is not the instrument for this and a green `ab.sh` table is not evidence** —
input does not change rendering. §7.8 lists what counts instead, and the check that matters most is
the negative one: `libretro_m2_input.cpp` is shared by all 83 sets, so **the pad path must be provably
unchanged**.

`next-session-prompt.md` rewritten around this, and CLAUDE.md's "Next" now names the lightgun and
carries both shelving decisions.

## 2026-07-27 — `devnotes/shortcuts/`: playing the core in RetroArch, and where ROMs actually go

Tooling only. **No core code changed, no commit** — `devnotes/` never ships, so HEAD is still
`eaa1355f451` and the upstream diff is still 30 lines. New: `devnotes/shortcuts/` with
`retroarch.sh`, `install-roms.sh`, `model2_libretro.info` and a README; one row added to
`devnotes/README.md`. This is the *interactive* path, kept deliberately separate from `ab.sh` /
`res.sh` / `perf.sh`, which avoid RetroArch on purpose.

**The question worth writing down: RetroArch has no ROM directory, and three things look like one.**
`rgui_browser_directory` (currently `~/Documents/ROMs/Lynx`) is only where the file browser *opens*;
`system_directory` is BIOS, not content; playlists are a scanner artefact, not a lookup path. What
actually decides the path is **our own core** — `retro_entry.cpp:337-340` builds MAME's rompath as
the directory the loaded zip sits in, plus `<system_directory>/model2`. So the folder is a
convention. Chose `~/Documents/ROMs/Model 2`: `~/Documents/ROMs` is where this machine already keeps
content, and `Model 2` is the core's `library_name`, which is also what RetroArch names the per-core
config directory (`config/Model 2/Model 2.opt` already existed under that name).

⚠️ **`install-roms.sh` copies the loose `manxttc/` and `overrev/` directories, not just the zips**,
because MAME searches each rompath entry for `<setname>/` as well as `<setname>.zip` and those two
hold files the zips are missing (roms.md, "Local patches"). Copying zips by hand loses them
**silently** — the set still loads, just without an EPROM it wanted. `" (1)"` download artefacts are
excluded; `manxttc (1).zip` is kept in `devnotes/roms` as the record of an unresolved naming question
but is not a loadable set and does not belong in a play folder. 35 zips, 655 MB.

**Two small hazards handled in `retroarch.sh` rather than discovered later.** It installs the dylib
**copy-then-rename**, because overwriting in place truncates a dylib a running RetroArch has mapped;
rename swaps the directory entry and leaves the old inode alone. And it **never edits
`retroarch.cfg`** — `config_save_on_exit` is `"true"` on this machine, which is right for interactive
play and is exactly what the measurement harnesses go out of their way to avoid. It reads
`video_driver` and warns rather than setting it, and prints the current `Model 2.opt` so an inherited
core option cannot go unnoticed (the same failure the harness scripts guard against).

Also wrote `model2_libretro.info`. Without it RetroArch lists a bare filename and has no extension
filter; with it the core is "Sega - Model 2 (Vulkan)", `hw_render = "true"`, `needs_fullpath = "true"`,
`supported_extensions = "zip|7z"`, `savestate = "false"`.

**Verified end to end**, `./devnotes/shortcuts/retroarch.sh vf2 --appendconfig … --max-frames=900`:
core loaded from RetroArch's cores dir, content from `~/Documents/ROMs/Model 2/vf2.zip`,
`SET_HW_RENDER, context type: vulkan`, `options: model2_renderer=vulkan`, ring of 3 built and
destroyed after 900 frames, `Content ran for a total of: 15 seconds` (900 / 57.52 = 15.6, so not
throttled — gotcha 6's check), **104.02 %**. 90.5 % of drawn polygons took the early-Z pipeline.

## 2026-07-27 — Double-clickable launchers, and the TCC wall that shapes them

Follow-on to the `shortcuts/` entry above; still tooling, still no core change and no commit. New:
`make-apps.sh`, `Model 2.command`, and the generated `apps/` bundles. ROMs re-installed (35 zips,
655 MB, idempotent as designed).

**A `.sh` file is not double-clickable** — Finder opens it in an editor. The two things that are: a
`.command` (Finder hands it to Terminal) and a `.app` bundle. Built both; `Model 2.app` is the one to
drag to the Dock, with an icon built from `screenshots/2026-07-26-step5-vf2.png` via `sips` +
`iconutil`, and a game picker so one icon covers all 35 sets.

🚨 **The finding worth keeping: an unsigned app launched by Finder is refused every path under
`~/Documents`, silently, with no TCC prompt at all.** The first design had `Contents/MacOS/launch`
call `retroarch.sh` directly. It wrote its log header and then stopped, leaving no process, no
dialog and no clue. A probe bundle pinned it down exactly:

| From a Finder-launched bundle | |
| --- | --- |
| read + exec inside **its own bundle** | works, even sitting in `~/Documents` |
| `ls ~/Documents/ROMs/Model 2` | `Operation not permitted` |
| exec `~/Documents/GitHub/…/retroarch.sh` | `Operation not permitted` |
| read `~/Library/Application Support/RetroArch` | works |

Both the repo and the ROM folder are under `~/Documents`, so the bundle cannot do the work itself —
and note the app *can* stat those paths (`[ -x … ]` returned true), so a guard written as an
existence check passes and the failure lands one line later. **The tell is `Operation not permitted`
on a path you can see in Finder.**

**Fix: the bundle is a three-line shim that `open -a Terminal`s the real script**, which lives in
`Contents/Resources/run.command` (inside the bundle, because `open -a Terminal <file>` cannot pass
arguments and a per-game app has to carry its game name). Terminal already holds the permission — 35
zips visible through the handoff — and RetroArch inherits it as Terminal's descendant. Verified by
actually double-clicking: `open "Model 2 (vf2).app"` gives
`RetroArch -L …/cores/model2_libretro.dylib …/ROMs/Model 2/vf2.zip` with `caffeinate` as its child,
and the picker app lists all 35 and launches the chosen one. The cost is a Terminal window alongside
the game; the alternative was asking for Full Disk Access in System Settings, which also breaks every
time the bundle is regenerated.

Two smaller ones. **macOS ships bash 3.2**, so no `mapfile` — the set list is a `while read` loop.
And **`CFBundleIdentifier` must be ASCII**: naming the per-game apps `Model 2 — vf2` put an em dash in
the generated identifier, so the names are `Model 2 (vf2)` and the id is filtered through
`tr -cd 'a-z0-9.-'`.

## 2026-07-27 — Smoke-test questions answered: the MoltenVK warning is not ours, coin works, and the pad mapping gets FBNeo parity

Three things came out of a RetroArch smoke test. **No core code changed**; the only write is
[lightgun.md](lightgun.md), which gained §2.5 and a step 6.

**1. `[mvk-warn] VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart` is
RetroArch's, not ours, and the attribution is measured rather than argued.** Two 900-frame `vf2` runs
under RetroArch, stderr captured (⚠️ **the warning does not go to `--log-file`** — grep the log and
you get zero, which reads as "it stopped happening"):

| run | our Vulkan pipelines created | warnings |
| --- | --- | --- |
| `model2_renderer=software` | 0 | **36** |
| `model2_renderer=vulkan` | 5 (2 geometry + 3 present) | **36** |

Identical with our renderer entirely absent, so all 36 belong to RetroArch's Vulkan display driver —
they arrive in three blocks of 12, one per video-driver/swapchain init. `retrohost --vk`, where only
our pipelines exist, prints **none at all**. [inferred] the reason ours are silent is that every
pipeline we create is `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`, where restart has no meaning, and
RetroArch's are strips; the 0-delta is the measurement, the topology is the explanation.

⚠️ **`vk_geom.cpp`'s header comment is wrong about this** — it says MoltenVK "says so once per
pipeline creation", and it does not say it for any of ours, on either MoltenVK build (RetroArch's
1.3.0 or Homebrew's 1.4.2 under `retrohost`). The 32-bit-index reasoning around it is still correct
and still load-bearing. Left as-is pending a code change to attach it to; fix it when something else
opens that file.

**2. Coin was reported as not working. It works.** `retrohost ./model2_libretro.dylib
devnotes/roms/vf2.zip 2200 out.ppm "1200:select:20"` takes vf2 from `CREDIT 0/2` to `CREDIT 1/2` —
read off the frame, not inferred from a digest. The path is MAME's own default,
`COIN1 = input_seq(KEYCODE_5, JOYCODE_SELECT_INDEXED(0))` (`inpttype.ipp:598`), landing on the
`ITEM_ID_SELECT` item the pad device adds; no code of ours is involved and nothing needed changing.

🚨 **What is worth keeping is why it looked broken: the `KEYCODE_5` half of that seq is dead in this
core.** The OSD registers no keyboard device — `libretro_m2_osd.cpp:136` points the keyboard module
slot at the same joystick module — so **RetroPad Select is the only route to a coin**, and on a
keyboard that is whatever the frontend binds Select to. RetroArch's default is `rshift`
(`input_player1_select = "rshift"`), not `5`. Same for `l2`/`r2`/`l3`/`r3`, which RetroArch leaves at
`"nul"`: the pedals and the service buttons have **no keyboard route at all**. That last one is a
real gap and step 6 below closes it.

**3. The pad mapping was checked against the other libretro arcade cores, and the user decided: match
FBNeo.** Read at `master` on this date — `FBNeo/src/burner/libretro/retro_input.cpp`,
`FBNeo/src/burner/libretro/retro_common.cpp`, `mame2003-plus-libretro/src/mame2003/mame2003.c`.

**Coin and start already match, unanimously**: FBNeo binds `"coin"` → `SELECT` and `"start"` →
`START`; mame2003-plus does `JOYCODE_n_SELECT → IPT_COIN(n)` and labels the descriptor `"Coin"`. Our
buttons 1–4 (`B, A, Y, X`) are also byte-for-byte mame2003-plus's default `PAD_CLASSIC` and FBNeo's
Classic. **Buttons 5 and 6 are swapped** — we have `L, R`, FBNeo Classic has `R, L`.

The divergence that matters is not the default, it is that both cores offer **alternatives as
controller device types** (`retro_set_controller_port_device`, a stub here) and expose the service
menu as a **button combo core option** rather than dedicated buttons. Written up as
[lightgun.md §2.5](lightgun.md) + step 6, placed in that phase because it reuses the very dispatch
step 2 has to build anyway.

Three places "exactly like FBNeo" cannot be literal here, all recorded in §2.5 with their evidence:

- **6-Panel has no customer.** It serves six-button fighters; the Model 2 button histogram is
  37/30/27/11/4/3/1/1 for `IPT_BUTTON1..8` and **vf2 is a three-button game** (Punch/Kick/Guard).
  Offer Classic and Modern only.
- **Modern collides with the pedals**, which FBNeo has no equivalent of: it wants `R2` as button 5
  and `R2` is the accelerator. Ship it anyway — it is a per-port choice, on exactly the sets that
  have a button 5 — and document rather than gate.
- **Model 2 has two service switches where FBNeo models one.** The combo drives `IPT_SERVICE` (test);
  `IPT_SERVICE1` (service coin) keeps L3, gated on the option being non-`None`.

The 5/6 swap is the only behaviour change in the whole section, and its blast radius is measured:
`IPT_BUTTON5`/`6` appear 4 and 3 times in `model2.cpp`, all view-change or gear buttons on driving
sets (`daytona` VR1–3, `desert` VR2/3, `srallyc` VR, `gears` GEAR 4, `segawski`, `skisuprg`).

**Follow-up the same day — the coverage audit, prompted by "should I play each game and check the
mappings?"** Answer: not for that question. The fault class a play-through is worst at — a control
with **no** mapping — is static, and it took two minutes: every `IPT_*` in `model2.cpp` against our
`configure()` plus MAME's `assignmenthelper.cpp`. Matrix and method are now
[user-options.md §3.1](user-options.md); the four gaps are folded into `lightgun.md` step 6.

Everything the platform uses is covered except four, and **two of them are in `daytona`**:

1. **`IPT_BUTTON9` ("VR4 (Green)") has no assignment.** Our comment says "no Model 2 game has nine
   buttons" — daytona has exactly nine: buttons 1–5 are the gearbox (read through
   `daytona_gearbox_r`'s `m_gears`) and 6–9 are the VR cameras. The `ITEM_ID_BUTTON9`/`10` items
   already exist on L3/R3, unassigned since P1.
2. 🚨 **`IPT_BUTTON7`/`8` collide with the pedals on daytona.** VR2/VR3 are buttons 7/8, which we put
   on the L2/R2 *trigger thresholds*, and daytona also has `IPT_PEDAL`/`PEDAL2` on those same axes.
   **Flooring the accelerator presses VR3.**
3. **`COIN3`/`COIN4`/`START3`/`START4` bind to nothing** — `airwlkrs`, a genuine 4-player cabinet,
   against `MAX_PADS = 2`.
4. **`IPT_SERVICE2`** — `powsled`, already out of scope. Won't fix, recorded so it is not re-raised.

⚠️ **Gap 3 is worth less than it looks and that is the finding: `airwlkrs` is `MACHINE_NOT_WORKING`
and is the only set using any of those four types** (one reference each). Raising `MAX_PADS` exposes
two more RetroPad ports on all 83 sets to serve a game that does not run, and cannot be verified by
playing. Doing it anyway follows §7.1's `hotd` rule — wire the port set, do not claim a result — but
it also forces the constant to **split into `MAX_PADS = 4` and `MAX_GUNS = 2`**, because the gun phase
sizes its device count off the same number and no Model 2 gun cabinet is more than two players.

**Nothing was patched, deliberately, and the reason is the useful part:** three of the four need
something step 6 frees or builds. Gap 1 needs R3, which `model2_service_buttons` holds until the
diagnostic combo replaces it; gap 2 is a layout bug and needs the read-time layout indirection to be
expressible at all; gap 3 needs the constant split that the gun device count also depends on.
Patching ahead of step 6 would have created collisions step 6 then removes.

**Gap 2 is additionally decided rather than scheduled: accept it and document it.** A RetroPad has no
free control — daytona wants 9 buttons + steering + 2 pedals, and after coin/start/d-pad/six there is
nothing left. The real fix is a per-set layout, which is the tier table `user-options.md` §6 proposes
and both plan files put out of scope. It goes in the release notes as a known limitation.

Also found in the reverse direction: **we assign `IPT_AD_STICK_Z` and no Model 2 set uses it** — dead
code with a fallback branch, harmless, but it reads as though some game needs it.

**Two of the four gaps fixed the same day, ahead of step 6.** Not committed — the tree carries them
uncommitted at `eaa1355f451`. Files: `libretro_m2_input.h`, `libretro_m2_input.cpp`,
`retro_entry.cpp`. **No upstream file touched, so the diff is still 30 lines.**

**Gap 1 — `IPT_BUTTON9` (daytona VR4) — did not need the diagnostic combo after all.** The plan said
it was blocked on freeing R3, and R3 turned out to be **already free in the default configuration**:
`model2_service_buttons` off binds `IPT_UI_MENU` to L3 and *nothing* to R3. So the assignment goes in
that same `else` branch and collides with nothing.

🚨 **Verified in a real daytona race, and the control is the point.** Getting there needed a longer
script than expected — a coin plus **five** `start` presses (`1200:select:20,1300:select:20,` then
start at 1500/1900/2300/2700/3100) over 4600 frames; at 3200 frames with one start the game is still
in its **attract demo**, which looks exactly like a race and ignores every input. That first attempt
produced three identical digests and read as "the binding does nothing".

| run | digest | |
| --- | --- | --- |
| no press | `e0f49fc40b568003` | |
| **`4000:r3:20`** | **`cbd2de9cae03fa99`** | camera moves to the high/wide chase view |
| `4000:l3:20` | `e0f49fc40b568003` | **identical to no press** — `IPT_UI_MENU` is inert, as expected |

The L3 arm is what makes this a binding result rather than emulation drift: same script, same frame,
same held length, one bit different in what the pad reports.

**Gap 3 — `MAX_PADS` 2 → 4, plus a new `MAX_GUNS` = 2**, and ports 2/3 added to `INPUT_DESCRIPTORS`.
⚠️ **Deliberately unverified**: airwlkrs is `MACHINE_NOT_WORKING` and is in neither local romset, so
§7.1's `hotd` rule applies — wire the port set, claim nothing. **`MAX_GUNS` has no reader yet**; it
exists so the gun phase cannot accidentally size off the pad count, and `lightgun.md` §2.3 is written
against it by name.

**What was verified is that it costs nothing:** vf2 2500 frames, **`16af05bb8d02a9a5`** under
`renderer=software` and **`55da761fecca5c01`** under `renderer=vulkan` — both documented baselines to
the digit, with two extra pad devices now created on every set.

Three comments corrected in the same edit, each of which had asserted something measurably false:
`MAX_PADS`'s "Model 2 tops out at two players; no set declares PLAYER3" (airwlkrs declares PLAYER3
*and* PLAYER4), `FIXED_BUTTONS`'s "no Model 2 game has nine buttons", and the pedal block, which now
carries gap 2's collision as a known limitation with the reason it cannot be moved.

## 2026-07-27 — Lightgun step 1: the instrument, and the stick's aim is not linear

`devnotes/lightgun.md` §3 step 1 — the read-out and the script controls, before any of the gun
device itself. **Committed as `607d9f6528b` "Report the resolved lightgun port values".** Files:
**new `src/osd/libretro_m2/m2vk_gunlog.h`**, plus `libretro_m2_osd.cpp` (two call sites); the
`devnotes/retrohost.c` half of the step never ships and is not in the commit. **No upstream file
touched — the diff is still 30 lines.**

### What was built

**`M2VK_GUN_LOG=<n>` prints the resolved `IPT_LIGHTGUN_X/Y` port values every n frames**, on the
emulation thread, from the OSD's `update()` just before the frame is handed over. It reports what the
*driver* will read — after assignment, `analog_field::apply_settings` and the `PORT_MINMAX` scaling —
not what the frontend was told, which is the only place the axis mapping can be checked at all
(§1.5: this OSD draws no crosshair, so there is nothing to look at).

Header-only, so it needs no entry in the two build scripts; ⚠️ it names ioport types and so must be
included **after** `emu.h`, which only a `.cpp` may include. `gun_log_close()` is called from
`osd_exit()` beside `sink_close()` — the located fields are pointers into the machine being torn
down.

The **offscreen column is `lightgun_offscreen_r` transliterated** (model2.cpp:1136) — same 5 %
border, same integer truncation, same inclusive tests, same OR of the two axes into one bit per
player. A copy and not a tap, because tapping means touching an upstream file.

`retrohost` gained the controls a gun needs: **`gun=<x>/<y>`** (normalised 0.0..1.0, reported as
`SCREEN_X`/`SCREEN_Y`), **`trigger`**, **`reload`**, **`offscreen`**, and **`--gun <port>`**, which is
`retro_set_controller_port_device`. The aim is the one control carrying a payload, because an
absolute pointer is the one thing a name cannot express.

**Added beyond the plan, and it is what actually delivers the exit criterion: a half-axis takes a
deflection fraction** — `lx+=0.35`. Without it the script could express three points and not a sweep,
and the sweep is the whole test.

### The read-out is correct, measured on vcop and vcop2

The reference line matches lightgun.md §1.4's worked example to the digit — vcop p1 X
`range 0x083..0x276  offscreen at <=0x09b or >=0x25e`.

**Full-scale stick input lands exactly on the ends of `PORT_MINMAX`**, which is §7.2's load-bearing
rule and had never been checked: `lx+=-1.0` → **0x083**, `lx+=1.0` → **0x276**, neutral → **0x17c**,
the port's own default. Monotone throughout an 11-point sweep. vcop2 the same on both players and
both axes (p1 X 0x089..0x276, p2 Y 0x024..0x1a9 driven from port 1), and RetroPad **Y+ (down) maps to
maxval**, which is the screen convention a gun will want.

**The offscreen bit trips exactly where the border says**: set at 0x083 and 0x095, clear at 0x0dc,
set again at 0x264 and 0x276. So §1.4's warning is now measured rather than predicted — **the outer
5 % of the playfield reads as offscreen**, and that is MAME's design.

🚨 **The finding: aiming with the stick is NOT linear, and the gun will not inherit that.** The
sweep's steps are 18, 71, 71, 71, 18 either side of centre, not eleven equal ones. That is
`input_device_joystick::adjust_absolute_value` (inputdev.cpp:475) applying MAME's
**`-joystick_deadzone 0.15` and `-joystick_saturation 0.85`** — fitted from the data before it was
found in the source, and the numbers agree. It is a **`DEVICE_CLASS_JOYSTICK` property only**:
`input_device::adjust_absolute_value` (inputdev.h:171) is the identity, so a `DEVICE_CLASS_LIGHTGUN`
device gets no deadzone and step 2's mapping will be straight through. Worth knowing in both
directions — it also means today's thumbstick aiming has a dead centre and a saturated outer 15 %,
which is part of why the gun is the biggest user-visible win on the list.

⚠️ **The read-out lags the script by exactly 4 frames**, measured with a 10-frame press: script
200..209, ports 204..213, duration preserved. A scripted check has to allow for it; it is a constant,
not a smear.

⚠️ **`gun=` reaches nothing today and that is the correct result.** With `--gun 0` and the aim swept
end to end, the ports sit at their defaults, because `retro_set_controller_port_device` is still a
stub and no gun device exists. Step 2 is what makes that line move; the instrument now measures it.

### Regressions

vf2 2500 frames: **`16af05bb8d02a9a5`** under `renderer=software` and **`55da761fecca5c01`** under
`renderer=vulkan` — both documented baselines to the digit, so §4 check 5 (the pad path is provably
unchanged) passes. With the variable unset the core does one predicate per frame and nothing else; on
a set with no gun ports it prints one line and stops. No pixel moved, so no screenshots — the
screenshot rule's step is step 4, where the reticle lands.

## 2026-07-27 — Lightgun step 2: the gun device, and the summing hazard is confirmed and closed

**Step 2 of `lightgun.md` §3, built on `607d9f6528b`.** `--gun 0` now moves the port. Three files,
all in `src/osd/libretro_m2/`: `libretro_m2_input.{h,cpp}` and `retro_entry.cpp`. **No upstream file
touched — the diff against mame0288 is still 30 lines in `model2_v.cpp`.** No new file, no shader, no
pixel.

### What was built

The input module held one device type and `input_module_impl<>` templates on exactly one, so the
first change is structural: a common base **`libretro_m2_device`** carrying the port and a virtual
`update(state_cb, device)`, with `libretro_m2_pad_device` and the new **`libretro_m2_gun_device`**
under it. One device list, one `for_each_device`, one call per frame drives both kinds.

- **`MAX_GUNS` = 2 has its reader**, which is what it was added for on 2026-07-27. A `static_assert`
  now says `MAX_GUNS <= MAX_PADS`, because a gun's port index indexes the pad-sized device array.
- **`retro_set_controller_port_device` stops being a stub.** Its comment was rewritten rather than
  deleted, per the plan: the next reader needs to know the old no-op was correct and why it is not
  any more. It logs the change, which is how a run proves the frontend actually asked.
- **`s_port_device[MAX_PADS]` lives in `retro_entry.cpp`, not in the input module**, and that is
  deliberate: the frontend is allowed to set a port device before content is loaded, when there is no
  module to tell, and it has to survive `retro_unload_game` so a second load keeps the player's
  choice. `poll_frontend()` takes the array; both ends run on the libretro thread, so nothing is
  synchronised.
- **`SET_CONTROLLER_INFO`** is sent for the first time — `RetroPad` / `Light Gun` on all four ports,
  one shared `PORT_DEVICES` array so that step 6's pad layouts are *entries*, not a second call.
  Gun descriptors added for ports 0 and 1 only.
- **`-lightgun` unconditionally**, `-nomouse` untouched, `-lightgun_device` deliberately absent
  (§1.3). Enabling the class for all 83 sets is what makes a mid-run device change free.

**Pointer device: decided against for now** (§6's open item). Light Gun only. Nothing here can drive
a touch pointer, so it would have shipped untested; the `PORT_DEVICES` array is where it goes if it
is ever wanted.

### Two departures from the plan, both toward less code

1. 🚨 **The gun device adds ONE assignment, not the one §2.3 named.** §2.3 says "`IPT_BUTTON1` → the
   trigger item, and nothing else", having justified adding no axis assignment on the grounds that
   `GUNCODE_X_INDEXED(n)` is already the core default. The same is true of the trigger:
   **`inpttype.ipp:34/151` give `IPT_BUTTON1` a default of `… | GUNCODE_BUTTON1_INDEXED(n)`**, and
   `IPT_BUTTON2` likewise. So the trigger and AUX_A are bound the moment the items exist and an
   assignment for them would be the second copy the plan's own rule rejects. **`IPT_BUTTON3` has no
   such default**, so AUX_B is the one that is spelled out — and it is spelled out by hand, because
   `joystick_assignment_helper::make_code()` hardcodes `DEVICE_CLASS_JOYSTICK`.
   ⚠️ **The device index in that code must be 0**: `apply_device_defaults` asserts on it
   (`ioport.cpp:2740`) and then rewrites it to the real index, which is how one assignment serves
   both players.
2. **`RETRO_DEVICE_NONE` silences the pad**, which the plan does not mention. It states an intent
   rather than fixing anything — a frontend with a port set to None already answers 0 to every
   `state_cb` — which is also why it is safe to ship unexercised, since `retrohost` cannot select it.

### The gate, and the measurement that proves it was needed

§2.2's rule — exactly one absolute source non-zero at a time — is four lines in
`libretro_m2_pad_device::update()`: on a `RETRO_DEVICE_LIGHTGUN` port, `m_axes[AXIS_LEFT_X]` and
`[AXIS_LEFT_Y]` are zeroed **after** the read, and nothing else is touched. Buttons, d-pad, triggers
and the right stick stay live, which is §2.4 and is the *absence* of a gate rather than new code.

🚨 **§1.2's hazard is real and the gate closes it, measured rather than argued.** `vcop`, `--gun 0`,
gun aimed at 0.75 **and** the left stick held hard left in the same frames: the port reads
**`0x1f9`** — the gun's value alone. Without the gate the two absolute sources sum and saturate, and
0.75 plus a full-left stick would have landed at `0x083`, the opposite end.

### Exit criterion: the sweep tracks, and it is linear where the stick is not

`M2VK_GUN_LOG`, `--gun`, both players, every value landing on the port's own `PORT_MINMAX`:

| | 0.00 | 0.25 | 0.50 | 0.75 | 1.00 |
|---|---|---|---|---|---|
| `vcop` p1 X (`0x083..0x276`) | 0x083 | 0x0ff | 0x17c | 0x1f9 | 0x276 |
| `vcop` p1 Y (`0x024..0x1a9`) | 0x024 | 0x085 | 0x0e6 | 0x147 | 0x1a9 |

**Linear to the unit** — 0.25 of `131..630` is 255.75 and `0x0ff` is 255 — which is step 1's finding
confirmed from the other side: `input_device_joystick::adjust_absolute_value` applies the deadzone
and saturation, `input_device::adjust_absolute_value` is the identity, and a `DEVICE_CLASS_LIGHTGUN`
device gets the identity. Centre lands on the port's default (`0x17c` / `0x0e6`), which is what makes
switching devices glitch-free in both directions.

`vcop2` the same on both players and both axes, driven in opposite directions simultaneously
(p1 `0x089..0x276` / `0x024..0x1a9`, p2 `0x086..0x273`), with each player's centre exact.

⚠️ **`vcopa` and `hotd` could not be run, for romset reasons and not code ones.** `vcopa` is in
neither local rompath. `hotd` fails audit on `epr-19696a.15` / `epr-19697a.16` and wants clone
`hotdo` (roms.md:45), which is also absent — separately from `hotd` being `MACHINE_NOT_WORKING`. The
port set is wired for them; nothing is claimed about them.

### The trigger reaches `IPT_BUTTON1`, and the proof is an equality rather than an impression

`M2VK_GUN_LOG` deliberately does not read buttons, so the check is behavioural. `vcop` driven into
actual gameplay (coin at 600, Start pulsed at 900/1200/1500/1800 — **Start at frame 700 is too early,
the credit has not counted yet and the run sits on the title with `CREDIT 1`**), then the trigger
pulsed six times between 2400 and 3400, 3600-frame whole-run digests:

| run | digest |
|---|---|
| coin + start only | `ddddd189735d508f` |
| + **pad B** pulsed | `9cee9fdf65501259` |
| + **gun trigger** pulsed, `--gun 0` | **`9cee9fdf65501259`** |
| `--gun 0`, no trigger | **`ddddd189735d508f`** |

Row 3 equals row 2: the gun's trigger is indistinguishable from the pad's button 1 over 3600 frames.
Row 4 equals row 1: **selecting the gun changes nothing by itself**, which is §2.2's "both sources at
0 is a well-defined neutral" measured over a whole run rather than asserted. Row 4 also carries §2.4
and §4 check 7 — its coin and Start presses arrived on a port set to `RETRO_DEVICE_LIGHTGUN` and
reached the game, so a gun port keeps its RetroPad buttons.

⚠️ **A shorter run measures nothing and says so unclearly.** At 2000 and even 4600 frames with the
too-early Start, the pad's own button changed no digest either — the game was never in play. Read the
frame before believing a null result here; a PPM of the last frame is what found it.

### Regressions

**§4 check 5 passes.** vf2 2500 frames: **`16af05bb8d02a9a5`** (`renderer=software`) and
**`55da761fecca5c01`** (`renderer=vulkan`), both documented baselines to the digit, re-run after the
final rebuild. `ab.sh vf2 2500` also reproduces the background reference identically across
renderers, coverage agreement 1.0000, 0 real interior disagreements, SSIM interior 0.996326 — the
step-8 table unchanged. **§4 check 6 passes**: `git diff mame0288` is 30 lines in `model2_v.cpp` plus
the one `model2.flt` line, untouched. The standalone `OSD=sdl3` build still links.

Two lightgun devices are now created on **every** set and the class is enabled on every set; the vf2
digests are what proves that costs nothing. No pixel moved, so no screenshots — step 4 is where the
reticle lands and where the screenshot rule bites.

---

## 2026-07-27 — lightgun step 3: offscreen and reload

`lightgun.md` §1.4 + §3 step 3. **Four lines of core code**, in
`libretro_m2_gun_device::update()` and nowhere else — no new file, no upstream file, no shader, no
pixel, and the diff against mame0288 is still 30 lines. The step was cheap for the reason §1.4
predicted: **the driver already implements offscreen**, so all this had to do was produce a value it
already recognises.

```cpp
const bool reload = state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0;
if (reload || state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
    m_axes[AXIS_X] = m_axes[AXIS_Y] = osd::input_device::ABSOLUTE_MIN;
if (reload)
    m_buttons[BUTTON_TRIGGER] = 0x80;
```

Three choices in that, each with a reason worth keeping:

- **`ABSOLUTE_MIN`, not a coordinate near the edge.** `lightgun_offscreen_r` trips on a 5 % border of
  each port's *own* `PORT_MINMAX`, so the target moves per set and per player; `ABSOLUTE_MIN` is the
  one value MAME's scaling maps exactly onto `minval` on all of them. Picking a point inside the
  border by hand would be the scale factor §5 says never to add, aimed at a moving target.
- **Both axes, not just X.** Either alone sets the bit — the driver ORs them — but the gun is meant
  to be pointing away from the screen, and leaving Y at the aim would have the game see a shot at a
  real place on the playfield.
- **The trigger ORs in rather than replacing**, so a player holding the physical trigger and pressing
  reload gets one press and not a dropped one.

### The read-out: both controls land on the two `minval`s exactly

`M2VK_GUN_LOG=5`, `vcop`, `--gun 0`, `200:gun=0.5/0.5:340,300:reload:30,400:offscreen:30`:

```
[gun] p1 x range 0x083..0x276  offscreen at <=0x09b or >=0x25e
[gun] p1 y range 0x024..0x1a9  offscreen at <=0x037 or >=0x196
[gun] f=300  p1 x=0x17c y=0x0e6 off=0  p2 x=0x179 y=0x0e8 off=0
[gun] f=305  p1 x=0x083 y=0x024 off=1  p2 x=0x179 y=0x0e8 off=0     <- reload
[gun] f=330  p1 x=0x083 y=0x024 off=1  p2 x=0x179 y=0x0e8 off=0
[gun] f=335  p1 x=0x17c y=0x0e6 off=0  p2 x=0x179 y=0x0e8 off=0
[gun] f=405  p1 x=0x083 y=0x024 off=1  p2 x=0x179 y=0x0e8 off=0     <- offscreen
```

`0x083`/`0x024` are p1 X's and Y's `minval`, to the digit, and both trip points are cleared with
room. p2 never moves, so the pin is per port and not per class. Step 1's **4-frame lag** holds
exactly (script 300..329 shows at 304..333). Repeated on `vcop2` with `--gun 1`: p2 pins to
`0x086`/`0x024`, that port's own minvals, and **p1 does not move**.

### The magazine: the exit criterion, and it needed a negative control to mean anything

`vcop`'s DSW1:1 defaults to `0x01` = **Normal** (model2.cpp), so the criterion's precondition needed
no config change. Four runs, identical scripts through frame 2249 — coin 600, Start pulsed
900/1200/1500/1800, aim held at 0.5/0.5 from 1900, six trigger pulses 2000..2159 — then:

| run | what was added at 2250 | cylinder at frame 2500 | digest |
|---|---|---|---|
| A | nothing | **1 round** | `dac0f2b52d75f1b2` |
| B | `reload` | **full (6)** | `ec023b3aefbfacbe` |
| C | `offscreen` alone | **1 round** | `e586ecb57aaf693c` |
| D | `offscreen` + a separate `trigger` | **full (6)** | `1eb28a353dfff75f` |

**B is the criterion; C is the run that makes it mean something.** Pointing off the screen does *not*
reload — the game reloads on a shot fired off the screen — so `RELOAD` had to be
`IS_OFFSCREEN` + a trigger the frontend never sent, and D is that composition reached by hand,
landing on the same cylinder. Without C, B would be consistent with "any offscreen value reloads",
which is false, and the synthetic trigger would look like decoration.

⚠️ **The cylinder is the read-out and the digest is not.** All four digests differ, including
C-against-A where the cylinder is identical — because the aim moving to the corner moves **the game's
own yellow reticle**, which is drawn in the frame. That reticle is also free independent evidence
that the aim arrives; it is the game's, not ours, and step 4's is still to come.

Pictures in `devnotes/screenshots/2026-07-27-gun3-vcop-magazine.png` (the four cylinders side by
side) and `-gameplay.png`.

### Regressions

**§4 check 5 passes**: vf2 2500 frames is **`16af05bb8d02a9a5`** (`renderer=software`) and
**`55da761fecca5c01`** (`renderer=vulkan`), both documented baselines to the digit. **§4 check 6
passes**: `git diff mame0288` is still 30 lines in `model2_v.cpp`. Nothing else could have moved —
the change is inside a branch only a `RETRO_DEVICE_LIGHTGUN` port reaches.

## 2026-07-27 — Lightgun step 4: the reticle, and the two blitters agree to the pixel

`devnotes/lightgun.md` §3 step 4, struck through and rewritten as-built. New files
`src/osd/libretro_m2/m2vk_reticle.{h,cpp}` and `renderer_vk/shaders/reticle.frag` (+ its generated
`_spv.h`); edits to `retro_entry.cpp`, `libretro_m2_osd.cpp`, `renderer_vk/vk_present.cpp`,
`scripts/src/osd/libretro_m2.lua` and `shaders/build_shaders.sh`. **No upstream file touched; the
diff against mame0288 is still 30 lines.**

MAME's own crosshair cannot be made to appear here — `render_crosshair::draw` adds a quad to the
screen's *render container* and this OSD reads pixels straight off `curbitmap()` — so §1.5 had
already established that the reticle is ours to draw. This is that.

### Where the plan was wrong, and it was one word

The Vulkan half went into **`vk_present.cpp`, not `vk_geom.cpp`**. `vk_geom` is the polygon pass —
vertex and index buffers, batching, the draw-order depth key. The reticle is a fourth fullscreen
triangle sharing the render pass, the pipeline layout and `build_pipeline()`'s sixty lines of structs
with the two 2D layers and the M2VK_SS resolve, so building it there is one `stages[1].module =` and
one more `vkCreateGraphicsPipelines`. In `vk_geom` it would have been a second copy of a pipeline
description, which is exactly what the comment above the overlay pipeline says not to make.

### What is shared, and what is honestly duplicated

`m2vk::RETICLE_SHAPE` — `{ half_thick 1, gap 2, arm 8, outline 1 }`, all in picture pixels from the
centre — is the **only** definition of the cross. The shader does not carry a copy: it is handed
those four floats in its push block, alongside the centre and the M2VK_SS scale.

What *is* duplicated is `reticle_covers()`, the four-line predicate that turns them into a pixel
test, written once in C++ and once in GLSL because there is no way to share it. Each names the other.
That is a safer duplication than P4 step 1's pipeline predicate, and for a reason worth writing down:
**this one is on screen the whole time a gun is selected**, so a drift between the two shows up
immediately as the two renderers disagreeing about a shape you are looking at, rather than as one
polygon in one frame.

### 🚨 Not alpha-blended, and that is a decision rather than a shortcut

The plan said "an alpha-blended quad". It is opaque, with an opaque 1 px black border and `discard`
everywhere else, because the two blitters write into **different backgrounds**: the CPU one writes
into MAME's finished frame before the frontend sees it, the shader writes into the GPU composite
after the OVER layer. An alpha blend would therefore produce different pixels on the two paths, and
"the two paths produce the same pixels" is the check that makes `renderer=software` a reference at
all — it is the whole reason the step could be verified by comparison rather than by eye.

The border is not decoration. A white cross is invisible on `srallyc`'s sky and `desert`'s sand, and
it costs one more evaluation of a predicate that is already being evaluated.

### Three smaller things that would be got wrong on a rewrite

- **The centre is published normalised, 0..1, not in pixels.** `publish_reticles()` in `retro_run`
  does not know the picture's size; both consumers do; and under `M2VK_SS` the Vulkan one is drawing
  into an attachment *n* times it. The shader divides `gl_FragCoord` by the scale, so one set of
  constants serves every internal resolution and the cross grows with the picture rather than
  shrinking into it.
- ⚠️ **The frontend pointer is read a second time rather than plumbed out of the input module.** The
  gun device turns the same two axes into ioport values and keeps no state a renderer could reach —
  it is behind the OSD, and the Vulkan side is on the far side of that. Two extra `state_cb` calls per
  gun per frame is the cheap half of the trade; the expensive half would be working back from the
  *port* value, which has already been through `PORT_MINMAX` and would put a calibration of ours
  between the pointer and the cross. That is precisely what §5 forbids.
- **The scissor is what makes it cheap.** One `vkCmdSetScissor` to the 18×18 bounding box per gun, so
  the draw shades about 400 fragments instead of 190464 of them discarding. The full extent is
  restored afterwards even though the pass ends on the next line, for the same reason `vk_geom`
  restores it.

### Verified against the geometry, not by eye

A checker walks every pixel of the box, classifies it with the C++ `reticle_covers` — cross, border,
or untouched — and compares both renderers' PPMs against the expected colour.

| run | cross px | border px | wrong | sw vs vk disagreeing |
|---|---|---|---|---|
| `vcop --gun 0` @ 0.25/0.4, white | **48** | **76** | **0** | **0** |
| `vcop2 --gun 1` @ 0.7/0.65, cyan | **48** | **76** | **0** | **0** |
| `vcop --gun 0`, `M2VK_SS=3 M2VK_SS_POINT=1` vs the 1x software blit | **48** | **76** | **0** | **0** |

48 is 4 arms x 2 wide x 6 long, i.e. the shape that was asked for. The third row is the invariance
check P4 step 2's machinery makes available for free: at an odd scale the centre subpixel *is* the 1x
sample point, so a reticle drawn at 3x and point-resolved has to land on exactly the same pixels, and
does.

**The two renderers being pixel-identical over the whole cross is the result that matters** — it is
what makes "two blitters" an acceptable answer rather than a thing to apologise for.

### The negative controls, which are the other half

- **`M2VK_NO_RETICLE=1`**: two otherwise identical `vcop --gun 0` runs differ in **exactly 124
  pixels** — 48 + 76, in an 18x18 box centred where the aim was — and in nothing else. So the switch
  is a true no-op in both directions, which is what lets a gun game be put through `ab.sh` at all.
- **Offscreen draws nothing**: a run with a scripted `offscreen` over its last frames is
  **byte-identical** with the reticle on and off. `RELOAD` pins both axes to `ABSOLUTE_MIN`, so the
  shot really is going into the corner; a cross where the player is pointing would say otherwise.
- 🚨 **§4 check 5 passes**: vf2 2500 frames is **`16af05bb8d02a9a5`** / **`55da761fecca5c01`**, both
  documented baselines to the digit. This was the one way this step could break something far away
  from itself: every fixture in `ab-baselines.md` and `res-baselines.md` differences against a
  background reference both renderers have to produce bit-identically, and a reticle drawn on a run
  that did not ask for one would have quietly invalidated all of it.

`git diff mame0288` is still 30 lines (§4 check 6), and the standalone `OSD=sdl3` build is clean.
Re-running `build_shaders.sh` reproduced all seven existing SPIR-V blobs **byte-identically** — same
shaderc v2026.3 — so the committed shaders did not churn.

### Colours, and why not yellow

Player 1 white, player 2 cyan, both over black. Cyan rather than yellow or red because **`vcop` and
`vcop2` draw their own aiming reticle in yellow**, and the whole value of ours in a screenshot is
being able to tell the two apart — which step 3 already leaned on, having used the game's yellow
reticle as independent evidence that the aim arrives. The colours are one table in
`m2vk_reticle.cpp` for the queued per-port colour option to read.

Screenshots: `screenshots/2026-07-27-gun4-vcop-reticle-p1.png` (white cross on the wharf wall, the
game's yellow reticle elsewhere in the same frame), `-vcop2-reticle-p2.png` (cyan, player 2's port),
`-vcop-reticle-ss3.png` (the 3x point run).

⚠️ **Not exercised under RetroArch**, where a real mouse would drive it: there is still no working way
to screenshot that frontend (gotcha 6) and `retrohost` has no pointer. What *is* verified is the whole
coordinate path from `state_cb` to the pixel. A frontend reporting `SCREEN_X/Y` on a different
convention would show up as an offset, not as a missing cross.

## 2026-07-27 — Lightgun step 5: the service buttons on a gun port, verified and not written

**Zero lines changed.** §2.4 predicted this step would be the *absence* of a gate rather than new
code, and it was: `libretro_m2_pad_device::update()` already reads the whole `RETRO_DEVICE_JOYPAD`
button set on a port set to a gun and gates only the two primary stick axes. The step was to find out
whether that is actually true from the game's side, and the whole deliverable is the runs below plus
three screenshots.

The reason not to skip it is in the plan and is worth repeating: step 6 puts the test switch on a
Start+button combo, but the combo can be set to `None`, and then the ungated L3/R3 are the only route
into a gun game's calibration menu — which is the menu a gun player most wants.

### Both controls arrive, shown in one picture

`vcop --gun 0`, `model2_service_buttons=enabled`, 1500 frames, R3 held from frame 900 to the end and
L3 pulsed at 1100/1200/1300:

    ./devnotes/retrohost --vk --gun 0 ./model2_libretro.dylib devnotes/roms/vcop.zip 1500 out.ppm \
      "900:r3:600,1100:l3:10,1200:l3:10,1300:l3:10"

Last frame is **TEST MODE with the cursor on `COIN ASSIGNMENT`** — three items down from `EXIT`, one
per L3 pulse. R3 alone opens the menu, so it reached `IPT_SERVICE`; the cursor having *moved* is the
separate proof of `IPT_SERVICE1`, and the game's own instruction line ("SELECT BY SERVICE BUTTON AND
PUSH TEST BUTTON") is Sega saying which is which. Away from the menu, L3 pulsed with no R3 takes the
attract screen from `CREDIT 0` to `CREDIT 1`.

| run (`vcop`, 1500 frames, `--gun 0` unless noted) | digest | reads |
|---|---|---|
| service enabled, no press | `381dd936274223e0` | `CREDIT 0` |
| service enabled, `900:r3:600` | `8f505a66da7c708b` | TEST MODE |
| **service disabled, same R3 script** | **`381dd936274223e0`** | `CREDIT 0` — identical to no press |
| service enabled, `900:l3:20` | `02708f3758e57bec` | `CREDIT 1` |
| service disabled, same L3 script | `381dd936274223e0` | identical to no press |
| service enabled, R3 held + 3x L3 | `a26121e74cbece37` | TEST MODE, cursor moved 3 |

🚨 **The negative control is the option, not the press.** Without the disabled-option rows, "R3
changed the picture" is equally consistent with R3 landing on something else; with them, the change
is the binding and nothing else.

### The equality that states §2.4 in full

A gun port and a pad port produce the **same frames**. `--gun 0` with `M2VK_NO_RETICLE=1` against the
same run with no `--gun` at all:

| | gun port + `M2VK_NO_RETICLE=1` | pad port |
|---|---|---|
| no press | `370b0991aecc9a80` | `370b0991aecc9a80` |
| `900:r3:600` | `46afe762908e21e6` | `46afe762908e21e6` |

Selecting a lightgun on port 0 changes exactly one thing about the output over 1500 frames, and it is
our own reticle. (Which is also why the gun-port and pad-port digests differ *without* the switch —
that is step 4 drawing a cross, not step 5 losing a button.)

`vcop2 --gun 0` agrees: R3 held + one L3 gives TEST MENU with the cursor moved one item to `MEMORY
TEST`, and its disabled-option control is byte-identical to its no-press run (`40c6ec1e9d3d07b7`).

### Two things that would otherwise be rediscovered

⚠️ **A vcop2 run with `--gun 1` measures nothing about this and looks identical to one that does.**
`IPT_SERVICE1`/`IPT_SERVICE` are player-0 types, so `apply_device_defaults` lands them on pad 1 alone
— with the gun on port 1, the scripted L3/R3 come from port 0, which is still an ordinary pad, and
the test menu opens whether or not any of this works. That run was done here first and its picture is
indistinguishable from the real one. **The gun has to be on the port the presses come from.**

⚠️ **`--gun 0` does not word-split in zsh.** Building the argument as `gun="--gun 0"` and passing
`$gun` unquoted gives retrohost the single argument `--gun 0`, which it rejects with `unknown option
--gun 0` — the same zsh trap as the `env $e` gotcha, in a new place. Three runs failed silently
inside a loop that only grepped for a digest line.

### The frontend behaviour underneath is confirmed in a shipping core, not assumed

`retrohost` reports `RETRO_DEVICE_JOYPAD` state on a port set to a gun; the open question was whether
a real frontend does. `flycast-aoj` answers it — `UpdateInputStateNaomi`'s `MDT_LightGun` branch
(`shell/libretro/libretro.cpp:2857`) polls `RETRO_DEVICE_ID_JOYPAD_L3`/`R3` explicitly on a gun port,
with a comment saying that stock flycast's omission is exactly what leaves its gun games with no
test-menu access. So another core depends on the same frontend behaviour we do. It also shows what
switching on the device type wholesale costs: flycast has to add the two reads back, where our pad
device never stopped doing them.

⚠️ **Our L3/R3 are the opposite way round from flycast's** — AoJ maps L3 to TEST and R3 to SERVICE,
ours is L3 to `IPT_SERVICE1` (service coin) and R3 to `IPT_SERVICE` (test switch). MAME's own
defaults settle nothing (F2 and 9). Recorded for step 6, which retires `model2_service_buttons` for a
combo and is the moment to choose deliberately instead of inheriting this by accident.

**§4 check 5 passes on the rebuilt binary**: vf2 2500 frames is `16af05bb8d02a9a5` /
`55da761fecca5c01`, both baselines to the digit. `git diff mame0288` is still 30 lines, trivially —
nothing was edited.

Screenshots: `screenshots/2026-07-27-gun5-vcop-testmenu.png`, `-vcop-servicecoin.png`,
`-vcop2-testmenu.png`.

## 2026-07-28 — Lightgun step 6 piece 3: the diagnostic combo, and `model2_service_buttons` is retired

§2.5.3 and step 6's third piece. **`model2_diagnostic_input` replaces `model2_service_buttons`**, with
FBNeo's eleven values verbatim and `None` as the default. Six files, all in `src/osd/libretro_m2/` —
`retro_options.{h,cpp}`, `libretro_m2_input.{h,cpp}`, `libretro_m2_osd.{h,cpp}` and `retro_entry.cpp`.
**No new file, no upstream file, no shader, no pixel: `git diff mame0288` is still 30 lines**, and vf2
2500 frames is `16af05bb8d02a9a5` / `55da761fecca5c01`, both baselines to the digit, with exit
criterion 1 holding and every `ab.sh` metric reproducing `ab-baselines.md`.

**One list, not two.** The eleven strings and the enum that numbers them live in `retro_options.h`;
the option table's `values[]` is written as `DIAGNOSTIC_VALUES[DIAG_*]` and the input module's combo
table is indexed by the same enum with a `static_assert` on its size. The obvious version — the
strings in the options file, a parallel table of controls in the input file — is two lists that agree
today, and the failure mode of their disagreeing is a combo the options menu offers and nothing
implements.

**The combo is a synthetic button item**, `ITEM_ID_BUTTON11` named "Diagnostic Combo", with
`IPT_SERVICE` assigned to it — so nothing about how the test switch reaches the machine is new, only
how its state is arrived at. The item is added whichever way the option is set (0 forever when
`None`), because an item list that changes with an option is a saved remap that changes meaning
underneath the player. `ITEM_ID_BUTTON11` is free: 1..8 are the numbered buttons and the two trigger
thresholds, 9 and 10 are L3 and R3, and `IPT_BUTTON11`'s own default in `inpttype.ipp` is `KEYCODE_M`
— a keyboard code, and this OSD registers no keyboard.

**`IPT_SERVICE1` stays on L3, gated on the option being anything other than `None`**; `IPT_UI_MENU`
keeps L3 when it is `None`. 🚨 **`IPT_BUTTON9` came out of that branch entirely**, which is what the
plan flagged as the thing most likely to be lost silently: the test switch no longer takes any pad
control, so daytona's VR4 no longer depends on an option being off.

### Every declared value reaches IPT_SERVICE, and only from its own combo

vf2, 1400 frames, the combo's controls held from frame 1200 to the end, so the last frame is the test
menu if it fired. Each value was run twice — once with the option set to it, once with the identical
script and the option at `None`.

| value | option set | option `None` |
| --- | --- | --- |
| Hold Start | TEST MENU | attract |
| Start + A + B | TEST MENU | attract |
| Hold Start + A + B | TEST MENU | attract |
| Start + L + R | TEST MENU | attract |
| Hold Start + L + R | TEST MENU | attract |
| Hold Select | TEST MENU | attract |
| Select + A + B | TEST MENU | attract |
| Hold Select + A + B | TEST MENU | attract |
| Select + L + R | TEST MENU | attract |
| Hold Select + L + R | TEST MENU | attract |

Ten for ten, and the ten `None` runs are the negative control that makes it the binding rather than
the press. A **cross** check completes it: with the option at `Start + A + B`, a scripted
`Select + L + R` held 200 frames gives `f6d92e1f74fd58ca` — the no-press digest, to the digit.

The three whole-run digests are themselves structural: every non-hold value lands on
`2bd1805ff3301e3a` (fires at 1200), every Hold-Start value on `ca7b788517a485a4` and every
Hold-Select value on `72c68e08f12030cf` (fire at 1258, and differ from each other only because Start
and Select do different things during the 58 frames before that).

**The timer is measured, not asserted.** The identical 20-frame press gives TEST MENU under
`Start + A + B` and the attract screen under `Hold Start + A + B`.

### 🚨 Consumption, read out of the game's own INPUT TEST

The plan's negative check is that `Start + A + B` does not also press Start. Neither the credit
counter nor the whole-run digest can answer that (see the two gotchas below), so the read-out is
vf2's INPUT TEST screen — reached with the combo, two `l3` presses and the combo again, then the
controls held while the run ends inside it:

| held, in INPUT TEST | START | PUNCH (MAME 1 = B) | KICK (MAME 2 = A) | TEST SW | SERVICE SW |
| --- | --- | --- | --- | --- | --- |
| `start` alone | **ON** | OFF | OFF | OFF | OFF |
| `a` + `b` | OFF | **ON** | **ON** | OFF | OFF |
| **`start`+`a`+`b`** (the combo) | **OFF** | **OFF** | **OFF** | **ON** | OFF |
| `l3` | OFF | OFF | OFF | OFF | **ON** |

Row 3 is the whole point: all three constituents are physically down, the machine sees none of them,
and it sees the test switch instead. Rows 1 and 2 are what stop that being "those buttons do not
work". Row 4 is `IPT_SERVICE1` proved directly rather than by a credit going up — and the two `l3`
presses that navigated the menu are the same binding proved a second way, since the cursor moved from
EXIT to INPUT TEST.

Screenshots: `screenshots/2026-07-28-gun6-vf2-it_{start,ab,combo,l3}.png`,
`-vf2-sel_on.png` (the menu itself).

### daytona's VR4 survived, and the digests are the ones already on record

Same script as the 2026-07-27 entry (coin twice, five `start` presses, the press at 4000), 4600
frames:

| run | digest | |
| --- | --- | --- |
| no press | `e0f49fc40b568003` | **the recorded value, reproduced** |
| `4000:r3:20`, option `None` | `cbd2de9cae03fa99` | **the recorded value, reproduced** |
| `4000:r3:20`, option `Start + L + R` | **`cbd2de9cae03fa99`** | **identical** — VR4 no longer depends on the option |
| `4000:l3:20`, option `Start + L + R` | `e0f49fc40b568003` | a service coin mid-race changes no pixel |

### The gun port keeps its route in (§2.4)

`vcop --gun 0`, 1500 frames, the combo held from 900: **TEST MODE**, against the attract screen on the
same script with the option at `None`. Step 5 proved this for R3; the route changed underneath it, so
it is proved again rather than assumed. Screenshot `screenshots/2026-07-28-gun6-vcop-testmode.png`.

### Two things that will otherwise be rediscovered as bugs

🚨 **A coin button held for 200 frames registers nothing at all.** vf2, `1200:select:200`, is
`f6d92e1f74fd58ca` — byte-identical to no press over 1400 frames — while `1200:select:20` gives
`74b626728a6aefbd` and `CREDIT 1/2`. It happens under both option settings, so it is the emulated
coin mechanism (a stuck chute) and not a binding. **A combo test scripted as a long hold therefore
looks exactly like a broken coin input**, and that is how this session's first three probe runs read.

⚠️ **The test menu does not close when the switch is released.** `PORT_SERVICE_NO_TOGGLE` is
momentary, but the game stays in the menu once entered: a 20-frame press and a 200-frame press give
the *same* whole-run digest `2bd1805ff3301e3a` over 1400 frames. So "fire the combo, release, read the
credit counter" measures nothing — the credit counter is never on screen again. INPUT TEST is the
read-out, and it is a better one anyway because it reports the port bits rather than their
consequences.

### Piece 2's exit check, closed on the way past

Step 6's second piece shipped in the tree with no worklog entry and no recorded verification, and
`retrohost` turned out to already have `--modern <port>`, so it cost four daytona runs. daytona's VR1
is `IPT_BUTTON6`, which Classic puts on L and Modern on R:

| run | digest | |
| --- | --- | --- |
| Classic, `4000:l:20` | `2e38eeab9d3ceb01` | VR1 |
| **Modern, `4000:r:20`** | **`2e38eeab9d3ceb01`** | **identical — the same MAME button, a different pad control** |
| Classic, `4000:r:20` | `e0f49fc40b568003` | button 5 is a gear; no visible change mid-race |
| Modern, `4000:l:20` | `e0f49fc40b568003` | L produces no MAME button under Modern |

The equality in row 2 is the layout dispatch working end to end; rows 3 and 4 are both the no-press
digest, so nothing else moved with it. ⚠️ **Piece 1 (the `m_buttons[]` re-index) still has no
verification of its own beyond the digests above**, which is weaker than the plan asked for but is
what a pure refactor can be checked against after the fact.

## 2026-07-28 — Lightgun step 7: the docs pass, and the phase closes

**No executable code changed.** Five writes — this entry, [lightgun.md](lightgun.md) struck through
and rewritten as-built, `CLAUDE.md`'s "Where we are" and "Next", [user-options.md](user-options.md)
§7 closed with §1's corrections folded back, and two missing sections in
[screenshots/README.md](screenshots/README.md). Nothing was re-measured and nothing needed to be:
step 6 reproduced both vf2 baselines at this working tree, and the docs steps of P3 and P4 set the
precedent that a docs pass which re-runs the harness is a docs pass that has changed something it
should not have.

### 🚨 The step's own headline task turned out not to exist

`lightgun.md` §2.5.3 flagged that the diagnostic combo "makes three core options where `CLAUDE.md`
says *Two core options exist and no more*", and step 7's line in the plan carries that forward as the
one thing in `CLAUDE.md` the step must fix. **It is not true, because piece 3 retired
`model2_service_buttons` in the same change that added `model2_diagnostic_input`.** The count is
unchanged at **two** — `KEY_RENDERER` and `KEY_DIAGNOSTIC_INPUT` are the whole of
`retro_options.h` — and the sentence had already been corrected in place on 2026-07-28 to name the
new option. Recorded here rather than silently dropped, because a reader working the plan's step-7
list will otherwise go looking for a wrong sentence and find a right one, and the natural conclusion
from that is that they are reading a stale `CLAUDE.md`.

The general shape is worth keeping: **a plan item that says "X will make document Y false" is a
prediction, and the step that lands X is allowed to make it false by a different route.** Check the
document, not the prediction.

### What was folded back into `user-options.md` §7, and it is three corrections, not a rubber stamp

§7 was the scoping written on 2026-07-27 *before* the tree was read. Three of its claims were wrong
and are now marked as such in place — struck, with the finding beside them — rather than quietly
rewritten, because the phase's plan file cites §7 by section number and a silently corrected source
makes `lightgun.md` §1 read as if it were arguing with nothing:

- **§7.4 — `-lightgun_device` is unnecessary and `-nomouse` stays.** It expected both options to be
  needed and `-nomouse` to "need a second look". A disabled device class contributes 0 before any
  item is read, and `init_autoselect_devices` early-outs once the class is enabled.
- **§7.6 — the conclusion held, the mechanism was wrong.** It has the clamp in `apply_min_max`
  against `PORT_MINMAX`; `PORT_MINMAX` arrives as `m_adjmin`/`m_adjmax` and sets the scale factors,
  while `m_minimum`/`m_maximum` stay at `ABSOLUTE_MIN`/`ABSOLUTE_MAX`. More to the point, **the whole
  question was already answered in the driver** — `lightgun_offscreen_r` reads a 5 % border off each
  port's own range, so offscreen is a value *pinned at* the edge and the clamp is the mechanism
  rather than the obstacle. That is why step 3 was four lines.
- **§7.8 check 1 — MAME's crosshair does not exist in this core.** `render_crosshair::draw` adds a
  quad to the screen's render container and this OSD reads pixels straight off the bitmap. The check
  it proposed as *the* honest read-out cannot be performed at all; step 1's logged port value
  replaced it, and step 4's reticle is ours.

§3.1's coverage matrix and §6's suggested order were updated for what shipped: gap 1 and gap 3 were
already marked fixed on 2026-07-27, and §6 item 1 (the lightgun device) is now done, with the pointer
device recorded as deferred rather than delivered.

### Screenshots: none new, and the two missing README sections are the actual gap

Step 7 moves no pixel, so the screenshot rule points at the steps that did — and they all complied:
nine files across steps 3, 4, 5 and 6, all `retrohost --vk`. What was missing is that
`screenshots/README.md` stopped at step 4, so the step-5 test-menu pair and the step-6 INPUT TEST set
were on disk with nothing saying what they show or why they were taken. Both sections are written
now.

⚠️ **The step-6 INPUT TEST shots are the ones most likely to be thrown away by a future tidy-up and
they should not be**, which is why the README says so: four near-identical pictures of a diagnostic
screen look like a duplicate set, and they are in fact the four rows of the consumption table — the
combo's constituents held down and the machine reporting it does not see them. That measurement has
no other read-out (the credit counter cannot see it and the whole-run digest cannot either), so the
pictures *are* the evidence.

### The phase's state at the end of it

**Steps 1–7 done. HEAD is still `e905bf4b159` and the working tree carries steps 4 and 6.** The two
are not separable by file: `libretro_m2_osd.cpp` has step 4's reticle blit and step 6's diagnostic
plumbing in different hunks, and `retro_entry.cpp` has both in twelve. A split commit is hunk-level
work; a single commit covering both is honest as long as its message says so. The docs in this entry
are `devnotes/`, which never ships, so they are not part of that decision either way.

Diff against mame0288 unchanged at **30 lines**, in `model2_v.cpp` and nowhere else — §4 check 6, and
the phase closes having touched no upstream file at all.

---

## 2026-07-28 — Lightgun steps 4 and 6 committed as `89bdbde4d33`; the phase is closed

The entry above ends with "HEAD is still `e905bf4b159` and the working tree carries steps 4 and 6".
That is a dated record and it is no longer true: the two went in together as **`89bdbde4d33`**, which
is the outcome that entry said would be honest, because they are not separable by file.

14 files, all `src/osd/libretro_m2/` plus `scripts/src/osd/libretro_m2.lua`, +1241 −128. **No upstream
file**, so the diff against mame0288 is still **30 lines**. The four stale "uncommitted" notes were
corrected in place — `lightgun.md`'s header and its §3 step 7 bullet, `README.md`'s index row, and
`next-session-prompt.md` — because those four are current-state documents rather than dated ones.

⚠️ **The hygiene checks were re-run before touching anything**, since a commit had landed in between:
`git check-ignore -v CLAUDE.md` names `.gitignore:52`, `git ls-files -v | grep -v '^H '` prints
`S .gitignore`, and `git add -A --dry-run` names neither `CLAUDE.md` nor `devnotes/`. Intact.

---

## 2026-07-28 — The first two `M2VK_*` switches become core options, and the `checker` stipple is fixed

`model2_internal_res` (`1x`/`2x`/`3x`/`4x`) and `model2_flat_shading` (`off`/`flat`). **Four core
options now, not two** — `DEFINITIONS[]` in `retro_options.cpp` is the authority, and the count in
prose has now been wrong twice.

The item is `user-options.md` §6 item 3, which called it "nearly free, already tested". **The plumbing
was; the feature was not.** An internal-resolution option could not honestly be offered while the
`checker` stipple was known to break above 1×, so that got fixed first and it is the substance of the
change. No upstream file, no new file; **the diff against mame0288 is still 30 lines**.

### The stipple, which is the actual work

P4 step 2 handed this to P5 with one polygon's proof and a description that turns out to be wrong in a
way worth recording. It does not "turn into a grey dither as it gets finer" (`p3-hw-geometry.md` §9)
— at an **even** scale a box resolve of a finer checkerboard covers *every* output pixel, so the
polygon goes fully opaque. The screen door does not soften; it disappears. Measured then on one vcop2
quad (`M2VK_ONLY_POLY=114`): **78968 px at 1×, 157945 at 2× — the entire hull, with 0.000 % of the
overlap the same colour.**

The fix is one integer divide. `gl_FragCoord` is in **attachment** pixels; the stipple is a screen door
in **picture** pixels, so the parity test needed `ivec2(gl_FragCoord.xy) / scale`. All n² subpixels of
a picture pixel then share a parity, and the screen door survives the resolve intact.

- **The scale reaches the fragment shader on the push constant block**, which grew from `{vec2
  half_size}` to `{vec2 half_size; uint scale}` and from `VERTEX` to `VERTEX|FRAGMENT`. Both stages
  declare the block identically even though each reads one field — the offsets have to agree, and a
  vertex shader that declares only `half_size` would leave the fragment shader reading the wrong four
  bytes. A specialisation constant would also have worked and was not used: the value is already in
  `geom_draw()`'s hand, and a spec constant would mean rebuilding both pipelines to change it.
- **At scale 1 the divide is by 1 and therefore bit-exact the old behaviour**, which is the property
  the whole change rests on and it was checked rather than argued — see below.

**Result on the same quad: 78968 px at 2× box, 100.000 % identical to the 1× draw.** The 3× point run
is unchanged at 78968 / 100.000 %, and that is expected: for odd n the parity survived by accident
already (`(n*x + (n-1)/2) + (n*y + (n-1)/2) ≡ x + y mod 2`), which is exactly why P4 step 2 said to use
a box resolve to see the problem.

### The option plumbing, and the one decision in it

🚨 **The `M2VK_*` switch overrides its option, never the reverse.** `ab.sh`'s `MODE=` and `res.sh`'s
scale arrive in the environment; a `.opt` file left in a non-default state by an interactive session
must not be able to rewrite a baseline. **Proven by digest rather than argued**, vf2 at 1300 frames:

| | digest |
|---|---|
| no options, no switches | `3452ad5414a1b0b9` |
| `model2_flat_shading=flat` + `M2VK_FORCE_SOLID=0` | `3452ad5414a1b0b9` — identical to plain |
| `model2_flat_shading=flat` | `e1e3f27080e65846` |
| `M2VK_FORCE_SOLID=2` | `e1e3f27080e65846` — identical to the option |

Both equalities matter and they say different things: the second row is the override working in the
"switch turns it off" direction (an explicit `=0` beats an option asking for flat shading), and the
fourth is the option and the switch producing the same picture, i.e. the option is not a second
implementation.

⚠️ **The `[model2] options:` line reports the OPTION, so it can disagree with the run** — and "read
`[model2] options:` before believing a result" is the standing rule, so the disagreement announces
itself: the core prints `M2VK_SS is set; it overrides the matching core option` when a switch is
present. Presence, not value, because `M2VK_SS=99` is refused back to 1× by its reader and a line
saying it was set is still the right thing to have printed.

**Only `M2VK_FORCE_SOLID=2` is reachable from `model2_flat_shading`.** Mode 1 clears the textured bit
but leaves translucency meaning "draw nothing", so the picture comes out with holes in it. That is a
*measuring* shape — it removes the same polygons from both renderers — and not something a player
should reach by accident. Mode 1 stays a switch.

**Where the values are read, and why they are setters rather than arguments.** Neither has a reader at
`retro_load_game` time: the supersample scale is latched at `context_reset`, which sizes every ring
slot's attachments and does not fire until after the load returns, and the flat-shading mode is read at
`sink_open()`. The env override lives at each reader rather than at the call site, because the
standalone `OSD=sdl3` build has no core options at all and must keep the switches working.

### Verification

- 🚨 **`ab.sh vf2 2500` reproduces both baseline digests byte-exactly** — `16af05bb8d02a9a5` software,
  `55da761fecca5c01` vulkan — with same-colour 95.059 %, SSIM covered 0.996300, 0 interior
  disagreements. That is the proof the push-constant change and the divide are a no-op at 1×, and it is
  the single most important check in this entry.
- **RetroArch 1.22.2 honours both options**, read off its own log: `model2_internal_res=2x
  model2_flat_shading=flat`, `supersample: drawing at 992x768 (2x)`, **104.58 %** average speed — i.e.
  full speed at 2× with flat shading, unchanged from the 104.50 % baseline.
- **The standalone `OSD=sdl3` build still links.**
- ⚠️ **RetroArch rewrites `Model 2.opt` on exit even with `config_save_on_exit = "false"`** — core
  options are saved separately from the main config. A test run that sets an option leaves it set. The
  invocation snippet in `CLAUDE.md` now writes all four keys and says so.

### 🚨 The bistability is much wider than recorded, and it cost most of this step

**4 of the 23 `res.sh` runs in the regeneration failed, and a fifth passed while measuring nothing.**
None of it is a regression, and the way that was established is the part worth keeping, because the
first two readings of it were both wrong.

**What it is.** Frame parity — `draw_framebuffer` picks its source with `m_screen->frame_number() & 1`
(`model2_v.cpp:765`) — gives a fixture two stable whole-run digests, and which one a run lands on
varies between otherwise identical invocations. `res.sh` compares a 1× run against an n× run, so a
**bistable fixture fails whenever the two runs land on opposite sides, which is about half the time.**
That is the mechanism behind all five anomalies and it was already on record for `schamp`
(`next-session-prompt.md`'s live caveat) and `lastbrnx`.

⚠️ **The middle reading was wrong and is the useful mistake here.** Re-running `schamp` and `waverunr`
reproduced their failures with *byte-identical digests*, which reads as "deterministic, therefore a
real bug" — and it is not, because two runs landing on the same side of a coin is not evidence the
coin has one face. **Two samples cannot distinguish bistable from deterministic.**

**The decisive test was building the parent commit** (`89bdbde4d33`) into a second dylib and running
the same comparisons on both:

- **`schamp` background: 8 runs on each binary, at 1× and 2×, all `8a04ae573b2b31b2`** — while the
  runs that failed produced `964db6922c299090`. Two stable values, both binaries, same distribution.
- **`waverunr` background: the two digests *swapped* between the binaries** — base gave
  `584419cd7f81aa06` at 1× and `bd071b6d5c610455` at 4×, new gave `bd071b6d5c610455` at both. Both
  values occur on both builds, which is bistability in one observation.
- **`overrev` at 3× point is bit-identical across the two binaries** (`85971911d2ae78f6`, 3/3 each),
  so the change is a provable no-op there — while its **1× run flipped** (`0f234dc8f9de8b9f` twice,
  `ef7485871e90d07c` once). That is the fixture that "passed while measuring nothing".

⚠️ **`res.sh` guards the background reference and NOT the 3D 1× reference**, which is the gap that let
overrev through: its background landed consistently, its 3D 1× did not, and the row it produced
(3× point, same colour 99.853 % → 36.801 %) is plausible, passing, and meaningless. **The tell is a
moved `covered 1x`** — nothing about a supersampling change can touch a 1× frame. The same tell caught
`dynamcop`, whose `covered by A` went 138692 → 138711 while its *box* run minutes earlier reported
138692 and reproduced both baseline rows to the digit.

🚨 **The bistable list is now `lastbrnx`, `schamp`, `waverunr`, `dynamcop` and `overrev` — five of the
twelve fixtures.** It is a property of the harness, not of any game, and it is the single biggest
obstacle to trusting `res-baselines.md`. The affected rows below were re-run until the 1× and n× runs
agreed.

### ⚠️ The original note on the first two failures, kept because the reasoning is the record

Both are the frame-parity bistability (`draw_framebuffer` picks its source with
`m_screen->frame_number() & 1`, `model2_v.cpp:765`), and **one of the two fixtures was already on
record for exactly this** — `next-session-prompt.md` carries it as a live caveat: *"schamp's background
digest is bistable and flipped on one run of three, on the `renderer=software` + `M2VK_NO_3D=1` path
where none of the renderer code runs."* So this is a known instrument problem being hit again, not a
discovery.

**`schamp` box tripped `BACKGROUND REFERENCE MOVED AT 2x`** (whole-run digest; the last frame agreed).
The background run is `M2VK_NO_3D=1`, where the polygon pass never executes and `poly.frag` is not
invoked at all — so the change cannot reach it even in principle. This is the caveat above, in its
documented form.

**`dynamcop` 3× point exited non-zero on 2447 real interior disagreements against a baseline of 0**,
and this one *is* a new fixture for the list:

- **Its own 1× reference had moved** — `covered by A` 138692 → **138711** — and nothing here can touch
  a 1× frame (the divide is by 1; vf2's digests are byte-identical).
- **The dynamcop *box* run in the same batch reported 138692, the baseline exactly**, and reproduced
  both its baseline rows to the digit. Same game, same frame count, same binary, minutes apart.

⚠️ **So the bistable list is `lastbrnx`, `schamp` and now `dynamcop`** — three of twelve fixtures, which
makes it a property of the harness rather than of any one game. `ab.sh` already compares the background
digest as well as the last frame for this reason and `res.sh` checks the background at every scale;
both are right to, and **the rule is re-run before believing a one-off disagreement**.

### `res-baselines.md` regenerated — where the stipple fix shows, and where it must not

Ten fixtures at 2×/3×/4×, plus the flat-shaded point trio. **The point-resolve table is unchanged**
and that is correct rather than disappointing: for odd n the stipple's parity survived by accident
already, so those runs never saw the bug. Every point row reproduces its baseline
(`dynamcop` 99.832 %, `vcop2` 99.739 %, `desert` 98.973 %, `overrev` 99.847 % against 99.853), and the
result is now **`A only` 0 on 9 of 10** where it was 8 of 10 — ⚠️ **not an improvement**: `schamp`'s 2/1
became 0/0 because that run landed on the other side of its frame parity.

**The box table is where the fix shows, and only on fixtures that have checkered polygons:**

| | `same colour` before → after | `B only` | SSIM covered |
|---|---|---|---|
| vcop2 2× | 44.158 % → **71.973 %** | 3749 → **38** | 0.5754 → **0.9828** |
| vcop2 4× | 42.729 % → **69.253 %** | 3749 → **67** | 0.5788 → **0.9878** |
| overrev 2× | 50.578 % → **52.940 %** | 0 → 0 | 0.9271 → **0.9641** |
| desert 2× | 13.232 % → **14.359 %** | 262 → 262 | 0.8793 → **0.9030** |

🚨 **The guard is the fixtures that did NOT move**: `dynabb97`, `dynamcop` and `vf2` reproduce their box
rows **to the digit**, which is what says the change is confined to checkered polygons rather than being
a general shift in the resolve.

⚠️ **`overrev`'s box row needed the physical-ordering check to be trusted.** A first clean-exit run gave
2× **28.129 %** and 4× **49.266 %** — a box resolve scoring *worse* at 2× than at 4× is backwards, since
less averaging cannot mean less agreement, and that inversion is the tell that its per-scale runs landed
on different parity sides. Re-run: 52.940 / 48.669, correct ordering. **`res.sh` cannot catch this** —
each row exits 0.

### One process note: the binary was rebuilt mid-batch, and that was checked rather than assumed

`set_option_supersample()`'s out-of-range complaint goes through `vk_log()`, which is **a silent no-op
until `set_log()` runs** — and `set_log()` is `declare_hw_render()`'s first act, which happens *after*
the setter did in the first draft. The two setter calls moved below it, and the core was relinked while
the retry batch was still running, so part of the table came from each binary. The change moves two
calls and touches no rendering, but "it obviously cannot matter" is the phrasing this worklog keeps
catching, so it was measured: **`ab.sh vf2 2500` on the final build gives `16af05bb8d02a9a5` /
`55da761fecca5c01`, same colour 95.059 %, SSIM 0.996300** — identical to the run taken before the
relink, to every digit.

---

## 2026-07-28 — The options only applied at load, which is a bug, and the shortcut hid it

Reported within the hour of the previous entry: **"none of those options actually work in RetroArch"**
— flat shading and internal resolution both. Two separate causes, both mine.

### Cause 1: the invocation snippet overwrote the options menu on every launch

The RetroArch command in `CLAUDE.md` carried

```sh
printf 'model2_renderer = "vulkan"\n…model2_internal_res = "1x"\nmodel2_flat_shading = "off"\n' \
  > ~/Library/…/Model 2.opt
```

which is right for a *measurement* run — a harness run must not inherit whatever was last chosen
interactively — and is a trap in a "just play it" shortcut, because it rewrites the file **before every
launch**. A setting changed in the menu is saved on exit and wiped on the next start, so it appears not
to work, and appears not to work after a restart either. The block is now flagged 🚨 DELETE THESE IF YOU
ARE PLAYING and a play invocation is written out beside it that also `env -u`'s the `M2VK_*` switches —
because those beat the option by design, so carrying one into a play session silently disables the menu.

⚠️ **The measured tell that it was the shortcut and not the core:** `Model 2.opt` held the defaults with
an mtime matching the moment *this session* reset it, hours earlier — RetroArch had not written it since,
so no menu change had survived to be read.

### Cause 2: the real one — the options were load-only

`retro_run()` saw `GET_VARIABLE_UPDATE` and logged *"they take effect the next time content is loaded"*.
For `model2_renderer` that is honest. **For anything a player is meant to play with it is a bug**: you
change it, nothing happens, and the correct conclusion is that the option is broken.

Both new options are now applied live:

- **flat shading** — `g_force_solid` is a plain global that `submit()` reads per polygon, so it is
  written at the same point in `retro_run()` that publishes input, i.e. with the emulation thread parked
  on the baton. Next frame.
- **internal resolution** — `present_frame()` now compares the wanted scale against the ring it built
  and rebuilds when they differ, exactly as it already did for a sync-mask change. `destroy_ring()`
  already brackets a `vkDeviceWaitIdle` with the frontend's queue lock, which is what makes a rebuild at
  an arbitrary frame safe; no new machinery. Next presented frame.

`read_supersample()` was split so the rebuild test and the build itself cannot disagree:
`wanted_supersample()` is silent and side-effect-free (it is called once per presented frame),
`read_supersample()` keeps the logging. `M2VK_SS` is parsed once rather than 57 times a second.

### 🚨 Why this was not caught: the harness could not change an option mid-run

Every check written for the previous entry set the option **before** the run — so all of them pass
identically whether the option is applied live or at load. The test that would have caught it did not
exist, which is the general shape worth remembering: **a verification that cannot distinguish the bug
from the fix is not a verification.** "RetroArch read the option" was measured; "the picture changed"
was not, and the two are different claims.

`retrohost` gained **`M2VK_HOST_OPT_AT=<frame>:<key>=<value>[,…]`** for exactly this — it rewrites
`g_options[]` mid-run and raises `GET_VARIABLE_UPDATE` once, which is the one frontend behaviour the
harness could not previously produce. Verified with it:

- `1150:model2_flat_shading=flat` — the frame-1300 picture is flat-shaded (screenshot evidence, not a
  log line).
- `1000:…=3x,1150:…=1x,1250:…=4x` — three ring rebuilds in one run, `1x -> 3x`, `3x -> 1x`, `1x -> 4x`,
  run completes clean. **Both directions**, which a one-way test would have missed.

⚠️ **Two harness traps met while writing that test, both worth knowing.** `M2OPT_<key>` in the
environment still wins in `option_value()`, so a scripted change to a key that is also pinned from the
environment silently does nothing — the first 3×→1× attempt read `3x` at frame 1150 for that reason and
looked like a failed rebuild. And **`timeout` does not exist on macOS**: a run wrapped in it exits 127
with no output, which reads exactly like the core hanging.

### ⚠️ And one self-inflicted false alarm worth recording, because it looks catastrophic

The regression guard run straight after the live-apply change reported the **software** digest moved
(`16af05bb8d02a9a5` → `9c20f1fac9d9fe92`), 37383 real interior disagreements and SSIM 0.279 — i.e. the
one renderer this work cannot touch appearing to have broken completely.

**`make` had been started while `ab.sh` was still running.** `ab.sh` dlopens the core four times in
sequence, so the relink landed between two of them and the software and Vulkan runs used different
binaries. Re-run with the build settled and nothing else in flight: `16af05bb8d02a9a5` /
`55da761fecca5c01`, same colour 95.059 %, SSIM 0.996300, **0 interior disagreements**.

**The tell is that the *software* number moved**: `renderer=software` runs none of this code, so a
change there is either the harness or the binary, never the renderer. Same tell as the bistable
fixtures' moved `covered 1x`, and the same rule — *when a number moves that structurally cannot move,
suspect the measurement.* Do not build while a harness run is in flight.

### The end-user install, because the terminal invocation was never the right way to play

Set up 2026-07-28 after the options-do-not-work report: **playing the core and measuring it are
different activities and should not share a command line.** Every invocation in this file pins options,
clears state and passes switches, which is right for a harness run and actively wrong for a session
where the options menu is supposed to be in charge.

What is now installed, none of it in the repo:

- **`~/Library/…/RetroArch/cores/model2_libretro.dylib` is a SYMLINK to the repo build**, not a copy.
  🚨 The copy that was there had been stale for two hours and predated the live-apply fix — i.e. the
  installed core and the tested core were different binaries, which is its own way of making a change
  "not work". A link means a rebuild needs no second step. `retrohost` against the installed path
  confirms it loads and runs.
- **`~/Library/…/RetroArch/info/model2_libretro.info` already existed** and is correct
  (`Sega - Model 2 (Vulkan)`, `zip|7z`, `supports_no_game = "false"`). Its cache
  (`info/core_info.cache`) was dropped so the new link is re-read.
- **`~/Documents/RetroArch/playlists/Sega - Model 2.lpl`** — 32 entries from `~/Documents/ROMs/Model 2`,
  **labelled from `mamemodel2 -listfull`** so the list reads "Daytona USA (Revision A)" rather than
  "daytona", with the core pinned per item and as the playlist default. `model1io`, `segabill` and
  `bel` are excluded: device/BIOS sets that cannot be launched.
- **`~/Desktop/Model 2.app`** — a two-file bundle whose executable is a `sh` script. It launches
  RetroArch with **no flags at all** and `env -u`'s every `M2VK_*` switch, so the options menu is the
  only thing deciding anything. No `--appendconfig`, no `.opt` rewriting.

⚠️ **`-L <core>` with NO content does not do what it looks like: RetroArch prints
`Libretro core requires content, but nothing was provided` and EXITS.** So "a shortcut that opens
RetroArch with the core preloaded, then let me browse" is not achievable that way — the core declares
`supports_no_game = "false"` and it genuinely needs a ROM. The playlist is the answer instead, and it is
also the ordinary RetroArch experience: open the app, pick the game, it launches on the right core.

## 2026-07-28 — P5: `model2_internal_res` becomes a real internal resolution, not an antialiasing setting

**The option shipped this morning was a menu on top of `M2VK_SS`, which renders at n× and resolves
back DOWN to 496×384.** That is supersampling. It works, it was verified in both directions, and it is
the wrong feature: the frontend still received a 496×384 picture. Caught by looking at the screenshots
of it — 1× has 782 unique colours in vf2's last frame and 4× has 11172, every extra one a blend
produced by throwing the resolution away.

**As built: nine absolute resolutions, `496x384` to `2848x2136`, drawn into and handed over as drawn.**
Full record in **[p5-internal-resolution.md](p5-internal-resolution.md)** — this entry is what was
learned rather than what was built.

Files: `vk_present.{h,cpp}`, `vk_geom.{h,cpp}`, `poly.{vert,frag}`, `reticle.frag`, `retro_entry.cpp`,
`retro_options.{h,cpp}`, `devnotes/retrohost.c`. **No new file, no upstream file — the diff against
mame0288 is still 30 lines.**

### The instruction was "look at how Flycast works, match that", and it changed the shape twice

First it settled the frontend handshake — `../flycast-aoj/shell/libretro/libretro.cpp:686-765`:
lazily-growing `max_width`/`max_height`, `SET_GEOMETRY` inside the max and `SET_SYSTEM_AV_INFO` only
when it must grow, and `base_width`/`base_height` pinned small so the frontend does not open a
2848-pixel window at startup (Flycast's own comment is "avoid gigantic window size at startup").

Then it settled the **values**: Flycast lists absolute resolutions, not multipliers. That looked
cosmetic and was not. **Every entry above native is 4:3; the hardware's picture is 1.2917.** So the
scale became **fractional and non-uniform** — 640×480 is 1.290× across and 1.250× down — and three
things had to change that an integer multiplier would never have touched:

- `geom_draw`'s per-polygon scissor, from integer arithmetic to two floats **rounded outward**
  (`floor` the near edge, `ceil` the far one). Outward can only keep a boundary pixel, never drop
  one. ⚠️ The far edges are computed in **double** because an unscissored batch carries `INT32_MAX`
  and the multiply has to survive as far as the clamp.
- The reticle: the aim scaled **per axis**, the cross's size by **one** factor. Scaling the shape by
  both would give a 4:3 target a crosshair with arms longer across than down.
- The 2D layers now magnify unevenly — at 1.29× some source columns double and some do not. Kept
  NEAREST anyway; LINEAR would soften HUD text that is meant to be crisp. **This is the one place a
  non-integer target genuinely looks worse than an integer one**, and it is written up as open.

### The verification that carries the claim, and why it needed a switch the menu cannot reach

**No entry in the list is an integer multiple of 496×384**, which costs the cheapest possible check:
a real 3× frame, point-downsampled by taking the centre subpixel, should be *bit-identical* to what
`M2VK_SS=3 M2VK_SS_POINT=1` already produces — a path measured over ten fixtures at P4 step 2. So
**`M2VK_RES=<w>x<h>`** exists, overriding the option the way every switch here does, and
`M2VK_RES=1488x1152` makes the check possible.

**0 differing pixels of 190464, on all ten `res-baselines.md` fixtures.** Exact, not statistical. It
holds for checkered polygons too even though the two runs use *different* stipple divisors, because
for odd n the centre subpixel `(3x+1, 3y+1)` has parity `x+y` either way.

⚠️ **`overrev` and `schamp` failed the first time and both are on the bistable list.** Nine pairings
of three runs each agree on overrev, four of two on schamp. The trap is exactly as documented: two
samples cannot distinguish bistable from broken, and the first sample of each looked like a real bug
with a plausible failure signature (115980 differing pixels on overrev).

### The stipple decision, and P4 step 2's hand-off is answered

`pc.scale` in `poly.frag` became **`pc.stipple_div`** — attachment pixels per square of the screen
door — set **per frame** rather than derived from the resolution, because the two things it can mean
are different pictures. `M2VK_SS` needs the picture's divisor or the resolve flattens the door into a
50 % blend (P4 step 2's measured bug); a real internal resolution wants **1**, the finest dither the
picture can carry, which reads as smooth translucency. The user chose the fine dither.

Measured on P4 step 2's own fixture (vcop2, `M2VK_ONLY_POLY=114`): the quad covers **57.401 %** of the
frame at native and **57.422 %** at 1440×1080 — still half, at three times the frequency.

### Four things worth carrying

- 🚨 **`ensure_limits()` has to run before the ring-rebuild TEST, not merely before the build.** Both
  go through `wanted_resolution()`; a clamp that appeared between them would make them disagree for
  ever and rebuild the ring on every frame of the run. Written that way from the start, but it is the
  non-obvious ordering constraint in the change.
- 🚨 **The device clamp is not a sanity bound.** This device reports `maxImageDimension2D` 16384, so
  `M2VK_RES=16000x16000` was *accepted*, allocated **5.7 GiB** and ran. Only the option's value list
  bounds what a player can ask for. Left as is: the switch is ours, and the device limit is the bound
  that will matter on Quest 3.
- **A `.opt` file holding `1x` or `2x` falls back to native**, which is right for `1x` and silently
  drops `2x`'s supersampling — there is nothing to map it to. The log prints `native` rather than the
  `0x0` the parser produces, because `model2_internal_res=0x0` reads as a broken option.
- **The pipelined speed barely moves with resolution and that is not a mistake in the measurement.**
  vf2, 1000 frames after 1500 skipped: `gpuwait` 0.743 → 4.253 ms from native to 2848×2136, `core`
  steady at 4.38–4.46, `pipe%` 396.8 → **390.0**. The emulation thread is still the long pole; 2848×2136
  is roughly where the GPU catches it. ⚠️ `serial%` falls 339 → 200 % and quoting it would be wrong for
  performance.md §2a's reason — the harness reads back and hashes every frame, and that cost scales
  with the pixel count.

RetroArch 1.22.2 at 1440×1080: `SET_GEOMETRY` accepted, **Average speed 104.46 %** against 104.50 % at
native, `Content ran for a total of: 24 seconds` for 1400 frames. Native is a proven no-op —
`ab.sh vf2 2500` reproduces `16af05bb8d02a9a5` / `55da761fecca5c01` byte-exactly and
`POINT=1 res.sh vf2 2500 3` reproduces `res-baselines.md`'s row to the digit.

---

# 2026-07-28 — `model2_transparency`: the `checker` screen door as a real blend

Asked for as a core option: true transparency instead of the screen-door transparency. Built, off by
default, Vulkan only. **[blended-transparency.md](blended-transparency.md) is the record.** No new
source file, no upstream file, no shader file added — `poly.frag` and `poly.vert` grew a push-constant
word, `vk_geom.{h,cpp}` grew a third pipeline and a deferred list, and the option is the usual four
lines in `retro_options.{h,cpp}` plus `retro_entry.cpp`. **Diff against mame0288 unchanged.**

## The design point, and it is not the blending

The first idea — `blendEnable = VK_TRUE` on the checkered polygons and nothing else — is wrong, and the
reason belongs to the stream rather than the polygon. **Model 2 submits front to back.** At the moment
a checkered polygon rasterises, the geometry behind it does not exist in the colour attachment yet, so
there is nothing correct to blend against. Keeping depth writes makes it occlude what it should be seen
through; dropping them lets the polygon behind draw opaquely over the blend.

So they are **deferred to a second pass**, drawn after every opaque polygon, out of a third pipeline
that **tests depth and does not write it**. The payoff is that the pass needs no sorting of the opaque
stream at all: the depth buffer the first pass leaves behind holds the key of the *lowest record index*
that claimed each pixel, and `GREATER` passes exactly where the deferred polygon's own index is lower —
i.e. where nothing opaque is in front of it. **That is P3's draw-order-key decision paying out.** With
an interpolated z-buffer the same trick would have inherited every coplanar tie the phase avoided.

The deferred pass is walked **back to front**, i.e. reverse record order. It is the only place in this
renderer where draw order decides a colour; everywhere else the key makes the picture
order-independent, and it still does between these polygons and the opaque ones. It is only each other
they have to be ordered against.

## The check that matters is the MEAN, not the screenshot

🚨 A 50 % screen door and a 50 % blend have the **same average** over any region larger than a texel —
the door draws the polygon at half the pixels and what is behind it at the other half. So a blend
against the *wrong* partner (the 2D under-layer instead of the geometry behind) would still look
plausible, and would move the mean a long way.

| | mean, stipple | mean, blended | delta | pixels differing | max delta |
|---|---|---|---|---|---|
| `vcop2` | 99.372 | 99.457 | **+0.085** | 74585 | 128 |
| `waverunr` | 75.540 | 75.541 | **+0.000** | 1065 | 122 |
| `sgt24h` | 84.416 | 84.417 | **+0.001** | 595 | 91 |

74585 pixels move by up to 128 of 255 and the frame's mean moves by 0.085 of 255. Coverage on `vcop2`:
`A only` **0**, `B only` **3749**, all on an edge — the screen door's holes being filled, and the same
3749 `res-baselines.md` records for the pre-fix 2× box resolve. Same geometry, seen from the other side.

## Verified

- **The default is a proven no-op on four fixtures**, whole-run digests against `ab-baselines.md`:
  `vf2` `16af05bb8d02a9a5` / `55da761fecca5c01`, `vcop2` `ccd47e79f2722f86`, `sgt24h`
  `c8f12a6a197619c0`, `waverunr` `a3ac5e6e50bf0353`. All byte-exact.
- **`M2VK_BLEND=0` beats `model2_transparency=blended`** — byte-identical to the accurate run. The
  override works in the direction that protects a baseline, which is the direction that is easy to get
  backwards.
- **The option alone** (no switch) is a byte-identical PPM to the `M2VK_BLEND=1` run.
- **It applies live.** `M2VK_HOST_OPT_AT=1200:model2_transparency=blended`: blended count 36439 → 27129
  over the run, last frame byte-identical to the always-blended run.
- Works at a real internal resolution (`M2VK_RES=1488x1152`) — there is no stipple, so `stipple_div` is
  irrelevant under the option.
- Standalone `OSD=sdl3` still builds.

## Three things worth carrying

- 🚨 **A checkered polygon must not reach the early-Z pipeline and does not**, because P4 step 1's
  predicate already excludes `FLAG_CHECKER`. `PIPE_BLEND` deliberately takes the **general** fragment
  module: a deferred polygon may also carry the translucent texel cutout, so it can `discard` and must
  test late. Giving it the early-Z module would claim pixels in depth it never colours — except that it
  writes no depth, so the symptom would instead be the cutout silently not cutting.
- **The blend flag is latched once per upload (`s_blend_frame`), not read twice.** `geom_upload` decides
  which polygons to defer and `geom_draw` pushes the constant the shader reads; a change landing between
  the two would stipple polygons that had been deferred, i.e. draw them twice, once with a screen door.
- ⚠️ **Two overlapping stippled surfaces are not equivalent between the modes and cannot be.** They share
  the `(x ^ y) & 1` parity, so under the screen door the farther one is drawn *nowhere* — it lands on
  pixels the nearer one already claimed. Blended, it contributes 25 %. That is a property of the feature,
  and it is where the two modes' means could legitimately diverge.

## Not in scope, deliberately

The **textured translucent cutout** (`FLAG_TRANSLUCENT`) is untouched. That is a texel index (15 =
transparent) thresholded at 50 %, and MAME's interpolated alpha lane exists only so a transparent texel
cannot drag colour out of the transparent region. Turning it into real per-texel alpha would antialias
cutout edges — a separate feature, with the same ordering problem, and not what "screen-door
transparency" names.

**GPU cost not measured.** The deferred pass adds draws and blended fragments; on desktop that is inside
the noise for performance.md §2a's reason, and on an Adreno 740 it is a Quest 3 question like every
other item in §4.

# 2026-07-28 — The core aborted on every quit, and the same bug was silently losing NVRAM

Reported as a crash report, not as a symptom: `EXC_CRASH (SIGABRT)`, `abort() called`, thread 0 at
`exit()` → `__cxa_finalize_ranges` → `std::terminate()`, with **thread 5 still alive inside
`libretro_m2_osd_interface::update(bool)` parked on the baton's condition variable**. That pair is the
whole diagnosis: `s_emu_thread` is a namespace-scope `std::thread`, the emulation thread was still
running, and **destroying a joinable `std::thread` calls `std::terminate()`**.

## Why the thread was still joinable — the frontend never unloads the content

`retro_unload_game` already did the right thing (`request_exit()` + `join()`) and was simply not
reached. Closing RetroArch's window on macOS takes AppKit's
`_scheduleCheckForTerminateAfterLastWindowClosed` → `-[NSApplication terminate:]` → **`exit()`**, which
runs the image's destructors without ever calling `retro_unload_game` or `retro_deinit`.

**Reproduced before touching anything**, which is what turned a plausible reading of a stack trace into
a fact: RetroArch launched windowed on `vf2`, then `osascript -e 'tell application "RetroArch" to quit'`
— `libc++abi: terminating` on the console and a fresh `.ips` with the identical signature. The log is
the other half of the proof: it ends at `[NS] stopping draw observer` with **no core-unload line and no
`Content ran for a total of`**, so nothing in the core's shutdown path had run.

⚠️ **The abort is the loud half; the quiet half is worse.** The machine never exits, so MAME writes
**neither NVRAM nor cfg** — every quit taken this way discarded the game's settings and high scores,
and nothing said so. That is why the fix joins the thread rather than just detaching it to dodge the
`terminate()`, which would have made the crash go away and left the data loss in place.

## The fix — `retro_entry.cpp` only, ~50 lines with the reasoning

`shutdown_at_exit()`: if content is loaded, `request_exit()`, wait up to **`SHUTDOWN_WAIT_MS` = 2000**
for the thread to signal it has fallen out of `emu_thread_main` (new `s_emu_finished` atomic), then
`join()` — or `detach()` if it did not, because the thread must not be joinable when its destructor
runs and a **hang at exit is worse than the crash it replaces**: the window is already gone, so there
is nothing for the player to see or dismiss.

🚨 **Registered from `retro_init`, and that is load-bearing rather than convenient.** Destructors and
`atexit` handlers share one list and run last-registered-first. This image's static constructors
register at `dlopen`; registering *later* than that puts the handler **ahead** of their destructors, so
`s_osd`, `s_options` and MAME's own statics are all still alive when the teardown runs. Registering it
at static-init time would invert the order and hand the teardown a half-destroyed image. The `static
const bool` guard is because a frontend may call `retro_init` more than once and the list keeps every
registration.

**No other path changed.** `retro_unload_game` still joins directly; the handler no-ops when
`s_running` is false, which is also what makes a `dlclose` (which finalises the image's handlers) safe.

## Verified

- **The crash is gone.** The identical reproduction produces **no new `.ips`** and the process exits
  rather than hanging.
- **NVRAM is now written on that quit**: `saves/Model 2/model2/nvram/vf2/{eeprom,backup1}` restamped by
  the run. Before the fix they were untouched.
- **Nothing moved.** `ab.sh vf2 2500` reproduces both documented whole-run digests byte-exactly —
  `16af05bb8d02a9a5` software / `55da761fecca5c01` vulkan — plus the background reference identical
  across renderers, coverage agreement 1.0000, 0 real interior disagreements, SSIM covered 0.9963.
- No upstream file, no new file, no shader; **the diff against mame0288 is still 30 lines**.

## Worth carrying

- ⚠️ **`~/Library/Application Support/RetroArch/cores/model2_libretro.dylib` had reverted to a plain
  copy again** (2026-07-28, dated 22:22) and was restored to the symlink. It reverts on its own; check
  it before concluding a fix "does not work" in `~/Desktop/Model 2.app`.
- **The reproduction is scriptable and cheap** — launch, `sleep`, `osascript -e 'tell application
  "RetroArch" to quit'`, then count `~/Library/Logs/DiagnosticReports/*RetroArch*.ips`. Worth reusing
  for anything about shutdown; the crash reports are the only read-out, since the frontend's own log
  simply stops.
- **A frontend is not obliged to unload content before exiting**, and this one does not on the
  commonest quit there is. Anything the core owns that needs an orderly shutdown has to survive
  `exit()` being called out from under it, not just `retro_unload_game`.

---

# 2026-07-28 — The per-game input audit, and the next step assigned: per-game RetroPad layouts

**No code changed.** Two new docs, three index/queue updates. Prompted by "what are the joystick
inputs/mappings for each Model 2 game", which turned into an audit and then into a plan.

## What was done

- **[input-map.md](input-map.md)** — the per-game input map. All 32 port sets, every `PORT_NAME` in
  `model2.cpp` lines 1632–2431, against the RetroPad control that produces it under Classic, Modern
  and Light Gun. Read out of `model2.cpp`, `libretro_m2_input.cpp` and `assignmenthelper.cpp`;
  **nothing was run**, and the claims that are inferences rather than transcriptions say so in place.
- **[per-game-input.md](per-game-input.md)** — the plan that came out of it, with `daytona` as the
  testbed. Six steps. Promoted **ahead of the analog steering curve**, which is now second.
- `user-options.md` §6, `CLAUDE.md`'s "Next step" and its queued list, and the `devnotes/README.md`
  index all updated to match.

## What the audit found

- 🚨 **A live bug: the shoulder-button descriptors are inverted.** `retro_entry.cpp:236-237` tells the
  frontend L = "Button 5", R = "Button 6"; `BUTTON_LAYOUTS[LAYOUT_CLASSIC]` does the opposite. The
  layout table is what runs, so the remap UI mislabels the pair on **both** layouts — on daytona, R is
  really GEAR 4 and L is really VR1. Under Modern it is wrong twice over: button 5 moves to R2 and
  **L does nothing at all**. Nothing else reads `INPUT_DESCRIPTORS`, so no input is lost; it is a
  label lying on the one screen a player consults when a control is missing. Fixed as part of step 1
  of the new plan, since that step is already in the file.
- ⚠️ **Three counts in `user-options.md` §1 are wrong.** It says **83** GAME entries; the tree has
  **90** (62 `MACHINE_NOT_WORKING`, 28 not). Its per-tier sizes and its "not flagged" column are short
  by the same 7. **The 32 port sets and the 6 tiers are right**, so nothing decided on the strength of
  that survey changes — the shape was correct, the arithmetic was not. Corrected in `input-map.md`
  rather than by editing §1, which is cited by section number elsewhere.
- ⚠️ **`desert`'s brake is `IPT_AD_STICK_Y`, not a pedal** (`model2.cpp:1775`) — so it is on left
  stick Y, the same physical control as its steering, and an absolute analog field takes the middle of
  the input range to the middle of the port range, so a centred stick should present it **half
  applied**. [inferred, not measured; `desert` is `MACHINE_NOT_WORKING`.] The cleanest argument in the
  tree for the per-set override table: the right answer is to put it on L2 with the other brakes, and
  only a per-set map can say that.
- ⚠️ **`waverunr` and `topskatr` each declare two ports of one analog type** (Handle + Roll both
  `AD_STICK_X`; Curving + Slide both `AD_STICK_X`), so one stick axis drives both. **Upstream's, not
  ours** — the driver's own comment on each says `// TODO: requires LEFT/RIGHT_AD_STICK in framework`.
- **The right analog stick is idle on 89 of 90 sets.** It is bound to `IPT_AD_STICK_Z` and **no Model
  2 set declares that type**; only `von`'s right twin stick uses the stick at all. That is the free
  resource the two findings above both want.
- ⚠️ **`von`'s face diamond is doing two jobs** — `add_twin_stick_assignments` takes the four face
  buttons as the right stick's digital fallback, and the same four are Buttons 1–4, i.e. every shot
  and dash. [inferred; `von` is `MACHINE_NOT_WORKING` and has never been run.] Also, the driver
  declares **one** player's twin sticks, so port 1's pad does nothing on `von`.
- `dynabb`'s asymmetric bats (P1 on pad 1's R2, P2 on pad 2's L2) re-derived and still **unverified**,
  as `user-options.md` §1.3 left them.

## Worth carrying

- 🚨 **`daytona_gearbox_r` LATCHES** (`model2.cpp:1610-1625`). The five `GEARS` bits are "select gear
  i", not "hold gear i": any set bit latches `m_gearsel`, and a read with nothing pressed returns the
  last gear. **This is the fact the whole layout design rests on** — a sequential shifter becomes a
  *held bit* with no pulse timing to get wrong, and direct-select (ABXY) and shift-up/down (L/R) can
  both be live simultaneously because both are writes to one index. **Do not build a pulse.**
- **The collision was never fixable by moving something, and now it is clear why.** VR2/VR3 are
  `IPT_BUTTON7`/`8`, and `configure()` welds those to the L2/R2 trigger thresholds
  (`libretro_m2_input.cpp:374-386`) — they are not layout-table entries, so **no layout can reach
  them**. `NUMBERED_BUTTONS` 6 → 9 is the whole fix: nine ordinary slots, with the trigger threshold
  demoted to just another *source* a row may name.
- **daytona has a completely free D-pad.** It declares no `IPT_JOYSTICK_*` anywhere — IN1's stick bits
  are the gearbox and VR4, IN2 is entirely `IPT_UNUSED`. Four inputs of headroom no other design gets,
  and where VR1–4 go. ⚠️ **Only safe on sets with no joystick**: the D-pad slots keep their
  `IPT_JOYSTICK_*` assignments, so a row naming a D-pad control as a numbered-button source is a
  double press on any set that declares one. `daytona` and `srallyc` both qualify; check per row.
- ⚠️ **MAME's gear numbering is offset by one** — `GEAR N` is `IPT_BUTTON1`, so `GEAR 1` is
  `IPT_BUTTON2`. The map is written in the player's numbering (B = "gear 1"), which means the row
  points B at MAME button 2. First row in the tree where B is not button 1; expect it to read as an
  off-by-one to anyone checking against `input-map.md` §1.1.
- **`ITEM_ID_BUTTON12` is safe for the diagnostic combo** once the numbered buttons take 1–9 and L3/R3
  take 10/11. The existing argument survives verbatim: `IPT_BUTTON12`'s player-1 default in
  `inpttype.ipp:45` is `KEYCODE_COMMA`, a keyboard code, and this OSD registers no keyboard. Player
  2's defaults for buttons 9–16 are all empty sequences.
- ⚠️ **`IPT_BUTTON9` → R3 is the one binding in the refactor that is currently *verified working***
  (daytona's VR4, measured in a real race twice) and it is the one the widening deletes. The step-1
  no-op check has to name it specifically.
- **The exit criterion is measured in daytona's own INPUT TEST screen**, with `RetroPad (Classic)` as
  the negative control — lightgun step 6's lesson applied before it costs anything: a consumed input
  is invisible to every other read-out, and "VR3 stopped firing" is otherwise equally consistent with
  the button having been dropped on the floor by `add_assignment()`.

---

# 2026-07-28 — `model2_flat_luma`: "No Lighting", the sixth core option

Asked for directly ("turns off the Model 2's lighting effects, so you just get the texture and
tint"). It is `user-options.md` §5's `model2_flat_luma` row, built as that row scoped it. **Four
files, all `src/osd/libretro_m2/`; no new file, no shader, and NO UPSTREAM LINE — the diff against
mame0288 is still 30.** Live from the options menu, acts on both renderers.

## What it does, and why that is the whole of "lighting off"

Model 2's lighting is real — `luminance = clamp(|dot(normal, light)| * diffuse + ambient, 0, 255)`
per face, `model2_v.cpp:1113-1119` — but the copro emulation has **already collapsed it to one 8-bit
number per polygon** before anything reaches the seam. So there is no lighting stage here to switch
off, and flattening that number is not an approximation of the feature, it *is* the feature. Hence
the key being named for the number. The option a player reads is called **No Lighting**, which is
accurate at the level a player cares about; the two names disagreeing is deliberate.

`m2vk::FLAT_LUMA` is **0xff**, and full scale rather than a taste decision is what makes the result
"texture and tint":

- **untextured** — `luma >> 2` is `0x3f`, the top entry of the `colorxlat` ramp the polygon's
  `palcolor` selects, so the polygon comes out its own colour at full strength.
- **textured** — `lumaram[lumabase + (t >> 1)] * luma / 256` loses its second factor, leaving the
  texture's own luma translation, i.e. the texel and nothing else.

⚠️ **It is a multiply by 255/256, not by 1, and an 8-bit field cannot do better.** A texel
translating to the very top of the ramp lands one of 64 entries low, at the extreme only. The
alternative is special-casing the multiply in two rasterisers to make one value mean something the
field cannot represent, which is a great deal more than the defect is worth. Written down so it is
not rediscovered as an off-by-one.

## The implementation is one write at the seam, and that is why it costs no upstream line

🚨 **The two renderers read the luma from DIFFERENT places** — MAME's rasteriser takes
`object.luma` out of `m2_poly_extra_data` (`model2rd.ipp:62,333`), the record copies `p.luma` off the
polygon — so neither one alone reaches both. The seam already had both in its hands:
`model2_3d_render` fills `extra` in immediately above the call and passes it, and **it was already
non-const at the call site**. Changing `submit()`'s parameter from `Extra const &` to `Extra &` and
writing `extra.luma` there covers the software path; `p.luma = luma` covers the record. One
assignment, both renderers, zero lines in `model2_v.cpp`.

The write sits **above the `active()` test**, with `force_solid` and `opaque_only`, for the same
reason they do: the software rasteriser has to obey it when nothing is recording at all. That is what
"acts on both renderers" means, and it is the difference between an option and a picture only one
path draws.

## Verified

- 🚨 **Default `off` is a proven no-op, not an argued one.** `ab.sh vf2 2500` reproduces
  `ab-baselines.md` byte-exactly — software `16af05bb8d02a9a5`, vulkan `55da761fecca5c01` — and
  SSIM covered 0.996300 against the documented 0.9963. The 1300-frame no-option digest is
  `3452ad5414a1b0b9`, the same value `model2_flat_shading`'s proof used.
- **It acts on both renderers**, which is the standing rule for anything that removes a feature.
  `MODE="M2VK_FLAT_LUMA=1" ab.sh vf2 2500` moves **both** digests (`cd65f030c24d2ec3` /
  `1f9e4d8908dc7de5`) and the two paths still agree with each other: coverage agreement 1.0000, **0
  real interior disagreements**, exit criterion 1 holds, SSIM covered 0.995773. A Vulkan-only
  implementation would have scored far worse here, which is what makes this the check that matters.
- **The switch overrides the option in BOTH directions**, by digest (vf2, 1300, vulkan):
  `option=on` **=** `M2VK_FLAT_LUMA=1` **= `f4d76132fefc0935`** (so the option is not a second
  implementation — both go through the one global); `option=on` + `M2VK_FLAT_LUMA=0` **=** no options
  at all **= `3452ad5414a1b0b9`** (the switch turns it off against an option asking for it on); and
  `option=off` + `M2VK_FLAT_LUMA=1` fires. The override notice logs.
- Standalone `OSD=sdl3` still builds — it compiles `m2vk_sink.cpp`, so the non-const `Extra` is
  checked there too.

## Screenshots — `2026-07-28-nolight-<game>-{off,on}.png`, four pairs

Every mean shift is **positive**, which is the only direction flattening to full scale can go:

| game | px changed | mean RGB shift | what it shows |
|---|---|---|---|
| `daytona` | 90145 (47.3 %) | +42.6 +43.2 +43.5 | the largest change of the four — road, trees and barriers all lit by one term |
| `vf2` | 22038 (11.6 %) | +28.4 +33.4 +28.8 | **the pair that makes the point.** Sarah's gi and Lau's jacket keep their texture detail and colour and lose the directional shading; the lantern goes dark red → bright orange |
| `desert` | 13456 (7.1 %) | +29.0 +20.6 +12.0 | the tank's shaded faces flatten and the hull markings become readable; terrain crack texture retained |
| `srallyc` | **274 (0.1 %)** | +61.8 +60.6 +60.1 | **the limitation shot.** srallyc@2500 is nearly all 2D tilemap, and the option only touches 3D polygons — so a frame like this barely moves. Included so the option is not oversold |

⚠️ **`srallyc@2500` is a championship-standings text card**, which is gotcha 5 in miniature: it looked
like a null result at a glance and is not one. Kept deliberately rather than re-shot at an in-race
frame, because "does nothing on a menu screen" is a true and useful thing for the record to hold.

---

## 2026-07-29 — Savestates: planned, built, partly working

Asked for a plan to get savestates working; wrote [savestates.md](savestates.md), then built it.
**`vf2` and `daytona` pass; `vcop2` and `vstriker` still diverge.** Nothing committed. §9 of that file
is the as-built record; this is the session log.

**P1's reason for deferring did not gate what it was thought to gate.** "No Model 2 set carries
`MACHINE_SUPPORTS_SAVE`" is true, but the flag drives a UI warning (`save.cpp:105`), the `-autosave`
load (`machine.cpp:250`) and a `fatalerror` on execute devices that register nothing
(`device.cpp:555`) — **`do_write`/`do_read` have no `supported()` check**. So `write_buffer` /
`read_buffer` ran on this driver already; the open question was completeness, which is answerable by
audit. Also free: every RAM block is auto-registered (`emumem.cpp:322`), and the 32-byte header
carries the game name plus a CRC32 of the registry structure, so mismatched states are *refused*.

**Scope, decided by measurement:** all 28 non-`MACHINE_NOT_WORKING` sets are 2O/2A/2B. The worst device
gap in the tree — `mb86235` (TGPx4), one `save_item` — is **Model 2C only**, so no working set touches
it.

🚨 **The finding of the day, and the plan did not predict it: the emulated state can be restored
perfectly and the picture still be permanently wrong.** daytona's failure was not a missing
`save_item`. `M2VK_SAVE_DIFF` between two states at the same emulated frame showed **5 of 5397 entries,
9 bytes** differing, none of it hardware (two `m_bank_count` bytes and the *Lua engine's* timer) — while
the pictures never reconverged. Two display caches are never invalidated on load:
`device_gfx_interface::interface_post_load()` early-returns because `segas24_tile_device` uses
`set_gfx()` rather than a gfxdecodeinfo table, and `tilemap_t::postload()` calls `mappings_update()`
but not `mark_all_dirty()`. **Fixed from our side at zero upstream cost** — both APIs are public.
Generalised lesson: *"the state round-trips" and "the machine behaves the same" are different claims,
and only the second matters.*

**The instruments are most of the value.** `devnotes/state.sh` (five runs; loads a state from a
*different* machine history and requires both `C == D` and `N != D` — 🚨 without the negative control
the obvious test passes with a `retro_unserialize` that restores nothing); `M2VK_SAVE_DIFF=<file>`,
which names the registry entries whose bytes differ and turned an unbounded guess-and-rebuild loop
into a read-out; `M2VK_SAVE_LOG=1`; and `retrohost`'s `SAVE_AT`/`LOAD_AT`/`ROUNDTRIP_AT`/`DIGEST_FROM`.

**Things that cost time and would otherwise be rediscovered:**
- ⚠️ **`update()` is reached twice before the save registry closes** (15 → 26 → 4319 entries on vf2).
  A size answered from the first frame is **189 bytes** and a frontend caches it for the session.
  `state_size()` refuses to cache while `save_manager::registration_allowed()` is true — the exact
  condition, not a proxy — and `retro_load_game` now spins frames until it is false.
- ⚠️ **`state.sh`'s save dirs were keyed on the run tag but not the GAME**, so three games sharing one
  output directory inherited each other's NVRAM: **three false FAILs**, indistinguishable from an
  incomplete registry. CLAUDE.md gotcha 7, quoted in that script's own header and got wrong anyway.
- ⚠️ **macOS bash 3.2 errors on empty-array expansion under `set -u`**, so `VKFLAG=()` +
  `"${VKFLAG[@]}"` aborts every software-path run.
- The `MACHINE_SUPPORTS_SAVE` probe (temporarily flagging vf2/daytona/vstriker) came back **clean** —
  no execute device registers nothing on 2O/2A/2B, and state sizes were byte-identical with the flag
  set. Reverted; it is a re-runnable probe, not a change.

**Upstream diff 30 → 112 lines**, 20 registrations across three `#ifdef M2VK` sites, all additive.
Largest omission was `m_fbvramA`/`m_fbvramB`, 512 KB each of ordinary CPU-mapped memory. ⚠️ About half
the 112 is comment and wants trimming — with no upstream push (decided today) the lines are permanent.
**`ab.sh vf2 2500` reproduces `16af05bb8d02a9a5` / `55da761fecca5c01` byte-exactly**, so two upstream
files changed and no pixel moved.

⚠️ **Not done:** vcop2 diverges in **230 entries / 180 KB** including the i960's `m_IP`, `m_r`,
`m_rcache` and the whole 68000 — a real machine divergence, perfectly reproducible across four sweeps,
so deterministic rather than a save-point race; `copro_fifo_in/m_full_triggered` is in the diff and the
FIFO's `INPUT_LINE_HALT` handshake is the leading suspect. vstriker undiagnosed. `lastbrnx`/`srallyc`
never measured. No RetroArch interactive check. **Next: bisect vcop2 with `M2VK_SAVE_DIFF` at load
N / diff N+1 to name the first entry to diverge.**

---

## 2026-07-29 (later) — Savestates: two more registry gaps found, `vstriker` fixed, `vcop2` narrowed

Picked up §9.6's assigned next step — bisect `vcop2` with `M2VK_SAVE_DIFF` at load N / diff N+1. The
bisect worked and gave an answer the plan did not anticipate: **at N+1 the whole machine has already
diverged** (72 of 4319 entries, all three CPUs), so a one-frame gap is not fine-grained enough to name
a first mover. The session's value is in what that forced — a new instrument that asks a *different*
question, four hypotheses killed by measurement, two genuine registry gaps found by audit, and one of
the two failing fixtures fixed.

**Nothing is committed.** `m2vk_savestate.{h,cpp}` are still untracked; `model2.cpp` gained two more
`#ifdef M2VK` registrations.

### The instrument that mattered: `M2VK_SAVE_VERIFY=1`

The N+1 diff conflates two failures — *the load did not restore X* and *the machine then ran one frame
differently* — and only the first is actionable. So `state_load` now serialises straight back out after
`read_buffer` and diffs against the bytes it was handed. **It is not the tautology it looks like**:
`read_buffer` runs `dispatch_postload`, whose device callbacks recompute derived fields, so an entry
can legitimately come back different.

🚨 **The answer was "the load is faithful", and that is what redirected the whole session.** On all four
fixtures the only entries that move are **timer `m_index`** and the Lua engine's own timer. So the
divergence is *not* in the registry, and every "find the missing `save_item` in the CPU" instinct was
pointed at the wrong place.

**The `m_index` churn is benign, and the proof is a fixture comparison rather than an argument**:
`vf2`, which passes the full divergence test, shows the identical churn. Mechanism, for the record —
`device_scheduler::presave()` (`schedule.cpp:676`) renumbers `m_index` by position in the active timer
list every time a state is written, and `postload()` re-sorts by `(expire, m_index)`; the numbering is
regenerated, not restored.

Two more instruments, both cheap and both reusable: **`M2VK_SAVE_DUMP=<substr>`** prints the serialised
bytes of matching entries (the diff says *which* entry, this says *what is in it*), and
**`M2VK_SAVE_DIFF_MAX=<n>`** raises the 40-entry print cap — §9.6 item 6, and it was needed within the
hour, because **timer entries sort late and were invisible under the default**.

### Four hypotheses killed by measurement

Each of these was plausible enough to have been "fixed" on argument alone. None survived a probe.

- 🚨 **Live anonymous timers — the strongest a-priori candidate, and clean.** `emu_timer::register_save`
  is only called when `!m_temporary` (`schedule.cpp:96`) and `device_scheduler::postload()` **deletes
  every temporary timer** (`schedule.cpp:705`), so a state taken while a one-shot is outstanding
  silently drops a scheduled event. MAME guards this with `can_save()` and **defers** rather than
  refuses (`machine.cpp:889` returns without cancelling). We have no defer — `retro_serialize` must
  answer now. Probed on all four fixtures: **no live anonymous timers at the save point.** The check is
  now permanent and reports rather than gates.
- 🚨 **FIFO contents, and upstream says so in a comment**: `gen_fifo.cpp:54` registers only
  `m_empty_triggered`/`m_full_triggered` and reads *"This is not saving the fifo, let's hope it's
  empty..."*. `m_values`/`m_extra_values` are in no state file MAME has ever written, and on Model 2
  those FIFOs carry the geometry command stream. Probed: **both FIFOs empty on all four fixtures.**
  That is not luck — the libretro save point is the parked frame boundary *after* the frame's polygons
  were consumed (§1.7), so it is structurally a good place to serialise. Also now a permanent report.
- **The i960's `m_stall_state.iswriteop` is a real unregistered field** — `i960.cpp:2323-2327` saves the
  other five and this one decides read-versus-write on stall resume (`i960.cpp:698`). Dumped it:
  `burst_mode == 0` and `m_stalled == 0` on all four fixtures, so it is a live hazard for an arbitrary
  save point and **not this bug**.
- **The 315-5881 protection chip has nine unregistered members** (`first_read`, `buffer_bit`,
  `buffer_bit2`, `buffer2`, `buffer2a`, `block_size`, `block_pos`, `block_numlines`,
  `done_compression`). Dumped: **vcop2 has no such device.** Irrelevant here, live for protected sets.

### The two real gaps, found by audit rather than by guessing

Cross-referencing every scalar member of `model2.h` against every `save_item`/`save_pointer` in
`model2.cpp` + `model2_v.cpp` leaves exactly **four** names, and two of them are genuine:

- 🚨 **`m_timerorig[4]` — the reload value behind the four hardware timers.** `m_timervals` and
  `m_timerrun` are registered upstream and this is not, which is the worst of the three to miss:
  `timers_r` recomputes `m_timervals = m_timerorig - elapsed` on **every read** (`model2.cpp:107`), so
  a machine that loads a state keeps its own reload values and hands the game a wrong countdown from
  the first poll onwards. **This is what fixes `vstriker`.**
- **`m_copro_atan_base[4]`** — the fourth TGP table base, missed when the other three went in. Not only
  a table pointer: `copro_atan_base_w` drives the TGP's `gpio0` line from a comparison of slots 0 and 1
  (`model2.cpp:594`), so a stale copy feeds the copro a wrong input *bit* as well as a wrong lookup.

The other two names are correct as they stand: `m_gamma_table` is registered by `save_pointer`
(`model2_v.cpp:2458` — the first pass of this audit grepped only `save_item` and produced a false
positive), and `m_xoffs`/`m_yoffs` are `model2_renderer`'s private copy of the CRTC offsets, which §9.5
excludes on purpose. `raster_state` and `geo_state` audit clean against their own registration sites,
the three absentees being §9.5's deliberate ones (`poly_list`, `poly_sorted_list`, `clip_plane`).

### Where `vcop2` now stands: structural, non-registry, and not yet named

- **It fails at every save point tried** — 700, 900, 1100, 1300, 1500, 1700. So it is not a transient
  coincidence at one instant, which is what the "save-point race" framing in §9.6 half-suspected.
- **A coin alone is enough.** Reducing the dirtying script from five events to `600:select:20` does not
  make it pass, so nothing about starting a game is required.
- **The registry is faithful across the load** (`M2VK_SAVE_VERIFY`), and the driver's registry is now
  complete by audit. Therefore the carrier is **outside the registry**.
- **The leading candidate is now the save POINT, not the save CONTENT.** `osd().update()` runs inside
  `screen_device::vblank_begin`, a `TIMER_CALLBACK_MEMBER` (`screen.cpp:1679`), i.e. inside
  `device_scheduler::execute_timers()`. MAME never saves or loads there — `handle_saveload()` is called
  from the scheduler loop *between* timeslices (`machine.cpp:358`). So `postload()` relinks and re-sorts
  the entire timer list while `execute_timers()` (`schedule.cpp:963`) still holds a reference to the
  executing timer and may then call `schedule_next_period()` on it. **This is characterised, not
  demonstrated** — the vblank timer re-`adjust`s itself at the end of the callback, which sets
  `m_callback_timer_modified` and would suppress exactly that. Next session's job.

### Two measurement gotchas, both of which produced a false result first

- 🚨 **Running `state.sh` on several games concurrently is not trustworthy.** Four backgrounded
  invocations reported `vf2` FAIL; run sequentially it passes, twice, with identical digests in all
  five slots. Each run already has its own `M2_SAVE_DIR`, so this is not gotcha 7 — it is something
  about wall-clock or contention leaking into the run. **Sweep sequentially.** A parallel sweep is how
  this session nearly recorded a regression in the one fixture that was already known good.
- ⚠️ **`Sega 315-5338A I/O Controller/:billboard:io/0/m_cmd` is run-to-run nondeterministic on vcop2.**
  Two clean runs with **no savestate activity at all** differ in that byte and in nothing else, with
  identical digests. It appears in every vcop2 diff and means nothing. Establish the no-op control
  before reading a one-byte difference as a finding.

### The sweep: 8 fixtures, 4 pass, and the failures point at ONE bug

Sequential (see the gotcha above), 2000 frames, save point 1500.

| ✅ PASS | ❌ FAIL |
| --- | --- |
| `vf2` (2A), `daytona` (2O), `vstriker` (2B), `sgt24h` (2B) | `vcop2` (2A), `srallyc` (2A), `desert` (2O), `lastbrnx` (2B) |

🚨 **Two readings matter more than the 4/4.**

1. **Not board-specific.** Every one of 2O, 2A and 2B has both a pass and a fail. Any explanation that
   turns on the copro type — which is where the previous session's suspicion sat, via the FIFO's
   `INPUT_LINE_HALT` handshake — is refuted before it is tested.
2. **All four failures share one signature.** `M2VK_SAVE_VERIFY` on `srallyc`, `desert` and `lastbrnx`
   reports exactly what it reports on `vcop2`: **nothing** outside timer `m_index` and the Lua engine's
   timer. Four faithful loads, four divergent machines. That is one bug affecting four fixtures rather
   than four missing registrations, and it is why the next step is to instrument the save *point* and
   not to keep auditing for `save_item`s.

⚠️ **`lastbrnx` is on CLAUDE.md's frame-parity bistable list**, so its FAIL alone is not evidence —
though its verify signature matching the other three is.

### Regression: the A/B no-op guard passes, and it had to

Two upstream files are modified, so this is not optional. `ab.sh vf2 2500` reproduces all three
documented digests **byte-exactly** — background `6b831e519ff46d42` identical across renderers, software
`16af05bb8d02a9a5`, vulkan `55da761fecca5c01` — with exit criterion 1 holding, coverage agreement
1.0000, 0 real interior disagreements and SSIM covered 0.996300. **No pixel moved.**

**Upstream diff is 93 lines**, measured (`git diff --stat` on `model2.cpp` + `model2_v.cpp`, the only
upstream files this fork touches). ⚠️ **`savestates.md` §9.5's "112" does not reproduce and has been
corrected** — it was ~79 when that was written and is 93 after this session's 14 lines. Count it; do not
carry the figure forward.

### 🚨 Discovered, PRE-EXISTING, and deliberately not fixed: `OSD=sdl3` does not link

Running the standard "standalone still builds" check found it already broken:

```
"m2vk::layer_end(int)", referenced from:
      model2_state::screen_update(...) in libmame_model2.a[14](model2_v.o)
ld: symbol(s) not found for architecture arm64
```

**Not caused by this session.** The reference is emitted from **`m2vk_sink.h:348`**, inline code pulled
into `model2_v.cpp`; `layer_end` is defined in `m2vk_frame.cpp`, which **only the libretro OSD
compiles**. This session's only upstream edit is 14 lines of `save_item(...)` in `model2.cpp`, which
cannot produce that reference — and `model2_v.cpp` (10:40) and `m2vk_sink.h` (2026-07-28) both predate
it.

**The mechanism, which is the part worth keeping:** `scripts/target/mame/model2.lua:82` defines `M2VK`
on the **driver project**, unconditionally, so it is set whatever the OSD is — and both OSD builds share
**one** object directory (`build/osx_clang/obj/x64/Release/`, a single `model2_v.o`; only
`build/projects/` is split per OSD). So the sdl3 link always pulls an object that calls into `m2vk::`
and never gets the definitions. It has been latent since the seam header grew out-of-line calls, i.e.
somewhere in P3 — **CLAUDE.md's repeated "standalone `OSD=sdl3` still builds" is stale**, and the check
evidently passed on a tree whose shared object predated that.

**Left alone on purpose**, because the fix is a scope decision rather than a bug fix: either stub the
`m2vk::` entry points for non-libretro OSDs, or make the `M2VK` define conditional on the OSD in
`model2.lua`. Neither belongs inside the savestate work. ⚠️ **What it does mean is that "the standalone
still builds" cannot be used as a regression check until it is decided** — do not treat a failure there
as evidence about a savestate change.

## 2026-07-29 (third session) — Savestates: the save-POINT hypothesis is DEAD, and the read-out that was going to be used to confirm it does not discriminate

The one task assigned to this session was a **measurement, not a fix**: prove or kill the theory that
our save point corrupts the scheduler, because `osd().update()` runs inside
`screen_device::vblank_begin` — a `TIMER_CALLBACK_MEMBER` (`screen.cpp:1679`), i.e. inside
`device_scheduler::execute_timers()` — while MAME itself only ever saves or loads *between* timeslices
(`machine.cpp:358`). **It is killed, three ways.** No code changed; nothing was committed.

### The mechanism is real, and it is genuinely self-defending

Reading it out properly first, because the ordering is what decides it. `vblank_begin`:

1. `machine().video().frame_update()` — **this is where our load happens**
2. the screen callbacks
3. `m_vblank_begin_timer->adjust(time_until_vblank_start())`
4. `m_vblank_end_timer->adjust(time_until_vblank_end())`

So the load lands **before** the timer re-adjust, and `adjust()` sets `m_callback_timer_modified = true`
when the timer being adjusted is the callback timer (`schedule.cpp:142`). `execute_timers()` then skips
`schedule_next_period()` (`schedule.cpp:964`). **The prompt's counter-evidence was right**: the
double-advance this theory predicts is suppressed by the driver of the theory itself.

Two further hazards the read turned up, both of which then measured clean:

- **`m_basetime` IS in the save registry** (`schedule.cpp:305`, as `global/0/m_basetime.{seconds,attoseconds}`).
  So `read_buffer` overwrites the exact variable `execute_timers()`'s own loop condition
  (`while (m_timer_list->m_expire <= m_basetime)`) is testing, mid-loop, with a whole rebuilt timer list
  under it.
- **`adjust()` does `m_start = m_scheduler->time()`, and `time()` returns `m_callback_timer_expire_time`
  when inside a callback** (`schedule.cpp:332`) — a *pre-load* copy that `read_buffer` never restores.
  So steps 3 and 4 above re-arm the screen timers off the pre-load timeline while every other timer
  carries post-load values.

### The measurements

The instrument is **differential against the correct future**, which is stronger than the prompt's
"compare against what the state said": run C loads the dirty state at N and saves at N+1 with
`M2VK_SAVE_DIFF` pointed at a *dirty reference run's* own state at N+1, `M2VK_SAVE_DIFF_MAX=1000000`
so the timer entries are visible at all (they sort late; that cap is why nobody had ever seen them).

1. **No timer's `m_expire` or `m_period` moved anywhere on vcop2.** The only timer entries differing are
   `m_start` on three FIFO sync timers and the known-benign lua engine one. A relinked or spuriously
   rescheduled timer list carries its offset forward — a periodic timer that gained a period stays a
   period ahead. **Nothing is offset.**
2. **`m_basetime` is identical one frame after the load.**
3. **The two histories are time-aligned at the save point** — diffing the clean and dirty states both
   taken at frame 1500 shows 529 of 4321 entries differing and `m_basetime` *not* among them. So the
   pre/post-load `machine().time()` confusion in `vblank_begin`'s tail is a **no-op here**, because the
   two timelines it mixes hold the same value.

🛑 **Do not move the save point.** The pause/resume refactor sketched in §9.6 — parking the emulation
thread at `machine.cpp:346-355` instead — is not justified by anything measured, and it moves the
load-bearing piece of the OSD threading model (the same baton the polygon tap and the input snapshot
rest on) to buy a theory that is now dead.

### 🚨 The read-out that was going to confirm it does not discriminate — the control caught it

`vcop2` (**FAIL**) differs from its reference in **73 of 4321 entries** one frame after the load. So
does `vf2` (**PASS**): **73 of 4321.** vf2 even has a timer whose `m_expire` moved
(`scsp_device::timerB_cb`) where vcop2 has none, and passes anyway.

**"Entries differ at N+1" is normal, passing behaviour.** Had the control not been run, vcop2's 73 and
its three drifting `m_start`s would have read as the confirmation the session was sent to find. This is
another instance of the standing rule — *when a new instrument's first output contradicts something
already written down, suspect the instrument* — and of the reload-negative-control lesson from
`lightgun.md` §7.6: the obvious two-run version of this test fits a false conclusion perfectly.

**What does discriminate is *which subsystem* diverges**, and it is clean:

| | entries differing at N+1 | where |
| --- | --- | --- |
| `vf2` (PASS) | 73 | **audio only** — MC68000 `audiocpu`, SCSP, `soundram`, sound stream, SCSP timers |
| `vcop2` (FAIL) | 73 | **the main path** — i960 (`m_IP`, `m_PIP`, `m_r`, `m_rcache*`, `m_localtime`, `m_totalcycles`, `m_stalled`, `m_suspend`, `m_nextsuspend`, `m_input.m_curstate`), the TGP's ALU registers + its data RAM (`0-ff`, `200-3ff`), `copro_fifo_out/m_empty_triggered`, `workram`, `bufferram`, and the driver's `m_geo_read_start_address` / `m_geo_write_start_address` / `m_copro_sincos_base` / `m_timervals[0]` |

vf2's delta is entirely in the sound path, which the picture digest never sees — which is *why* it
passes with 73 entries differing. **Use the subsystem, never the count.**

**The divergence grows monotonically and never reconverges** (vcop2, entries / bytes at N+k):
`k=1` 73 / 3427 · `k=2` 82 / 6680 · `k=5` 88 / 15683 · `k=20` 125 / 63489 · `k=100` 194 / 515556.
For scale, the clean and dirty machines differ in 529 entries at N, so the load transfers ~86 % of the
gap and then loses ground.

### Four more candidates closed, so they are not rediscovered

- 🚨 **`mb86233::m_stall` (`mb86233.h:107`) is genuinely unregistered** — every other member on lines
  101–105 is in `save_item`, this one is not. **It is not this bug**: it is a within-instruction
  transient, set by `stall()` from the FIFO's read-empty callback and consumed in the same
  `execute_run` iteration at `do_stall:` (`mb86233.cpp:1223-1225`, `m_pc = m_ppc; m_stall = false;`),
  and no device is inside `execute_run` at the save point. Exactly the shape of the i960's
  `iswriteop`. **Added to §9.6 item 7's list of real-but-not-this hazards.**
- **The driver's four hardware timers are registered** as `timer/timer_device::generic_tick/0..3` and do
  **not** drift across a load. So `m_timers[offset]->elapsed()` agrees, `timers_r` (`model2.cpp:107`)
  computes the same countdown, and **`m_timervals[0]` in the N+1 diff is a consequence, not a cause** —
  the other half of the `m_timerorig` fix from the previous session, and it is already correct.
- **Texture RAM is registered** — `memory/:maincpu/0/:textureram0` and `textureram1`, 2 MB each, the two
  largest entries in the registry. Not a gap.
- **The halt handshake is in the same state in both fixtures at the save point.** Dumped: TGP
  `m_suspend = m_nextsuspend = 0x01` (SUSPEND_REASON_HALT), maincpu and audiocpu `0x00`,
  `copro_fifo_in/m_empty_triggered = 01`, `copro_fifo_out = 00` — **byte-identical on vf2 and vcop2**.
  So vcop2 is not being saved in some special halt configuration that vf2 avoids.

### Where the next session should start

The carrier is **unregistered state that (a) differs between the clean and dirty machines at frame N and
(b) reaches the i960/TGP path within one frame** — because everything *registered* round-trips faithfully
(`M2VK_SAVE_VERIFY`, previous session) and the scheduler is now cleared. The most causally upstream
entries in vcop2's N+1 set are the i960's `m_suspend`/`m_nextsuspend`/`m_stalled`/`m_input.m_curstate`
and `copro_fifo_out/m_empty_triggered` — i.e. the maincpu↔copro HALT handshake. ⚠️ **§9.6 records that
handshake as "tested and wrong on both counts", but what was tested was FIFO *emptiness* and live
anonymous timers — not whether the halt-line, suspend and `m_*_triggered` trio stay mutually consistent
across a `postload` that re-runs none of the FIFO's edge callbacks.** That is not the same question and
it is still open.

⚠️ **The upstream diff is 123 lines, not the 93 recorded in §9.5** — `git diff --stat mame0288` gives
37 in `model2.cpp` + 86 in `model2_v.cpp`. §9.5's own instruction applies to itself: count it, do not
carry it forward.

## 2026-07-29 (fourth session) — Savestates: the reference future was never reproducible, and the cause is ours

The assigned start was the maincpu↔copro HALT handshake. It is implicated, but not the way the
handoff predicted, and getting there went through a finding that invalidates a lot of earlier
measurement: **`state.sh`'s `D` — the dirty history whose future `C` had to reproduce — was not
reproducible run to run, and nothing had ever checked.** Two code changes came out of it, both
`src/osd/libretro_m2/`, no upstream file.

### How it was found: the same state, loaded at different frames

The first experiment was meant to separate "the transfer loses something" from "the receiving machine
contributes something": load one `vcop2` state at several host frames and compare the futures. It
does not usually matter when you load, because after a load the machine is supposed to be a function
of the state alone.

| load at host frame | 1500 | 1501 | 1502 | 1503 | 1520 | 1550 | **1574** | **1575** | 1600 | 1650 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| digest | X | Y | X | Y | Y | Y | **= D** | **= D** | Y | Y |

Three repeats of each: deterministic. **Loading at 1574–1576 reproduced the dirty run's own future
exactly** — the savestate working perfectly — while loading the same bytes at 1500 or 1550 did not.

### 🚨 The negative control nobody had run: `D != D`

Then the boring run. **Two independent dirty `vcop2` runs, with no savestate activity at all, differ
in 133 of 599 frames.** Five runs partition into exactly two branches (`{D2,D4}` against
`{D3,D5,D6}`). So `vcop2` is bistable, it was not on CLAUDE.md's bistable list, and **`C != D` had
been carrying no information about the savestate at all** — `C` was being asked to reproduce whichever
of two futures `D` happened to take that time.

Diffing two `D`-branch state files at frame 1500 named the carrier immediately:
`Video Screen/:screen/0/m_frame_number` and the driver's `m_framenum` are among the 158 differing
entries. **The two runs are at different EMULATED frames at the same HOST frame.**

### The root cause is a wall-clock measurement inside ROM loading

A counter of `update()` calls against `screen.frame_number()` (new, in the probe below) shows the
offset settling at **−6 or −7**, fixed within a run, a coin flip between runs, and set before the
first vblank:

```
upd=1..5  frame=0  t=0.000000000 phase=1 PAUSED     <- 5 runs
upd=6     frame=0                                   <- the 6th, sometimes
upd=6|7   frame=0  t=0.019024000 phase=3            <- first real frame
```

`romload.cpp:649` calls `mame_ui_manager::set_startup_text(text, force=false)`, and that
(`ui.cpp:916`) calls `machine().video().frame_update()` whenever **more than a tenth of a wall-clock
second** has passed since the last one. So the number of startup pumps is `floor(rom_load_seconds *
10)`. Every one of them reached `osd().update()`, parked on the baton, and **cost `retro_run()` a
whole frame**. Host frame *k* therefore mapped to emulated frame *k−6* or *k−7* depending on how fast
the ROMs loaded that time.

**Fix — `libretro_m2_osd.cpp`, four lines and a long comment:** an `update()` reached before
`machine_phase::RUNNING` returns without parking. Safe against the two things that could deadlock:
`retro_load_game()`'s startup loops drive the machine with `release_frame()`/`wait_for_frame()` and an
early release is discarded because the parking branch clears `m_go` itself under the lock; and
`soft_reset()` (`machine.cpp:967-979`) sets RESET and RUNNING inside one call with no video pumped
between, so no `update()` is ever reached at that phase. EXIT is above RUNNING, so the shutdown path
is untouched.

**Measured after: 8 `vcop2` runs give offset −1 and one identical digest.** Before, the same command
was a coin flip.

⚠️ **This shifts which emulated frames a fixed-length run covers, so every whole-run digest in
`ab-baselines.md` and `res-baselines.md` moves.** They need regenerating with `ab-table.py` /
`res-table.py`. The committed polytap fixture is keyed on *rendered* frame count, which is emulation
side, so it is unaffected.

### `state.sh` had a second, independent design bug

`B` (which wrote the state file) and `D` (which produced the reference digest) were **two separate
runs**, and the whole comparison assumed they were the same machine history. With the game bistable
they can land on opposite branches, at which point no savestate however perfect can pass. **`D` now
saves the state and supplies the reference in one run**, and a new **`E`** re-runs the dirty command
and reports `🚨 NONDETERMINISTIC` — *not* FAIL — when `D != E`, because a coin flip read as a
savestate bug is exactly the failure that produced two sessions of work.

### The eight fixtures, re-verdicted

**5 of 8 pass, up from 4, and — more to the point — all eight verdicts now have a green determinism
control.** `srallyc` flipped to PASS; it was never broken. `lastbrnx` is no longer excluded as
bistable: `D == E` on it now.

| | vf2 | daytona | vstriker | sgt24h | srallyc | vcop2 | desert | lastbrnx |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | ✅ | ✅ | ✅ | ✅ | ✅ **new** | ❌ | ❌ | ❌ |

### The three real failures are the FIFO, in both directions

With the noise gone, the receiver probe finally correlates. Loading one `vcop2` state at eight
consecutive frames gives exactly two outcomes, and they partition **8 of 8** on the receiving
machine's `copro_fifo_in` occupancy:

| load@ | 1500 | 1501 | 1502 | 1503 | 1504 | 1505 | 1506 | 1507 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `copro_fifo_in` | 6 | 6 | 8F | 7 | 7 | 6 | 8F | 8F |
| digest | X | X | Y | Y | Y | X | Y | Y |

And the save side, from the core's own warning across all eight fixtures: **`desert` (4 words) and
`lastbrnx` (6) are the only two whose `copro_fifo_in` is non-empty when the state is taken — and both
fail.** So `gen_fifo.cpp:54`'s *"This is not saving the fifo, let's hope it's empty..."* bites in two
directions at once: words lost on the way out (desert, lastbrnx), and the receiver's stale words left
behind on the way in (vcop2).

⚠️ **This corrects §9.1a's "FIFO contents: both FIFOs empty on all four".** That was true of the
fixtures it looked at and false in general, and the instrument that would have said so — the warning
in `state_save` — was already in the tree and had never been read across the whole set.

### New instruments

- **`M2VK_SAVE_PROBE=<substr>`** — the machine's **live** condition rather than a recording: every
  FIFO's occupancy, every execute device's suspend mask and HALT line, the screen frame number, the
  scheduler time, the phase and the pause flag, plus the live bytes of any registry entry matching the
  substring. 🚨 This is the only way to see the receiver, which is by construction in no state file.
- **`M2VK_SAVE_PROBE_FROM/TO=<frame>`** — the same probe every frame over a window, with no savestate
  activity, so the condition can be traced across the frames where a load works and the frames where it
  does not **in one run**.
- **`M2VK_HOST_FRAME_HASH=<frame>`** (retrohost) — a per-frame picture hash. The whole-run digest
  cannot tell "diverged at k and recovered" from "never recovered", and those are different bugs.
  Align two runs by subtracting each one's load point.

### The fix: a FIFO trailer, and one ordering decision that is load-bearing

`m2vk_savestate.cpp`. `state_size()` now reports MAME's size **plus a fixed slab per
`generic_fifo_u32_device`** (a count and 64 words each; the hardware FIFOs hold 8 and 16), while
`read_buffer`/`write_buffer` are still handed exactly their own size — `s_mame_size`, kept separately
for that reason, because both of their length checks are equalities. `peek()`, `size()`, `clear()` and
`push()` are all public, so this costs **no upstream line**.

On load: **`clear()` first, then replay.** The clear is the half that is easy to leave out and it is
the half `vcop2` needs — it drops the words the *receiving* machine happened to be holding.

🚨 **The FIFO work happens AFTER `read_buffer`, deliberately, and the reason is a trap worth writing
down.** `clear()` and `push()` fire `m_empty_cb`, `m_on_fifo_unfull` and `m_on_fifo_unempty`, all of
which reach `set_input_line()` — which does not act immediately but **enqueues** into
`device_input::m_queue` behind a `scheduler().synchronize()` **temporary** timer. `dispatch_postload`
**deletes every temporary timer** (`schedule.cpp:705`), and `m_qindex` is in no registry. So doing the
FIFO work before `read_buffer` would leave a queue holding a pending event with no timer to drain it —
and `set_state_synced` only arms a new timer when the queue *was* empty (`diexec.cpp:684`), so the HALT
line would stop responding **for the rest of the session**. After `read_buffer` there is no postload
left to run and the events drain on the next timeslice.

**Result: `desert` and `lastbrnx` pass**, with the logs naming the mechanism exactly —
`:copro_fifo_in carried 4 words in the trailer` / `restored 4 words`, and 6 for lastbrnx.

### 7 of 8

| vf2 | daytona | vstriker | sgt24h | srallyc | desert | lastbrnx | vcop2 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |

⚠️ **`vcop2` is the one left and it is now a much sharper question.** Its own FIFO is empty at save, so
nothing is carried; the load drops 6 stale receiver words and **the digest does not move**. So the
8-of-8 occupancy correlation is real but the occupancy is a *proxy* on this fixture. Excluded by
measurement now: the registry (complete by audit, faithful by `M2VK_SAVE_VERIFY`), the save point
(§9.1b), the reference future, and the FIFO contents. The live lead is the halt-line / suspend-mask /
`m_*_triggered` consistency question at the end of §9.1b — asked now against a machine whose FIFOs
agree.

### Three earlier conclusions this corrects, and the shape they share

- **§9.1a: "FIFO contents — both FIFOs empty on all four."** True of what it looked at, false in
  general. The instrument that would have said so was already in `state_save` and had never been read
  across the whole fixture set.
- **§9.1a: "four faithful loads, four divergent machines ⇒ one bug, not four omissions."** The premise
  held; the conclusion did not. 🚨 **The signature was shared because it is also what a PASSING fixture
  looks like** — a faithful load is normal, and `vf2` has it too. A shared signature is evidence of a
  shared cause **only if the passing fixtures do not share it**.
- **§9.1b: "vcop2's N+1 delta is the main path."** Measured at a load point whose receiver held six
  stale FIFO words. At a clean load point the main path is byte-correct one frame later and only SCSP
  and the lua timer differ.

All three are the same species, and it is the species §9.1b already named and then fell to anyway:
**a read-out was believed without running it on a case that should have come out the other way.** §9.1b
caught it once by running the *passing* fixture through the instrument. The rule generalises past
fixtures to whole harnesses — `state.sh` had no control asking whether its own reference was
reproducible, and for two sessions it was not.

### ⚠️ What this leaves for the next session, beyond `vcop2`

**Regenerate `ab-baselines.md` (`ab.sh` + `ab-table.py`) and `res-baselines.md` (`res.sh` +
`res-table.py`).** Both files are flagged stale at the top. Nothing about the renderer changed — a
fixed-length run simply covers different emulated frames now — so the relation to check is software ==
vulkan and the `M2VK_NO_3D=1` background identical across renderers, not the absolute values.
⚠️ **And re-run the five "bistable" fixtures**: that entry is superseded, the mechanism it named
(`draw_framebuffer`'s `frame_number() & 1`) only exists in render test mode and never could have
explained `waverunr`.

### The A/B no-op guard: the relation holds, the absolute numbers moved

`ab.sh vf2 2500`, run at this tree:

```
bg  software  c3aaa56633c1c4f7      bg  vulkan  c3aaa56633c1c4f7
3d  software  9c20f1fac9d9fe92      3d  vulkan  de94f44a06151f71
background reference identical across renderers, last frame and whole-run digest
coverage agreement 1.0000, real interior disagreements 0, exit criterion 1 holds
ssim covered 0.996985 over 107570 px   (ab-baselines.md records 0.9963, on other frames)
```

🚨 **Read the first line, not the digests.** The `M2VK_NO_3D=1` background coming back **identical
across renderers** is what makes every coverage number in `ppmdiff.py` valid, and it still does. The
3d digests are new because the run covers different emulated frames now — nothing about the renderer
changed, and the checks that do not depend on frame indexing (coverage agreement, interior
disagreements, exit criterion 1, SSIM) all reproduce.

### Both baseline files regenerated, and the regeneration is itself the strongest evidence for the fix

42 runs, sequential: 12 A/B fixtures, 20 resolution runs (10 point at 3×, 10 box at 2× and 4×), 5
regression-guard runs. **Every one passed on the first attempt.**

That is the result, not the tables. The *previous* regeneration (2026-07-28) had **4 of 23 resolution
runs fail outright and a fifth pass while measuring nothing**, and the five fixtures blamed for it —
`lastbrnx`, `schamp`, `waverunr`, `dynamcop`, `overrev` — are all in this sweep and all clean.
`lastbrnx` also passed the savestate harness with its own determinism control green. **The
"bistable fixtures" problem is gone because it was never the fixtures.**

| | A/B (12) | resolution point (10) | resolution box (10) | guards (5) |
| --- | --- | --- | --- | --- |
| passed first try | 12 | 10 | 10 | 5 |
| background identical across renderers / at every scale | 12 | 10 | 10 | 5 |
| real interior coverage disagreements | 0 | 0 | 0 | 0 |

**Cross-check between the two harnesses, which had never been possible before**: `covered 1x` in
`res-baselines.md` now agrees with `ab-baselines.md` to the pixel on every fixture. Two independent
harnesses measuring the same 1× frame and landing on the same number is what a stable frame mapping
buys.

**Two claims survived the shift unchanged and both are the ones that should have:**

- **`MODE=M2VK_FORCE_SOLID=2` still scores SSIM 1.0000** on `vcop2` and `dynamcop` (0.9999 srallyc,
  0.9998 vf2), with `same colour` 100.00 % on three of four. That is a statement about the rasterizer,
  depth key, scissor and composite being pixel-identical, and it has nothing to do with which frames a
  run covers — so it should not have moved, and it did not.
- ⚠️ **`vcop2` 154203 and `srallyc` 136116 flat-shaded coverage did not move at all**, across a change
  that moved every other number in both files.

⚠️ **The exceptions, so nobody reads them as reproducible:** `M2VK_SW_3D=1`'s recorded digest
`16af05bb8d02a9a5` is dead — `renderer=software` on vf2 2500 is now `9c20f1fac9d9fe92`. **The
invariant is the equality (`M2VK_SW_3D=1` == `renderer=software`), never the digest.** The
`M2VK_FORCE_SOLID=2` **point-resolve** table in `res-baselines.md` is the one table left at
2026-07-26 numbers, and is marked as such.

### One loose end, deliberately not chased

`schamp`'s post-fix digests are `964db6922c299090` (background) and `b3c2896438f248d0` (3D vulkan) —
and **both are values this tree currently records as anomalies**: the first as "the failing runs" value
from the 2026-07-28 regeneration, the second as P3 step 6's *"anomalous, non-reproducing digest"* that
never came back in eleven runs. Post-fix they are the stable values, on a first-try clean run.

⚠️ **Suggestive, and NOT claimed as explained.** The startup fix changed which emulated frames a
2500-frame run covers, so an exact 64-bit match to a pre-fix digest is not something the current model
accounts for, and "the anomaly was the coin flip" is the comfortable story rather than a measured one.
Recorded here because a future session will find `b3c2896438f248d0` described as an anomaly in
CLAUDE.md and as the normal value in `ab-baselines.md`, and should know the discrepancy is known.

# 2026-07-29 (fifth session) — Savestates: 8 of 8. `vcop2` was the SCSP envelope phase

`vcop2`, the last failing fixture, passes. The carrier was **`SCSP_EG_t::state` — the envelope
generator's phase — which MAME does not register**, and the fix is one `save_item` in
`src/devices/sound/scsp.cpp` plus making the enum nameable from outside the class. All eight fixtures
now pass with all three controls green, on the software path and the Vulkan path, over a 1500-frame
future as well as a 500-frame one.

## The route there, which is the part worth keeping

Everything below came from **one instrument that had never been pointed at this fixture**:
`M2VK_HOST_FRAME_HASH`, the per-frame picture hash. The whole-run digest says *these two runs differ*;
the sequence says *where*. Three facts fell out in the first two runs, and each one killed a standing
belief:

- 🚨 **The load is byte-perfect for 486 frames.** Frames 1501–1986 are identical hash for hash between
  the loaded machine and the dirty reference; the first difference is at **1987**, and from there it
  never recovers. Every previous session had been reasoning about this as a machine that loads wrong.
  It does not — it loads *almost* right and then drifts.
- 🚨 **The save point is not a variable and the dirtying script is.** At save point 1200 the fixture
  **passes outright** (0 of 999 frames differ, negative control 999 of 999). §9.6's "it fails at every
  save point from 700 to 1700" was measured with a fixed script and a whole-run digest, and it read a
  script property as a save-point property.
- 🚨 **One press in the script carries the whole failure, and it is the gun trigger.** Splitting
  `1300:a` from `1400:b`: `a` alone passes, `b` alone fails at 1987. ⚠️ **And `a` passing is not
  evidence of anything** — RetroPad A maps to `IPT_BUTTON2`, which `vcop2` does not declare, so that
  arm of the split was a vacuous press. RetroPad B is `IPT_BUTTON1`, the trigger. **Firing the gun is
  what creates state the save does not carry.**

## Then the registry diff, with the control run first

`M2VK_SAVE_DIFF` between the loaded machine and the dirty reference one frame after the load, with
`M2VK_SAVE_DIFF_MAX=1000000`. 🚨 **The control came first this time** — two independent dirty runs
diffed against each other report **0 of 4321 entries differ, 0 bytes**, so the machine is fully
deterministic and every entry in the real diff is signal. That is §9.1c's rule applied prospectively
rather than after losing two sessions to it.

⚠️ **The first attempt at this diff was wrong and the reason is CLAUDE.md gotcha 7.** Reusing an
`M2_SAVE_DIR` between the two runs put `:eeprom`, `global/m_coin_count`, `memory/:backup1`,
`memory/:workram` and `:copro_tgp/m_totalcycles` in the diff — five entries that look exactly like a
main-path divergence and are entirely NVRAM history. **Fresh save dirs on both sides, every time.**

With that fixed, the diff at N+1 is **34 of 4321 entries, and every one of them is sound**: SCSP DSP
`TEMP`/`EFREG`/`RINGBUF`, eight slots' `EG.volume` / `active` / `cur_addr` / `nxt_addr` / `udata`, the
two sound-stream output buffers, one FIFO `sync_full` timer and the known-benign lua timer. **The
i960, the TGP, workram, the video registers and the tilemaps are byte-identical.**

Scanning the same diff forward — frames 1550, 1700, 1850 — the picture never changes shape: the
audio 68000 and its 512 KB `soundram` join in (205430 of 524288 bytes differ by 1700), the SCSP
timers A/B/C drift, and **nothing outside the sound subsystem ever differs, right up to the frame the
picture changes.** So the question was never "what is missing from the machine state", it was "what is
missing from the SCSP".

## The bug: `EG.state` is not saved, and it is not merely audible

`scsp.cpp`'s `device_start` registers `EG.volume`, `EG.step`, `AR`, `D1R`, `D2R`, `RR`, `DL`,
`EGHOLD` and `LPLINK` — **every field of `SCSP_EG_t` except `state`**, the attack/decay1/decay2/release
phase itself. A loaded state therefore keeps the *receiving* machine's phase for all 32 slots.

Two independent reasons that reaches the game rather than just the speakers:

- **`UpdateSlot` takes a different volume path in `SCSP_ATTACK`** (`scsp.cpp:1250`) — attack applies
  `EG_Update` linearly, everything else goes through `m_EG_TABLE`. So `EG.volume` walks a different
  curve from the first sample, which is exactly the eight slots in the N+1 diff.
- 🚨 **The phase is readable by the sound CPU.** `SGC` in the slot status register is
  `(slot->EG.state) & 3` (`scsp.cpp:924`). The sound driver polls it, branches on it, and writes
  different things to `soundram` — which is precisely the divergence measured at 1550, and the
  mechanism by which a sound-only difference becomes a *picture* difference 487 frames later when the
  main CPU next waits on the sound side.

**Not `Prev`**, the other unregistered slot member: it is written to zero in `StartSlot` and read
nowhere in this tree, so interpolation is dead code here. **Not the LFO `table`/`scale` pointers**
either — `device_post_load` already re-runs `Compute_LFO` for all 32 slots, so upstream handled that
one.

## The fix, and why it is NOT `#ifdef M2VK`

`src/devices/sound/scsp.h`: `enum SCSP_STATE` moves from `private` to `public` so that
`ALLOW_SAVE_TYPE(scsp_device::SCSP_STATE)` after the class can name it — the `ay31015` device is the
precedent. `src/devices/sound/scsp.cpp`: one `save_item(NAME(m_Slots[slot].EG.state), slot)` and a
comment. **+11 lines, −2.**

⚠️ **The mergeability rule says `#ifdef M2VK`, and here that would be inert.** `M2VK` is defined on
the **`mame_model2` driver project only** (`scripts/target/mame/model2.lua:82`); `scsp.cpp` compiles
into `liboptional`, where the define does not exist, so a guarded fix would simply not be compiled.
It is also a plain upstream bug — correct for Model 2, Model 3, Saturn and ST-V alike — so it is left
unguarded deliberately rather than by omission.

## Verified

- **8 of 8 fixtures PASS**, one sequential sweep, 2000 frames / save point 1500: `vf2`, `daytona`,
  `vstriker`, `sgt24h`, `srallyc`, `desert`, `lastbrnx`, `vcop2` — each with `D == E`, `N != D` and
  `A == R` green.
- `vcop2` also passes with a **1500-frame future** (3000/1500) and **through the Vulkan path**
  (`VK=1`), so the fix is not a 500-frame accident and does not depend on the renderer.
- **The A/B no-op guard reproduces `ab-baselines.md` byte-exactly**: background `c3aaa56633c1c4f7`
  identical across renderers, software `9c20f1fac9d9fe92`, vulkan `de94f44a06151f71`, coverage
  agreement 1.0000, real interior disagreements 0, SSIM covered 0.996985. A `save_item` cannot move a
  pixel, and it did not.
- **No screenshots this time, deliberately** — the picture is bit-identical to the recorded baselines,
  so there is nothing a screenshot could record.

⚠️ **The state file grew 128 bytes** (8826884 → 8827012 on `vcop2`; 32 slots × 4), so **every savestate
written before this change is refused**. Checked rather than assumed: the core prints
`refusing to load, need 8827012 bytes and was offered 8826884`, `retro_unserialize` returns false and
the run continues. That is the correct behaviour and it is the header doing the job §1.3 describes.

## Worth carrying

- **The per-frame hash should be the FIRST instrument on a savestate failure, not the last.** It cost
  two runs and it converted "the machine loads wrong" into "the machine loads right and drifts", which
  is a different investigation. Three sessions of registry auditing were answering the wrong question.
- **A two-arm split is only a bisection if both arms can fail.** `a` vs `b` looked like a clean
  isolation and half of it was a button the game does not read. The check is one line of the driver's
  `INPUT_PORTS`, and it should be run before the split, not after it.
- **Run the boring arm first — this time it was run first.** The 0-of-4321 control diff took one run
  and made every subsequent diff trustworthy. §9.1c wrote that rule after it had cost two sessions;
  this is the first session where it was applied before the fact rather than after.

# 2026-07-29 (sixth session) — Per-game input step 1: nine numbered buttons

The refactor that unblocks the daytona layout. **`NUMBERED_BUTTONS` 6 → 9**, so `IPT_BUTTON1..9` are
nine ordinary layout slots and a trigger threshold is just another *source* a row may name. No
behaviour changes: both existing layouts name the same controls the welds used to, and every check
below is an equality against a binary built without the change.

## What shipped

- **`libretro_m2_input.h`** — `NUMBERED_BUTTONS` 6 → 9, slot enum gains `BUTTON_7/8/9`, and a
  `read_source()` declaration.
- **`libretro_m2_input.cpp`** — layout rows are nine entries; two tagged sources (`SOURCE_L2_AXIS`,
  `SOURCE_R2_AXIS`) sit above every RetroPad id so the diagnostic combo's id comparison cannot
  mistake one for a button; `read_source()` resolves an entry to a button state; `configure()` builds
  nine identical items instead of six plus a trigger pair; item ids shift (L3/R3 → `BUTTON10/11`,
  combo → `BUTTON12`); the `IPT_BUTTON9` → R3 assignment is gone because the layout carries it now;
  `trigger_button_get_state` is deleted.
- **`retro_entry.cpp`** — the §5.1 shoulder-descriptor inversion, fixed as the plan asked. **L was
  labelled "Button 5" and R "Button 6" while the layout table has had button 5 on R throughout**, so
  on daytona the remap UI named GEAR 4 and VR1 (Red) the wrong way round.

**Two ordering facts that are load-bearing.** The numbered-button read in `update()` now runs **after**
the axes, because a threshold source reads `m_axes`; both orders see the same frontend snapshot, so
nothing else about moving it matters. And `IPT_BUTTON9` must be assigned **once** — leaving the old
explicit R3 assignment in alongside the layout's would bind one type to two items.

## 🚨 The verification was vacuous on the first attempt, and the tell was two scripts that should have disagreed

The first battery pressed every daytona control at frames 1100–1750 and reported eight clean
equalities. All of the daytona ones were meaningless: at those frames the game is still on its
attract and entry screens and **no button does anything**. The signature was there to be read —
`RetroPad (Classic)` and `RetroPad (Modern)` produced the **same digest** despite differing on slots
5 and 6, and a script pressing only `r3` produced the same digest as one pressing `l` and `r`. Only
the coin and Start were registering at all.

`per-game-input.md` §6 already said this: *"Getting daytona into a race is not obvious, and a run that
fails to measures nothing while every log line looks healthy. Screenshot the last frame before
believing a null result."* The screenshot is what settled it. **A coin at 600 and Start pulsed every
300 frames to 2400 puts daytona on the track at about frame 3400**, and the presses were moved to
3500–4300 over a 4500-frame run.

⚠️ **The discrimination check is now part of the harness rather than an afterthought**: the race
script asserts `Classic != Modern` *before* it compares either against the reference. Under Classic
`r` is button 5 (GEAR 4) and `l` is button 6 (VR1); under Modern `r` is button 6 and **`l` is named by
no slot at all**, so the two layouts cannot agree unless the run is measuring nothing.

## Verified

| check | result |
| --- | --- |
| Classic vs Modern, in a race | **differ** (`36f9169fdff21b63` / `968587196d2e43a5`) — the script discriminates |
| Classic, pre- vs post-refactor | identical |
| Modern, pre- vs post-refactor | identical |
| R3 held vs not held, in a race | **differ** (`45224b16a7dac8f1` / `6f410b99e508ee38`) — VR4 fires |
| R3 held, pre- vs post-refactor | identical |
| 8-case battery (idle, full sweep, r3, triggers, combo, vf2, srallyc) | all identical |
| `vf2` and `srallyc` scripts vs idle | differ — those two cases are not vacuous |
| `ab.sh vf2 2500` | background `c3aaa56633c1c4f7` identical across renderers, software `9c20f1fac9d9fe92`, vulkan `de94f44a06151f71`, SSIM covered 0.996985 — every recorded baseline byte-exact |

The reference binary was built by stashing **only** the three input files, so it carries the SCSP
savestate fix and differs from the new one in nothing but this refactor.

**Screenshots**: `2026-07-29-input1-daytona-vr4-{off,on}.png`, the two camera angles.

## Worth carrying

- ⚠️ **`per-game-input.md` §5 item 1 quotes stale digests** — `16af05bb8d02a9a5` / `55da761fecca5c01`
  are pre-startup-fix values. The current vf2 2500 pair is `9c20f1fac9d9fe92` / `de94f44a06151f71`.
  Corrected in place.
- **retrohost cannot script a partial trigger pull.** Its control table has no analogue trigger and
  `parse_script` refuses a value on a digital control, so `l2`/`r2` presses always arrive saturated
  (`update()` substitutes 32767 for a digital press). Fine for an equality between two binaries;
  **not** a test of where the threshold sits, and step 2's collision test will need to care.
- 🚨 **The zsh trap bit again, in an ad-hoc shell command rather than a script.** `flags="--modern 0";
  retrohost $flags …` passes ONE argument in zsh, and retrohost said `unknown option --modern 0` —
  which is at least loud. The same construction inside a `#!/usr/bin/env bash` script works. The
  harness scripts are bash for this reason; ad-hoc commands in this shell are not.

---

## 2026-07-30 — Per-game input, authored in a browser: the layout editor, one device type, per-game labels

**The user's framing, and it reset the scope:** the core should be *the most user-friendly way to play
Model 2* — load a game and touch nothing, and if you do open RetroArch's Controls menu, every entry
says what the button actually does on that cabinet. Plus: **get rid of `RetroPad (Cabinet)`**, and
*"I want to make those decisions of how the keys are bound, and I don't want to have to explain to you
which button does which — so build me an HTML5 tool."*

**[devnotes/padmap-tool.md](padmap-tool.md) is the record.** What landed, in one line: the machine now
describes its own controls to a browser, the browser authors a table, a generator compiles it, and the
core reads one table for both the pad and the frontend's labels.

### What was built

- **`M2VK_INPUT_DUMP`** (`src/osd/libretro_m2/m2vk_inputdump.h`, header-only) — every ioport field a set
  declares, as JSON, including **the driver's own `PORT_NAME`**. That is the whole point: nobody has to be
  told which button does which, because `model2.cpp` already says ("VR1 (Red)", "GEAR N", "Hand Brake").
- **`devnotes/tools/padmap-sweep.sh`** → `padmap-data.js`. **31 sets dumped**, covering 11 of the 12
  in-scope port sets, plus the driver table (90 entries, 32 port sets) extracted from `GAME()` lines.
- **`devnotes/tools/padmap.html`** — the editor. A RetroPad diagram, the game's declared inputs shown by
  the driver's own names, drag-or-click to assign, editable labels, and a **live validator** that refuses
  the two rules which are invisible until somebody plays the game.
- **`devnotes/tools/padmap-test.sh`** + `padmap-test.js` — the editor's logic headless under `jsc`, all 32
  port sets, every rule.
- **`src/osd/libretro_m2/input_layouts.json`** (data of record) + **`input_layouts.ipp`** (generated by
  `padmap-gen.py`, `--check` proves they agree). **12 rows naming 15 sets**, covering all 28
  non-`MACHINE_NOT_WORKING` GAME entries.
- **The core change**: one pad device type; one table for sources *and* labels; per-game descriptors built
  at load. `RETRO_DEVICE_M2_PAD_MODERN`/`_CLASSIC`, `PORT_DEVICES_CABINET`, `CONTROLLER_INFO_CABINET`,
  `has_cabinet_layout()`, `BUTTON_LAYOUTS[]`, `CABINET_LAYOUTS[]` and the static `INPUT_DESCRIPTORS[]`
  are all gone.
- **`M2VK_HOST_DESCRIPTORS=1`** in `retrohost.c` — the read-out. The labels had none: they show in
  RetroArch's Quick Menu → Controls, which is interactive.

⚠️ **No upstream file touched. The diff against mame0288 is unchanged at 30 lines.**

### The findings worth carrying

- 🚨 **`M2VK_INPUT_DUMP` cannot be taken from `input_init()`, and it was.** `osd().init()` is
  `machine.cpp:156` and `m_ioport.initialize()` is `169`, so the dump came out as `"fields": []` on a set
  with twenty — valid JSON saying the set has no controls. **`m2vk_gunlog.h` already carried this exact
  warning about this exact trap**, and it was read afterwards rather than before. Both the header and the
  sweep now shout on a zero-field dump.
- 🚨 **The `sets[]` a row must name is computable and must never be typed.** A name is needed when the
  entry is a **parent**, and when it is a **clone whose parent uses a different port set** — which is real
  (`rchase2a`). So `vf2`'s row names four (`vf2, hpyagu98, fvipers, lastbrnx`) and the rest name one. The
  editor computes it; the generator refuses a name that is not a `GAME` entry and refuses a name claimed
  by two rows.
- 🚨 **The generator exists so the labels can be DERIVED from the sources.** Two hand-written copies of
  that relationship is what put daytona's GEAR 4 and VR1 the wrong way round for months
  ([input-map.md](input-map.md) §5.1). Deriving makes it structurally impossible, and the collision check
  falls out of the same pass. **Do not add a second label table anywhere.**
- 🚨 **The `lightgun` row flag was missing and the gun descriptors went out on EVERY set** — daytona's
  remap screen listed a lightgun trigger called "GEAR 1" and vf2's listed one called "Punch".
  `M2VK_HOST_DESCRIPTORS` found it; nothing else could have.
- 🚨 **The descriptor send has to sit BELOW the options read.** L3 is `IPT_SERVICE1` only while
  `model2_diagnostic_input` names a combo and an inert `IPT_UI_MENU` otherwise, so its label cannot be
  decided before `diagnostic` is read. From the old position it would have labelled a dead control on
  every default run.
- **`relabelFromRow` was not idempotent** — it appended to the analog label slots without clearing them,
  so a second render read `"Steering / Steering / Steering / Stick X"`. Caught by the headless test, not
  by reading. The test now asserts idempotence on all 32 port sets.
- **Both hard validator rules must be gated on the buttons a set DECLARES.** Ungated, `motoraid` (two
  buttons, two pedals) failed on slots 7 and 8, which reach no part of the machine.
- ⚠️ **`game_driver::parent` is the literal string `"0"`**, not empty, so an unguarded compare looks for a
  row named `"0"` on every parent set in the tree.
- ⚠️ **`srallyc` is entirely `MACHINE_NOT_WORKING`** — all five entries — so per-game-input.md step 5's
  "second customer, the proof the table generalises" cannot be verified. `motoraid` took that role. 28
  working entries collapse to **12 port sets**, not the 13-of-90 the plan estimated.

### Verified

**The A/B no-op guard passes with all nine digests byte-exact against [ab-baselines.md](ab-baselines.md)**
on `vf2` (an authored row), `schamp` and `dynamcop` (both unauthored, both on the generic row) — which is
the proof the 62 unauthored entries are untouched. `padmap-gen.py --check` and `padmap-test.sh` pass.
`M2VK_HOST_DESCRIPTORS` confirmed on daytona / vf2 / vcop / schamp.

**The collision is fixed and the negative control works.** daytona in VR1's in-car view with the
accelerator floored to 167 km/h keeps that camera
(`screenshots/2026-07-30-input-daytona-collision-fixed.png`); rebuilt with daytona forced onto the generic
row the identical script **snaps the camera back to the chase view**
(`...-collision-negative-control.png`).

### 🛑 And the decision that came out of the verifying: NO MORE AUTOMATED BUTTON-PRESS TESTING

**Set by the user, 2026-07-30, and written into CLAUDE.md as a READ FIRST rule.** It costs far too much
for what it returns. This session spent most of its budget on nine 4000-frame `daytona` runs to establish
two things a human with a pad settles in a minute.

🚨 **And the sweep's first result was VACUOUS in exactly the documented way.** At frame 3500 daytona is
*already* in VR3's view, so pressing VR3 changes nothing and both arms came back equal — which reads
exactly like the collision still existing. A screenshot and four more runs were needed to tell "the fix
works" from "the test is blind". That is per-game-input.md §5 step 0 recurring in a new costume, and it is
the strongest argument for the ban.

⚠️ **The gear buttons were NOT settled and the null is not evidence.** All four give an identical
whole-run digest pressed or not, and **the car reaches 167 km/h with GEAR N latched** — so the
transmission is behaving as automatic here and the game may simply be ignoring the gearbox. In passing
that **answers per-game-input.md §2.1's open question: daytona does not require leaving N to move.**
Whether the four gear buttons reach the machine is an INPUT TEST question and is now the user's to answer.

---

## 2026-07-30 (later) — the editor writes the file itself: `padmap-serve.py`

**Asked for directly**, against README §4 ("getting it into the core"): *can't this be a button?* It can.
Download → `mv` from `~/Downloads` → `padmap-gen.py` → `make` is now **Save to core** and **Rebuild core**
in the editor's header. `devnotes/` only — no core file, no upstream file, no pixel.

New: `devnotes/tools/padmap-serve.py` (~170 lines, python3 stdlib). `padmap.html` grew a served-mode block
and two buttons; the file-load path was refactored into one `adoptParsed()` used by both the file picker
and the server. Docs: `tools/README.md` §1/§1.1/§4/§8, `padmap-tool.md` §1 + a new §3.2.

- 🚨 **The reason it is a server and not `showSaveFilePicker()`** — which was the obvious cheaper answer,
  since the File System Access API can write the JSON in place from `file://`. It cannot run
  `padmap-gen.py`, and **the generator is the half that matters**: it refuses a broken table and it derives
  the labels from the assignments. A button that writes the JSON and leaves the `.ipp` stale manufactures
  the one disagreement `--check` exists to catch, silently, which is worse than four manual steps.
- **A refused save writes nothing** — the old JSON is held in memory and put back if the generator exits
  non-zero. Measured: a row with a numbered button on `SELECT` returns 400 carrying the generator's own
  sentence, and both files hash identically to before the click.
- **Round-tripping the current file through the button reproduces both files to the hash**
  (`f4e732c5…` / `3da5805c…`), because what is written is the POST body verbatim rather than a
  re-serialisation. That is the check that the button is the `mv` it replaces and not a second writer.
- **The `file://` path is untouched and that is the requirement, not a courtesy.** `/api/state` not
  answering leaves the two buttons hidden and the Download loop exactly as it was. `padmap-test.sh` passes,
  which is what proves it: the whole script block runs under `jsc`, so `initServed()` guards on
  `typeof location` — a bare read of an undefined `location` at top level would take the headless run down.
- ⚠️ **Served startup loads the file on disk, but never over unsaved scratch work.** If localStorage holds
  an edit the file becomes the *diff baseline* instead and the header says `editing local changes against
  …`. Saving then re-baselines, so the "Changed since the file was loaded" panel empties instead of going
  on showing the change it just wrote.
- **`X-Padmap: 1` required on both POSTs**, plus binding `127.0.0.1`: a cross-origin form post cannot set a
  custom header and a cross-origin fetch that sets one is preflighted, which this answers for nothing.
  Verified — a POST without it is 403.
- `/api/build` streams `make`'s output line by line and ends with `__EXIT__ <rc>`; the header shows the
  current line, then `build ok — now play it`. Exercised end to end: rc 0, `model2_libretro.dylib` relinked.

⚠️ **The served path has no headless test** — it is `fetch` and streams and `jsc` has neither. The browser
half is the user's to click.

### …and a one-button launcher for it: `~/Desktop/Model 2 Pad Editor.app`

Asked for straight after: *is there a Mac shortcut — one button that starts it up and it works?* There is
now. Double-click, and the server starts and the editor opens. New: `devnotes/tools/padmap.command` (the
portable, in-repo version) and the Desktop bundle, plus `POST /api/quit` and a **Stop server** button in the
page.

- 🚨 **A .app CANNOT read this repo, and the bundle therefore hands the .command to Terminal.** The checkout
  is under `~/Documents`, which is TCC-protected: the bundle got `Operation not permitted` on
  `padmap-serve.py`, and **no permission prompt was offered — unsigned or ad-hoc signed, both measured**.
  So there is nothing to allow and nothing to grant in System Settings. Terminal already has the access.
  ⚠️ The failure message names the file, so it reads exactly like a moved checkout; **do not "tidy" the app
  into running the server itself**, it fails identically every time.
- **Relaunching while it is already up reopens the tab and starts nothing** — verified, one server before
  and after. The port probe is what does it.
- Because the bundle exits and leaves the server detached (LSUIElement, no Dock icon, nothing to quit), the
  page carries **Stop server**. `shutdown()` blocks until `serve_forever` returns, so it goes on its own
  thread; verified the process ends and the Terminal window prints `stopped`.
- **The build subprocess gets an augmented PATH** (`/opt/homebrew/bin`, `/usr/local/bin`) set in
  `padmap-serve.py`, not in the launcher: a Finder-launched process inherits launchd's PATH and the build
  reaches for Homebrew — `glslc` is there. Setting it server-side means every launcher gets one build
  environment.

`padmap-test.sh` and `padmap-gen.py --check` still pass.

### …and the last button: ▶ Play (2026-07-31)

Asked for straight after the launcher: *a button to launch straight into RetroArch with that game loaded
with the properly built core, so I can check the mapping.* **Since the row's exit criterion is a person
with a pad (scripted press-testing is banned, CLAUDE.md 2026-07-30), the loop was one manual step short of
finishing.** New: `POST /api/play` in `padmap-serve.py` and a `▶ Play <game>` button in the header, shown
in served mode only. Two files, both `devnotes/tools/`. **No core code, no upstream file, no pixel.**

- 🚨 **It launches `<repo>/model2_libretro.dylib` BY PATH, so the installed-core symlink is out of the
  loop.** That symlink has twice reverted to a stale copy on its own, and the copy is byte-identical when
  it happens — so it looks right until the next rebuild and then you check a mapping against an old core.
  RetroArch loads a core from any path and keys the `.opt` file on `library_name` ("Model 2"), not on the
  path, so the options survive.
- **`M2VK_*` and `M2OPT_*` are stripped by prefix**, not by list (the Desktop app's `env -u` names them
  one by one and would go stale). Verified: launched with `M2VK_SS=3 M2OPT_model2_renderer=software` in
  the parent, the child sees **0** variables of either prefix.
- **It refuses a stale core and names the button to press first**: `padmap-gen.py --check` must pass, and
  the dylib must be newer than `input_layouts.ipp`. Measured both — touching the `.ipp` forward gives
  `409 the built core is older than input_layouts.ipp — click Rebuild core first`, and the page offers
  *Launch anyway*. Judgement calls are `409` and overridable; facts (no ROM, no RetroArch, no selection)
  are `400` and are not offered as decisions.
- **A second click while one is running is refused, not stacked** (`409`, overridable).
- **ROM resolution: the port set's name, then its entries with the working ones first.** Verified against
  the driver table: exactly one set takes the fallback (**`dynabb` → `dynabb97.zip`**), and exactly **4 of
  32** have nothing playable (`airwlkrs`, `rchase2a`, `powsled`, `model2crx`), where the button is disabled
  and says `no ROM for <set>`. `~/Documents/ROMs/Model 2` first, `devnotes/roms` second.
- Verified with a stand-in for RetroArch so nothing opened a window: argv is
  `-L <repo>/model2_libretro.dylib <rom>`, `X-Padmap` still required (403 without), and every refusal
  above fires with the right code. `padmap-test.sh` passes — `renderPlay()` runs under the DOM shim, and
  from `file://` the button stays hidden and nothing changes.

**Same day, the first click found two bugs — one mine, one four days old.** `▶ Play doa` reported
`playing doa` and nothing appeared.

- 🚨 **The button lied, and the design let it.** RetroArch **exits with status 0** when content fails to
  load, seconds after the POST has already returned 200, and the child's output was going to `devnull` —
  so a launch that died was indistinguishable from a game that started. Fixed: the child's output goes to
  `/tmp/m2vk-play.log`, and **`GET /api/playing`** answers `idle`/`running`/`exited` with the reason
  distilled out of that log; the page polls it for 25 s and shows the audit lines verbatim. Verified on
  `von` (no complete dump here): `exited`, `rc 0`, `epr-18832.15 NOT FOUND … Fatal error … failed to
  start`. **Do not go back to `devnull` and do not trust the return code.**
- 🚨 **The actual `doa` failure was NOT the button: `~/Documents/RetroArch/system/model2` was an EMPTY
  DIRECTORY, and that is the core's second rompath** (`retro_entry.cpp:669`). So every set needing
  `Polydiver/roms` — `doa` is the one left, [roms.md](roms.md) — failed its audit **under RetroArch
  only**, while every retrohost run passed, because the harness is always pointed at both paths. Created
  2026-07-27 and empty ever since; nothing else looks there, so nothing else could have noticed. Fixed
  with a symlink to `Polydiver/roms`; `doa` now boots and runs at 1440×1080 under RetroArch, and the same
  set boots under retrohost with `M2_SYSTEM_DIR` pointed the same way.
- ⚠️ **The lesson generalises past this button**: an instrument that only reports the *start* of an
  action reports success for the whole class of failures that happen a second later. Same shape as the
  `73 of 4321` read-out and the vacuous press sweep — check the thing you actually care about, later than
  feels necessary.
- ⚠️ **And the first cut of that fix had the mirror-image bug**: it reported a game you **quit** as a game
  that would not start, because `doa` audits with a `WRONG CHECKSUMS` *warning* and runs fine. The
  discriminator is the core's own **`started '<set>'`** line, not the presence of alarming text in the
  log. Both directions now verified — `doa` (ran, then closed) reports nothing, `von` reports the audit.

### 🚨 The labels stopped following the assignments, and it shipped (2026-07-31)

Reported as *"my DOA buttons don't match what I assigned in the tool, saved to core, and built, and
played."* They did not: the built core bound **BUTTON1 (Hold) to Y** while telling the frontend that **Y is
"Kick"**, B "Hold" and A "Punch". Every one of the three was named as a button it does not press.

- 🚨 **`adoptParsed()` marked EVERY label loaded from `input_layouts.json` as a manual override**
  (`__manual[k] = true`). `relabelFromRow`'s `put()` skips manual slots, so from the moment the file was
  loaded the labels froze at the file's values while the sources moved with each drag. **Served mode loads
  that file at startup**, which is why this only became reachable when `padmap-serve.py` landed (2026-07-30)
  and why exactly **one row of twelve** — the one re-edited afterwards — is affected.
- **This is §1.1's drift, reintroduced by the LOADER rather than by a second table.** The tool's whole
  premise is that the label is derived from the assignment and therefore cannot disagree with it; a manual
  override is a second copy of the same fact, and marking everything manual made every label one.
- **Fixed three ways, because one of them is what failed:**
  1. `adoptParsed` marks a loaded label manual **only for a control no button feeds** (coin, start, d-pad,
     an analog axis — those have nowhere else to keep a hand edit), and re-derives the rest. A button's own
     wording lives in `buttons[i].label` and is loaded with it, so nothing is lost.
  2. Typing a label for a control **one** button feeds now edits **that button's** wording, not the
     control's, so it travels when the button moves. (Shared pedal+button triggers keep the old
     behaviour — `"Accelerator / Button 8"` is nobody's single wording.)
  3. **`padmap-gen.py` now REFUSES the drift**: a control fed by exactly one button must carry that
     button's wording. It printed the feeding button in the comment beside every label all along and never
     compared the two. Run against the shipped file it reports all three doa rows.
- 🚨 **`padmap-test.sh` did not catch it because the round-trip test HAND-ROLLED `adoptParsed`'s body** —
  the copy in the test had the same wrong line as the copy in the tool, so the two agreed and neither was
  right. It calls the real function now. **Never re-implement the code under test in the test.**
- **The new invariant is tested in both directions**: with the fix reverted the suite reports 4 failures
  naming the exact drift, and passes with it in place. Reproduced first in `jsc` — load the file, drag Hold
  onto Y, and BUTTON1 lands on a control labelled "Kick".
- **doa's row repaired** (labels take the buttons' wording: Y=Hold, B=Punch, A=Kick), regenerated, rebuilt.
  `M2VK_HOST_DESCRIPTORS=1` now reports B=Punch, Y=Hold, A=Kick, which is what the binding table says.
  ⚠️ **The sources were never wrong** — they are the faithful record of the drags — so the fix moved the
  labels onto them, not the other way round.
- ⚠️ **A reload alone would not have cleared it**: `persist()` writes the manual flags into localStorage, so
  scratch work saved before the fix carries them. `restore()` now normalises on the way in — one
  `normaliseManual()` shared with `adoptParsed`, because the two ways a row enters the editor must agree.
- **`ab.sh vf2 2500` reproduces all three `ab-baselines.md` digests byte-exactly** (`c3aaa56633c1c4f7` /
  `9c20f1fac9d9fe92` / `de94f44a06151f71`), coverage agreement 1.0000, 0 real interior disagreements —
  the no-op guard, since input changes no pixel.

**And the launcher crashed on a second start (2026-07-31).** `padmap.command` met a server already on
8733 — mine, left running from the session above — and exited with `OSError: [Errno 48] Address already in
use` and a full `socketserver` traceback. **The port probe existed only in the `.app` bundle**, so the
`.command` and a plain shell invocation had none. Moved into `padmap-serve.py`: `/api/state` on the port
answering `served` means it is one of ours, so open the tab and exit 0; anything else holding it gets one
sentence naming `--port`. Both verified. ⚠️ **A traceback out of the Python runtime reads as a broken
tool**, which is the whole cost here — the thing that had happened was "it is already open".

## 2026-07-31 — "my custom layouts do not apply": the browser scratch is a PARTIAL document

Reported as a core bug — vf2 and doa playing their defaults after the layouts had been authored. **It is
not a core bug and the core was never wrong.** The built dylib applies exactly what
`src/osd/libretro_m2/input_layouts.json` says, verified statically on the shipped binary with
`M2VK_HOST_DESCRIPTORS=1` (doa: B=Punch, Y=Hold, A=Kick; vf2: B=Punch, Y=Guard, A=Kick), against a
`padmap-gen.py --check` that passes and a `libretro_m2_input.o` newer than the `.ipp`. Everything between
the two was ruled out and is worth writing down so it is not re-ruled-out: **no RetroArch remap file
exists** (`config/remaps/` is empty), the per-game MAME cfgs in the frontend's save dir carry **coin
counters and a mixer and no `<input>` block at all**, and the cores directory is still the symlink to the
repo build.

🚨 **The edit was in Chrome's localStorage and had never been saved.** Read out of the leveldb: the
scratch holds **two rows, `doa` and `vf2`**, where the file holds twelve — and its doa row is
`BUTTON2←A, BUTTON3←B`, the swap the player made, against the file's `BUTTON2←B, BUTTON3←A`. Its vf2 row
is byte-identical to the file, which is why vf2 "looks like the default": **it IS the generic order**
(B/A/Y), so there was never an edit there to lose.

🚨 **AND THE NEXT SAVE WOULD HAVE DELETED TEN AUTHORED ROWS, silently, with a 200 back.** A row is seeded
when a port set is first selected, so a browser that has touched two of them holds a two-row document, and
`exportDoc()` writes exactly the rows the document has. `initServed` deliberately does not adopt the file
when a scratch exists — right, and not enough: **the rows the scratch has nothing to say about were being
shadowed rather than kept.** Fixed by merging any baseline row the scratch lacks into the document on
load (a deep copy, or an edit would move both sides and the diff panel would go blank mid-change), plus a
`confirm()` in `Save to core` naming the rows a save would drop. The status line now says how many rows
came from the file.

- ⚠️ **The diff panel could not have shown this and still cannot**: `renderDiff` compares the *current
  row* against its baseline. A row that is absent from the document is absent from the panel too.
- ⚠️ **The save endpoint was never at fault** — POSTing the file back to `/api/save` returns
  `200 wrote … 12 row(s)`. The failure mode is a save that was never made, not one that failed.
- **`padmap-test.sh` passes** (32 port sets, 31 dumps). It runs the script as global code, so the edits
  parse; the served path itself is behind `typeof location` and is not exercised there.

---

## 2026-08-02 — Compatibility audit: a boot sweep of the whole local ROM set, and three sets that were misnamed rather than missing

**Asked for a compatibility list — game name, supported or not, which ROM set to use.** Written as
[compatibility.md](compatibility.md); the index row is in `README.md` and it is the one doc here aimed
at a reader who is not us. **No code changed.**

**Method: a boot sweep, not an opinion.** Every zip in `devnotes/roms/` run for 1800 host frames
through `retrohost` against the built core with `M2VK_POLYTAP_SUMMARY` on — which answers "did the
ROMs load", "did the machine run" and "did it submit geometry" in one pass, sequentially, with its own
`M2_SAVE_DIR` per set. 33 sets, about four minutes. **29 games render, 3 do not, 5 have no ROMs.**

🚨 **THREE SETS FAILED AND ALL THREE WERE FIXED THE SAME DAY WITH NO NEW DUMPS. The pattern was a zip
named after a PARENT holding a CLONE.** `hotd.zip` holds `epr-19696.15`/`epr-19697.16` (no `a`), which
is `hotdo`; `von.zip` holds `epr-18664b.15`/`epr-18665b.16`, which is `vonj`. Copied to the clone's
name, both render — `hotdo` 823 3D frames, `vonj` 633. **This retracts
[tools/README.md](tools/README.md) §7's "two games are genuinely missing files", which was exactly
these two**, and it is struck there. **No set in the library is now blocked on a dump we do not have.**

🚨 **`vcop` WAS BROKEN BY THE 2026-07-31 ROM CONSOLIDATION AND NOBODY NOTICED FOR TWO DAYS.** It wanted
`epr-17181.6` (the `model1io2` I/O board BIOS) and `hd44780_a00.bin`, and both exist **only** inside
`Polydiver/roms/vcop.zip` — so `devnotes/roms` was *not* the superset that day's entry claimed, and the
"it cost nothing" measurement missed it because `vcop` was not one of the sets re-run. Fixed by
extracting the two files and `zip -j`-ing them into the local zip; original kept as `vcop.zip.bak`.
**The lesson is about the shape of that check, not about `vcop`: a per-set verification is only as good
as the set list, and the set list that day was the games being worked on.**

- ⚠️ **The clone zips were ADDED, not renamed over.** `hotd.zip`/`von.zip` are still there and still
  fail; the playlist was repointed (`Sega - Model 2.lpl.bak2` is the backup). Deleting the two dead
  zips is a free tidy-up whenever.
- ⚠️ **`vonj` needs `segabill.zip` in the same directory** and the failure names the missing *file*,
  not the missing *set* — a first attempt from a scratch directory reported `epr-18022.ic2 NOT FOUND`
  and read exactly like a bad dump. Device sets must travel with the game zips.

**What the sweep settles about `MACHINE_NOT_WORKING`, with a number this time: 62 of 90 entries carry
it and 20 of those are in the supported list.** Only `skisuprg` fails for the reason its own flag
names. The three genuine failures are `manxtt`/`manxttdx` (DX mode, unemulated — `manxttc` is the same
game and works), `skisuprg` (drive board) and `topskatr` (SIGABRT; a geometrizer limitation *upstream*
of our seam, so nothing this core does can reach it). **All three are ROM-proof — better dumps change
none of them.**

⚠️ **The sweep is a boot-and-attract check and the document says so in its own second paragraph.**
"Renders" means the game reached attract mode and submitted geometry. Depth of real verification varies
enormously across the 29 and the table carries it as a column: 12 A/B fixtures, 8 savestate fixtures, 2
lightgun games, 12 authored input rows, and a long tail whose only evidence is this run.

✅ **One instrumentation gap from `feature-survey.md` is closed for free**: a game that renders no 3D
used to write *no* summary file, so a missing file was the signal. The tap now writes `frames=0
no_3d=1`, which is what made a 33-set sweep readable in one pass.

---

## 2026-08-06 — an Android arm64 core, cross-built (asked for directly; not a phase)

**It builds and links. It has not been run on a phone.** `./devnotes/build-android.sh` produces
`model2_libretro_android.so`, aarch64, 89.6 MB unstripped, all 25 `retro_*` entry points exported.
**[devnotes/android.md](android.md) is the record.**

🚨 **THE HEADLINE IS THAT THE CORE WAS ALREADY PORTABLE AND NOT ONE LINE OF IT CHANGED.** The tree was
checked rather than assumed before any glue was written: **zero Apple-specific code** in
`src/osd/libretro_m2/` (no `__APPLE__`, no CoreFoundation, no `mach_*`; MoltenVK appears in comments
only), **the core links no Vulkan library** so Android's `libvulkan.so` needs no handling at all, and
**the shaders are committed SPIR-V** at `--target-env=vulkan1.0`. The P2 decision to resolve every
entry point from the frontend's `vkGetInstanceProcAddr` is what made this cheap, and it was taken for
unrelated reasons. **Every edit below is build-system glue.**

- 🚨 **`osdlib_unix.cpp` includes `<SDL2/SDL.h>` unconditionally and needs it for the clipboard and
  nothing else.** macOS has never hit it (`osdlib_macosx.cpp`), and `LIBRETRO_M2_TARGETOS` maps
  everything not-Windows-not-macOS to `unix`, so Android is the first target of this OSD to reach the
  file. **This is the second TIME the fork has touched upstream outside `src/mame/sega/` — the third
  file, since the first episode was `scsp.cpp` *and* `scsp.h` — and unlike that one this edit IS
  guarded**: upstream already stubs the clipboard under `SDLMAME_ANDROID`, so
  `OSD_LIBRETRO_M2` joins that condition and the include goes behind the same test. Byte-identical for
  every other OSD. ✅ **Side effect: a Linux build of this core was blocked on exactly this.**
- 🚨 **`links{}` ACCUMULATE AND GENIE CANNOT TAKE ONE BACK — that cost a whole-tree build.**
  `mainProject()`'s `configuration { "android*" }` links `EGL`/`GLESv2`/**`SDL2`** for the SDL
  android-project app. `maintargetosdoptions()` runs later and *did* override its
  targetprefix/targetname/extension, so the block looked handled; all 1095 objects compiled and the
  link died on `ld.lld: error: unable to find library -lSDL2`. The block has to be **skipped**, not
  overridden. ⚠️ **It is also where an android build normally gets `-shared` and its soname**, both
  now reissued in `libretro_m2.lua` — skipping it without noticing that yields an executable.
- 🚨 **`-static-libstdc++` is a phone-side runtime failure fixed at build time.** The NDK's clang
  links `libc++_shared.so` by default and the first successful link had it as a `NEEDED` entry; a
  frontend whose APK does not ship that library cannot `dlopen` the core. Measured: six `NEEDED`
  entries became five and `libc++_shared` is gone. `-Wl,--exclude-libs,ALL` goes with it so the core's
  now-private libc++ cannot bind against the frontend's — **the `retro_*` exports survive because
  `retro_entry.cpp` is a direct object rather than an archive member**, which is true for the reason
  the lua's own comment already gave. Dynamic exports: 46, of which 25 are `retro_*`.
- **bionic has no libpthread** — not a `.so`, not a `.a`, it is inside libc — so android is excluded
  from the OSD's `links { "m", "pthread" }` or the link fails outright.
- **`_android` in the filename is ABI, not decoration**: RetroArch on Android looks for
  `<name>_libretro_android.so` and strips the suffix again to find `<name>_libretro.info`. And the
  soname matters because **Android's loader dedupes by soname** — `mainProject()`'s generic
  `libmain.so` inside a frontend's process is a collision waiting to happen.
- **`build-android.sh` bypasses the makefile's `android-arm64` rule** (it hard-codes `--osd=sdl` and
  demands `SDL_INSTALL_ROOT`, `makefile:1195`) and calls genie directly, **harvesting `PARAMS` from the
  makefile at run time** rather than retyping it. Objects land in `build/android/obj/arm64`, so the two
  builds share nothing — worth stating because `OSD=sdl3` and `OSD=libretro_m2` *do* share one, which
  is the latent breakage CLAUDE.md records.
- ⚠️ **NDK install: Google's zip, not brew.** The `android-ndk` cask is broken
  (`undefined method 'command_wrapper'`) and the `sdkmanager` route needs a JDK this machine lacks.
  r27d at `~/Library/Android/sdk/ndk/android-ndk-r27d`; `ANDROID_NDK_HOME` overrides. The host dir is
  called `darwin-x86_64` on Apple Silicon too and that is correct — the binaries are universal.

✅ **The host build is proven untouched, which is the only guard that applies here.**
`make SUBTARGET=model2 OSD=libretro_m2 REGENIE=1` links, and `ab.sh vf2 2500` reproduces
`ab-baselines.md` to the digit: background **`c3aaa56633c1c4f7`** identical across renderers, software
**`9c20f1fac9d9fe92`**, vulkan **`de94f44a06151f71`**, coverage agreement 1.0000, real interior
disagreements 0, SSIM covered **0.996985**. Every change is either android-config-only or guarded by
`OSD_LIBRETRO_M2`, so a moved digest would have meant one of them was not.

⚠️ **`--NOASM=1` is passed, matching the upstream android rules, and it is a confound for the one
question the port exists to answer.** If the phone's speed is bad, re-check with the arm64 DRC before
concluding anything about the hardware.

🚨 **NONE OF THE HARNESS TRAVELS.** `retrohost` does not cross-compile, so `ab.sh`, `res.sh`,
`state.sh` and every digest in `ab-baselines.md` are host-only. On-device verification is playing it
and looking at it, and **a phone screenshot is not comparable to anything in `devnotes/`**.

⚠️ **Unrelated, noticed in passing: the installed-core symlink has reverted to a plain copy again**
(`~/Library/Application Support/RetroArch/cores/model2_libretro.dylib`, 81 MB regular file dated
2026-08-02). Third recorded reversion. Restore it before concluding a change "does not work" in the app.

---

## 2026-08-07 — the Android core RUNS, on an Adreno 740

✅ **`vf2` and `daytona` boot, render and play on an AYN Odin 2 Portal** (Snapdragon 8 Gen 2,
`Adreno (TM) 740`, Android 13, RetroArch 1.22.2, Vulkan driver). Screenshots in
`devnotes/screenshots/2026-08-07-android-odin-{vf2,daytona}.png`. **First light needed no code change
at all** beyond yesterday's build glue. **[devnotes/android.md](android.md) is the record** — §5a is
the device read-out and §7 the install/launch loop.

🚨 **THE DEVICE IS THE QUEST 3's GPU.** XR2 Gen 2 is the same silicon family as the 8 Gen 2, so a
phone picked for convenience turns out to be the deployment target's proxy. That was not the plan and
it is the most useful thing about the session.

🚨 **`vulkan-target.md` IS A MOLTENVK DOCUMENT AND SEVERAL OF ITS "FACTS" ARE APPLE'S.** Measured
here: instance API **1.3.0** (not a 1.1 ceiling), **`D24_UNORM_S8_UINT` EXISTS** with `filter-linear`
(the file's headline limitation), **`geometryShader`, `wideLines`, `multiViewport` all present**, and
the ring is **4** images not 3. Timestamps: **48 bits at 52.1 ns** — so performance.md's "GPU
timestamps deliberately not built, revisit on Quest 3" is now actionable rather than hypothetical.
Memory types 4/5/6 are `device-local | host-visible`, which **confirms** the shared-bus assumption
§1 had only inferred.

- **The renderer's paths are exercised, not merely loaded.** `daytona` logs `1707 polygons, 1 window
  run, 664 scissor draws` and then **grows all four geometry slots 2048 → 4096** — a resize path that
  on desktop had only ever run under synthetic pressure.
- 🚨 **RetroArch Android logs essentially NOTHING to logcat**, which reads exactly like a core that
  failed to load. The first launch looked dead and was not. The log that matters is the **file**, off
  by default: `log_to_file`/`log_verbosity`/`libretro_log_level` in `retroarch.cfg` (adb-writable,
  under `Android/data`), then `/sdcard/RetroArch/logs/retroarch.log`. ⚠️ Those are **currently ON** on
  the device; `devnotes/odin-retroarch.cfg.bak` is the config as found.
- 🚨 **The install is scriptable and the whole menu dance is unnecessary.** RetroArch's cores dir is
  not adb-writable (no root), but pushing the `.so` to `core_assets_directory` and then launching with
  `-e LIBRETRO <that path>` makes RetroArch **copy it into place itself** —
  `Core installation complete`. ⚠️ **The copy ANRs the UI thread** (57 MB) and the ANR dialog is not a
  failure. Thereafter point `LIBRETRO` at the internal cores path. Full `am start` line in android.md §7.2.
- ⚠️ **Redirecting `libretro_directory` at /sdcard was the first plan and was rejected**: it works, and
  it hides every other core the device already has. Not an acceptable thing to do to a handheld
  somebody plays.

**ROMs now live on the SD card** (`fsLabel=RPFlip2`, `/storage/F8B2-FD4C/ROMS/model2`), at the user's
direction. All **38 zips verified byte-size-identical** to `devnotes/roms`, 712 MB. The three files
put on internal storage the previous evening (`vf2`, `daytona`, `segabill`) and the two directories
created for them are removed; `/sdcard/ROMs` is empty again as it was found.

- 🚨 **The two LOOSE-FILE directories went too, and skipping them would have been the `vcop` trap
  again.** `devnotes/roms/manxttc/` and `overrev/` each hold `epr-18643.7`, a BIOS the matching zip
  does not — MAME reads a rompath entry named after the set as loose files. A copy of "all the zips"
  leaves both sets present-looking and broken.
- ⚠️ **The card is located by `fsLabel`, never by mount point** — the mount point is the volume UUID
  and changes with the card. `deploy-android.sh` reads it out of `dumpsys mount`.
- 🚨 **Nothing is pushed to `<system dir>/model2` any more.** The core's FIRST rompath entry is the
  content's own directory (`retro_entry.cpp:669`), so `segabill.zip` and friends work by sitting
  beside the games — one directory, one answer to "where is it", as on the desktop.
- ⚠️ **ES-DE will not use this core**: `ROMS/model2/systeminfo.txt` names
  `mamearcade_libretro_android.so`. Left alone deliberately — it is the device owner's frontend config.

⚠️ **NOTHING ABOUT SPEED OR ACCURACY IS MEASURED.** Two games reaching a playable screen is first
light. `retrohost` still does not cross-compile, so no digest, SSIM or `perf.sh` figure exists for
this device, and `--NOASM=1` remains a confound for any speed impression. android.md §6 stands in full.

---

## 2026-08-07 — the repo was renamed `mame-model2-vk` → `m2-vk`

Asked for directly. The GitHub home is now **https://github.com/mcwild77/m2-vk**; `git remote set-url
origin` done and verified (`git ls-remote` returns `798aa61fa10`, i.e. the new URL is the same repo at
the same HEAD). **No code changed and no commit was needed.**

🚨 **The headline finding is that NOT ONE COMMITTED FILE HAS EVER NAMED THE REPO.** A tree-wide search
returned nine hits and every one is local-only or untracked: `CLAUDE.md` (×3), `devnotes/README.md`,
`devnotes/legalstuff.md`, `devnotes/worklog.md`, `.vscode/settings.json` (untracked — confirmed with
`git ls-files .vscode/`, which prints nothing) and the three `devnotes/shortcuts/apps/*.app` launchers.
So the rename is **invisible to the public tree**: nothing to commit, nothing for an upstream merge to
conflict with, and §9's release checklist is untouched.

⚠️ **The search had to be run as `command grep`.** The shell's `grep` here is a function that passes
`--ignore-files`, i.e. it honours `.gitignore` — so a plain `grep -rn "mame-model2-vk" .` over this
tree returns **nothing at all**, because `CLAUDE.md` and the whole of `devnotes/` are exactly the files
that are gitignored. That reads as "the name appears nowhere" when the truth is "the name appears only
in the files you cannot see". Every hit above is in a gitignored path. **Use `command grep` for
anything that needs to see `devnotes/` or `CLAUDE.md`.**

**The local directory was deliberately NOT renamed.** It is still
`~/Documents/GitHub/mame-model2-vk`. Four things hold that absolute path — the three `.app` launchers
in `devnotes/shortcuts/`, `.vscode/settings.json`, the additional working directories a session is
launched with, and the installed-core symlink at
`~/Library/Application Support/RetroArch/cores/model2_libretro.dylib` — and a GitHub rename does not
require a local one. Renaming the directory is a separate job that breaks all of those at once; it
is available whenever it is wanted, and it is not free.

**It moves the trademark analysis, which is the only substantive consequence.** legalstuff.md §5's
first bullet argued that a source fork named `mame-model2-vk` reads as "a fork of MAME" and is
nominative use — true, ordinary practice, low risk, and the weakest of the three arguments in that
section because it rested on a reader making the inference. **`m2-vk` does not carry the wordmark at
all, so the argument is now moot rather than merely low-risk.** The binary half is unchanged and was
already handled (`retro_entry.cpp:220`, `library_name = "Model 2"`).

⚠️ **And it makes §5's item 2 — the README — MORE load-bearing, not less.** The old repo name carried
provenance for free: "mame-model2-vk" told a visitor what this is derived from before they read a
word. `m2-vk` tells them nothing, while `README.md` is still upstream's, completely unmodified, and
still presents the project *as MAME*. The one piece of public-facing prose the release needs now has
to do the provenance job as well as the disclaimer job. **Still the user's to write** — that decision
(2026-07-27) stands.

**Docs updated:** `CLAUDE.md` (title + a rename note under it + the release paragraph),
`devnotes/README.md`, `devnotes/legalstuff.md` §5 (both bullets and item 1, corrected in place), and
this entry. The `mame-model2-vk` strings left in this file above are dated records and were not
rewritten.

---

## 2026-08-07 — the core is called `m2-vk` now: `library_name`, `corename`, `display_name`

**Asked for directly**, in two parts: first "make the core list say `Sega - Model 2 (m2-vk)`", then
"do the full rename right now". The first is one string; the second moves three user-data directories
and is the part with consequences.

**What the frontend actually reads, because it is four different fields and only one of them is the
core's own:**

| Shown as | Comes from |
| --- | --- |
| Core list / Load Core | `display_name` in `model2_libretro.info` |
| Core Information → Core name | `corename` in the `.info` |
| `config/<name>/<name>.opt`, `saves/<name>/`, `states/<name>/` | **`library_name` from the core**, not the `.info` |
| The playlist's row in the sidebar | the playlist *filename*, `Sega - Model 2.lpl` |

Only the third is load-bearing, and it is the one nothing in the `.info` can influence.

**The nomenclature question was answered by survey, not by taste.** All 294 installed `.info` files
fall into exactly two families: `Arcade (CoreName)` for cores that run many unrelated arcade
platforms (MAME ×7, FBNeo, FB Alpha, HBMAME, DICE, Daphne — every one of them also
`systemname = "Arcade (various)"`, `manufacturer = "Various"`), and `Manufacturer - System (CoreName)`
for everything single-platform. 🚨 **The case that settles it is `SNK - Neo Geo AES/MVS (Geolith)`** —
a dedicated core for *arcade* hardware, and it does **not** take the `Arcade (…)` form. So the
prefix is a claim about scope, not about cabinets, and a Model-2-only core belongs with Flycast,
Kronos and Geolith. **Nothing anywhere uses a three-part `Arcade - Sega - Model 2`**; that shape is
ES-DE's folder taxonomy, not libretro's.

⚠️ **The old `corename = "Model 2"` was the odd one out and the device proves it.** The Odin's
`/sdcard/RetroArch/config/` holds `Flycast/`, `MAME/`, `Genesis Plus GX/`, `Snes9x/`, `mGBA/` — core
names, all of them — and `Model 2/`, ours, which was the *system's* name sitting in a directory
series otherwise made of core names.

**Changed:** `retro_entry.cpp:349` `library_name` → `"m2-vk"` (with a comment saying why it is not a
string to adjust casually); `corename` and `display_name` in both copies of the `.info`
(`devnotes/shortcuts/` and the installed one, kept byte-identical); the playlist's `default_core_name`
plus all 32 per-item `core_name` fields — those must match `display_name` or RetroArch treats the core
association as stale. `systemname = "Model 2"` / `systemid = "sega_model2"` deliberately **unchanged**:
they describe the hardware and match Kronos's `Saturn`/`sega_saturn`. No `database` field, also
deliberate — there is no `Sega - Model 2.rdb` among libretro's 141 (Naomi and Naomi 2 exist, Model 2
does not), and naming a nonexistent one breaks scanning.

🚨 **The rename orphans real user data, and that is the whole cost of it.** Three directories are
keyed on `library_name`, confirmed live rather than assumed — `retroarch.cfg` has
`sort_savefiles_enable = "true"` and `sort_savestates_enable = "true"`, which is what makes the save
and state ones exist at all:

- `config/Model 2/Model 2.opt` → `config/m2-vk/m2-vk.opt`
- `saves/Model 2/` → `saves/m2-vk/` — **12 games' NVRAM and 13 `.cfg` files**, i.e. credits, high
  scores and dipswitch settings
- `states/Model 2/` → `states/m2-vk/`

All three **copied, not moved**, on the Mac *and* on the Odin (which had daytona's NVRAM, its cfg and
an auto-savestate). The originals are left in place: a stale binary anywhere still finds its data, and
nothing is lost if the rename is reverted.

**Proven a no-op by digest.** `ab.sh vf2 2500` reproduces all three `ab-baselines.md` values
byte-exactly — background `c3aaa56633c1c4f7` identical across renderers, software `9c20f1fac9d9fe92`,
vulkan `de94f44a06151f71` — with coverage agreement 1.0000, 0 real interior disagreements and SSIM
covered 0.996985, the documented figure to six places. **No shader, no new file, no upstream file;
the diff against mame0288 is still 30 lines.**

**The android core was rebuilt too** (incremental — one object and a link) and `strings` confirms
`m2-vk` in the `.so`, 25 `retro_*` exports, no `libc++_shared` NEEDED. ⚠️ **It was not deployed** —
the device's directories are already migrated, so a `deploy-android.sh` will land on them whenever one
happens, and until then the installed core still reports `Model 2` and reads the old directories,
which are still there. Nothing is broken in either state.

⚠️ **The installed-core symlink had reverted to a plain copy for the FOURTH time** (found as an 81 MB
regular file dated 2026-08-06 09:30). It was still byte-identical to the repo build, so nothing was
stale *yet* — which is exactly how this one hides, since it only bites at the next rebuild. Restored.

**Docs updated:** this entry, `CLAUDE.md` (the gotcha-6 options path, the pinned-options command in
the play section, and "Where we are"), `devnotes/shortcuts/README.md` (the paths table, the
Load Core line, the `.opt` name, and the paragraph that claimed the ROM folder name was the core's
`library_name` — that coincidence is gone), `devnotes/shortcuts/retroarch.sh` (the `.opt` it reads and
the menu hint it prints), and `devnotes/legalstuff.md` §5 (the `library_name` paragraph and item 1,
corrected in place). 🚨 **The rename is naming consistency, NOT a trademark change** — neither
`Model 2` nor `m2-vk` carries the wordmark, so §5's conclusion is untouched; what it does is close
item 1 by making the repo name, the `corename` and the `library_name` all agree. Dated records
elsewhere in this file and in `p2-vulkan-passthrough.md` that quote `config/Model 2/Model 2.opt` were
**not** rewritten.

---

## 2026-08-07 — The lightgun reticle is OFF BY DEFAULT (asked for directly)

**One function, `m2vk::reticle_enabled()` in `m2vk_reticle.cpp`.** The default flipped: it now draws
**only** when `M2VK_RETICLE=1` is set, and `M2VK_NO_RETICLE=1` still means off, so every harness
script that sets it is unaffected and needs no edit. **No new file, no upstream file, no shader; the
diff against mame0288 is still 30 lines.**

**The reason is a frontend behaviour, not a fault in the reticle.** Playing `vcop2` on the Odin with
a finger on the screen, RetroArch Android reports a lightgun position only **while a finger is
down** and holds the last one after release — so the cross parks wherever the last shot landed and
sits there for the rest of the round. Nothing here can see the difference between "aiming there" and
"nobody is touching the screen": `reticle_publish` is handed a position and an on-bit, and the
on-bit is true because the port is a `RETRO_DEVICE_LIGHTGUN`.

🚨 **On `vcop` and `vcop2` it was a second crosshair on top of a working one anyway.** Both games
draw their **own** aiming reticle, in yellow, and `m2vk_reticle.cpp:16` records that ours was
coloured white/cyan precisely so the two could be told apart *in a test screenshot*. That was its
job during the lightgun phase; it is not a player-facing feature on the two sets that have one
already, which is most of what anyone plays with a gun here.

**The code is kept rather than deleted**, because the games that do *not* draw their own reticle
(`gunblade`, `rchase2`, `zerogun`, `hotd`) still want it, and because the touch case may be fixable
rather than merely avoidable — see below.

**Verified both ways, on `vcop --gun 0` with a scripted aim at 0.4/0.6:**
- **Off is now the default**: an ordinary `--gun 0` run and an `M2VK_NO_RETICLE=1` run are
  **byte-identical PPMs** and one digest, `75afb19f7812b192` over 900 frames.
- **`M2VK_RETICLE=1` brings it back intact**: digest `d79d085f8952d144`, and it differs from the off
  run in **exactly 124 pixels** — 48 cross + 76 border, the figure lightgun step 4 recorded when the
  asset was built. The shape is unchanged; only the default moved.

**Deployed to the Odin 2 Portal** (`deploy-android.sh`, 57 MB stripped, pushed to RetroArch's
downloads directory). ⚠️ **It still needs the install step on the device** — Load Core → Install or
Restore a Core, or the intent in `android.md` §7.2 pointed at the *downloads* copy — so until that is
done the device runs the previous build and still draws the reticle.

⚠️ **Open, and deliberately not taken here.** The honest fix for the touch case is a *timeout* or a
`RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN`-style gate, so the cross hides when no finger is down rather
than never drawing; RetroArch's Android touch mapping is what would have to be read to know which
signal is available. And a **core option** (`model2_reticle`) is the right home for this in the end —
it belongs with the per-port reticle colour already queued under CLAUDE.md's core-options item, which
is the entry the colour table in `m2vk_reticle.cpp` has been waiting for. An env switch is a harness
tool, not something a player on a handheld can reach.

---

## 2026-08-07 — steering curve step 1: the detector, MAME's analog settings, and the read-out

[steering-curve.md](steering-curve.md) step 1. **It shapes nothing** — that is the whole point of the
step, and the evidence for it is that `ab.sh` reproduces all nine `ab-baselines.md` digests
byte-exactly on three fixtures and the read-out prints `shaped == raw` on every line of every sweep.

Four files, all `src/osd/libretro_m2/`: one new header `m2vk_steer.h`, plus
`libretro_m2_input.{h,cpp}` and `libretro_m2_osd.cpp`. **No upstream file, no shader, no new build
script entry** (header-only, the `m2vk_gunlog.h` pattern); `git diff --numstat mame0288 -- src/devices
src/mame` is unchanged.

### What shipped

- **The detector.** `IPT_PADDLE` / `IPT_PADDLE_V` scanned out of `machine.ioport().ports()` in the
  OSD's `update()` one-shot, behind `safe_to_read()`, next to where `gun_log_frame` and
  `input_dump_frame` already resolve. `m2vk::steer().active` is what step 2 will gate on; nothing
  reads it yet.
- **MAME's `joystick_deadzone` / `joystick_saturation`,** captured in `input_init()` where the
  machine's options are in hand, for step 2's §3.3 pre-compensation to invert.
- **`M2VK_STEER_LOG`** — unset is silent (the detector still runs), `=0` is the one-shot resolve
  report, `=n` adds a line every n frames carrying the raw axis, the shaped axis and the resolved
  `IPT_PADDLE` port value.

### §7 question 2 is answered: the flag is set before the first frontend sample

`resolved on frame 0 after 0 frontend poll(s)`, on every set run. §3.2 argued this from
`libretro_m2_osd.cpp:274-278` and left it to be measured; it holds, and the read-out prints the poll
count at resolve time so it stays measured rather than remembered. The failure mode if it ever
changes is one unshaped frame, because the flag defaults to off.

### The sweep confirms §1's chain to the count — including the saturation plateau

`daytona`, deflection in → `STEER` port value out, range `0x20..0xe0` (±96 around `0x80`):

| deflection | 0.00 | 0.05 | 0.10 | 0.15 | 0.20 | 0.40 | 0.50 | 0.60 | 0.80 | 0.85 | 0.90 | 1.00 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| port | `0x080` | `0x080` | `0x080` | `0x080` | `0x087` | `0x0a2` | `0x0b0` | `0x0be` | `0x0d9` | `0x0e0` | `0x0e0` | `0x0e0` |

Every point is `0x80 + round(96 · (u − 0.15) / 0.70)` to the count. **The complaint in §1 is now a
measurement**: the first 15 % of travel is dead and the last 15 % is a plateau, so lock to lock is
the middle 70 % of about 13 mm of thumb, linearly. 🚨 **The plateau at 0.85–1.00 is step 2's
signature** — its §5.1 table calls a straight line *with* a plateau today's behaviour and a straight
line *without* one the proof that the pre-compensation inverts MAME's transform.

### The detector's library sweep — the step's other deliverable

Run across every set in `devnotes/roms` at 30 frames with `M2VK_STEER_LOG=0`. **It fires on 11 of the
35 sets that boot**, and those 11 cover **all eight** paddle-bearing port-set macros in the driver:

| port set | fires on | GAME entries |
|---|---|---|
| `daytona` | `daytona` | 8 (incl. `daytonat`/`daytonata`) |
| `desert` | `desert` | 1 |
| `manxtt` | `manxtt`, `manxttc`, `manxttdx` | 3 |
| `motoraid` (← `manxtt`) | `motoraid` | 2 |
| `srallyc` | `srallyc` | 5 |
| `indy500` | `indy500`, `stcc` | 11 |
| `sgt24h` (← `indy500`) | `sgt24h` | 1 |
| `overrev` (← `indy500`) | `overrev` | 3 |

**30 of the 90 GAME entries steer**, and the detector's answer is exactly that set. Every other set
run reports `0 IPT_PADDLE/IPT_PADDLE_V field(s)` — including all four §3.2 names as wrong to curve
(`vf2`, `von`/`vonj`, `vcop`, `vcop2`) — so the negative arm is measured and not merely intended.

Three sets could not be run and none of them is a gap: `hotd` fails its audit and is covered by
`hotdo` (same `PORT_INCLUDE(vcop2)`, reports 0), `von` likewise by `vonj`, and `segabill` is not a
game.

### 🚨 §7 question 1 has one real answer, and it is not either line the plan named

The plan flagged `model2.cpp:2036` and `:2120` as `IPT_AD_STICK_X` sets needing classification.
Both are correctly excluded — 2036 is `skytargt`, a flight stick, and 2120 is `rchase2`, a turret.
So are `rchase2a`, `gunblade`, `bel`, `skisuprg` ("Inclining") and `segawski`/`topskatr` ("Slide",
"Curving"). **The borderline case is `waverunr`, which the plan did not name**: its wheel is
`PORT_NAME("Handle Bar")` on `IPT_AD_STICK_X` (`model2.cpp:2349`), a jet ski's handlebar, which is a
steering control the detector will **not** fire on. Whether it should is a step 5 question with a pad
in hand and not a code one — and the answer, if it should, is a per-row override in
`input_layouts.json` rather than widening the type test, for the same reason §7 question 3 gives for
`manxtt`/`motoraid` (both of which *do* declare `IPT_PADDLE` and are detected, exactly as predicted).

### The no-op guards

- `ab.sh vf2 2500` — `c3aaa56633c1c4f7` / `9c20f1fac9d9fe92` / `de94f44a06151f71`, SSIM covered
  0.996985, 0 real interior disagreements. Byte-exact against `ab-baselines.md`.
- `ab.sh srallyc 2500` — **a steering fixture**, `49f86e1309ca422b` / `6fcc26a931ab2b01` /
  `172bb47c8ba8f383`, covered 136116, SSIM 0.988401. Byte-exact.
- `ab.sh schamp 2500` — the generic layout row, `964db6922c299090` / `3a270db490e1bc96` /
  `b3c2896438f248d0`, covered 50696, SSIM 0.998384. Byte-exact.
- `padmap-gen.py --check` (22 rows / 26 sets, `.ipp` matches `.json`) and `padmap-test.sh`
  (32 port sets, 31 dumps) both pass; `M2VK_HOST_DESCRIPTORS=1` still emits 53 descriptor lines on
  `daytona`. Nothing here touches layout data and that they are untouched is the evidence.

⚠️ **`ab.sh` is a guard here and nothing more.** Input changes no pixel, so a green table is evidence
of nothing *breaking*, never of anything working. The working evidence is the sweep table above.

**Next:** step 2 — the deadzone/curve/range/pre-compensation pipeline, reachable only through
`M2VK_STEER_*`, verified against the §5.1 sweep by the disappearance of the `0.85` plateau.

---

## 2026-08-07 — steering curve step 2: the pipeline, behind the switches only

`devnotes/steering-curve.md` step 2. **Three files, all `src/osd/libretro_m2/`** — `m2vk_steer.h`
(the shaping, the config reader, two lines of read-out), `libretro_m2_input.{h,cpp}` (the call site
and the write-back). `libretro_m2_osd.cpp` changed one comment. **No upstream file, no shader, no new
file, no pixel**; the diff against mame0288 is unchanged.

### What shipped

`m2vk::steer_shape()` is §3.4 in one function — deadzone (rescaled, not clipped), gamma curve, range
cap, then the §3.3 pre-compensation. `libretro_m2_pad_device::publish_steer()` became
`shape_and_publish_steer()`: it runs the chain on `m_axes[AXIS_LEFT_X]`, **writes the result back**,
and publishes port 0's before-and-after for the read-out. It is still called last in `update()`, after
the lightgun gate, so what it shapes is the value MAME is actually handed.

**Shaping is per port; only the read-out is port 0's.** The plan's step 1 note that "the shaping
itself will apply per port and needs none of this" is honoured — every pad shapes its own axis, and
the published sample is the wheel, which is player 1's.

🚨 **The gate is "the machine steers AND a shape was named", and at step 2 the second half is the
whole no-op argument.** With no `M2VK_STEER_*` switch set the pipeline does not run at all — not "runs
with default parameters" — so this step is a no-op *by construction* rather than by measurement. That
is deliberate and it goes away at step 3, when the core options supply the values.

`M2VK_STEER_LINEAR` takes a **value, not a presence** (the `M2VK_BLEND` discipline): `=1` forces the
chain off, `=0` explicitly does not, which is how a harness run will pin shaping ON once step 3 makes
it the default. Measured both ways below.

The switches are read in `input_init()` (`m2vk::steer_config()`), beside the existing
`joystick_deadzone`/`joystick_saturation` capture, rather than at the detector's one-shot — so the
configuration is complete long before the first frontend sample and a machine with no paddle still
parses and reports them.

### 🚨 The pre-compensation inverts MAME's transform to the count — the plateau is gone

`daytona`, 12-point deflection sweep into `STEER` (`0x20..0xe0`, ±96 around `0x80`). Ideal linear map
is `0x80 + round(96·u)`:

| deflection | 0.00 | 0.05 | 0.10 | 0.15 | 0.20 | 0.40 | 0.50 | 0.60 | 0.80 | 0.85 | 0.90 | 1.00 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| unshaped (= step 1) | `0x080` | `0x080` | `0x080` | `0x080` | `0x087` | `0x0a2` | `0x0b0` | `0x0be` | `0x0d9` | `0x0e0` | `0x0e0` | `0x0e0` |
| **γ=1, dz=0, range=1** | `0x080` | `0x085` | `0x08a` | `0x08e` | `0x093` | `0x0a6` | `0x0b0` | `0x0ba` | `0x0cd` | `0x0d2` | `0x0d6` | `0x0e0` |
| ideal `0x80+96u` | 128.0 | 132.8 | 137.6 | 142.4 | 147.2 | 166.4 | 176.0 | 185.6 | 204.8 | 209.6 | 214.4 | 224.0 |
| γ=2.2 | `0x080` | `0x080` | `0x081` | `0x081` | `0x083` | `0x08d` | `0x095` | `0x09f` | `0x0bb` | `0x0c3` | `0x0cc` | `0x0e0` |
| range=0.7 | `0x080` | `0x083` | `0x087` | `0x08a` | `0x08d` | `0x09b` | `0x0a2` | `0x0a8` | `0x0b6` | `0x0b9` | `0x0bc` | `0x0c3` |
| dz=0.05 | `0x080` | `0x080` | `0x085` | `0x08a` | `0x08f` | `0x0a3` | `0x0ad` | `0x0b8` | `0x0cc` | `0x0d1` | `0x0d6` | `0x0e0` |

**Every point of the γ=1 row is the ideal linear map rounded — the dead first 15 % and the `0x0e0`
plateau are both gone, and the round trip is exact to a single count.** That is §5.1 row 2 and it is
the whole proof of §3.3: MAME's `joystick_deadzone 0.15` / `joystick_saturation 0.85` still run, and
`input_device_joystick::adjust_absolute_value` is now the identity for this one axis.

The other three rows are §5.1's remaining expectations: γ=2.2 is monotone and concave and still
reaches exactly `0x0e0`; dz=0.05 holds `0x080` to 5 % and still reaches `0x0e0` (and note 0.10 lands
on `0x085` rather than `0x08a` — the deadzone is *rescaled*, so it gives its travel back); range=0.7
keeps the shape and tops out short of lock.

⚠️ **§5.1's "topping out near `0xa4`" for range=0.7 is a slip in the plan and is corrected in place.**
`0x80 + 0.7·96` is `0xc3`, which is what it measures. The *shape* claim it was making holds.

### A second steering set, unplanned and worth having: `srallyc`

Its `PORT_NAME("Steering Wheel")` is a full-range `IPT_PADDLE` (`0x000..0x0ff`), not daytona's ±96,
so it exercises the arithmetic on a different port range. Same defect unshaped (dead to 0.15,
`0x0ff` plateau from 0.85), same fix: under γ=2.2 the sweep is `0x080 0x080 0x081 0x082 0x084 0x091
0x09c 0x0a9 0x0ce 0x0d9 0x0e5 0x0ff` — monotone, concave, no plateau, and exactly `0x0ff` at lock.

### The no-op guards — the strongest one is a binary comparison

🚨 **A build of HEAD, with neither step 1 nor step 2 in it, gives the same whole-run digest and a
byte-identical last frame as this tree over a full 12-point analog steering sweep on `daytona`:**
`2a8f0b31bc690e6c`. So do this tree silent, with `M2VK_STEER_LOG=5`, and with `M2VK_STEER_LINEAR=1`.
Four ways of running the new code and one way of not having it, one digest.

| run | digest |
|---|---|
| HEAD build (no step 1, no step 2) | `2a8f0b31bc690e6c` |
| this tree, no `M2VK_STEER_*` at all | `2a8f0b31bc690e6c` |
| this tree, `M2VK_STEER_LOG=5` | `2a8f0b31bc690e6c` |
| this tree, `M2VK_STEER_LINEAR=1` | `2a8f0b31bc690e6c` |
| `M2VK_STEER_LINEAR=1 M2VK_STEER_GAMMA=2.2 M2VK_STEER_DEADZONE=0.1` | `2a8f0b31bc690e6c` |
| `M2VK_STEER_GAMMA=2.2` | `edf6880f51c7e395` |
| `M2VK_STEER_LINEAR=0 M2VK_STEER_GAMMA=2.2` | `edf6880f51c7e395` |
| `M2VK_STEER_GAMMA=1` (pre-compensation only) | `af4e5d968640ff45` |
| `M2VK_STEER_GAMMA=1 M2VK_STEER_RANGE=0.7` | `95fb4be5059f2f52` |
| `M2VK_STEER_GAMMA=1 M2VK_STEER_DEADZONE=0.05` | `5d0f9c36eb94b1bd` |

Rows 5 and 7 are the override discipline measured in both directions: `LINEAR=1` beats a named shape,
`LINEAR=0` does not. **And the four shaped rows all differ from each other and from the unshaped one,
which is the evidence that the shaping reaches the game's picture** — a curve that only moved a
read-out would leave the digest alone.

- `ab.sh vf2 2500` — `c3aaa56633c1c4f7` / `9c20f1fac9d9fe92` / `de94f44a06151f71`, SSIM covered
  0.996985, 0 real interior disagreements. Byte-exact against `ab-baselines.md`.
- `ab.sh srallyc 2500` — `49f86e1309ca422b` / `6fcc26a931ab2b01` / `172bb47c8ba8f383`, covered
  136116, SSIM 0.988401. Byte-exact.
- `ab.sh schamp 2500` — `964db6922c299090` / `3a270db490e1bc96` / `b3c2896438f248d0`, covered 50696,
  SSIM 0.998384. Byte-exact.
- `padmap-gen.py --check` (22 rows / 26 sets) and `padmap-test.sh` (32 port sets, 31 dumps) pass;
  `M2VK_HOST_DESCRIPTORS=1` still 53 descriptor lines on `daytona`.

### 🚨 The negative control, and it took two attempts to make it mean anything

§5.2's arm: **`vf2` under a strong curve must be byte-identical to `vf2` unshaped**, or the detector
is not gating and every fighting game has had its stick bent. It is, at `c721a3b11e35d07d` with
`M2VK_STEER_GAMMA=2.2 M2VK_STEER_DEADZONE=0.2`, and the read-out says why —
`vf2: 0 IPT_PADDLE/IPT_PADDLE_V field(s)` and `shaping is ON but this machine has no paddle`.

⚠️ **The first attempt at it was vacuous and looked identical to a pass.** Coin at 600 and Start
pulses left `vf2` on the staff-roll attract screen reading `CREDIT 1/2` — it wants two coins — where
the stick moves nothing, so *no stick input at all* gave the same digest as the sweep. **The control
the control needs is a third run**: with two coins (600, 700) and Start pulsed to 2400 the game is in
a real Akira-vs-Lau round, and `no stick` (`087eb9a69ffd15d6`) now differs from `sweep`
(`c721a3b11e35d07d`) while `sweep` and `curve` still agree. Without that third arm, "the curve is
correctly excluded" and "the stick does nothing in this run" fit the same evidence. Same species as
the lightgun phase's §2 lesson and the input step's vacuous first pass.

### Two smaller things measured rather than assumed

- **The read-out costs nothing.** `M2VK_STEER_LOG=5` reads `ioport_port::read()` 831 times over a
  4120-frame `daytona` run — through a port that also carries `daytona_gearbox_r`, a
  `PORT_CUSTOM_MEMBER` — and the digest is unmoved. Worth knowing, because a custom read handler with
  a side effect would have made the instrument perturb the thing it measures.
- ⚠️ **`for p in $pts` does not word-split in zsh** and it produced a whole round of runs with a
  malformed input script — one entry reading `3600:lx+=0.00 0.05 …:40` — which retrohost quietly
  ignored, so every arm had *no analog input* and every digest agreed. It reads exactly like a clean
  no-op result. This is gotcha 8 in CLAUDE.md, one round further on: it is not only `env $e`, it is
  any unquoted list. The harness scripts are `#!/usr/bin/env bash` for this reason; ad-hoc loops in
  the shell are not.

**Next:** step 3 — `model2_steering_deadzone`, `model2_steering_response` and `model2_steering_range`
as core options, live, with the switches overriding them and joining the announcement loop at
`retro_entry.cpp:613`. Step 5's hand-check is where the defaults are decided.

## 2026-08-07 — steering curve step 3: the three core options, live, with the switches overriding them

[devnotes/steering-curve.md](steering-curve.md) §4 step 3, now as-built. Four files, all
`src/osd/libretro_m2/`: `m2vk_steer.h` (the option storage and the composition), `retro_options.{h,cpp}`
(the table and the getters), `retro_entry.cpp` (the reads, the log line, the switch announcements).
**No upstream file, no shader, no new file, no pixel.** `git diff --numstat mame0288 -- src/devices
src/mame` unchanged.

**`Steering Response` (Linear / Slight / **Medium** / Strong / Very Strong), `Steering Deadzone`
(0 %…**5 %**…20 %) and `Steering Range` (**100 %**…60 %)**, all live, all overridden one for one by
`M2VK_STEER_GAMMA` / `_DEADZONE` / `_RANGE`, with `M2VK_STEER_LINEAR=1` still turning the whole chain
into the identity. The gammas (1.0/1.3/1.7/2.2/3.0) sit in `STEERING_RESPONSE_GAMMA[]` beside the value
strings — one list, so the words and the numbers cannot drift, which is the rule `DIAGNOSTIC_VALUES`
already follows.

### 🚨 The step-2 no-op argument is gone, and what replaces it is narrower

Step 2's whole claim was structural: no switch meant the pipeline did not run *at all*, so the step
could not move a pixel. A default run of a steering game is shaped now. What keeps every
`ab-baselines.md` fixture byte-exact is a smaller property and it is worth stating before anyone
changes the chain: **a centred stick returns a hard zero at every setting** — `mag <= dz_opt` exits
before the curve, and it is the only exit that returns zero — and **no accuracy fixture scripts an
analog axis**. Both were measured rather than assumed: `ab.sh` on `vf2`, on `srallyc` (a steering
fixture, now shaped by default) and on `schamp` reproduces all nine `ab-baselines.md` digests
byte-exactly, with `covered` and `ssim covered` to the digit.

### The evidence is six digests off one 12-point `daytona` sweep

`0ed27e15f8e29021` defaults (the report says `gamma=1.70 (core option)`); `04dd594589f84d38` under
`M2VK_STEER_LINEAR=1`, where `raw==shaped` on every line and the `0x0e0` plateau from 0.85 is back;
`34b7ede0a24688e4` for γ=1/dz=0/range=1 **whether it is asked for by switches or by core options** —
one pipeline, two sources, byte-identical; `242ac01a4f2e3600` for the `Very Strong` option alone; and
`Very Strong` + `M2VK_STEER_GAMMA=1.7` back to `0ed27e15f8e29021`, i.e. **the switch overriding the
option byte-exactly**. The straight-line arm is still `0x80 + round(96·u)` to the count, so step 2's
pre-compensation survives the new composition path unchanged.

🚨 **A build of HEAD, with none of the three steps in it, gives `04dd594589f84d38` and a
byte-identical last frame to this tree under `M2VK_STEER_LINEAR=1`.** That is the guard that matters
now that shaping is the default: the harness can still pin a run to the pre-steering behaviour, and it
is proved against a binary rather than against a table.

### ⚠️ The liveness test passed vacuously first, for a reason worth writing down

§3.5 says all three must apply without a content reload, and `M2VK_HOST_OPT_AT` is the instrument
built for exactly that. The first run — `3800:model2_steering_response=Very Strong` — produced the
**same digest as the static Very Strong arm**, which reads as "the option was applied from the start"
and is not what happened: the resolve report shows the run started at `gamma=1.70`, and the
`core options changed:` line fires at 3800. Every frame before the change was simply pixel-identical
between the two configurations, because the sweep's early points are inside the deadzone or barely off
centre with the car not yet moving. **The discriminating run is
`3900:model2_steering_range=60%`**: static 100 % `0ed27e15f8e29021`, static 60 % `610a63398586c048`,
live `936a8df95c4d1ac9` — different from both. **A live-change test needs a change point where the two
static arms visibly differ afterwards, or it cannot fail.**

### The negative control, on the option path this time

`vf2` again, and all three arms again, because the two-arm version is vacuous (step 2's lesson): with
coins at 600 **and 700** and Start pulsed to 2400, no-stick `8a2b4fe8d155ca9e` **differs from** sweep
`2b469cf302928efa`, and the sweep is `2b469cf302928efa` under the defaults, under `Very Strong` + 20 %
deadzone **and** under `M2VK_STEER_LINEAR=1`. One digest, three configurations — the detector excluding
`vf2` from an option nobody set. ⚠️ These are not step 2's numbers; the sweep script has different
deflection points and a negative half. **The invariant is the equality, never the number.**

### ⚠️ `declare_variables()` reorders, and that had been a no-op until now

The pre-options form wants the default listed first. Every option's default was also its first value
until `model2_steering_deadzone`, whose values run 0 % → 20 % in the order a player scrolls them with a
default of 5 %. The code already hoisted the default and its comment said it relied on the two being
the same thing; the comment was wrong and is corrected in place, with a note not to "simplify" the
hoist into a straight copy.

**Also:** the option values live **outside `steer_state`**, because that struct belongs to the machine
and `steer_close()` resets it while the options belong to the player and are parked by
`retro_load_game()` before the machine exists. `steer_apply()` is a function rather than four lines in
each caller for `apply_force_solid()`'s reason — a live change and a change at load have to mean the
same thing. `padmap-gen.py --check` and `padmap-test.sh` both pass, which is this step's stated
evidence that no layout data moved.

**Next:** step 4 — the `Steering / Stick X` label in `input_layouts.json`, checked under a curve on the
sets the detector fires on. Then step 5, the RetroArch hand-check, which is the user's and is where
Medium + 5 % is accepted or moved.

## 2026-08-07 — steering curve step 4: the labels, the one gap they had, and a check that keeps it shut

[devnotes/steering-curve.md](steering-curve.md) §4 step 4, now as-built. Two files —
`src/osd/libretro_m2/input_layouts.{json,ipp}`, the second generated from the first — plus
`devnotes/tools/padmap-gen.py`. **No C++, no upstream file, no shader, no new file, no pixel.**
`git diff --numstat mame0288 -- src/devices src/mame` unchanged.

### "Under a curve" is not a variable, and saying so is the first half of the step

Input descriptors are a property of the layout row, not of the shaping. `M2VK_HOST_DESCRIPTORS=1` gives
a **byte-identical** analog descriptor list with the pipeline on and under `M2VK_STEER_LINEAR=1`, on
`desert`, `daytona`, `srallyc`, `vf2` and `waverunr` — five sets, ten runs, `cmp` on the port-0 analog
entries. That is the equality the step asked for, stated as an equality rather than as an impression.

### The cross-tab is the deliverable, and it found exactly one gap

All 90 `GAME` entries resolved through `layout_for()`'s own rule — exact set name, then parent, then
generic — against each machine's own paddle fields, read from the sweep's dumps. **30 entries steer**
(the same 30 step 1 measured) and **29 of them already name the wheel**: `Steering` on
`daytona`/`manxtt`/`motoraid`, `Steering Wheel` on `srallyc`/`indy500`/`stcc`/`sgt24h`/`overrev`, with
every clone inheriting through the parent pass — `manxttc`, `manxttdx`, the five `srallyc` sets, the four
`stcc` sets and daytona's seven all land on their parent's row.

🚨 **`desert` was the one on the generic row.** It is shaped by the curve — its `IPT_PADDLE` is the first
thing `M2VK_STEER_LOG=0` prints — while the Controls menu offered it the fallback's hedge,
`Steering / Stick X`.

### The `desert` row, and the two labels the hedge could not have got right

Its buttons are **the generic row's order unchanged** (B/A/Y/X/R/L for MAME buttons 1–6), so nothing a
player has learned moves and only the wording arrives: *Machine Gun · Cannon · Shift · VR1 (Blue) ·
VR2 (Green) · VR3 (Red)*, `LSTICK_X` **Steering**. Slots 7–9 are `NONE` — the machine declares no
`IPT_BUTTON7/8/9`, so the generic row's trigger thresholds and R3 were binding nothing there.

The two the fallback could not know, both about pedals:

- **BRAKE is an `IPT_AD_STICK_Y`** (`model2.cpp`, `PORT_START("BRAKE")`), i.e. the **left stick's Y
  axis** rather than a trigger. `LSTICK_Y` is labelled **Brake**; the hedge said "Stick Y".
- **There is no `IPT_PEDAL2` at all**, so L2 does nothing. It is unlabelled now; the hedge said
  "Brake / Button 7", which is a control claiming to do something.

Descriptor count goes **76 → 44**: the fallback labels every control it has a string for, a row labels
the ones that exist.

### The coupling is a check now — `padmap-gen.py --check`, both directions

The curve applies **iff the machine declares an `IPT_PADDLE`** (nothing authored, nothing per game) and
the `LSTICK_X` label is the **only** place the frontend says what the stick does. Neither side can see
the other, and a mismatch shows up in no build, no digest and no screenshot. So `--check` refuses a
paddle-bearing set with no row or with a row whose `LSTICK_X` does not say steering, **and** a row saying
steering whose sets declare no paddle. The paddle half reads `padmap-data.js` — the machines' own dumps,
the same fact the detector asks at runtime — so the check cannot drift from what the curve will do; it is
silent when that file is absent (it is `devnotes/`) and treats an undumped port set as *unknown* rather
than paddle-free, which is what stops the reverse half being an argument from silence.

⚠️ **A check that cannot fail is worth nothing, so all three arms were fired by mutating the JSON and
restoring it.** Row removed → *"'desert' declares an IPT_PADDLE ("Paddle") … but it has no row"*;
`desert`'s `LSTICK_X` set to `Stick X` → *"the one place that says so does not"*; `vf2` labelled
`Steering` → *"the label promises shaping the detector never applies"*. 🚨 **In the first arm the checker
named `desert` and nothing else** — the 29-of-30 coverage claim measured rather than asserted, out of the
same pass that would have failed had any of the other 29 been wrong.

### The generic hedge stays `Steering / Stick X`, deliberately

No paddle-bearing set can reach the generic row any more, so the hedge's steering half now only appears
on machines the curve does not touch — which reads as an argument for narrowing it to `Stick X`, and is
not one. **`waverunr`'s `PORT_NAME("Handle Bar")` on `IPT_AD_STICK_X` is a steering control the detector
does not fire on** (§7 question 1), and it is on the generic row; "Steering / Stick X" is the honest
wording for a cabinet nobody has authored, and `Stick X` would state the wrong thing there. §5.3 item 8
is where that is decided with a pad, and it is unchanged by this step.

### Guards

- `ab.sh desert 2500` — the changed set — `0c5533aa763ce5c3` / `bcf3237a5747a53b` / `444c4d30a83c91f4`,
  covered 138222, SSIM covered 0.999376, 0 real interior disagreements. Byte-exact against
  [ab-baselines.md](ab-baselines.md).
- `ab.sh vf2 2500` — `c3aaa56633c1c4f7` / `9c20f1fac9d9fe92` / `de94f44a06151f71`, covered 107568,
  0.996985. Byte-exact.
- `padmap-gen.py --check` — **23 rows naming 27 sets** (was 22 / 26), `.ipp` matches `.json`.
- `padmap-test.sh` — 32 port sets, 31 dumps, all rules.

⚠️ As at every input step, `ab.sh` is evidence of nothing *breaking*: a label moves no pixel. The working
evidence is the cross-tab, the descriptor equality and the three fired check arms.

**Next:** step 5 — the RetroArch hand-check (§5.3), which is the user's. It is where Medium + 5 % + 100 %
is accepted or moved, where `waverunr`'s handlebar is decided, and where `desert`'s row gets its
`verified:` line (it says NOT YET MEASURED IN GAME, and the brake on the stick's Y axis is the part worth
a thumb on it).

---

## 2026-08-08 — a launcher for the steering hand-check, and the symlink thief caught

Session work, no core code. Three files, all `devnotes/`: new
`shortcuts/Model 2 Steering.command`, plus edits to `shortcuts/retroarch.sh`, `shortcuts/README.md`
and `steering-handcheck.md`. **No upstream file, no shader, no pixel, nothing rebuilt.**

### `Model 2 Steering.command` — Part 0 of the hand-check, done for you

Asked for directly: a sibling of `Model 2.command` set up for the driving games only, so step 5 can
be walked through without doing the setup by hand each time. It:

- lists **only** the eleven relevant sets — the nine that declare an `IPT_PADDLE` (taken from
  `padmap-data.js`, i.e. the machines, not a genre guess), plus `waverunr` (Test 8, steers and is
  deliberately *not* shaped) and `vf2` (Test 6, the negative control) — each tagged with the test it
  serves and its own control crib. `manxttc` and not `manxtt`/`manxttdx`;
- launches `<repo>/model2_libretro.dylib` **by path**, as the padmap ▶ Play button does, so the
  installed core is not in the loop;
- forces `model2_diagnostic_input = None` before every launch (§0.3), backing the `.opt` up once to
  `m2-vk.opt.handcheck-backup` before its first edit, and offers `r` to reset the three steering
  options to `Medium / 5% / 100%`;
- strips `M2VK_*` / `M2OPT_*` and names what it dropped;
- loops, printing the settings **as RetroArch left them** after each game — core options are saved on
  exit regardless of `config_save_on_exit`, so that is the row to write down.

### 🚨 `retroarch.sh` was eating the installed-core symlink, and that is the fourth reversion

Found while writing the above. `retroarch.sh` installed the core with `cp` + `mv -f` onto
`~/Library/Application Support/RetroArch/cores/model2_libretro.dylib` on **every** launch — and
`mv -f` swaps the directory entry rather than following the link, so any run of the play launcher
silently turned the symlink into a plain copy. That fits every recorded symptom: the reversion is
invisible at the time (the copy is byte-identical), and it only bites at the *next* rebuild.

**Fixed**: the install is skipped when the entry is already a symlink to this repo's build, and
copies otherwise. Both branches exercised — symlink present → `core: … (symlink, left alone)` and the
link survives; symlink moved aside → an 81 MB regular file appears, as before. The symlink was
restored afterwards and is as it was.

⚠️ This is a *likely* cause, not a proven one — nothing was instrumented to catch the reversion in the
act. But it is a mechanism that demonstrably does it, in a script that runs on every play session.

### 🛑 Open, raised by the user: the diagnostic-input combo needs rethinking

`model2_diagnostic_input` defaults to `None`, but the value remembered on this machine was
**`Hold Start`** — and **Start is a real in-game button**. In Daytona you press it to start the race
and again in menus, so holding it during play drops you into the cabinet's test menu, which reads as
a crash. That is why the hand-check's §0.3 exists and why the new launcher forces `None`.

**A safety setting whose value list is mostly gameplay buttons is the problem**, not the one value.
FBNeo's eleven combos were adopted verbatim (lightgun step 6) for parity, and parity was the right
call then; what nobody checked is whether any of them is safe to *leave on* while playing a Model 2
driving game. Not scoped, not designed — recorded here so it is not rediscovered mid-race. Decide it
with the per-game input work, since that is what knows which buttons a cabinet actually uses.

---

## 2026-08-08 (2) — the steering display bar: `model2_steering_display`

Asked for directly: *"visualizing the input… I need a way of seeing it"* — a bar across the top of the
picture, red when the wheel is idle, green growing out from the centre towards the side being steered,
sized by **how much steering the game is actually receiving**. Built, and it is the tenth core option.

**New files:** `src/osd/libretro_m2/m2vk_steerbar.{h,cpp}`,
`renderer_vk/shaders/steerbar.frag` + its generated `steerbar_frag_spv.h`. **Edited:** `m2vk_steer.h`
(the publisher), `renderer_vk/vk_present.cpp` (pipeline + draw), `libretro_m2_osd.cpp` (the CPU blit),
`retro_options.{h,cpp}`, `retro_entry.cpp`, `scripts/src/osd/libretro_m2.lua`,
`shaders/build_shaders.sh`. **No upstream file — the diff against mame0288 is still 30 lines.**

### It is the reticle again, and that is why it was cheap

Every hard part was solved by devnotes/lightgun.md step 4 and is reused rather than re-argued: a
normalised state published once per frame from the thread that knows the number, two blitters (a
scissored fullscreen triangle after the OVER layer, a CPU blit into MAME's finished frame for
`renderer=software`), opaque so the two paths produce the *same pixels* rather than the same picture,
one shared definition of the geometry and the colours with the predicate written twice — once in C++,
once in GLSL — each pointing at the other. Drawn **inside** the supersampled pass, so `M2VK_SS` and a
real internal resolution both antialias it with the picture.

### 🚨 What it draws is the PORT VALUE, and that is the decision worth not undoing

The green bar is the resolved `IPT_PADDLE` port value normalised about its own centre — the number the
driver reads, after our shaping, after `analog_field::apply_settings`, after `PORT_MINMAX` scaling.
Not the stick and not the shaped axis: both are upstream of something that can still change the
answer, and "what percentage is actually going into the game" is the question the bar exists to
answer. Normalising about the field's own centre is what makes `daytona`'s `0x20..0xe0` and
`srallyc`'s `0x00..0xff` mean the same thing on screen.

**The raw stick is drawn too, as a white notch, and it is the most useful part.** The gap between the
notch and the end of the green *is* the response curve, live. Measured on the screenshots below: at
half deflection left the green ends at x=185 while the notch sits at x=134 — the wheel is well short
of where the thumb is, which is γ=1.7 doing its job — and at full lock the two coincide (green ends
467, notch 468), which is the "full lock stays reachable at every setting" claim visible rather than
asserted.

### Verification

- **The two renderers are pixel-identical inside the bar: 0 differing of 8028**, on both a half-left
  and a full-right arm of `daytona`. That is the reticle's discipline holding, and it is what keeps
  `renderer=software` usable as a reference with the bar on.
- **The fill is correct and responds**: centre → all red, no green; half left → green 185..247 (63 px,
  left of the centre at 248); full right → green 248..467, the whole right half.
- **It reaches the picture**: option on `6dbf3cce72de8fe6`, off `e7002f0e01bff0f7`.
- **The switch beats the option in BOTH directions, byte-exactly** — `M2VK_STEERBAR=0` with the option
  on reproduces the off digest and last frame; `M2VK_STEERBAR=1` with the option off reproduces the on
  ones. The core announces the override on its own line, as the other switches do.
- **The no-op guard passes on a steering fixture, which is the one that matters**: `ab.sh srallyc 2500`
  gives `49f86e1309ca422b` / `6fcc26a931ab2b01` / `172bb47c8ba8f383`, covered 136116, SSIM 0.988401 —
  byte-exact against [ab-baselines.md](ab-baselines.md), as is `ab.sh vf2 2500`
  (`c3aaa56633c1c4f7` / `9c20f1fac9d9fe92` / `de94f44a06151f71`).
- Screenshots: `devnotes/screenshots/2026-08-08-steerbar-daytona-{centre,left-half,right-full,
  left-half-software,off}.png`, all from `retrohost --vk` except the one named software.

### ⚠️ Off by default, and no port is read when it is off

The same argument the reticle rests on: every `ab-baselines.md` and `res-baselines.md` fixture
differences against a background both renderers must produce bit-identically, and a bar across the top
is by construction pixels neither renderer's 3D path produced.

The stronger half is that **the paddle port is not read at all unless the bar is on**. It would be
harmless to read it — step 2 measured 831 reads over a 4120-frame `daytona` run moving no digest — but
"harmless" and "does not happen" are different guarantees, and only the second one survives someone
later putting a side effect behind `ioport_port::read()`.

### ⚠️ It is drawn over the game, including over Daytona's lap counter

Deliberate and not fixable by moving it: the top 5 % of a Model 2 picture is where every one of these
games puts its own HUD. It is an instrument for setting the steering up, not a thing to leave on, and
the option's description says so. `Model 2 Steering.command` toggles it with `d` for that reason.

---

## 2026-08-08 (3) — an outside survey of pad-to-wheel practice; the hand-check reframed, no code

**Docs only. No file in `src/` touched, nothing rebuilt, nothing measured** — so there is no digest in
this entry and there should not be. Three files:
[steering-handcheck.md](steering-handcheck.md), [steering-curve.md](steering-curve.md) §2/§3.5/§6/step
6/§7 q4, and this log.

The user brought a survey of how other projects map a pad onto an arcade wheel — Supermodel, MAME,
Cannonball, Flycast, BeamNG, plus what players of those actually hand-tune — and asked how it maps onto
what is built. It maps well: §2 had already read the same four codebases from local checkouts and every
reading reproduced, **including the negative one that none of them implements a gamma curve**. So
nothing in the design is wrong and nothing was corrected.

### 🚨 The finding: `model2_steering_range` is Supermodel's "saturation above 100 %", and it is off by default

The one place the vocabularies differ, and it hid a shipped feature in plain sight. Both mean *physical
full deflection gives less than full lock*, so their recommended 130–150 % is our `80%`/`70%` — and
their players cite it as **the** fix for an arcade wheel on a thumbstick, **more often than a curve**.
Ours defaults to `100%`.

That is the same "ship the defect and ask the player to find the fix" §3.5 explicitly rejects for
Response, applied to a different option, which is why the survey moved a default into contention rather
than merely confirming one.

### What it changed, and it is only what gets compared

Two arms added to the hand-check, both head-to-heads because both are close-run:

- **Test 3** gains `Slight` vs `Medium` back to back (answer 1b). Practice recommends a *mild* expo;
  `Medium` (γ 1.70) is one notch above that, `Slight` (γ 1.30) is it. They are the closest pair on the
  ladder, so a straight five-value sweep is the worst way to separate them.
- **Test 5 is reframed** from "is Range worth keeping?" to "should `80%` be the **default**?" (answers
  3a/3b). The old framing could only ever drop values from the list; it had no way to promote one.

⚠️ **The survey decides neither, and the docs say so in both files.** It is other people's taste on
other hardware — exactly the class of question §5.3 exists to answer with a pad in hand. 5 % deadzone
is the one number it does not argue with (practice clusters 5–8 %).

### 🚨 The caveat that makes the survey's numbers quotable at all: §3.3

**None of those emulators sits inside MAME**, so none has `joystick_deadzone 0.15` /
`joystick_saturation 0.85` applied downstream of its own shaping. The pre-compensation is what makes
our percentages mean the same thing as theirs — without it a 5 % deadzone stacks to ~19 % effective
with the top 15 % of travel a flat plateau, which is precisely the defect step 2 measured and removed.
Written into §3.5 and the hand-check, because a future reader lifting a number from a Supermodel ini
would otherwise get it wrong by 14 points and by a plateau.

### Two things named that do not exist, both deliberately not started

- **An output slew limit** ("steering filter") — standard practice alongside a curve, and added to §6
  rather than to the steps. The reason is the guard: `steer_shape()` is a pure function of the current
  sample and **a centred stick returns a hard zero at every setting** is what keeps every fixture
  byte-exact. A limiter converges to zero but not in one frame. No baseline actually moves (no accuracy
  fixture scripts an analog axis), so it is not blocked — but the guard would have to be re-argued
  rather than re-run, which is a step of its own.
- **A rate path for digital input.** 🚨 **This flips step 6's reason and the new one is stronger.**
  Rate was deferred for needing per-port accumulators; practice says the **positional model wins for a
  thumbstick** and rate is the fallback for **d-pad and keyboard**. So it was not the better model
  postponed — it was the wrong model for the control it was scoped against, and what is left of it is a
  different feature aimed at controls that today get no shaping at all.
  🚨 **And it is a layout question first: `daytona` spends its entire d-pad on VR1–4**
  (`input_layouts.json`), so on this phase's own testbed there is no digital control left to steer
  with. `srallyc`'s d-pad is free.

### Step 5 is still the next step and is still the user's

Unchanged by any of this — the hand-check is longer by two arms (~40 minutes, was ~30) and its answer
block grew two lines. Nothing before it moved.

## 2026-08-08 (4) — the pad's own analog stick, measured: it clips, it is not uneven

**No file in `src/` touched, nothing rebuilt, no digest** — the deliverable is a `devnotes/`-only tool
and three logs. The user's Bluetooth pad "looked wonky" on the stick and the browser gamepad page
showed nothing at all, so the question was whether the controller outputs its analog axes evenly.

New, and it never ships: **`devnotes/tools/sticktest.c`** + `build-sticktest.sh`, an SDL2 terminal
tester. **SDL2 deliberately, not a browser or IOKit** — RetroArch reads the pad through SDL, so this
measures the same values the core will receive. Raw `SDL_JoystickGetAxis` int16, no mapping, no
deadzone. It draws a live polar envelope in ASCII and writes `sticktest.log`: one CSV row per *change*
in raw axis values, then a summary block with per-axis min/max/rest/distinct-values/step, per-stick
roundness, reach symmetry, drift, rail-hit counts, and the full 72-sector envelope with visit counts.

### 🚨 The finding: the envelope is a clipped SQUARE, and the stick itself is clean

Device is a Switch Pro Controller. Full sweeps, 72/72 sectors swept on both sticks:

| | roundness | min r | max r | drift | rail hits | distinct |
|---|---|---|---|---|---|---|
| HIDAPI on, left | 0.7114 | 1.0038 | 1.4110 | 0.0103 | 12 % | 551 |
| HIDAPI on, right | 0.7756 | 1.0038 | 1.2941 | 0.0043 | 19 % | 630 |
| HIDAPI off, left | 0.7097 | 1.0037 | **1.4142** | 0.0021 | 41 % | 295 |
| HIDAPI off, right | 0.7097 | 1.0036 | **1.4142** | 0.0000 | 17 % | 161 |

Three numbers say it is not an unevenness problem. **Minimum radius is 1.0038 across all 72 sectors** —
there is no direction that fails to reach full scale, which is the opposite of a flat spot. **Maximum is
1.4142 = √2**, both axes railed at once on the corner. And **roundness 0.7097 against 1/√2 = 0.70711**,
which is what a perfect square scores.

**Proved rather than eyeballed:** a stick saturating in every direction traces
`r = 1/max(|cos θ|,|sin θ|)`. Measured envelopes deviate from that analytic square by **rms 0.0299,
worst 0.058** (HIDAPI off). So the rim being traced is the *electrical clip*, not the plastic gate.

🚨 **The tell that makes it conclusive is an accidental control: with HIDAPI off, both sticks report
identical statistics to four decimal places** — roundness 0.7097, mean 1.1460/1.1461, sd 0.1337 each.
Two different physical sticks cannot agree that closely. What the instrument is measuring there is the
clip geometry and nothing about the sticks.

⚠️ **`SDL_JOYSTICK_HIDAPI=0` is a genuinely different driver, which is why it is a control and not a
repeat**: it enumerates as `Pro Controller`, **4 axes / 18 buttons / 0 hats**, against
`Nintendo Switch Pro Controller`, **6 axes / 16 buttons / 1 hat**. Both clip. So the clipping is not
SDL's Switch calibration — that was the hypothesis going in and it is wrong.

**HIDAPI on is the better path and is already RetroArch's default**: 551 distinct values against 295,
and 12 % of samples railed against 41 %. Twice the resolution, a third of the clipping.

### 🚨 What it decides for the hand-check: do NOT default `model2_steering_range` below 100 %

This lands directly on entry (3) above, which put `80%` into contention as a default on the strength of
Supermodel practice. **That argument assumes a pad that *under*-reaches; this one over-reaches.** Range
below 100 % makes full lock arrive earlier in stick travel, and this pad already saturates before its
gate in every direction — so lowering it stacks a second plateau on a measured one. Keep `100%` here.

⚠️ **And it changes how test 5 must be read**: a Range preference measured on this pad would be
measuring the clip, not the player's taste. Noted in [steering-handcheck.md](steering-handcheck.md).
Deadzone `5%` is confirmed sound from the other side — worst drift is 0.0103, comfortably inside it.

### ⚠️ The instrument lied first, and the lesson is about sample density

The first run reported `roundness 0.210` with sectors reading exactly `0.00` at 32° and 62°, neighbours
at 1.05–1.10. That read as a hardware notch and was not. **The pad reports on change, not on a clock**,
so the log carried ~5 movement samples a second: measured median angular step at the rim **5.7° against
a 5° sector width, with 53 % of steps skipping a whole sector**. A physical notch cannot be one sector
wide with full reach either side — that was the tell, and it was in the data before the diagnosis was.

Two fixes, both in the tool: the envelope now **fills along the segment between consecutive samples**,
and **only sectors taken past half the observed reach count toward roundness**, so an incomplete sweep
reports as `N/72 swept` instead of impersonating a defect. A trigger axis is also no longer paired with
its neighbour into a meaningless "stick 2" (it needs >8 distinct values to qualify).

⚠️ **Its roundness grade still colours a clipped square red as "UNEVEN".** That is true of the
magnitude and misleading about the cause; read `min r` and `max r` before the colour.

### Two smaller things worth keeping

- **The summary is written on SIGINT only.** Two runs were lost to the Terminal window being closed
  instead — the CSV rows survived (the analysis above was reconstructed from them once), but the
  summary block did not. Ctrl-C, don't close.
- **`argv[1]` sets the log path**; default is next to the binary, so a run launched from `$HOME` still
  writes into `devnotes/tools/`.

Logs kept: `sticktest-run1.log` (the sparse one, superseded), `sticktest-run2-hidapi.log`,
`sticktest-run3-nohidapi.log`.

---

## 2026-08-08 (5) — the hand-check answered: **`Slight` is the default now**, and `vf2`'s "no inputs work" is not the steering chain

Step 5 of [steering-curve.md](steering-curve.md) — the one step that is a person with a pad — came
back. Three of its four decisions are made, one of them against the survey, and the fourth is a bug
report about something else entirely.

### The answers, in the player's words

- **Response: `Medium` "feels awful and twitchy and barely better than linear". Ship `Slight`.**
- **Deadzone: "fine, clear that part"** — `5%` unchanged.
- **Range: "needs to be 100%, the end"** — unchanged.
- **`vf2` (Test 6, the negative control): "no inputs work, wow."**

🚨 **The Response answer is the *too strong* failure and it wears the too-weak one's clothes.** "Barely
better than linear" reads as "not enough curve" and is the opposite: a strong γ buys a fine centre with
a coarse outer travel, so a correction that leaves the middle overshoots — darty, exactly as `Linear` is,
at a different part of the sweep. Worth remembering before anyone reads that sentence as an argument for
`Strong`.

**The survey guessed `Slight` and did not decide it.** Its other contention — that `80%` Range should be
the default, the thing Supermodel players tune most — is now settled **against**, twice over: by the
player outright, and by 2026-08-08 (4)'s stick tester, which found this pad clips *before* its physical
gate, so `80%` would have stacked a second plateau on a measured one.

### What changed in the tree

Two numbers and their prose. `DEFINITIONS[]`'s default and `get_steering_response()`'s fallback go
`STEER_MEDIUM` → `STEER_SLIGHT` in `retro_options.cpp`; `detail::g_opt_gamma` goes `1.7f` → `1.3f` in
`m2vk_steer.h`. ⚠️ **Those two must agree and nothing checks that they do** — the header cannot see
`DEFINITIONS[]` without dragging `libretro.h` into a header the emulation thread includes, which is why
the comment above `g_opt_gamma` names the file to keep it in step with. `Model 2 Steering.command`'s
reset arm follows.

**No pixel and no upstream line.** The guard is the steering fixture, not `vf2`: `ab.sh srallyc 2500`
reproduces `49f86e1309ca422b` / `6fcc26a931ab2b01` / `172bb47c8ba8f383` byte-exactly, covered 136116,
SSIM covered 0.988401 — which holds for the reason it held when shaping became the default at step 3,
namely that a centred stick returns a hard zero at every setting and no accuracy fixture scripts an
analog axis. The resolve report reads `gamma=1.30 (core option)` on `daytona`.

### `vf2` — measured, and the steering chain is ruled out

The symptom is *no input at all*, which the chain cannot produce: it multiplies one axis, it does not
gate a pad, and `vf2` declares no `IPT_PADDLE` so `steer_shape()` is the identity there anyway. Measured
rather than argued, on the binary that session played (08:40) and under that session's exact options
(`vulkan / 1440x1080 / blended / steering_display=on / Response=Linear`), `retrohost --vk` over 2600
frames:

| arm | digest |
|---|---|
| no input | `a3854c40fd484bbe` |
| coin ×2 + Start | `cc5a5af7f6deb4de` |
| + B, A, d-pad left | `4a1c70243d1974e3` |

Three distinct digests: coin, Start, face buttons and d-pad all reach the machine and change the
picture. The same three-way split holds on the software path (`203a2c657145fca3` / `ba3b7180e8d6c86a`).

**So the fault is in the RetroArch session, not the core**, and by the standing ban it is the user's to
narrow — it is a felt check on a live frontend. What the session's own log
(`/tmp/m2vk-steering.log`, 30 s of content) does say: `[Autoconf] Pro Controller configured in port 1`,
the `mfi` joypad driver, **no `[model2] port N set to device` line at all** (so port 0 stayed on the
default `RETRO_DEVICE_JOYPAD`), no remap files on disk, and `input_descriptor_hide_unbound = false`.
Nothing there is a core-side gate. ⚠️ Note also that the steering tests exercise the **stick and the
triggers**; `vf2` is the first game in the check that needs the **face buttons and the d-pad**, so
"steering worked" is not evidence those were ever bound.

### Still open in the hand-check

Test 7 (`srallyc` under the new default) and Test 8 (`waverunr`'s handlebar — ANSWER 4, which decides
whether the detector gains a per-game override).

### ⚠️ Correction, same evening: `vf2`'s inputs work — Test 6 PASSES

Withdrawn by the user on retrying with the pad. The section above stands as the measurement and its
conclusion was right as far as it went ("the fault is not in the core"), but the framing — a live bug to
narrow in the frontend — was wrong: there was no fault. **The tell was available before any of it was
written and is the thing to carry: the reported symptom could not have been the named mechanism.** Test 6
fails as *mushy* movement, and "no input at all" is not something a chain that multiplies one axis can
produce. Check whether the mechanism can express the symptom before investigating a bug test's failure as
that bug.

The three-digest table stays useful: it is Test 6's claim measured from the code's side, and it now has
the player's side agreeing with it.

## 2026-08-08 (6) — Rate mode: built, played, **removed**. Supermodel read properly. No net code change.

Asked for directly after the shipped `Slight` default still felt bad. Ended the evening with the tree
byte-identical to where it started, which is the point of writing this up rather than nothing.

**Supermodel's steering, read from source** (`~/Documents/GitHub/Supermodel`, clean at `77d28ee`; the
sibling emulators are all already checked out there — do not re-clone). §2 of steering-curve.md
reproduced exactly, and the negative finding is the important one:

- `CJoyAxisInputSource::ScaleAxisValue` (`Src/Inputs/InputSystem.cpp:2484-2510`) is one call to
  `Scale` (`Src/Inputs/InputSource.cpp:74-129`), which is **two straight line segments**. Per-axis,
  per-direction deadzone and saturation (`:2476-2481`), defaults **3 %** and **100 %**
  (`InputSystem.h:59-66`). **No curve anywhere in the chain.**
- 🚨 **Their README recommends our `model2_steering_range` by name** (`Docs/README.txt:1110-1112`):
  *"For playing driving games with a game pad, it is sometimes a good idea to use a value larger than
  100 % so that the steering feels less sensitive on a thumbstick."* Saturation 150 % = our `70%`, and
  **full deflection then genuinely cannot reach full lock** — the saturation point is past what the
  stick can physically produce, so the top third of the wheel is unreachable. Ours does the same by
  the same arithmetic (`a *= range_opt`).
- Their `Sensitivity` config section is *only* the digital ramp (`InputSystem.cpp:2314-2343`,
  attack 25 / decay 50, 10000 full scale) plus mouse deadzone. ⚠️ Their README says deadzone defaults
  to 2 % and the code says 3 — their drift, noted so it is not read as ours.

**The arithmetic that made Rate look worth building, and which still holds:** sensitivity under a
gamma is `96·range·γ·u^(γ-1)` counts per unit of stick, so γ>1 moves sensitivity **outward**. At 80 %
stick, `Slight` is 22 % *more* sensitive than plain Linear and `Medium` 46 % more. With full lock
pinned to full stick the average slope is fixed and a curve can only decide where it sits — which is
why no γ removes the twitch, and why `Medium` played back as the too-strong failure.

**Rate mode was built on that reasoning and it was wrong anyway.** Two options
(`model2_steering_mode`, `model2_steering_rate_speed`), per-port accumulator in the pad device, gamma
shaping the rate, unwind on release. It passed every guard — `ab.sh srallyc` byte-exact, `vf2`
untouched by the detector, all four override arms, integrator matching `M2VK_STEER_LOG` exactly. Then
it was played: **"unusable and atrocious"**, at a first speed band 4x too slow *and* at a second band
chosen by the user. **Removed in full; see steering-curve.md §4 step 6 for why it is closed rather
than shelved.** `ab.sh srallyc 2500` back to `49f86e1309ca422b` / `6fcc26a931ab2b01` /
`172bb47c8ba8f383`, covered 136116, SSIM 0.988401; ten core options; upstream diff untouched.

🚨 **A process failure worth more than the feature was.** A 4000-frame `daytona` race was scripted to
compare Rate against Direct — i.e. the exact automated button-press testing banned at the top of
CLAUDE.md — and it got past that ban by being reasoned about as *a digest test*, which is the
sanctioned category. **The category is decided by the question being asked, not by the read-out being
collected.** The cheap version was vacuous too: the same comparison on daytona's attract screens gave
identical digests, because steering moves no pixel there. Build it, run the static guards, then hand
the user a numbered list and wait.

---

## 2026-08-11 — steering damping: an input-layer slew limit (`model2_steering_damp_drive` / `_return`)

The user timed the official Sega Model 2 emulator's displayed wheel on Daytona: slamming the stick
reaches full lock in **~4 frames at 60 fps**, releasing recentres in **~7**. A physical stick can hit
either end in one frame, so the reference is applying a **rate limit in the input layer**, before the
game reads the axis — and the asymmetry (faster out than back) is a self-centring wheel: forced
deflection beats spring return. This is the "output slew limit" steering-curve.md §6 had deferred over
the byte-exactness guard. The user confirmed it's input-layer and asked for it as an option, choosing
**two knobs** (separate drive/return) over a single preset so the ratio itself is tunable.

**Built, twelfth core option pair. UNCOMMITTED.** Six files, all `src/osd/libretro_m2/`:
`m2vk_steer.h` (the `steer_damp()` stage + two option globals + two switches), `libretro_m2_input.{h,cpp}`
(per-seat carry `m_steer_damp`, wired after `steer_shape()` in `shape_and_publish_steer()`),
`retro_options.{h,cpp}` (the two definitions + `frames_option()` parser + getters), `retro_entry.cpp`
(read/set at load and live, two switches in the override-log loop, `damp=drive/return` in the options
line). **No upstream file, no shader, no pixel; the diff against mame0288 is unchanged at 30 lines.**

**The model** is a hard rate limit, not an exponential low-pass — the user's crisp "reaches the side at
frame 4" rules out a low-pass, which would creep. Per frame:
`step = (|target| < |state|) ? return_step : drive_step; state += clamp(target-state, -step, step)`,
in ±STEER_ABS_MAX units, `step = STEER_ABS_MAX / frames` (0 frames = instant sentinel). It rides on
top of `steer_shape()`'s output, so the curve decides *where* the wheel goes and damping decides *how
fast*. Crossing centre in one motion counts as growing toward the far lock, so a full left-to-right
sweep runs at the drive rate throughout — no slow patch at centre.

**Defaults `Off`/`Off`, deliberately** — step 5's hand-check picks the real value (the Medium→Slight
precedent). The measured reference is **4 drive / 7 return**; in our ~57.5 Hz frames vs the 60 fps it
was timed at that is a <7 % rate difference, imperceptible, so 4/7 is where the hand-check starts, not
a shipped default.

🚨 **The no-op guard held without re-arguing, which was the whole worry.** `steer_shape()` stays a pure
function of the current sample; the *state* moved into the pad device. `steer_damp()` is an exact
identity when both rates are `Off` (default) and under `M2VK_STEER_LINEAR`, and it tracks its target
the instant damping is off — so a centred stick still reaches MAME as a hard zero on frame 1. Since no
ab-baselines fixture scripts an analog axis, the limiter never leaves rest on any of them. Measured:
`ab.sh srallyc 2500` (a steering fixture) reproduced `49f86e1309ca422b` / `6fcc26a931ab2b01` /
`172bb47c8ba8f383` (covered 136116, SSIM 0.988401) and `ab.sh vf2 2500` its baseline (SSIM 0.996985),
byte-exact.

**Positive check by read-out, not gameplay.** `M2VK_STEER_LOG=1 M2VK_STEER_DAMP_DRIVE=4
M2VK_STEER_DAMP_RETURN=7` on a scripted full-right-hold-then-release `daytona`: `shaped` climbs
`0.25→0.50→0.75→0.85` (four frames, drive step 65536/4) and the port `p1` tracks
`0x080→0x08e→0x0b0→0x0d2→0x0e0`; on release it falls in 1/7 steps `0.85→0.71→…→0` over ~7 frames. The
`*` lag markers appear only while raw≠shaped. `M2VK_STEER_LOG` also prints a damping line at resolve.

⚠️ **Still the user's, step 5:** the hand-check that picks the default drive/return (start 4/7), and
whether it wants a per-game override for the motorcycle/jet-ski sets. Do NOT script gameplay for it —
build it, run the static guards (done), hand a numbered list, wait.

**Next:** the hand-check list handed to the user; then fold the chosen defaults in (two DEFINITIONS
defaults + the log) and commit the steering work.

---

## 2026-08-20 — steering defaults finalised: Range → `80%`, damping → `4` drive / `8` return

The user came back with a direct answer rather than a further hand-check session. Five `DEFINITIONS[]`
entries in `retro_options.cpp` now read: `model2_steering_response` = `Slight` (unchanged, from
2026-08-08), `model2_steering_deadzone` = `5%` (unchanged), **`model2_steering_range` = `80%`**
(reversing the 2026-08-08 hand-check's `3b` answer of `100%`), **`model2_steering_damp_drive` = `4`**,
**`model2_steering_damp_return` = `8`** (both were `Off` since the 2026-08-11 damping feature shipped).

⚠️ **The Range reversal is not a re-run of Test 5** — [steering-handcheck.md](steering-handcheck.md)'s
5a/5b transcript is untouched and still says `100%`, on a pad that steering-handcheck.md §5 itself
flagged as clipping before its physical gate (measured on `sticktest.c`), which is exactly the
confound that would make an `80%` preference read as "no" on that particular controller. This is a
straight instruction, not a retraction of that finding — both are recorded, in that order, in
steering-handcheck.md.

**`get_steering_range()`'s hard-fallback (used only if the frontend returns something unparseable)
moved to `0.8f` alongside the `DEFINITIONS[]` default**, matching the existing discipline for
`get_steering_response()` (whose fallback is `STEER_SLIGHT`) and `get_steering_deadzone()` (`0.05f`) —
the header/table agreement rule from earlier sessions. `frames_option()`'s hard-fallback for the two
damping keys stays `0` (unparseable → instant) rather than being repointed at `4`/`8`; that fallback
only ever fires on a malformed `.opt` value, and it predates this change.

**Verified byte-exact, both fixtures**, after a full `REGENIE=1` rebuild: `ab.sh srallyc 2500` reports
digest `172bb47c8ba8f383`, covered 136116, SSIM covered 0.988401 — identical to every prior recording —
with the run's own `options:` line confirming the new values resolved
(`model2_steering_range=80% model2_steering_damp=4f/8f`). `ab.sh vf2 2500` likewise reproduces its
SSIM-covered 0.996985 baseline with exit criterion 1 holding. Expected: no `ab-baselines.md` fixture
scripts an analog axis, so a default-only change cannot move a digest, damping or not.

No upstream file, no shader, no new file; diff against mame0288 unchanged. Still uncommitted, along
with the rest of the damping feature from 2026-08-11 — **next step is committing the whole steering
block** (curve + display bar + damping + these five finalised defaults) as one piece of work.


---

# Worklog archive — System 22 / Super System 22 and System 21, closed

All committed by HEAD `6e62265dff6` (2026-08-25). S22: S0 boot → S1 seam → S2a–d GPU geometry
(untextured → textured → 2D-over → shading/gamma) → sprites → per-quad scissor → `system22_texture_filter`
+ per-family option-visibility. S21: T0 boot → T1 seam → T2 GPU geometry (real z-buffer + layer-0 z-mix)
→ T3 family routing/options → T4 Winning Run → T5 savestates/pads/A-B/compat. Newest at the bottom.

## 2026-08-22 — Track B cleanup, and System 22 port begins

- CLAUDE.md cut 2429 → 300 lines: working brief (state, next step, load-bearing gotchas, build/run,
  option table) with phase detail delegated to `devnotes/*.md`. Removed the "Where we are" mega-block
  and every `✅ … DONE` monument. Fixed the false "30 lines" upstream-diff claim everywhere — the
  measured figure is **135 insertions / 2 deletions across 5 files** (`git diff --shortstat mame0288
  -- src/devices src/mame`, 2026-08-22).
- worklog archived: 7283 lines of closed Model 2 history moved to `worklog-archive.md`; this file reset.

## 2026-08-22 — S0: ridgerac boots as a software core

`ridgerac` renders full 3D in software through the **shared** `libretro_m2` OSD (no sibling OSD; the
shell is driver-agnostic). New subtarget = `src/mame/namcos22.flt` + `scripts/target/mame/namcos22.lua`
(generator output + one `MACHINES["GEN_FIFO"]` line the shared savestate module needs); one OSD edit
names the core `<subtarget>_libretro` so the two drivers don't clobber each other's dylib
(`model2_libretro.dylib` unchanged, new `namcos22_libretro.dylib`). First 3D at ~frame 200 (~3.3s,
vs vf2's ~16s); RAM-test POST at ~frame 40. **Software baseline: digest `2678231fae7f3aa6` over 1800
frames**, reproducible across cold boots. Detail + boot-lag table in
[s0-software-boot.md](s0-software-boot.md). Next: **S1** — tap the quad/sprite stream and frame
brackets, draw nothing, keep output byte-identical; record the real upstream diff.

## 2026-08-22 — S1: System 22 seam + passthrough

Tapped the quad/sprite stream and frame brackets in `namcos22_v.cpp`, recording, drawing nothing.
`ridgerac`'s software digest is unmoved (`2678231fae7f3aa6` / 1800 frames) **with the tap on as well as
off** — observation-only, no render mutation. Three guarded `#ifdef S22VK` sites (quad before
`render_triangle_fan`; sprite per node; `frame_begin`/`frame_end` inside `render_scene`, which covers
both `screen_update` variants from one function — there are only two call sites, not three). New
driver-type-free files `src/osd/libretro_m2/s22_seam.{h,cpp}`; the sink is compiled in the driver
project for every OSD (System-22-specific, kept out of the shared OSD). `namcos22.lua` gained
`defines{S22VK}` + a `files{}` block. Model 2 build untouched. The S1 tap (`M2VK_S22TAP=1`) proves the
hooks fire: ridgerac 1.69M quads / 1755 scenes, raverace 948k, acedrive 1.94M — all quads, **zero
sprites** (all three ROMs are plain-S22 racers; the sprite hook is wired but awaits an SS22 game).
**Upstream diff: namcos22_v.cpp +24/-0; total vs `mame0288` now 159/2 across 6 files** (was 135/2 / 5).
Detail in [s1-seam-passthrough.md](s1-seam-passthrough.md). Next: **S2** — GPU geometry, untextured
first.

## 2026-08-22 — S2a: System 22 GPU geometry, untextured

`ridgerac`, `raverace`, `acedrive` render their 3D on the GPU through the shared Vulkan path — flat
Gouraud-shaded untextured quads, in their base palette colour (`pens[0]`) scaled by the per-vertex
hardware brightness. Clear first light: the ridgerac attract (road, wall, sky, the little red car) and
the f1600 waving checkered flag both project and shade correctly. Depth is a **painter's algorithm** —
the System 22 tree is walked back-to-front, so the pass draws in record order with the depth test off
and never touches the ring's (Model 2's) depth attachment; no depth inversion in any of the three.
The seam suppresses software 3D (`sw_owns_3d()` gates the `render_triangle_fan` calls) and the GPU
draws over MAME's finished 2D frame via the passthrough path — so 2D overlays that belong over the 3D
(ridgerac's banner) are covered for now, a later step. New files
`src/osd/libretro_m2/renderer_vk/s22_geom.{h,cpp}` + `shaders/s22.{vert,frag}`; the seam
(`s22_seam.{h,cpp}`) **moved from the driver project into the shared OSD** so the Model 2 build resolves
the `s22::` symbols too (inert there). Model 2 unaffected — links and presents unchanged (vf2 vk 1200
frames, no S22 capture). **Upstream diff: namcos22_v.cpp +26/-0 (+2 for the guard); total 161/2 across
6 files.** Detail + screenshots in [s2-gpu-geometry.md](s2-gpu-geometry.md). Next: texture tail, then
2D-over compositing, sprites and the SS22 shading path.

## 2026-08-22 — S2b: System 22 texture tail

The quads now sample the tile texture per fragment instead of the flat `pens[0]`. `s22.frag`
transliterates `renderscanline_poly`'s fetch: perspective-divide the screen-linear `uoz/voz/ooz`
varyings, walk `ttmap`/`ttattr`/`ayx`/`ttdata` to a pen, apply the `cmode` pen mask/shift and the
palette, then the per-pixel shade (`scale_imm_and_clamp(shade<<2)`). The ridgerac attract road,
buildings, guard rail and sky now match the software reference closely; the Rave Racer title logo
(flame/metal/stone) reads correctly. Fog/fade/poly-alpha are still not applied (the SS22-tail step), so
a fogged scene still differs. The tile system (`ttmap` 2 MB, `ttattr` 1 MB, `ttdata` 16 MB, `ayx` 4 KB)
is static and uploaded once into buffers shared by every slot; only the 128 KB palette is re-uploaded
per frame. The driver hands the pointers over a new `s22::set_texture_ram()` call in the frame bracket.
Pipeline gained a 5-buffer descriptor set and subsumes the S2a untextured pipeline (one pipeline, flag
switches). Model 2 unaffected (vf2 vk 1200 frames, no S22 capture). **Upstream diff: namcos22_v.cpp
+29/-0; total 165/2 across 7 files.** Detail + screenshots in [s2-gpu-geometry.md](s2-gpu-geometry.md).
Next: 2D-over compositing (the HUD the 3D covers), then SS22 fog/fade/alpha and sprites.

## 2026-08-22 — S2c: System 22 2D-over compositing

System 22 now uses Model 2's UNDER/OVER sandwich instead of riding the passthrough: UNDER 2D → GPU 3D →
OVER 2D, all three draws in one render pass (`record_and_submit`). UNDER is the finished 2D frame
(background + text baked, software 3D stripped); the GPU 3D draws over it; the text/HUD is redrawn on
top. ridgerac's "TODAY'S BEST" banner (+ subtext) and "CREDIT 0/2" now sit above the road/wall exactly
as the `M2VK_SW_3D=1` reference at frame 1000, instead of being painted out by the 3D (the S2b bug). The
OVER layer is captured by a new guarded `s22::capture_over(bitmap, screen.priority(), prival, cliprect)`
hook at the end of each `screen_update` (prival 2 plain, 6 SS22): priority-buffer pixels equal to prival
become `0xff000000 | bitmap[x]` (the mixed, gamma-corrected text pen), the rest transparent; the high
byte is forced 0xff so pure-black text is not discarded by `overlay.frag`. Buffer lives in
`s22_seam.cpp` (inert in tap/Model 2 builds); `present_frame` reads it via `s22::over_pixels()` into the
existing `slot.layers[LAYER_OVER]` staging (no new Vulkan resource). Faithful for opaque HUD; known
limits (alpha/shadow text blends against UNDER not the 3D; behind-poly text still shows; SS22
sprite-in-tree z not resolved) are acceptable for the racer set and noted in the doc. Model 2 unaffected
(vf2 vk 1100 frames, no S22 capture). **Upstream diff: namcos22_v.cpp +44/-0; total 180/2 across 7
files.** Detail + screenshot in [s2-gpu-geometry.md](s2-gpu-geometry.md). Next: SS22 fog/fade/alpha,
then sprites.

## 2026-08-22 — S2d: System 22 shading tail (fog / fade / poly-alpha) + a GPU-path crash fix

The polygon fragment now finishes `renderscanline_poly` / `renderscanline_poly_ss22`: **poly fog**
(both variants, transliterated as integer `>>8` math to match `rgbaint_t`), and for Super System 22
**per-z fog** (the `recalc_czram` table), **poly fade**, **screen fade** and **per-pixel poly alpha**.
The two paths differ in order (plain S22 fogs before it shades; SS22 shades first). Only fog is per-quad
(rides two new flat vertex words); the SS22 globals — screen fade, poly fade, alpha factor/pen — are the
same every quad and ride the push constant via a new `set_shading_state()` frame-bracket hook; the four
z-fog tables ride a sixth per-slot storage buffer, re-uploaded each frame like the palette. Alpha is the
one fixed-function step: the fragment emits `(0xff-poly_alpha)/255` for an alpha pixel and `1.0`
otherwise, `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` does the mix; opaque pixels pass through bit-exact, alpha
pixels carry a small `/256`-vs-`/255` residual (expected, SS22 tail). **Spot is not the poly tail** (it
is a mixer-stage effect) — out of scope.

**Verified on `raverace`**: `ridgerac`'s attract never programs poly fog (the S1 tap, extended to count
fog quads, reports max fog 0 over 4400 frames), but Rave Racer and Ace Driver fog heavily (max 255/196,
~2000 of ~2800 scenes). Rave Racer's fogged bridge/tunnel matches `M2VK_SW_3D=1` closely — frame-aligned
by rendering both from a pristine `M2_SAVE_DIR` (bit-deterministic vk-vs-vk); exact-pixel diff is the
wrong tool (the no-fog title already differs mean ~41/255 from GPU-vs-scanline coverage, the fogged
tunnel is the same ~48, not a fog error). A fog sweep of all 18 sets then found the SS22 z-fog games
render heavily in attract too (a too-short frame count had hidden them): **Tokyo Wars' hazed highway
(SS22 z-fog) also matches software** (screenshots `2026-08-22-{raverace-…-s2-fog,tokyowar-…-s2-zfog}.png`).
Direct fog and z-fog are verified; screen fade and poly-alpha are wired and stable but show only lightly
in attract, so they stay an in-game hand-check — `devnotes/shortcuts/System 22 Fog A-B.command` is the
compare tool and its README lists which games fog.

**Crash fix (latent since S2a):** with the GPU owning the 3D, `poly3d_drawquad` allocates one
`object_data` per quad but enqueues no render unit, so `poly_manager::wait()` early-outs and never
resets its poly arrays — the arena grew every frame and `poly_array::next()` overflowed the stack after
~4000 frames (`ridgerac` SIGBUS at ~4200). One guarded line reclaims it:
`if (!s22::sw_owns_3d()) object_data().reset();`. Every earlier test ran ≤1800 frames, which is why it
went unseen. Model 2 unaffected (vf2 vk 1200 frames, zero `s22:` lines). **Upstream diff: namcos22_v.cpp
+84/-0; total 220/2 across 7 files.** Detail in [s2-gpu-geometry.md](s2-gpu-geometry.md).

## 2026-08-22 — S2d fix: the final gamma LUT (things were ~half as bright)

Playtest caught the GPU 3D reading much too dark on plain System 22 (and a "blue cast" on SS22). Root
cause: **both mixers run every output pixel through a gamma LUT as the last step, and the GPU path
skipped it.** The pens are not pre-gamma'd (unlike Model 2, which bakes gamma into its colour LUT), so
this had to be applied after all shading/fog/fade. Two sources: plain S22 uses a static ROM PROM
(`m_gamma_proms`, `namcos22_mix_text_layer`); Super System 22 uses mixer RAM at byte 0x100
(`screen_update_namcos22s`), which changes per frame and is u32-word ordered so the byte index is
swapped (`^3` on little-endian). The driver hands the active LUT over each frame (both members are
protected → fetched via public `memregion`/`memshare`); it rides a per-slot 768-byte buffer and
`s22.frag` applies it last, `^3` gated on the `ss22` flag.

Result, frame-aligned vk vs `M2VK_SW_3D=1` from a pristine save: **raverace 98.5 % exact / mean 0.14**
over the 3D (was mean ~49), every region's brightness ratio exactly 1.000; **tokyowar mean 58 → 23,
88 % exact**, ratio 1.000 (remaining residual = edge coverage on a busy scene + not-yet-drawn sprites +
the alpha-in-gamma-space approximation). The Tokyo Wars "blue cast" was this same missing gamma, not a
compositing bug (the SS22 LUT is per-channel); the missing SS22 top letterbox bar / wrong sky layer is
the genuine 2D-compositing item and is still open. **This corrects the earlier claim that the
plain-S22 mean-41/255 gap was GPU-vs-scanline coverage noise — it was almost all the missing gamma; the
true coverage residual is tiny.** Model 2 unaffected (vf2 vk 1100 frames, zero `s22:` lines). Upstream
diff: `namcos22_v.cpp` +99/-0; total 235/2 across 7 files. Review shots in
`devnotes/screenshots/s2d-review/`.

## 2026-08-22 — SS22 letterbox fixed: the per-quad clip window was a missing scissor

The Tokyo Wars "missing top letterbox / sky bleed" turned out **not** to be the UNDER/OVER 2D sandwich —
it was a missing per-quad scissor in the GPU 3D pass. `poly3d_drawquad` clips every quad to `m_cliprect`
(the scene viewport `vl/vr/vu/vd` ∩ visible area); a temporary dump at the seam showed tokyowar runs its
gameplay 3D in a `L0 R639 T112 B367` window (256-px-tall, black bars top and bottom), the sky being a
`direct=0` quad clipped to it. The GPU pass drew every quad full-frame, so the sky bled to y=0. Column-320
probe: software black to y=111 then sky `(184,223,255)`; the old GPU had sky from y=0.

Fix mirrors `vk_geom`'s scissor: the clip window crosses the seam per quad (`quad.clip_*` in
`s22_seam.h`, `+4` args at the `submit_quad` call), `geom_upload` groups consecutive quads sharing it
into `draw_batch` runs, `geom_draw` sets one scissor per run scaled to the attachment (floor top-left,
ceil bottom-right — outward, so a fractional internal-res scale never shaves a column) and restores the
full scissor before the OVER overlay. The rect is the *same* one `render_triangle_fan` clips to, so it
can only match software better. `M2VK_NO_SCISSOR=1` collapses windows to full-screen (the pre-fix
behaviour / attribution switch, as on Model 2).

Frame-aligned vk vs `M2VK_SW_3D=1` from a pristine save: **tokyowar 70.08 % → 99.25 % exact, mean 192 →
0.079, brightness ratio 1.000**. Plain S22 provably inert — ridgerac's full-run digest is bit-identical
with the scissor and with `M2VK_NO_SCISSOR` (`62db03fd8ee89035`), raverace still 99.30 %. Model 2
unaffected (vf2 vk 1200 frames, zero `s22:` lines). Upstream diff: `namcos22_v.cpp` +100/−0; total
**236/2 across 7 files**. Shots: `devnotes/screenshots/2026-08-22-tokyowar-{vk-s2-letterbox-fixed,sw-letterbox}.png`.
Still open: sprites (`render_sprite`) and the SS22 prival-6 sprite/text z (§A.3) — the GPU owning the 3D
means no polygon writes the priority buffer, so the prival-6 OVER capture is effectively empty; harmless
where SS22 text never overlaps the 3D (tokyowar's HUD is in the bottom bar), untested where it does.

## 2026-08-22 — S22 sprites on the GPU (the last unexercised seam)

Super System 22 sprites now render on the GPU, interleaved in z with the polygons, and the SS22
text-vs-3D compositing that was coupled to it is resolved. Sprites were the live open item: with the GPU
owning the polygons they had been left in software in the passthrough, so every sprite sat behind every
GPU polygon — wrong, because `render_scene_nodes` walks quads AND sprites in one back-to-front tree, so a
sprite can sit at any z among the polys.

**The ordering model.** Sprites join the SAME GPU record stream as quads, in tree-walk order (`s_order`
in `s22_geom.cpp`), so the painter's pass reproduces the interleave exactly. Per-tile hook at
`poly3d_drawsprite` (`s22::submit_drawsprite`, the sibling of `submit_quad`); the software
`render_polygon` is guarded off with `sw_owns_3d()`. Sprites reuse the polygon pipeline, vertex format,
palette buffer and scissor/batch machinery — an `ATTR_SPRITE` flag in `s22.frag` switches the fetch to
`renderscanline_sprite`'s (affine u/v in the perspective slots with ooz=1, pen 0xff = transparent, the
global-fog/screen-fade/alpha tail, SS22 final gamma). One new static storage buffer (binding 7) holds
the `"sprite"` ROM region (gfx(2), 32×32×8bpp tiles), uploaded once like `ttdata`; plain S22 has no
gfx(2), so it binds a placeholder and the sprite path never fires there.

**Text-vs-3D, solved with draw order not priority.** MAME decides text over/under each primitive from
the priority buffer (normal → text over it at prio 6; **prioverchar** primitive — poly `cmode&7==1`,
sprite `cz==0xfe` — covers the text at prio 7). With the GPU owning the 3D nothing writes priority, so
`capture_over` now grabs ALL SS22 text (priority 4, the tilemap's own value) as the OVER overlay drawn
above the 3D — text over every normal primitive. A second GPU pass (`geom_draw_over`, wired after the
overlay in `vk_present`) redraws just the prioverchar-flagged primitives over the text, reproducing the
prio-7 case. So text ordering is a per-primitive flag we already have at the seam; nobody writes 3D
priority. This is the case the driver comment (`namcos22_v.cpp:1712`) calls "trusted by testmode and
timecris".

**Verified** frame-aligned vk vs `M2VK_SW_3D=1` from pristine `M2_SAVE_DIR`s (bit-deterministic), every
region's vk/sw brightness **ratio 1.000**: **timecris** 0.11–0.29 % of pixels differ (mad 0.014–0.070),
**propcycl** 0.33–0.71 % (mad 0.075–0.107) — the expected edge-coverage + alpha-in-gamma-space residual.
Screenshots `devnotes/screenshots/2026-08-22-{timecris-vk-sprites,propcycl-{vk,sw}-sprites}.png` (propcycl's
gold score/time sprite frames, flying character, and the wooden-banner sprite with text over it all
composite correctly). Plain S22 provably inert: ridgerac digest `62db03fd8ee89035`, bit-identical to the
letterbox baseline, 0 sprite tiles. Model 2 unaffected (vf2 vk 1200 frames, zero `s22:` lines). Upstream
diff: `namcos22_v.cpp` +121/−0; total vs `mame0288` **257/2 across 7 files**. Detail in
[s2-gpu-geometry.md](s2-gpu-geometry.md).

**Follow-up same day — `M2VK_NO_3D` fixed for System 22.** It had never suppressed the S22 software 3D:
`set_gpu(false)` couples "GPU off" to `sw_owns_3d() == true`, so the rasteriser kept drawing the 3D into
the "background" (coverage read ~0.3 %). New `s22::set_no_3d()` (seam) hands `sw_owns_3d()` back false as
well, so neither path draws and the picture is just the 2D layers; `retro_entry` calls it under
`M2VK_NO_3D`. Now the bg-based harness works on S22: timecris frame 2200 `ppmdiff coverage` reports 49.98 %
of the picture covered, **coverage agreement 1.0000** (GPU covers exactly software's pixels, sprites
included), 99.79 % same colour, and `exact` holds on the 2D-only pixels. Model 2's own `M2VK_NO_3D` path
(`m2vk::set_rasterize`/`vk_geom`) is untouched and still inert (vf2 vk, zero `s22:` lines). This is a
new-file/OSD change plus a two-line `retro_entry` swap — the upstream diff is unchanged at 257/2.

---

## 2026-08-23 — S22 texture filtering (bilinear), first per-family option

First S4 option: **`system22_texture_filter`** (Off/On), a bilinear smoothing pass on the textured 3D
poly tail — System 22 point-sampled its textures, so this is an enhancement, off by default. In
`s22.frag` the texel fetch is factored into `texel_pen`/`pen_to_rgb`, and under a new `tex_filter`
push-constant bit the textured branch takes 4 taps, resolves each neighbour through the palette, and
bilerps in **RGB space** (the pens are indices — cannot be averaged before the LUT). The alpha-pen test
stays on the point-sampled centre pen, so cutout/alpha shape is unchanged; sprites stay point-sampled.

Also built the **per-family option-visibility mechanism** the S4 plan wanted: `m2opt::hide_option(key)`
+ filtered declare in all three forms; `retro_entry` hides the option on the Model 2 build (positive
family detect, same `driver_list::find("ridgerac")` gate as `set_native_resolution`). `s22_geom` parks
the option (`set_option_filter`); `M2VK_S22_FILTER=0|1` overrides at draw time; applies live (push
bit, no rebuild).

**Verified:** off is a bit-exact no-op — ridgerac 1800f vulkan digest `000263dec4db0fa1`, identical to a
build with the shaders reverted to HEAD. On differs (`74b1a2b0d88545e9`), so it reaches geometry.
Model 2 declares 12 options with `system22_texture_filter` absent; namcos22 declares it. Both cores
build clean. In-game look is the user's hand-check. No upstream-file change — all in the OSD (new/edited
new files + shaders).

## 2026-08-23 — T0 confirmed: Star Blade (System 21) boots to 3D in software

Built the standalone `namcos21` subtarget (untracked `namcos21.flt` + `namcos21.lua` from last session,
`.lua` deps verified by a clean build) and confirmed `starblad` renders 3D in software through the shared
`libretro_m2` OSD.

The one real blocker was **not** ROMs or boot — it was frame capture: namcos21_c67 draws a palettized
`bitmap_ind16`, but the OSD's `capture_frame()` accepted only `BITMAP_FORMAT_RGB32`, so every frame was
dropped and the core timed out with `'starblad' failed to start`. Added an IND16 branch that resolves
each index through `screen->palette().pens()` to XRGB8888 (our own OSD file; no upstream hook). RGB32
path unchanged — Model 2 and namcos22 unaffected.

Software baselines (cold boot, NVRAM cleared, deterministic across two runs, 496×480): 1800f
`5bd8e9631fddfcf6` (title/insert-coin, 2D only) and **4500f `8ae63fbb7bd812fa`** (attract "HALL OF GREAT
FIGHTERS" — flat-shaded 3D floor + horizon with C355 sprites over). T1's passthrough baseline is 4500f;
1800 never reaches `blit_single_quad`. Details in [t0-software-boot.md](t0-software-boot.md).

T1 frame bracket decided: `screen_update` (namcos21_c67.cpp:464), not the device copy. Next: write
`s21_seam.{h,cpp}` + the three guarded `S21VK` hooks, record and draw nothing, prove the 4500f digest
unmoved.

## 2026-08-23 — T1 + T2a: Star Blade 3D on the GPU (System 21)

**T1 (seam tap, done).** Two guarded hooks in `namcos21_3d.cpp` (29 insertions): a `frame_end/begin`
bracket at the framebuffer swap and a quad tap at the end of `blit_single_quad` (both quad sources funnel
through it; the device + swap are shared by all three S21 drivers, so one tap covers the family). New
`s21_seam.{h,cpp}`, `S21VK` scope. Records the stream, draws nothing — 4500f digest byte-identical
(`8ae63fbb7bd812fa`) with the tap on. The stream confirmed the design: flat shading, pre-projected,
per-quad depth (the edge-interpolated `.z` in `rendertri` is dead), pens `0x2100..0x3fff`.

**T2a (untextured 3D on the GPU, done).** New `renderer_vk/s21_geom.{h,cpp}` + `s21.vert`/`s21.frag`:
16-byte vertex, palette CLUT, and a **real per-quad z-buffer** (`z = 1 − zsort/32768`, clear 0.0 /
`COMPARE_GREATER` / write — S21's accurate model, unlike S22's painter's pass). Seam grown with
`set_gpu`/`sw_owns_3d`/`record_*`/`set_palette` + the `over_*` overlay; the record is **double-buffered**
(the single swap site fires `frame_end`+`frame_begin` back-to-back, which zeroed a single-buffer record —
the first-light bug). `namcos21_c67.cpp` skips the software 3D and, via `capture_over_sprites`, draws the
C355 bands that sit over the 3D into a transparent overlay that `vk_present` sandwiches after the GPU 3D
(`s21_sandwich`, parallel to `s22_sandwich`). Build: seam + geom moved into the shared OSD lua (both
dylibs resolve `s21::`), the S1→S2 move S22 made.

Result (starblad attract, 4500f, --vk): GPU vs software **97.1% exact**, mean diff 0.34, residual on
polygon edges (`y 140–273`) — the GPU/scanline coverage residual Model 2 has. Model 2 / namcos22 builds
unchanged (every `s21::` path inert, gated on `find("starblad")`). **Deferred to T2b:** the `pri1==4`
z-mixed layer-0 (`mix_layer0_sprites`) — its sprites are omitted from the overlay until the z-mix moves
onto the GPU depth buffer. Detail in [t1-seam-tap.md](t1-seam-tap.md), [t2-untextured-gpu.md](t2-untextured-gpu.md).

## 2026-08-24 — T2b: the `pri1==4` layer-0 z-mix, on the GPU depth buffer

The gap T2a left: `pri1==4` gameplay (cybsled, aircomb attract) gates its layer-0 C355 sprites against
the polygon z-buffer per pixel, and the CPU loses that buffer once the software rasteriser stops drawing.
Fix: the driver captures the sprite pixels plus a per-pixel tag (0 = never shown, 1+bank = gated on
`pri[bank]`, 255 = unconditional show — `mix_layer0_sprites`' own three branches, faithfully, bugs and
all) into a plain storage buffer; a new GPU pass (`s21_geom.cpp`, `shaders/s21_mix.frag`) recomputes
`pri[bank]` per fragment and tests it `GREATER_OR_EQUAL` against the SAME depth attachment the polygon
pass just wrote, write disabled — hardware doing the comparison the CPU no longer can. No image/sampler
needed: a fullscreen triangle (`fullscreen.vert`) reading a mapped buffer, the same no-staging shape the
palette CLUT already uses. Drawn between the 3D and the OVER band.

Found and fixed in passing, not T2b-specific: the T2a overlay capture (`capture_over_sprites`, and now
`capture_mix_sprites`) fired under `M2VK_NO_3D` too — gated on `!sw_owns_3d()`, which the "neither draws"
background reference also sets — so the coverage baseline wasn't actually background-only for S21. New
`s21::gpu_owns_3d()` (true only for `set_gpu(true)`) is what `over_begin`/`mix_begin` gate on now.

Verified on cybsled (`retrohost --vk`, software vs vulkan, several frame counts, visual + pixel diff):
HUD boxes, the pilot-portrait briefing screen, and combat frames all match, modulo the same
edge-rounding residual T2a measured on starblad. Gotcha for next time: `model2_internal_res`'s default
applies a real 0.8× vertical **rescale**, not a crop — a naive top-384-rows crop compares misaligned
content and looks like a 40% structural mismatch that isn't there. Found, not fixed (out of scope): a
cybsled in-HUD sub-viewport renders solid green on the GPU path where software shows detail — looks like
`s21.h`'s "S21 does not window the 3D" note not holding for every C67 game, a T2a-era gap. Detail in
[t2-untextured-gpu.md](t2-untextured-gpu.md).

---

## 2026-08-24 — T4: widen to all System 21 (Winning Run on the GPU)

Added `namco/namcos21.cpp` (Winning Run / Suzuka GP / '91) to the `namcos21` subtarget — `.flt` +
regenerated `.lua` (new `MB87077` sound dep; force-re-archived `liboptional.a` when the object was built
but not picked up). All ten S21 drivers now link into one core.

The plan's "the seam already covers them" was only half-right: geometry *capture* is in the shared
`namcos21_3d.cpp`, so quads flowed immediately, but the whole option-B **present** path (`set_palette`,
`capture_under`, `capture_over`) lived only in the C67 driver. Winning Run's 2D model is different — a
single GPU **bitmap layer** (`bitmap_draw`, no C355) with a priority that flips 3D under/over 2D — so the
hooks were ported into `namcos21.cpp` (all `#ifdef S21VK`, +76/-0). Two things C67 never exercised:

1. **Partial screen updates.** Winning Run splits the frame with `update_partial` (raster scroll), so
   `screen_update` fires several times per frame with a band cliprect; the per-band capture kept only the
   last strip (3D world + top HUD vanished, only the lower dashboard survived). Fix: accumulate each band
   into its persistent buffer and hand the seam the whole visible area once, on the last band.
2. **Backdrop-sentinel shadow.** `bitmap_draw` pen 0x00/0x01 draws *opaque* where the pen beneath is the
   backdrop sentinel (`base ^ 0x10ff`) and a palette shadow elsewhere. Reusing `capture_over` shadowed
   unconditionally, so the SUZUKA map / S-CURVE boxes over the sky came out green instead of blue. Added a
   `shadow_enable`/`sentinel`/`opaque_base` push to `s21_finish.frag`; the driver sets it per frame, C67
   passes `shadow_enable=0` (unchanged).

Verified `retrohost --vk` sw-vs-vk, `M2VK_NO_3D` background, `ppmdiff coverage`: `winrun` 0.9998 (18 edge
px, 0 interior), `winrungp` **1.0000**; no C67 regression — `starblad` 0.9934 (1 interior, same as before),
`cybsled` 0.9995 (mix path, 0 interior). `winrun91` ROM not on hand, untested. Not committed.

## 2026-08-24 — T5: System 21 polish (savestates, per-game pads, A/B, compat)

Polish pass for the System 21 family. No renderer or driver changes; the work was verification plus
tooling and layout data.

- **Savestates: all 4 fixtures PASS with zero code changes** — `winrun`, `winrungp`, `cybsled`,
  `starblad`. The module is driver-agnostic (MAME registry + the generic FIFO trailer, which is a no-op
  here since none of these drivers uses a `generic_fifo`), so it applied unchanged. `state.sh` gained a
  `CORE=` override and a `roms/system22/` fallback. All four deterministic (`D == E`), save-clean
  (`A == R`), and reproduce the dirty future (`C == D`).
  - **`starblad` FAILed twice before it passed, and both FAILs were test artefacts** (savestates.md §10.1):
    (1) the dirtying script *held* input past the save point, so the loaded clean run diverged on input
    the state never carried; (2) the corrected re-run was CPU-contended with a concurrent run. Cleared by
    a contamination-free **single-history diff** — one dirty run saves both the load-state (N) and the
    reference future (N+1, N+300) via the new retrohost `M2VK_HOST_SAVE_AT2`; the loaded state is
    byte-identical to the reference at N+1 *and* N+300, which on a deterministic machine is proof the
    registry is complete.
  - **Surfaced a genuine upstream MAME quirk:** `device_rom_interface::m_bank_count` is `save_item`'d but
    left **uninitialised** when a device uses a configured address map (`dirom.ipp:123-147`). It is saved
    *garbage*, differs every boot, and made two-separate-run state diffs name phantom carriers
    (`c68mcu` RAM, then `c140/m_bank_count`). Dead (unread without banking), so it never affects a load —
    left unpatched (high-traffic core file); the workaround is the single-history diff.
  - **Two diagnostics added, kept, env-gated off:** retrohost `M2VK_HOST_SAVE_AT2` and
    `M2VK_SAVE_DIFF_HEX` (byte-offset dump in `m2vk_savestate.cpp`). A speculative `m37710` DMA-register
    fix was tried and **reverted** (wrong CPU — the C68 is an M37450).
- **Per-game RetroPad layouts authored** for `winrun`/`winrungp` (wheel + gas/brake pedals + d-pad
  gearshift, no fire buttons), `starblad` (AD-stick aim, fire on B), `cybsled` (twin-stick tank; missile/
  gun/view on the face buttons). `input_layouts.json` rows added, `.ipp` regenerated, `padmap-gen.py
  --check` green. Tooling made S21-aware: `padmap-gen.py` reads the three S21 driver files
  (`namcos21.cpp`, `namcos21_c67.cpp`, `namcos21_de.cpp`); `padmap-sweep.sh` has a `system21` family
  (one family, three source files). ⚠️ Full browser-editor bring-up (running the sweep → S21 in
  `padmap-data.js`, TIERS entries, `padmap-test.js` total bumps) left for when more S21 pads are authored.
- **A/B baselines** recorded for the four fixtures via `ab-table.py` — [ab-baselines.md](ab-baselines.md)
  "System 21". `starblad` (coverage 0.998) and `cybsled` (0.9985) sit on their T4 numbers and are clean
  at frame 2500; `winrun` shows no 3D at 2500 and `winrungp`'s 2500 is a fast-motion frame (0.596) — a
  sample-point artefact, not a renderer disagreement (T4's controlled-frame coverage is 0.9998 / 1.0000).
  A per-set 3D frame for the two Winning Runs is the open tidy-up.
- **Compatibility matrix** — first Track-D-format table added to [compatibility.md](compatibility.md)
  (Game/romset/Driver/Boots/Renders/On GPU/Input mapped/Key-map verified/Savestate/Notes) for the four
  S21 sets; `aircomb`/`solvalou`/`driveyes`/`winrun91` listed as not-yet-exercised.
- **Owed hand-checks** (rendering is verified statically, controls are not): winrun/winrungp steering &
  gearshift feel; cybsled's **second AD stick** (right tread) — its binding is unverified.

## 2026-08-25 — T5 hand-checks: Winning Run verified, Cyber Sled right tread fixed

Hand-check results and the one fix they required.

- **Winning Run / Winning Run GP: driving controls verified** (wheel, gas/brake, d-pad gearshift).
  Compat matrix Key-map column → ok.
- **Cyber Sled: the right tread was dead** — its second stick is player 2's `IPT_AD_STICK_X/Y`, which
  `apply_device_defaults` binds only to pad 2 (device index == player), so on a single controller
  nothing drove it. Fixed by a new **`m2vk_twinstick.h`**: it ORs pad 1's RIGHT stick onto the player>1
  AD-stick *type* default via `ioport_manager::set_type_seq`, run once from `update()` (the port list is
  empty at `input_init`, the usual trap). **Gated on a new `twin_ad_stick` layout-row flag**, not a
  detector — a genuine two-player analog game (Model 2's `gunblade`, `rchase2`) is indistinguishable at
  the ioport level (it also declares a player-2 AD stick), and there the cross-bind would drag player 2's
  aim with player 1's stick. Only cybsled's row opts in, so the shared OSD carries zero risk to the
  Model 2 core. Flag plumbed through `input_layouts.json` → `padmap-gen.py` → the `.ipp` and the
  `game_layout` struct; `padmap-gen --check` and `padmap-test.sh` green. Binding confirmed created
  (`[twinstick] cybsled: bound … 2 … axis field(s)`); the feel (correct axis, not inverted) is the
  re-check owed.
