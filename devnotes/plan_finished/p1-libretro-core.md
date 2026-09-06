# P1 — the Model-2-only libretro core (software renderer)

**Status: planned 2026-07-25, not yet implemented.** Plan of record for P1 as scoped in
`../Polydiver/PDDocs/model2/model2_libretro_core.md` §3. Read [seam.md](../reference/seam.md) first for what P0
established; this doc covers everything *around* the seam — the build wiring, the OSD, and the
libretro ABI.

## What P1 delivers

`model2_libretro.dylib` — a libretro core that boots a Model 2 ROM in RetroArch, presents MAME's
stock software-rendered frame as a normal 2D `video_cb` frame, with sound and input. No Vulkan, no
poly rendering. Two things it must be, beyond "it runs":

1. **The A/B ground-truth generator.** Everything P3 is measured against comes out of this build.
2. **The permanent home of the seam.** The P0 tap moves in behind the OSD's sink here and never
   moves again.

Explicit non-goals for P1: Vulkan, HW-render context negotiation, widescreen, any per-game tuning.

## The four wiring problems, and how each is dodged

This is the part that looked fiddly. It is, but all four have clean answers, and the outcome is
**zero edits to upstream files outside `model2_v.cpp`.** Every one of the pdmame patch series'
upstream touches (`PDTooling/mame_patches/0002`–`0006`) turns out to be avoidable at mame0288.

### 1. Getting `-DM2VK` into `model2_v.cpp` and nowhere else

P0 used `ARCHOPTS_CXX=-DM2VK_POLYTAP`, which sprays the define across every translation unit in the
build. The genie-correct place for a per-driver define is the **subtarget project**, which today we
don't have — `SOURCES=src/mame/sega/model2.cpp` makes genie auto-generate the driver project from
`makedep.py` output (`genie.lua:1315-1340`), and an auto-generated project can't carry our define.

Fix: stop using `SOURCES=` and become a real subtarget. Two new files:

- **`src/mame/model2.flt`** — one line, `sega/model2.cpp`. Drives `drivlist.cpp` generation
  (`main.lua:320`) so the core links only Model 2 drivers.
- **`scripts/target/mame/model2.lua`** — the subtarget project. genie prefers a hand-written
  subtarget script over the filter path (`genie.lua:527-532`), and inside `project ("mame_model2")`
  we add `defines { "M2VK" }`. That reaches `model2_v.cpp` and `model2.cpp` and nothing else.

It does not have to be written by hand. `makedep.py` will emit exactly the file we want:

```sh
python3 scripts/build/makedep.py -r . sourcesproject -t model2 -l src/mame/mame.lst \
  src/mame/sega/model2.cpp > scripts/target/mame/model2.lua
```

114 lines — the `CPUS`/`SOUNDS`/`MACHINES`/`VIDEOS`/`BUSES` set plus `createProjects_mame_model2`
and `linkProjects_mame_model2`. Add the `defines` line, a header comment, and the
`linkProjects` stanza is already there. Regenerating it after an upstream merge is a one-liner, so
driver-dependency churn costs nothing.

Build invocation becomes `make SUBTARGET=model2 OSD=libretro_m2` — no `SOURCES=`, no `ARCHOPTS_CXX`.

> Sanity check on the generated list: it pulls in `IE15`, `SWTPC8212`, `VOTRAXTNT`, `HEATHZENITH_H19`
> and other oddities. That is not a mistake — those are terminal/keyboard devices reachable from
> Model 2's serial ports in the device tree. Leave them; trimming them is how you get a link error
> six months from now.

### 2. Producing a shared library instead of an executable

`mainProject()` hard-codes `kind "ConsoleApp"` (`main.lua:25`). pdmame patched `main.lua` for this
(patch 0003). We don't have to: `mainProject` calls **`maintargetosdoptions(_target, _subtarget)`**
at `main.lua:214`, and that function is defined by *our* OSD script. Same genie project scope, later
call wins — so our `maintargetosdoptions()` re-issues `kind "SharedLib"`, `targetprefix ""`,
`targetname "model2_libretro"`, `targetextension ".dylib"`.

Cost of not patching `main.lua`: it still `links { "bgfx", "bimg", "bx" }` unconditionally
(`main.lua:206`), so those 3rdparty libraries still build even though nothing references them.
Several minutes of cold build time, no runtime cost. Accept it for P1; revisit only if cold builds
become the bottleneck.

### 3. Linking without SDL, bgfx or any platform backend

