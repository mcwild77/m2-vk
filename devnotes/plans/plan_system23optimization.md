# System 23 optimization plan — reclaim the frame after the DRC win

Make **Namco System 23 / Super System 23** (Time Crisis II, Crisis Zone) run at full speed on the
Quest 3. The renderer is already done and off the critical path
([plan_system23.md](plan_system23.md), 23-0 → 23-7, texel-exact to software); this plan is about the
**CPU**, which is what actually sets the frame here — same conclusion as every family on this device
([performance.md](../reference/performance.md) §6.1, [retroarch-quest-perf.md](../reference/retroarch-quest-perf.md) §1).

## Status / headline (2026-09-04)

- **The native ARM64 recompiler was the first and biggest win.** The Android build's `drcbearm64` UML
  backend (switched on 2026-09-01, [build-android.sh:98-100](../build-android.sh)) recompiles System 23's
  R4650 (MIPS III) main CPU instead of running it through the portable C backend. Confirmed linked
  (32 `drcbe_arm64` symbols in the `.so`). On the Quest under RetroArch, **timecrs2 went from ~7fps to
  ~32fps** — a stale/C-backend core was most of the problem. **If a System 23 core still reads ~7fps,
  it predates this backend — rebuild first, before anything in this plan.**
- **Still ~half speed (32 of 57.5).** Profiled on-device 2026-09-04; the remaining wall is two
  **interpreted MCUs the recompiler does not touch.** This plan is the path from 32fps to full.

## The measurement — where the frame goes

**timecrs2, heavy in-game scene, `PROFILER=1` core** (throwaway build; the profiler roughly doubles
frame time, so read **percentages, not fps**). Rock-steady across f=2940–3180 (`m2prof` logcat,
method = [retroarch-quest-perf.md](../reference/retroarch-quest-perf.md) §4.1):

| Device | % of frame | What it is | Recompiled? |
|---|---|---|---|
| `:maincpu` | **29%** | R4650 (MIPS III) — main game CPU | ✅ `drcbearm64` |
| `:jvs:namco_tssio:iocpu` | **23%** | JVS **I/O board** MCU (**H8/3334** C78 — see O1) | ❌ interpreter |
| `:subcpu` | **20%** | H8/3002 — sound/inputs MCU | ❌ interpreter |
| Video Update | 8% | GPU-side / present | — |
| OSD Blitting | 6% | | — |
| Unaccounted/Overhead | 12% | inflated by the profiler's own tick reads | — |
| Timer Callbacks / Sound Gen / Input | ~2% total | | — |

The three CPUs are **72% of the frame**. `:maincpu` at 29% is already recompiled and about as low as the
main CPU goes — that headroom was spent on the 7→32 jump. **The remaining wall is the two interpreted
MCUs (43% combined), and neither is game logic:** one is an I/O board, one is the sound MCU.