`osd_common_t::register_options()` (`osdobj_common.cpp:213-349`) references ~50 module symbols, and
**most of the references are unconditional** — `FONT_OSX`, `SOUND_COREAUDIO`, `MONITOR_MAC`,
`RENDERER_BGFX`, `DEBUG_IMGUI` and friends are outside any `#ifdef`. That function is compiled into
`osdobj_common.o`, which we obviously link, so every one of those symbols must resolve. This is what
forced pdmame's patch 0005 (an `#ifdef OSD_PDMAME` branch in that function).

The saving grace, checked file by file: nearly every backend guards its platform includes on
`OSD_SDL` / `SDLMAME_*` / `USE_OPENGL` / `OSD_WINDOWS`, and collapses to a `MODULE_NOT_SUPPORTED`
stub when none are defined. So `font_osx.cpp`, `coreaudio_sound.cpp`, `monitor_sdl.cpp`,
`sdl_sound.cpp`, `drawogl.cpp`, `taptun.cpp`, `portmidi.cpp` … all compile clean with no external
headers, exactly as pdmame's script comment claims.

**The only two that don't are `render/drawbgfx.cpp`** (includes `window.h` and the bgfx tree
unconditionally, and drags in ~80 more files) **and `debugger/debugimgui.cpp`.**

So: compile the trimmed "none-backends" module set (pdmame's `pdmodulesbuild()` list ports over
essentially verbatim), and add one new file of ours —

- **`src/osd/libretro_m2/module_stubs.cpp`** — `MODULE_NOT_SUPPORTED` + `MODULE_DEFINITION` pairs for
  every symbol `register_options()` names that we don't compile. ~45 one-liners.

That replaces patch 0005 with a file we own. It has the same maintenance coupling to upstream (a new
module upstream ⇒ one more line) but it **fails loudly at link time** with the exact missing symbol
name, and it can never conflict during a merge.

### 4. Talking to the module system without patching visibility

pdmame's patch 0002 widened `osdobj_common.h`'s `private:` to `protected:` so a subclass could reach
`m_mod_man` and `select_module_options<>`. We don't need either:

- `module_type` is just `std::unique_ptr<osd_module> (*)()` (`osdmodule.h:58`). Any module exported
  with `MODULE_DEFINITION` can be instantiated directly — `extern const module_type RENDERER_NONE;`
  then `auto m = RENDERER_NONE();` — no manager involved.
- Every pointer we actually need to populate (`m_render`, `m_sound`, `m_debugger`, `m_midi`,
  `m_network`, `m_*_input`, `m_output`, `m_monitor_module`) is already `protected`
  (`osdobj_common.h:316-329`).
- The one private member, `m_font_module`, is reachable only from `font_alloc()` and
  `get_font_families()` — both `virtual`, so we override them and never touch it.
- `register_options()` never has to *run*. `main()` calls it explicitly (`sdlmain.cpp:98`); the
  frontend doesn't. Our entry point simply doesn't call it. (It still has to *link* — see §3.)

So `libretro_m2_osd_interface : osd_common_t` overrides `init_subsystems()` and populates the
protected pointers with instances it owns. Everything else — the watchdog, `output_callback`,
option handling, the sound-API delegation — comes from `osd_common_t` unchanged.

Most of the delegating virtuals we override outright rather than supply a module for:
`sound_stream_*` (straight to the libretro audio ring), `init_debugger`/`wait_for_debugger` (no-op),
`create_midi_*`/`open_network_device` (empty), `font_alloc` (a null font). That leaves very few real
module instances: `RENDERER_NONE` and `OUTPUT_NONE`, both taken from upstream's factories.

## File layout (all new files)

```
src/mame/model2.flt                      driver filter (1 line)
scripts/target/mame/model2.lua           subtarget project; carries defines{"M2VK"}
scripts/src/osd/libretro_m2.lua          OSD build rules; maintargetosdoptions → SharedLib
scripts/src/osd/libretro_m2_cfg.lua      defines OSD_LIBRETRO_M2
src/osd/libretro_m2/
  libretro.h                             vendored from the RetroArch tree (permissive licence)
  retro_entry.cpp                        the retro_* ABI, the emu thread, the frame handoff
  libretro_m2_osd.h / .cpp               osd_common_t subclass
  libretro_m2_input.h / .cpp             the RetroPad input_module
  module_stubs.cpp                       MODULE_NOT_SUPPORTED for the platform backends
  m2vk_sink.h / .cpp                     frame_begin / submit / frame_end — the seam's landing pad
  m2vk_polytap.h                         P0's tap, demoted to one debug consumer behind the sink
  retro_options.h / .cpp                 the core option table and its three declaration forms
```

(`audio_ring.h` was planned and is not there: the audio path turned out to need only a `std::vector`
filled by the sink and drained inside the frame baton, so there is no ring and nothing lock-free.)

`src/mame/sega/model2_polytap.h` is **deleted** in the same commit; its content moves to
`m2vk_polytap.h`. After that, upstream directories contain nothing of ours except the four guarded
blocks in `model2_v.cpp`.

## The seam, promoted

**Done 2026-07-25 (step 6).** P0's hook shape (`frame_begin` / `submit` / `frame_end`) was already
the permanent one — it is what a Vulkan renderer wants — so only what sits behind it changed:

- guard renamed `M2VK_POLYTAP` → `M2VK`, set by `scripts/target/mame/model2.lua` (step 1)
- include is now `#include "libretro_m2/m2vk_sink.h"` — `src/osd` is on the driver project's include
  path, `src/` is not, so it is *not* spelled `osd/libretro_m2/…` as this doc originally said
- the three calls dispatch to a sink; the tap is one consumer behind it

The diff in `model2_v.cpp` stays at four blocks / 16 lines, and this is the last time the include
line changes.

### What crosses the seam: a snapshot, not the driver's structures

`submit()` converts `model2_state::polygon` + `m2_poly_extra_data` + the clipped viewport into a
plain `m2vk::poly` — screen-space vertices plus every resolved texture/lighting parameter, no MAME
types at all. Consumers therefore compile without the driver's headers, an upstream change to
`model2.h` breaks one conversion instead of every consumer, and the snapshot is already close to
what a vertex-buffer upload wants. Cost is ~230 bytes copied per polygon, skipped entirely when
nothing is attached (`m2vk::active()` is a plain bool read).

Two details worth keeping:

- The conversion is a **template**, deduced at its single call site, purely so `m2vk_sink.h` can
  also be compiled in translation units that have never heard of `model2_state` — `m2vk_sink.cpp`
  and the tap are exactly that.
- It **zeroes the texture fields for non-textured polygons.** `model2_v.cpp` fills them only when
  `renderer & 2`, and the `object_data()` slot they live in is recycled, so what is in there
  otherwise is the previous polygon's. The tap always guarded its own printing; the snapshot makes
  it a property of the data instead.

`m2vk::vertex::rz` is 1/z for textured polygons and **raw z for solid ones** — the reciprocal is
taken only in the textured branch, because the solid scanline renderer ignores the vertex
parameters. This is pre-existing behaviour, recorded here because the tap's `1/z` and `rz_*` figures
mix the two and P3 will care.

### Lifetime, and `no_3d=1`

The sink is one object with static storage duration in `m2vk_sink.cpp`, so it always exists. The OSD
brackets a machine's run with `sink_open()` in `init()` and `sink_close()` in `osd_exit()`;
consumers are built in the first and destroyed in the second, so a second game loaded into the same
process gets a fresh tap rather than the first game's totals.

That fixes what the feature survey ran into: a game rendering no 3D used to never construct the tap
singleton and therefore wrote *no* summary file at all (which is how `skisuprg` was caught). It now
writes `frames=0` / `no_3d=1`. Verified: 200 frames of `vf2` — well short of its first 3D frame at
~16 emulated seconds — produces exactly that.

A build whose OSD does not call `sink_open()` (the plain `OSD=sdl3` binary) is still fully
supported: the first `frame_begin()` opens the run implicitly and `~sink` closes it at process exit,
which is P0's behaviour exactly. Such a run reports nothing if it renders nothing — there is nowhere
left to report from.

### The tap is now off by default

Attached only if one of `M2VK_POLYTAP`, `M2VK_POLYTAP_EVERY`, `_DUMP`, `_DUMP_FILE`, `_SUMMARY` or
`_TAG` is set (`M2VK_POLYTAP=0` forces it off). It used to be on in any `M2VK` build, which is not
what a core that people play should do. The sweep scripts set `_SUMMARY`/`_TAG`, so they are
unaffected.

### Both build shapes keep working

The sink lives in the OSD directory because that is where the Vulkan renderer will live, but the
seam that calls it is in the driver project, and the driver project is linked *before* the OSD
archive. That direction is fine; the reverse is not, which is why the OSD never has to reach into
the driver. For any OSD other than `libretro_m2` — which does not compile the sink — a HAND-ADDED
block in `scripts/target/mame/model2.lua` compiles it into the driver project instead, so
`make SUBTARGET=model2 OSD=sdl3` still links and still carries the tap.

### Verified

- `vf2` frames 100/300/400/500/600 from the standalone binary reproduce the numbers in
  [seam.md](../reference/seam.md) exactly.
- The full per-polygon dump of `vf2` rendered frame 800 is **byte-identical** to the P0 fixture
  `devnotes/fixtures/vf2-frame800-polytap.txt`. That covers every field of the conversion, including
  the texture parameters and all five vertex floats.