Device identities confirmed in the driver: `:subcpu` = `h83002_device` m_subcpu
([namcos23.cpp:1932](../../src/mame/namco/namcos23.cpp#L1932)); the JVS board is a `bus/jvs/` device
([namcos23.cpp:1234](../../src/mame/namco/namcos23.cpp#L1234), `#include "bus/jvs/namcoio.h"`) connected to
the H8's serial port #0; the `namco_tssio` board runs the real **Hitachi H8/3334** (C78) — corrected in O1
from an earlier "MB90242A F2MC-16" mis-label ([namcoio.cpp:545](../../src/devices/bus/jvs/namcoio.cpp#L545),
`H83334(config, m_iocpu, 14.7456_MHz_XTAL)`).

---

## Lever 1 — HLE the JVS I/O board (23%) — **do this first**

The I/O board costs **almost as much as the entire recompiled main game CPU**, and all it does is read a
light gun, a foot pedal, and coins. This is the same pattern as Model 2's drive-board Z80 — flagged in
performance.md as "6% of force-feedback we never use" — except **~4× bigger** and on the input path.
It is the single fattest, most disproportionate slice, and it is *not* game logic being emulated, it is a
peripheral microcontroller (an **H8/3334** — see O1) being interpreted instruction-by-instruction.

**The scaffolding to replace it already exists.** MAME has a JVS HLE framework —
`jvs_hle_device` ([src/devices/bus/jvs/jvshle.h](../../src/devices/bus/jvs/jvshle.h)) — that speaks the JVS
protocol over the serial link **with no MCU emulated at all**. It is proven: `namco_em_pri1_01_device`
already derives from it ([namcoio.cpp:1511](../../src/devices/bus/jvs/namcoio.cpp#L1511)). The catch: that
existing HLE board is a *printer/emblem* board, not an *input* board — so no board the System 23 games
currently accept is HLE'd. Lever 1 is therefore **write a small `jvs_hle_device` input board** (gun X/Y,
trigger, pedal, coins, start, service), not adapt an existing one — but it is a bounded task on existing
scaffolding, not a from-scratch device.

### Phase O1 — investigate (cheap, no build) — **DONE 2026-09-04**

All three sub-steps are answered from the source; the conclusion is **proceed to O2 — there is no free
board.** Findings:

**Correction to the profile label.** `:jvs:namco_tssio:iocpu` is **not** an MB90242A/F2MC-16. The default
board for timecrs2 (`namco_tssio`, [namcoio.cpp:1131](../../src/devices/bus/jvs/namcoio.cpp#L1131)) derives
from `namco_c78_jvs_io_device`, which instantiates a **Hitachi H8/3334** (C78) at 14.7456 MHz
([namcoio.cpp:545](../../src/devices/bus/jvs/namcoio.cpp#L545)). So **both** interpreted MCUs on the critical
path are H8-family: `:subcpu` = H8/3002, `:jvs:namco_tssio:iocpu` = H8/3334. (The MB90611A/MB90F574 F2MC-16
parts are on the AMC/FCA boards, which timecrs2/crszone do not use.) The lever is unchanged; the label was
wrong.

1. **`jvs_hle_device` read.** The base ([jvshle.cpp](../../src/devices/bus/jvs/jvshle.cpp)) implements the
   **entire** JVS protocol with no MCU: RESET / SETADDRESS / IOIDENT / CMDREV / JVSREV / COMMVER / FEATCHK /
   MAINID / SWINP / COININP / ANLINP / ROTINP / SCRPOSINP / RETRANSMIT / COIN\* / OUTPUT\*. A subclass only
   overrides `device_id()` and the count virtuals (`player_count`, `switch_count`, `coin_slots`,
   `screen_position_input_channels`, `…_xbits/ybits`, `analog_input_channels`, `analog_output_channels`,
   `output_slots`); `feature_check()` (jvshle.cpp:167) auto-emits the feature list from those counts, and
   `execute()` handles every standard command. Inputs are read through the **same** `device_jvs_interface`
   hooks the real boards use — `system_r` / `player_r` / `coin_r` / `analog_input_r` / `rotary_input_r` /
   `screen_position_x_r`/`_y_r`/`_enable_r` ([jvs.h:106-113](../../src/devices/bus/jvs/jvs.h#L106)).

2. **The handshake is generic — the game does NOT gate boot on `device_id`.** The driver header states the
   games accept *several* boards — "TSS-I/O, FCA, ASCA3, ASCA5 and the common JVS I/O boards manufactured by
   Sega" ([namcos23.cpp:14-16](../../src/mame/namco/namcos23.cpp#L14)) — and those boards carry completely
   different IDENT strings and feature sets, so the `:subcpu` "check" is a **standard JVS enumeration**
   (reset → assign address → IOIDENT/FEATCHK), not a string match. `configure_jvs`
   ([namcos23.cpp:6884](../../src/mame/namco/namcos23.cpp#L6884)) binds the board's hooks to the machine
   ioports (`JVS_SYSTEM`, `JVS_PLAYER1`, `JVS_COIN1`, `JVS_ANALOG_INPUT1-8`, `JVS_ROTARY_INPUT1`,
   `JVS_SCREEN_POSITION_INPUT_X1/Y1`) — the **exact** hooks the HLE base reads — and does so for *every*
   option in the list ([namcos23.cpp:6877](../../src/mame/namco/namcos23.cpp#L6877)). An HLE board therefore
   plugs into the existing plumbing with zero driver-input changes; **boot needs only a well-formed JVS
   enumeration, and gun aim needs the board to advertise the screen-position function.**

3. **No free board — the one-line `set_default_option` win does not exist.** Every entry in
   `jvs_port_devices` ([jvs.cpp:432](../../src/devices/bus/jvs/jvs.cpp#L432)) except the printer HLE
   (`namco_empri101`) emulates a full MCU — the C78 boards (asca1/3/5, tssio, csz1, xmiu1) an H8/3334, the
   AMC/FCA boards an F2MC-16, cyberlead a display MCU. There is **no** lighter already-emulated *input*
   board to point the default at. **Proceed to O2 and write the HLE input board.**

**Open Question 2 resolved — HLE delivers gun aim, at zero ADC cost.** The gun is a **JVS screen-position
input**, not an ADC read. The real TSS-I/O digitizes it in `gun_r` by calling `screen_position_x_r(0)` /
`screen_position_y_r(0)` ([namcoio.cpp:1222](../../src/devices/bus/jvs/namcoio.cpp#L1222)); the game then
fetches it with the JVS **SCRPOSINP** command, which the HLE base already handles
([jvshle.cpp:426](../../src/devices/bus/jvs/jvshle.cpp#L426)). The `subcpu:adc` the plan worried about is
**wired to constant 0** on all four channels ([namcos23.cpp:6826-6829](../../src/mame/namco/namcos23.cpp#L6826))
— it plays no part in aim. One trap checked and cleared: the HLE `SCRPOSINP` path gates on
`screen_position_enable_r(index)`, which the driver never binds — but the callback's **default is 0xffff**
([jvs.cpp:278](../../src/devices/bus/jvs/jvs.cpp#L278)), i.e. "on-screen," so the gun reports position
normally. Aim flows through the identical `JVS_SCREEN_POSITION_INPUT_X1/Y1` ioports either way.

---

### Phase O2 — build the HLE input board — concrete spec — **DONE 2026-09-05, validated on-device**

Implemented essentially as spec'd below. `namco_tssio_hle_device` (bus/jvs/namcoio.cpp, next to
`namco_em_pri1_01_device`), registered as `NAMCO_TSSIO_HLE` / `"namco_tssio_hle"` in namcoio.h/jvs.cpp.
`timecrs2()`/`timecrs2v4a()`/`crszone()` in namcos23.cpp gate their `set_default_option()` behind
`m2vk_jvs::tssio_hle_enabled()` (new files `src/osd/libretro_m2/m2vk_jvs.{h,cpp}`, `M2VK_JVS_HLE` env
override), under `#ifdef S23VK` — the only namcos23.cpp edit. Also wired as a real menu entry,
`system23_jvs_hle` ("JVS HLE I/O Board"), hidden from every non-System-23 family; its DEFINITIONS
default_value is the one platform-conditional default in retro_options.cpp — "enabled" under
`__ANDROID__`, "disabled" elsewhere — so a desktop/harness A/B keeps comparing against the real board
until validation passes. `make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1` builds and
links clean (Windows box, 2026-09-04).

**UPDATE 2026-09-05: root-caused and fixed. timecrs2 boots bit-identical to the real board; crszone
boots to attract but is not yet byte-perfect.** `namco_tssio.7z`/`namco_csz1.7z` (the real I/O boards'
own small MCU ROM sets) were added to `devnotes/roms`, which made a real-vs-HLE differential trace
possible — the decisive tool this investigation was missing before.

**Root cause, found by tracing the real board.** `:subcpu` (H8/3002) is *itself* the JVS master — its
SCI0 is the JVS wire (`sci_set_external_clock_period(0, JVSCLOCK/8)` in namcos23.cpp) — and as part of
its own POST it sends a Namco vendor command, `0x70` with a fixed 3-byte body (`70 04 70 02` on the
wire), that it will not proceed past "SUBCPU INITIALIZING..." without a specific 11-byte reply. Traced
by temporarily enabling `h8_sci_device`'s `LOG_DATA` (the H8 SCI hardware peripheral itself, not the
board — this traces the real board's full-CPU emulation too, which `jvshle.cpp`'s own VERBOSE cannot)
against both real boards on `:subcpu:sci0`. Captured each board's exact reply and hardcoded it into a
`jvs_70_reply()` virtual on `namco_tssio_hle_device`, overridden per board. Not a generic ack — it
looks like a canned response to a fixed challenge, board-specific, not something synthesizable
generically. The same trace also turned up:
- The real boards' `command_revision`/`jvs_revision` are 0x11/0x20, not jvshle's base-class defaults
  of 0x13/0x30 — now overridden to match.
- The real `switch_count` is 12, not the 16 originally guessed from the driver's INPUT_PORTS bit
  layout — the O1/O2 planning note above this update is superseded on that number; 12 is ground truth
  from the real FEATCHK, not a bit-position guess.
- **crszone's real board is a CSZ1 MIU-I/O, not a TSS-I/O** — the "one class serves both games" premise
  below was wrong on this point. CSZ1 reports a different `device_id` string, 4 general-purpose
  outputs (not 3), and one analog output channel TSS-I/O doesn't have at all (the kick motor;
  `namco_csz1_device` really does add `analog_output_w` over the base `namco_tss_io_device`). Added
  `namco_csz1_hle_device : public namco_tssio_hle_device`, mirroring the real boards' own inheritance
  shape, registered as `NAMCO_CSZ1_HLE`/`"namco_csz1_hle"`; `crszone()` now defaults to it instead of
  the TSS-I/O class when the HLE option is on.
- A genuine upstream bug in `jvshle.cpp`'s `device_start()`: `m_screen_position_input_ybits` was
  never assigned — a copy-paste line assigned `screen_position_input_ybits()`'s result back into
  `m_screen_position_input_xbits` instead. Fixed (one line). Invisible for this driver specifically
  (both games' real xbits and ybits happen to both be 16), but a real bug for any future user of
  `screen_position_input_ybits()` where the two differ.

**Result, 3000-frame no-input runs, software renderer (`M2OPT_model2_renderer=software`), real board
(`M2VK_JVS_HLE=0`) vs HLE (`=1`):**

| Game | Real board digest | HLE digest | Match? |
|---|---|---|---|
| timecrs2 | `3a151f72c23d01db` | `3a151f72c23d01db` | **bit-identical** |
| crszone | `f6b25722eaf584e6` | `8e4577de95811f98` | visually identical, digest differs |

timecrs2 is a clean pass — full textured 3D attract intro, matching the plan's own "STARLINE NETWORK" /
"CREDIT 0/4" description, byte-for-byte identical to the real board over the whole 3000-frame run.

crszone reaches the same attract scene as the real board (the URDA commander, "INSERT 2 COINS") and
looks correct side-by-side, but the digest does not match — confirmed reproducible (re-ran the real
board twice, same digest both times, so this isn't run-to-run noise) and confirmed *not* a gross
protocol failure (same JVS command set exchanged in the same order on both, no extra resets/retries,
`RMSE` ≈ 13% between the two final frames — real but not a different scene). Not root-caused. Given
the one structural difference from timecrs2 is CSZ1's extra ANLOUT round-trip in the poll bundle
(5 commands/poll vs TSS-I/O's 4), a residual timing artifact from HLE's near-zero response latency
vs a real H8's actual instruction-cycle cost is the leading hypothesis, but this is speculation, not
verified. Worth revisiting if it matters for a future validation pass; not blocking for now given
crszone's own separately-noted "input issues" (Open Question 3, plan_system23.md) mean this game's JVS
path was already flagged as the one to watch.

**Vulkan render path is separately blocked on this box** (unrelated to the above, not investigated
further this session): the GPU had ~6.5 of 8GB used by other running apps, so even the known-good `vf2`
(Model 2) baseline hits `VK_ERROR_OUT_OF_DEVICE_MEMORY` and renders zero frames. Free up GPU memory
before trusting any Vulkan-path result, including `ab.sh`.

**Status against the original three validation steps:**
1. Host A/B bit-identical — **done for timecrs2**; crszone close but not exact (see above).
2. Boot to attract/service — **done for both games** (no scripted input; static digest/screenshot
   checks only, per CLAUDE.md).
3. Quest re-profile + numbered hand-check — **DONE 2026-09-05.** Headset came back online same
   session. Full sequence: `build-android.sh` REGENIE=1 (fast) → `deploy-android.sh` → user does
   Install-or-Restore-a-Core + hand-check → `rm -rf build/android/obj && REGENIE=1 PROFILER=1
   build-android.sh` → deploy → user runs the game → `adb logcat -d -s m2prof:V` → restore fast core,
   redeploy. Repeated for both games (the profiler pass needed doing twice — see the build gotcha
   below).

   **Result: `:jvs:...:iocpu` does not appear in the profile at all, for either game** — it's not
   merely down to ~1-2% as predicted, it's zero, because the HLE board has no CPU device
   (`device_add_mconfig` only calls `add_jvs_port`). Remaining cost sits exactly where the plan
   predicted: `:maincpu` (game logic/geometry — timecrs2 ~35-40%, crszone ~39-46%, higher because
   crszone's scene runs 3-4x the polygon count) and `:subcpu` (the H8/3002 *sound* MCU — a different
   chip from the JVS I/O board's H8/3334 this session targeted; ~20-25% on both games, unaffected by
   this work and squarely the plan's separately-noted "Lever 2," out of scope here). Nothing anomalous
   or wasteful showed up from either HLE class during real gameplay.

   **User-reported result:** timecrs2 "runs smoothly." crszone had been ~6fps before this session's
   work (not previously Quest-profiled on its own — the plan's headline numbers were captured on
   timecrs2 only) and is now ~45fps — not a full 57.5fps lock, but a ~7.5x improvement, and the
   profile shows the shortfall is legitimate `:maincpu`/`:subcpu` cost proportional to crszone's
   heavier scene, not a JVS-related inefficiency. Hand-check (gun aim/trigger, pedal, coin/start) came
   back confirmed for both games with no issues raised.

   ⚠️ **Build gotcha hit twice this session, worth flagging for next time:** an incremental
   `REGENIE=1` build (no `rm -rf build/android/obj`) does NOT reliably reflect a changed `PROFILER`
   value — GNU Make only rebuilds a `.o` when its *source* is newer, not when the *compiler flags*
   genie just regenerated for it change, so stale object files from an earlier build silently carry
   over into the new link. This produced a "fast" build that was byte-identical to the profiler build
   the first time it happened. **Always `rm -rf build/android/obj` before switching `PROFILER` on or
   off**, not just before turning it on — confirmed by size/hash/string-search
   (`grep -ac m2prof <the .so>` — 0 for a real fast build) before trusting either side of a rebuild
   that flips this flag.

Optionally still open: chase crszone's digest gap (visually identical to the real board, ~13% RMSE,
not root-caused — see above) if exact host-side parity matters later. Not blocking; the game plays
correctly and the performance win is confirmed on-device.

**One class serves both timecrs2 and crszone — CORRECTED 2026-09-05, this was wrong.** The premise
below (gun calibration lives in INPUT_PORTS, not the board, so the board is game-agnostic) is still
true for input handling, but the *boards themselves* are not the same part: crszone's is a CSZ1
MIU-I/O, not a TSS-I/O, with a different device_id, feature set, and vendor-command reply (see the
2026-09-05 update above). The as-built shape is `namco_tssio_hle_device` plus a small
`namco_csz1_hle_device : public namco_tssio_hle_device` override, mirroring the real boards' own
inheritance (`namco_csz1_device : public namco_tss_io_device`) — two classes, not one, but the second
is a handful of overrides on the first, not a rewrite.

**Placement (lowest-friction, most-upstreamable).** Add the class in
[namcoio.cpp](../../src/devices/bus/jvs/namcoio.cpp) in the anonymous namespace **next to
`namco_em_pri1_01_device`** (jvshle.cpp:1511) — that file already houses every Namco JVS board and the one
existing HLE board, so this matches upstream's own layout and touches no build scripts. Register it with
`DECLARE_DEVICE_TYPE(NAMCO_TSSIO_HLE, device_jvs_interface)` in
[namcoio.h](../../src/devices/bus/jvs/namcoio.h), `DEFINE_DEVICE_TYPE_PRIVATE(...)` at the bottom of
namcoio.cpp, and `device.option_add("namco_tssio_hle", NAMCO_TSSIO_HLE)` in `jvs_port_devices`
([jvs.cpp:447](../../src/devices/bus/jvs/jvs.cpp#L447)). *(The m2-vk "new logic in new files" rule targets the
renderer hooks; for an upstream `bus/jvs/` device, co-locating with the existing HLE board is the more
mergeable choice. A standalone `namcoio_hle.cpp` is the alternative but needs a `bus/jvs` genie edit.)*

**Class skeleton** (mirrors `namco_em_pri1_01_device` — note the empty `device_add_mconfig`: **no CPU**):

```cpp
class namco_tssio_hle_device : public jvs_hle_device
{
public:
    namco_tssio_hle_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0) :
        jvs_hle_device(mconfig, NAMCO_TSSIO_HLE, tag, owner, clock) {}
protected:
    virtual void device_add_mconfig(machine_config &config) override ATTR_COLD { add_jvs_port(config); }

    virtual const char *device_id() override
        { return "namco ltd.;TSS-I/O;Ver2.02;JPN,GUN-EXTENTION"; } // string non-critical; boot is generic

    // feature signature (drives feature_check() automatically)
    virtual uint8_t player_count() override                    { return 1; }
    virtual uint8_t switch_count() override                    { return 16; } // 2 bytes: trigger/pedal/enter/service/up/down/link
    virtual uint8_t coin_slots() override                      { return 1; }
    virtual uint8_t screen_position_input_channels() override  { return 1; }  // the gun
    virtual uint8_t screen_position_input_xbits() override     { return 16; } // informational; value passed raw
    virtual uint8_t screen_position_input_ybits() override     { return 16; }
    // analog_input / rotary / output / analog_output all default to 0 — see notes
};
```

- **No `device_input_ports()`, no `device_rom_region()`, no `execute()` override.** `configure_jvs` binds
  the hooks (so `m_default_inputs` is false and the board reads the driver's ports); the base `execute()`
  covers every command timecrs2/crszone issue.
- **`switch_count = 16`** covers every used bit in `JVS_PLAYER1` for both games (trigger 0x01, pedal 0x8000,
  enter 0x02, service 0x40, up/down 0x10/0x20, link-id 0x4000, crszone motor-test 0x2000 — all ≤ bit 15).
  ⚠️ This is the one number not lifted from a spec: if a button misreads on the hand-check, capture the real
  `namco_tssio` FEATCHK response (host run with `bus/jvs` command logging) and match its exact player/switch
  counts. Generic acceptance means a wrong count degrades a button, **not** boot.
- **Outputs left at 0.** timecrs2's gun-recoil solenoid and crszone's kick motor are general-purpose /
  analog JVS outputs — but `configure_jvs` binds **neither** `output()` nor `analog_output()`, so they are
  no-ops in the driver today. Setting `output_slots`/`analog_output_channels` would only feed unwired
  callbacks; leave them 0 until/unless force-feedback is wired (out of scope). crszone's "Motor test shows
  NG" is an **input** bit (`JVS_PLAYER1` 0x2000, [namcos23.cpp:7453](../../src/mame/namco/namcos23.cpp#L7453)),
  satisfied by switch input.

**Driver wiring — gate it, don't flip the default.** Per Posture, keep the accurate MCU path available.
Add the HLE board as the default **only under a core option** (default-on for Android): in
`timecrs2()`/`timecrs2v4a()`/`crszone()` ([namcos23.cpp:6914,6926,6973](../../src/mame/namco/namcos23.cpp#L6914)),
choose `set_default_option("namco_tssio_hle")` vs the real board from the option the way the sound-thread /
drive-board levers are gated. This is the **only** edit to `namcos23.cpp` (a guarded default-option line);
everything else is new device code in `bus/jvs/`.

**Validation (in order):**
1. **Host A/B, must stay bit-identical.** `./devnotes/ab.sh timecrs2` (and crszone) with the HLE board vs
   the real board — this changes input plumbing, not a pixel, so the `digest:` line must match
   [plan_system23.md](plan_system23.md)'s 23-x baselines exactly. A digest change is a bug, not progress.
2. **Boot both games** to attract/service with the HLE board (host `retrohost`, no input needed).
3. **Re-profile on Quest** (`PROFILER=1`, clean rebuild, `m2prof` ranking): target `:jvs:...:iocpu`
   dropping from 23% toward ~1–2%. Read percentages, restore the fast core after.
4. **Hand the user a numbered hand-check** (no scripted input — Posture): timecrs2 gun trigger fires,
   aim tracks, foot-pedal reload works, coin/start register; crszone the same plus watch whether its
   separately-noted input issues (Open Q3) change. Include the negative control (board set back to
   `namco_tssio` → identical behaviour, slower).

**Expected gain:** ~20% of the frame (23% → ~2%). On its own this moves 32fps toward ~45–50fps.

---

## Lever 2 — thread the H8/3002 sound MCU (20%)

`:subcpu` (the H8/3002, sound + inputs) is 20% and interpreted. This is the **direct analog of Model 2's
sound-68000**, which already has a threading plan and a proven Stage-0 gate
([m1audio-thread-plan.md](m1audio-thread-plan.md)): run the sound MCU on a second big core, let its reply
to the main CPU lag a frame or two, and the pixel digest stays bit-identical. Moving the H8 off the
emulation thread removes ~20% from the **critical** thread (it becomes parallel work, not removed work).

**Bigger job than Lever 1, and higher risk** — cross-thread latency, savestate interaction, and the H8
also handles *inputs*, so the JVS serial timing (Lever 1's territory) and the H8 thread interact. Sequence
it **after** Lever 1 lands, so the input path is settled before it moves to another thread. Reuse
m1audio-thread-plan's Stage-0 host gate (delay the reply, prove the digest holds) before building the
thread.

**Expected gain:** ~20% off the critical thread, contingent on the second big core being free (the Quest
gives cpu2–5 as big cores, [retroarch-quest-perf.md](../reference/retroarch-quest-perf.md) §4.2; the affinity pins are
already built, `m2vk_affinity.h`).

---

## Lever 3 — the smaller slices (only if 1+2 don't clear the bar)

- **`:maincpu` hot-path (29%, recompiled):** limited. It is already JIT'd; DRC-tuning (block linking,
  fastmem for the R4650's RAM windows) might shave a few percent, but Amdahl caps it and the code is not
  ours. Low priority.
- **Video Update 8% / OSD Blitting 6%:** GPU-side + the whole-frame image→buffer copy. The dirty-range
  upload idea ([performance.md](../reference/performance.md) §3.3) applies to Adreno bandwidth here where it was dead on
  desktop — but it is a small slice and should wait until the CPU wall is gone.
- **Unaccounted 12%:** partly the profiler's own tick reads (it disappears in a non-profiler build); do
  not chase it as if it were real work.

---

## The full-speed math

At 32fps the frame is ~1.8× too long (57.5 / 32). `:maincpu` (29%) is fixed. **Lever 1 removes ~21%
outright; Lever 2 moves ~20% to a parallel core.** Serial emulation-thread work drops from ~72% (three
CPUs) toward ~29% + overhead — roughly the halving required. **Full speed is a credible target, not
marginal polish** — but it needs *both* levers, and each is a real change, not a config toggle.

Order of attack, by bang-for-buck: **Lever 1 (I/O board HLE) → re-profile → Lever 2 (H8 thread) →
re-profile → decide on Lever 3.**

---

## Posture / gotchas (same rules as every phase here)

- **Measure, don't guess.** Every step ends with a re-profile (`PROFILER=1`, `m2prof` logcat ranking) and
  a host-side digest A/B. The ranking is the signal; the profiler's absolute fps is meaningless.
- **The profiler build is a clean-tree full rebuild** — it flips a global `MAME_PROFILER` define, so it
  needs `rm -rf build/android/obj && REGENIE=1 PROFILER=1` ([build-android.sh:80-82](../build-android.sh)),
  and it produces a deliberately ~2× slow core. **Back up the fast core and redeploy it after profiling**
  so the headset is never left on the slow one.
- **Windows build/deploy gotchas** (cost a session 2026-09-04, now in the memory): the Android build must
  run through `C:\msys64` MINGW64 bash with **`cd /e/m2-vk` as the first statement** and
  **`export OS=Windows_NT`**; the deploy needs adb on PATH, `M2VK_ANDROID_ROMDIR` set (no SD card on the
  Quest), and **`MSYS2_ARG_CONV_EXCL='*'`** or MSYS2 rewrites adb's `/storage`,`/sdcard` remote paths into
  `C:\…`. Scripts in the scratchpad avoid the inline-quoting trap.
- **The Install-or-Restore-a-Core step is mandatory after every deploy** — RetroArch silently keeps
  running the stale core otherwise ([retroarch-quest-perf.md](../reference/retroarch-quest-perf.md) §2.1); this reads
  exactly like "the change did nothing."
- **No scripted button-press testing.** Gun trigger / pedal-reload / aim verification is the user's
  hand-check ([plan_system23.md](plan_system23.md) 23-7, `lightgun.md`) — build it, run the static/digest
  guards, then hand a numbered list. Lever 1 in particular must not silently break aim.
- **Ship default vs core option.** An HLE I/O board that is accuracy-perfect can be the default; if it is
  merely *good enough for a pad/gun on a headset*, gate it behind a core option (default on for Android)
  the way the sound-thread and drive-board levers are gated, so the accurate MCU path stays available.
- **Commit hygiene:** all new logic in NEW files (a `jvs_hle_device` subclass is its own file); the only
  edits to upstream (`namcos23.cpp`) are a guarded default-option line. No AI nomenclature anywhere;
  `devnotes/` stays local-only.

## Open questions
1. ~~Which board identities do timecrs2 / crszone accept, and is any already-emulated board cheaper?~~
   **ANSWERED (O1):** the handshake is generic JVS enumeration, not a `device_id` match (several boards
   accepted, namcos23.cpp:14-16); and **no** already-emulated *input* board is lighter — every one runs a
   full MCU (the accepted `namco_tssio` is an **H8/3334**, not an MB90242A). No one-line win; build the HLE
   board.
2. ~~Can the HLE board deliver light-gun aim, or does HLE cost aim?~~ **ANSWERED (O1):** yes, fully. The
   gun is a JVS **screen-position** input (SCRPOSINP), handled by the HLE base; `subcpu:adc` is constant-0
   and unrelated; the unbound screen-position *enable* defaults to 0xffff (on-screen). No aim cost.
3. Does crszone's separately-noted "input issues" ([plan_system23.md](plan_system23.md) scope) interact
   with the I/O board choice — i.e. could Lever 1 fix *or* worsen it? **(Watch on the O2 hand-check — the
   HLE board reads the same ioports, so any csz1-firmware-specific input quirk would change.)**
4. After Lever 1, is `:subcpu` still 20%, and does threading it (Lever 2) actually free a big core, or does
   the JVS serial coupling to the H8 serialize them again? **(Note: with the iocpu now known to be an H8
   too, both remaining MCUs are H8-family — a shared H8-threading approach may cover both levers.)**
5. **NEW (O2 open):** the exact `switch_count` / FEATCHK bytes the real `namco_tssio` firmware reports —
   spec'd at 1 player / 16 switches, safe for boot; confirm against a real-board JVS trace only if a button
   misreads on the hand-check.