- Through the core (`retrohost`, 1100 frames): same frame-100 line, and a summary written at
  `retro_unload_game` rather than at process exit.
- With no `M2VK_POLYTAP*` variable set the core prints nothing and attaches nothing.

## The run loop — lock-step, not latest-wins

pdmame publishes frames latest-wins because Unity polls at its own cadence. libretro is the opposite
contract: `retro_run()` must advance **exactly one frame** and return. So P1 does not reuse pdmame's
publishing model; it uses a baton.

`emulator_info::start_frontend()` blocks for the life of the machine, and calls
`machine().osd().update(skip_redraw)` once per emulated frame (`video.cpp:244`). That call is the
handoff point.

- **emu thread** (spawned in `retro_load_game`): runs `start_frontend()`. In `osd->update()`:
  capture the screen bitmap → `post(frame_done)` → `wait(go)` → return.
- **`retro_run()`**: poll libretro input → publish it → `post(go)` → `wait(frame_done)` →
  `video_cb(...)`, `audio_batch_cb(...)` → return.

Notes that matter:

- **`retro_load_game` must block on the first `frame_done`.** The frontend calls
  `retro_get_system_av_info()` immediately afterwards and needs real geometry, which only exists
  once the machine has started.
- **Failure must not deadlock.** A missing ROM or a `fatalerror` unwinds before the first
  `update()`. The emu thread posts `frame_done` with a `died` flag on any exit path, so
  `retro_load_game` returns `false` instead of hanging.
- **MAME must never throttle or sync itself** — the frontend paces us. Force `-nothrottle`,
  `-video none`, `-sound none`-equivalent behaviour through the synthesized command line.
- C++20 is on (`genie.lua:708`), so `std::binary_semaphore` is available; no need for `osd_event`.
- Threads exist anyway — `poly_manager` farms scanlines to workers — so a coroutine/`libco`
  approach buys no determinism we don't already have. P0 measured bit-repeatable runs with the
  worker pool live.

### av_info

`model2.cpp:2538` — `set_raw(32_MHz_XTAL/2, 656, 0, 496, 424, 0, 384)`. Visible **496×384**, pixel
clock 16 MHz, so **fps = 16e6 / (656 × 424) = 57.524 Hz**. `aspect_ratio` 4/3. Take the geometry
from the `screen_device` at runtime rather than hard-coding it — `2c` sets differ.

## Audio

MAME 0288's OSD audio API is the reworked `sound_stream_sink_*` set on `osd_interface`, and
`pdmame_sound.cpp` is already written against it — advertise one node with a stereo sink,
`m_default_rate = 0` ("use the configured rate"), and take interleaved `int16` in
`stream_sink_update`. Port it, but as **overrides on our OSD class** rather than as a `sound_module`
(no module ⇒ nothing to register ⇒ one less stub).

Sink → lock-free ring → drained in `retro_run` → `audio_batch_cb`. Pass `-samplerate 48000`. At
57.524 Hz that is ~834 frames per video frame; MAME's own per-frame sample count already tracks the
refresh, so no resampling and no rate correction are needed in P1. Underruns get zero-filled.

## Content loading

`retro_load_game` receives a path such as `…/roms/vf2.zip`. Derive the system name from the
basename, point `-rompath` at the containing directory, and synthesize a MAME command line. Model 2
parent/clone sets mean the frontend may hand us a clone name; that's fine, MAME resolves it.

The synthesized command line also carries the option-derived arguments and the frontend's
directories — see the next section.

## Core options — step 7

**Done 2026-07-25.** `src/osd/libretro_m2/retro_options.{h,cpp}`. Two options, and that is the whole
of the core's configuration surface. MAME's own options are not exposed and never will be: anything
a frontend already does better — scaling, audio latency, input remapping, shaders — belongs to the
frontend, and everything else in a single-driver core is either a build decision or a bug.

| key | values | default | applied |
| --- | --- | --- | --- |
| `model2_renderer` | `vulkan`, `software` | `vulkan` | at load |
| ~~`model2_service_buttons`~~ | ~~`disabled`, `enabled`~~ | ~~`disabled`~~ | ~~at load~~ |

⚠️ **The set is FOUR as of 2026-07-28, not two.** `model2_diagnostic_input` (see the box below) and
then `model2_internal_res` / `model2_flat_shading`, the first two of the `M2VK_*` switches promoted to
options — [user-options.md](../reference/user-options.md) §4 and §5 are the scoping, and the current list is always
`DEFINITIONS[]` in `retro_options.cpp` rather than any prose here.

| key | values | default | applied |
| --- | --- | --- | --- |
| `model2_diagnostic_input` | `None` + FBNeo's ten combos | `None` | at load |
| `model2_internal_res` | `1x`, `2x`, `3x`, `4x` | `1x` | **live** |
| `model2_flat_shading` | `off`, `flat` | `off` | **live** |

🚨 **The two new ones apply LIVE, and that was a bug fix rather than a nicety.** They shipped
"applied when content is loaded" on 2026-07-28 and it was wrong within the hour: a player changes a
setting in the options menu, nothing happens, and the only reasonable conclusion is that the option is
broken. `retro_run()` now re-reads both when the frontend raises `GET_VARIABLE_UPDATE` — flat shading
lands on the next frame, internal resolution on the next presented one via a ring rebuild.
`model2_renderer` and `model2_diagnostic_input` genuinely cannot be live (one decides whether hardware
render was declared at all, before the machine started; the other is baked into the input assignments)
and the core's log line names which two are which.

⚠️ **The header comment at the top of `retro_options.h` still says every option is read once in
`retro_load_game()`.** That is now true of two of the four.

🚨 **`model2_service_buttons` was RETIRED on 2026-07-28** by the lightgun phase's step 6 and replaced
by **`model2_diagnostic_input`** — FBNeo's eleven-value combo list, `None` by default, with
`IPT_SERVICE` on a synthetic button item and `IPT_SERVICE1` still on L3 whenever the option is not
`None`. The reason is in [lightgun.md](../reference/lightgun.md) §2.5.3: **L3/R3 are `"nul"` in RetroArch's default
keyboard binds**, so the old option was not merely unconventional, it was unreachable without a
gamepad. The count of core options is unchanged at two. Everything below this box is P1's as-built
record and is kept as a dated record — ⚠️ **a hand-written RetroArch `.opt` file still carrying the old
key is inert**, which reads exactly like a broken option.

**`model2_renderer` defaults to `vulkan` now, in a build that has no Vulkan renderer.** That is
deliberate: nothing ships until the Vulkan path works, so the shipping default is the default from
day one and no user's saved config ever needs migrating. Until P3 lands, a `vulkan` selection logs

```
[model2] the Vulkan renderer is not built into this core yet; using the software renderer
```

and runs software. `software` stays a supported value forever — it is both the A/B ground truth and
the fallback on a machine with no usable Vulkan driver.

**`model2_service_buttons` puts the service coin on L3 and the test switch on R3.** This core draws
none of MAME's menus, so with it off there is genuinely no way to reach a game's test mode, change
its settings or clear its backup RAM. Off by default, because a stick clicked by accident should not
drop a service coin mid-game. Both `IPT_SERVICE1` and `IPT_SERVICE` are player 0 in `inpttype.ipp`,
so `apply_device_defaults()` lands them on pad 1 alone and pad 2's identical copy is skipped — no
port check needed in `configure()`.

Both are read once, in `retro_load_game()`, because both are settled before the machine starts: one
picks a draw path, the other is baked into the input devices' default assignment vectors at machine
start. `retro_run()` checks `GET_VARIABLE_UPDATE` and logs one line per change saying it applies at
the next load, rather than half-honouring it.

### Declaration: v2, with the older forms derived

`declare()` queries `GET_CORE_OPTIONS_VERSION` and offers `SET_CORE_OPTIONS_V2`, falling back to v1
and then to the pre-options `SET_VARIABLES` — each step on refusal as well as on an older version,
since a frontend that answers the version query and then rejects the call is better served by the
older form than by nothing. The v1 and legacy tables are *derived* from the v2 table at run time, so
there is one table to keep correct. The legacy conversion assumes the default is the first value
listed, which is true of both options; there is a comment on it.

`get()` falls back to our own declared default when the frontend has no value — which is what makes
the core behave identically under `retrohost`, which had no option support at all until this step.

### The frontend's directories, and why it is not cosmetic

MAME resolves `nvram_directory`, `cfg_directory` and friends relative to the process's working
directory. For a core that is the *frontend's* working directory, which is not ours to litter. All
six now go under `<save dir>/model2/` (`nvram`, `cfg`, `inp`, `sta`, `diff`, `snap`) via
`GET_SAVE_DIRECTORY`; MAME creates the paths on first write. `GET_SYSTEM_DIRECTORY`, if the frontend
offers one, adds `<system dir>/model2` as a second `rompath` entry so a clone whose parent set lives
elsewhere still loads.

This matters to the A/B harness more than to tidiness. Model 2 keeps credits and settings in battery
RAM, and the input session already found that carried-over NVRAM makes credit counts look
non-deterministic across runs. A `(rom, frame)` fixture is only reproducible if the NVRAM state is,
so knowing exactly where it lands — and being able to delete it — is a precondition.

**`-noreadconfig` is on for the same reason.** MAME's default `inipath` includes the working
directory and `$HOME/.mame`, so without it a stray `mame.ini` belonging to someone's standalone MAME
silently changes how the core runs and the harness would never know. It gates ini parsing only
(`mame.cpp:261`); the per-game `cfg` that carries input remaps still loads.

### One bug found on the way

`buttonitems[L3]` and `[R3]` were `ITEM_ID_INVALID` — the stick clicks were never added as device
items, only named in `RETROPAD_BUTTON_NAMES`. `add_assignment()` skips an assignment naming an
invalid item and returns false, so the pre-existing `IPT_UI_MENU` binding on L3 had been silently
doing nothing since step 5, and the first cut of the service option did nothing either. They are now
in `FIXED_BUTTONS` as `ITEM_ID_BUTTON9`/`BUTTON10` — deliberately not in `NUMBERED_BUTTONS`, since no
Model 2 game has nine buttons and automatic `IPT_BUTTON9/10` assignments would be clutter.

### retrohost grew option support

`env_cb` now answers `GET_CORE_OPTIONS_VERSION` (2), `SET_CORE_OPTIONS_V2` (records and prints the
declared defaults), `GET_VARIABLE`, `GET_VARIABLE_UPDATE`, `GET_SYSTEM_DIRECTORY` and
`GET_SAVE_DIRECTORY`. A value is overridden for a run with `M2OPT_<key>` in the environment:

```sh
M2OPT_model2_service_buttons=enabled ./devnotes/retrohost ./model2_libretro.dylib \
  devnotes/roms/vf2.zip 1400 /tmp/f.ppm "1200:r3:250"
```

Its save directory defaults to `./retrohost-save`, or `$M2_SAVE_DIR`. **`rm -rf retrohost-save`
before a fixture run** — that is where the NVRAM now lives.

### Verified

- Defaults through `retrohost`: options declared, `renderer=vulkan` warns and falls back,
  `renderer=software` does not warn, and `<save>/model2/{cfg,nvram}` is populated.
- `model2_service_buttons=enabled`: two L3 presses in `vf2` attract read as **CREDITS 2/2**; R3 held
  brings up **TEST MENU**. With the option off, the same script changes nothing at all.
- Input regression after the L3/R3 item change: the step-5 script (two coins, start, two rights)
  still reaches PLAYER SELECT with the cursor on the third character.
- Seam regression: `vf2` rendered frame 100 through the core reproduces [seam.md](../reference/seam.md)'s line
  exactly, field for field.
- `OSD=libretro_m2` and `OSD=sdl3` both build.

## Savestates — a finding that changes the plan

> ⚠️ **CORRECTED 2026-07-29 — see [savestates.md](../reference/savestates.md). Kept in place rather than rewritten,
> because this text is *why* savestates were deferred and the deferral was a reasonable call on what
> was known.** Two things below are wrong:
>
> 1. **"The flag only gates a warning and the autosave feature"** understates it — it *also* gates a
>    `fatalerror` on execute devices that register nothing (`device.cpp:555`), which turns out to be a
>    free instrument: flag one set, boot it, and MAME names any silent device. Run on 2O/2A/2B, it
>    comes back clean.
> 2. **"use the stream form, `write_buffer` doesn't [give an exact byte count]"** is wrong at mame0288.
>    `ram_state::get_size()` (`save.cpp:598`) is exactly `retro_serialize_size()`, and `write_buffer` /
>    `read_buffer` (`save.h:308`) are the right pair. ⚠️ `write_buffer`'s size check is an *equality*,
>    so serialise into an owned exact-size buffer and `memcpy` out.
>
> The conclusion in item 2 of this section — **the A/B harness must not depend on savestates** — still
> stands and is reaffirmed. Fixtures stay keyed on frame number.


**`src/mame/sega/model2.cpp` contains zero `MACHINE_SUPPORTS_SAVE` flags.** Upstream does not claim
savestate support for any Model 2 set. The flag only gates a warning and the autosave feature
(`gamedrv.h:76`), so `save_manager::write_stream()` / `read_stream()` will still *run* — but nothing
guarantees the registered state is complete, and untested savestate paths on a driver this complex
are exactly where silent corruption lives.

Two consequences:

1. `retro_serialize`/`retro_unserialize` are best-effort in P1. `save_manager::write_stream(std::ostringstream&)`
   gives an exact byte count for `retro_serialize_size` (`save.h:308`); `write_buffer` doesn't, so
   use the stream form. Serialize only at the frame boundary — which the lock-step handoff already
   gives us for free.
2. **The A/B harness should not depend on savestates.** The feature survey closes by asking for
   "gameplay-parked savestate fixtures" to get clean ratio columns. Given the above, prefer what P0
   actually proved: runs are bit-repeatable frame-for-frame, so a fixture is
   `(rom, frame number)` — optionally with `.inp` playback from frame 0 to reach gameplay. Slower to
   reach a given scene, but trustworthy.

## Input — a real `input_module`

**Decided 2026-07-25: the module route, not ioport injection. Implemented the same day.**

`libretro_m2_input.cpp` implements `input_module` and registers one joystick device per player with
`machine().input()`, mapping RetroPad buttons to `JOYCODE_BUTTON1…n` and the analog sticks/triggers
to `JOYCODE_XAXIS`/`YAXIS`/etc. MAME's stock default assignments for arcade drivers already reference
those codes, so every game in the library gets working controls with no per-game table — and MAME's
own remapping UI and `ctrlr` files keep working on top.

The alternative (pdmame's route: resolve `ioport_field` pointers once and write them every frame) is
much less code but needs a bespoke field table per game. pdmame only ever did VF1. The Model 2
library spans 3-button fighters, wheel+pedals, lightguns and twin sticks across 29 sets, so the
table approach does not scale and was rejected.

Consequence for §"Talking to the module system": this is one real module instance we construct
ourselves (ours, not an upstream factory), assigned to all four of `m_keyboard_input`,
`m_mouse_input`, `m_lightgun_input`, `m_joystick_input`, with `input_init()` overridden to call it
once rather than four times.

### What "default assignments" actually buys, and why the device index matters

Two mechanisms, and it is worth keeping them apart:

1. **`inpttype.ipp` defaults** are fixed strings baked into MAME. Only two matter here, and both
   already read the joystick device index: `START1..8` default to `KEYCODE_n OR
   JOYCODE_START_INDEXED(n-1)` and `COIN1..8` to `KEYCODE_n OR JOYCODE_SELECT_INDEXED(n-1)`
   (`inpttype.ipp:586,598`). So exposing `ITEM_ID_START` and `ITEM_ID_SELECT` on each pad is the
   whole of coin and start — no assignment needed, but the device registered for RetroPad port *n*
   **must** be the *n*th joystick device MAME sees. Nothing else does the pairing.
2. **`osd::input_device::set_default_assignments()`** carries everything else. `ioport_manager::
   apply_device_defaults()` (`ioport.cpp:2685`) rewrites each assignment's device index to the
   device's own, and matches on `entry.player() == device->devindex()` — so the same vector,
   supplied by both pads, lands on P1 and P2 respectively.

`configure()` is `sdl_game_controller_device::configure()` with the availability probing deleted: a
RetroPad always has every control, so all the `SDL_GameControllerHasAxis` branching collapses. The
useful consequence is that `add_directional_assignments()` from `assignmenthelper.cpp` is where
Model 2's analogue inputs get their bindings for free — that one call covers `IPT_PADDLE` (the
steering sets), `IPT_AD_STICK_X/Y` (twin-stick and flight) and `IPT_LIGHTGUN_X/Y` (the gun games),
which between them are every analogue type in `model2.cpp` except the pedals.

### Polling is inverted

A normal OSD pulls from a device driver when MAME asks. Here the item pointers handed to
`add_item()` address the device object's own state directly, and `retro_run()` writes that state on
the libretro thread — while the emulation thread is parked on the frame baton, which is what makes
it safe without a lock. `poll()` is therefore a no-op and `poll_if_necessary()`'s 2 ms wall-clock
throttle is irrelevant. A frame sees exactly one input sample, taken at a fixed point in the
libretro cycle, so runs stay reproducible for the A/B harness.

Triggers: `RETRO_DEVICE_INDEX_ANALOG_BUTTON` is optional in libretro, and a frontend that does not
implement it silently returns 0, so L2/R2 fall back to the digital button at full deflection. Either
way the trigger axis rests at 0 and runs to `ABSOLUTE_MIN`, matching the convention the SDL OSD uses
and which the `ITEM_MODIFIER_NEG` pedal assignments require.

Two pads, because no set in `model2.cpp` declares `PLAYER3`.

## Build recipe

```sh
python3 scripts/build/makedep.py -r . sourcesproject -t model2 -l src/mame/mame.lst \
  src/mame/sega/model2.cpp > scripts/target/mame/model2.lua   # then re-add defines{"M2VK"}

make SUBTARGET=model2 OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10
```

`makefile:454-474` only picks a *default* OSD, and `makefile:701-703` forwards `--osd=` to genie
unconditionally, so no makefile target is needed (pdmame's patch 0004 existed only because the
android targets hard-code `--osd=sdl`). `genie.lua:1370` resolves the OSD script as
`scripts/src/osd/libretro_m2.lua`.

### Three gotchas, all learned the hard way on 2026-07-25

1. **`REGENIE=1` is mandatory after editing `scripts/target/mame/model2.lua`.** The makefile's
   `$(SCRIPTS)` dependency list (`makefile:885-898`) does *not* include the subtarget script — the
   line that would is commented out at `makefile:908`. Without `REGENIE=1` the build silently reuses
   the previous project files. The first attempt at this "succeeded" while still compiling against
   P0's `-DM2VK_POLYTAP` project files, and proved nothing.
2. **genie's gmake output does not rebuild objects when compiler flags change.** Only header
   dependencies are tracked. After adding or renaming a define, `touch` the affected sources (or
   delete that project's objects) or you link a mix of old and new. Same trap as (1), one layer down.
3. **The binary is now `./mamemodel2`, not `./model2`.** With `SOURCES=` unset, `main.lua:15-19`
   names the project `_target .. _subtarget`. Any script or note carrying the old name needs
   updating.

Verified: `M2VK` appears in `mame_model2.make` and no other generated project file, and the tap's
frame-100 line reproduces the VF2 numbers recorded in [seam.md](../reference/seam.md) exactly.

## Order of work

1. `model2.flt` + `scripts/target/mame/model2.lua`; verify `make SUBTARGET=model2 OSD=sdl3` still
   builds the same `./model2` binary P0 built. **Nothing else changes yet** — this isolates the
   subtarget conversion from the OSD work.
2. `libretro_m2_cfg.lua` + `libretro_m2.lua` + `module_stubs.cpp` + a stub OSD that does nothing.
   Goal: it *links*. This is where the ~45 stubs get discovered, one link error at a time.
3. `retro_entry.cpp` — thread, baton, `retro_run`, video only. Boots vf2 in RetroArch with picture.
4. Audio.
5. Input. *(done — see the input section above)*
6. Seam move: `model2_polytap.h` → `m2vk_sink.h` + `m2vk_polytap.h`, guard rename, `no_3d=1`.
   *(done — see the seam section below)*
7. Core options, plus the frontend's directories. *(done — see the core options section above;
   savestates were dropped from this step, see below)*

Step 2 is the risky one and it fails fast and loudly, which is why it comes before any behaviour.

## Decisions settled 2026-07-25

- **Input: a real `input_module`** (above). Rejected: per-game ioport injection.
- **Savestates: deferred past P1.** Given no `MACHINE_SUPPORTS_SAVE` on any Model 2 set, P1 ships no
  `retro_serialize`/`retro_unserialize`; A/B fixtures key on frame number, which P0 proved is
  bit-repeatable. This narrows P1 against the plan doc's stated scope ("ROM load, run, input, audio,
  savestates") — deliberately, and the reason is recorded above. Revisit when the Vulkan path exists
  and there is a concrete reason to want scene-parking.

  Step 7 in "Order of work" therefore covers core options only.
- **`model2_renderer` defaults to `vulkan` from day one**, in a build that cannot honour it. Nothing
  ships until the Vulkan path works, so the shipping default costs nothing now and saves a config
  migration later. Rejected: declaring `software` as the only value until P3.
- **Two options and no more.** Rejected: a `model2_polytap` option (the tap stays an
  `M2VK_POLYTAP*` environment switch — it is a developer tool, and the sweep scripts and retrohost
  already drive it that way), `model2_samplerate`, and a `model2_bios` knob for the driving sets'
  drive-board ROM (`ROM_DEFAULT_BIOS` is already right for all eight).

## Risks

- **`-fPIC`.** Linking every MAME static library into a shared object is free on macOS/arm64 and
  x86-64 (PIC by default) but not on Linux x86-64. A Linux core build will need `-fPIC` added
  globally. Not a P1 blocker on this host; note it before promising Linux builds.
- **`maintargetosdoptions` overriding `kind`.** Relies on genie letting a later `kind` call win
  within a project scope. Verified by reading, not by building. If it doesn't hold, the fallback is
  a small `main.lua` patch (pdmame's 0003) and the upstream diff grows by one file.
- **The stub list drifts.** Upstream adds a backend ⇒ link error. Loud, thirty-second fix, but it
  will happen on merges and should be in the merge checklist.
- **Model 2 video code is moving upstream** (microtextures Oct 2025, front-to-back fill buffer
  Nov 2025 — see [feature-survey.md](../reference/feature-survey.md)). The ground truth P1 generates has a
  shelf life tied to the pinned tag.
