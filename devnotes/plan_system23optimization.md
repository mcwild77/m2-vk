# System 23 optimization plan — reclaim the frame after the DRC win

Make **Namco System 23 / Super System 23** (Time Crisis II, Crisis Zone) run at full speed on the
Quest 3. The renderer is already done and off the critical path
([plan_system23.md](plan_system23.md), 23-0 → 23-7, texel-exact to software); this plan is about the
**CPU**, which is what actually sets the frame here — same conclusion as every family on this device
([performance.md](performance.md) §6.1, [retroarch-quest-perf.md](retroarch-quest-perf.md) §1).

## Status / headline (2026-09-04)

- **The native ARM64 recompiler was the first and biggest win.** The Android build's `drcbearm64` UML
  backend (switched on 2026-09-01, [build-android.sh:98-100](build-android.sh)) recompiles System 23's
  R4650 (MIPS III) main CPU instead of running it through the portable C backend. Confirmed linked
  (32 `drcbe_arm64` symbols in the `.so`). On the Quest under RetroArch, **timecrs2 went from ~7fps to
  ~32fps** — a stale/C-backend core was most of the problem. **If a System 23 core still reads ~7fps,
  it predates this backend — rebuild first, before anything in this plan.**
- **Still ~half speed (32 of 57.5).** Profiled on-device 2026-09-04; the remaining wall is two
  **interpreted MCUs the recompiler does not touch.** This plan is the path from 32fps to full.

## The measurement — where the frame goes

**timecrs2, heavy in-game scene, `PROFILER=1` core** (throwaway build; the profiler roughly doubles
frame time, so read **percentages, not fps**). Rock-steady across f=2940–3180 (`m2prof` logcat,
method = [retroarch-quest-perf.md](retroarch-quest-perf.md) §4.1):

| Device | % of frame | What it is | Recompiled? |
|---|---|---|---|
| `:maincpu` | **29%** | R4650 (MIPS III) — main game CPU | ✅ `drcbearm64` |
| `:jvs:namco_tssio:iocpu` | **23%** | JVS **I/O board** MCU (MB90242A, F2MC-16F) | ❌ interpreter |
| `:subcpu` | **20%** | H8/3002 — sound/inputs MCU | ❌ interpreter |
| Video Update | 8% | GPU-side / present | — |
| OSD Blitting | 6% | | — |
| Unaccounted/Overhead | 12% | inflated by the profiler's own tick reads | — |
| Timer Callbacks / Sound Gen / Input | ~2% total | | — |

The three CPUs are **72% of the frame**. `:maincpu` at 29% is already recompiled and about as low as the
main CPU goes — that headroom was spent on the 7→32 jump. **The remaining wall is the two interpreted
MCUs (43% combined), and neither is game logic:** one is an I/O board, one is the sound MCU.

Device identities confirmed in the driver: `:subcpu` = `h83002_device` m_subcpu
([namcos23.cpp:1932](../src/mame/namco/namcos23.cpp#L1932)); the JVS board is a `bus/jvs/` device
([namcos23.cpp:1234](../src/mame/namco/namcos23.cpp#L1234), `#include "bus/jvs/namcoio.h"`) connected to
the H8's serial port #0; the TSS-I/O runs the real MB90242A F2MC-16
([namcos23.cpp:933](../src/mame/namco/namcos23.cpp#L933)).

---

## Lever 1 — HLE the JVS I/O board (23%) — **do this first**

The I/O board costs **almost as much as the entire recompiled main game CPU**, and all it does is read a
light gun, a foot pedal, and coins. This is the same pattern as Model 2's drive-board Z80 — flagged in
performance.md as "6% of force-feedback we never use" — except **~4× bigger** and on the input path.
It is the single fattest, most disproportionate slice, and it is *not* game logic being emulated, it is a
peripheral microcontroller being interpreted instruction-by-instruction.

**The scaffolding to replace it already exists.** MAME has a JVS HLE framework —
`jvs_hle_device` ([src/devices/bus/jvs/jvshle.h](../src/devices/bus/jvs/jvshle.h)) — that speaks the JVS
protocol over the serial link **with no MCU emulated at all**. It is proven: `namco_em_pri1_01_device`
already derives from it ([namcoio.cpp:1511](../src/devices/bus/jvs/namcoio.cpp#L1511)). The catch: that
existing HLE board is a *printer/emblem* board, not an *input* board — so no board the System 23 games
currently accept is HLE'd. Lever 1 is therefore **write a small `jvs_hle_device` input board** (gun X/Y,
trigger, pedal, coins, start, service), not adapt an existing one — but it is a bounded task on existing
scaffolding, not a from-scratch device.

### Phase O1 — investigate (cheap, no build)
1. **Read `jvs_hle_device`** ([jvshle.h](../src/devices/bus/jvs/jvshle.h)/`.cpp`) — the virtuals a
   subclass fills (`device_id`, feature/analog/coin-slot counts, the read hooks).
2. **List the boards each in-scope game accepts.** timecrs2 defaults to `namco_tssio` (the heavy MCU
   board); the driver also iterates the whole option list and configures each
   ([namcos23.cpp:6803](../src/mame/namco/namcos23.cpp#L6803)). Determine, from the `:subcpu` handshake
   ("get past the subcpu check", [namcos23.cpp:14-16](../src/mame/namco/namcos23.cpp#L14)), **which board
   identities the game will accept** — the HLE board must answer the same `device_id`/feature query the
   real TSS-I/O does, or the game rejects it and hangs at boot.
3. **Cheapest-possible first probe:** before writing anything, try pointing the default option at a
   *lighter already-emulated* accepted board and re-profile — if any accepted board runs a cheaper core
   than the MB90242A, that alone might buy most of the 23% for a one-line `set_default_option` change.

### Phase O2 — build the HLE input board (if O1 shows no free board)
Subclass `jvs_hle_device` for the TSS-I/O's input map, wire it as a selectable `bus/jvs/` option, and set
it as the System 23 default (or gate it behind a core option — see Posture). **A/B against software on the
host first** ([ab.sh](ab.sh) digest vs [plan_system23.md](plan_system23.md)'s 23-x baselines): the render
output must stay bit-identical — this changes *input plumbing*, not a pixel. Then re-profile on the Quest:
the target is `:jvs:...:iocpu` dropping from 23% toward ~1–2%.

**Risk:** the light-gun analog aim path. The H8's ADC ([namcos23.cpp:1679](../src/mame/namco/namcos23.cpp#L1679),
`m_adc("subcpu:adc")`) and the board's analog channels must still deliver gun X/Y. An HLE board that drops
buttons-only would break aim. Verify aim in the hand-check (no scripted input — Posture).

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
gives cpu2–5 as big cores, [retroarch-quest-perf.md](retroarch-quest-perf.md) §4.2; the affinity pins are
already built, `m2vk_affinity.h`).

---

## Lever 3 — the smaller slices (only if 1+2 don't clear the bar)

- **`:maincpu` hot-path (29%, recompiled):** limited. It is already JIT'd; DRC-tuning (block linking,
  fastmem for the R4650's RAM windows) might shave a few percent, but Amdahl caps it and the code is not
  ours. Low priority.
- **Video Update 8% / OSD Blitting 6%:** GPU-side + the whole-frame image→buffer copy. The dirty-range
  upload idea ([performance.md](performance.md) §3.3) applies to Adreno bandwidth here where it was dead on
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
  needs `rm -rf build/android/obj && REGENIE=1 PROFILER=1` ([build-android.sh:80-82](build-android.sh)),
  and it produces a deliberately ~2× slow core. **Back up the fast core and redeploy it after profiling**
  so the headset is never left on the slow one.
- **Windows build/deploy gotchas** (cost a session 2026-09-04, now in the memory): the Android build must
  run through `C:\msys64` MINGW64 bash with **`cd /e/m2-vk` as the first statement** and
  **`export OS=Windows_NT`**; the deploy needs adb on PATH, `M2VK_ANDROID_ROMDIR` set (no SD card on the
  Quest), and **`MSYS2_ARG_CONV_EXCL='*'`** or MSYS2 rewrites adb's `/storage`,`/sdcard` remote paths into
  `C:\…`. Scripts in the scratchpad avoid the inline-quoting trap.
- **The Install-or-Restore-a-Core step is mandatory after every deploy** — RetroArch silently keeps
  running the stale core otherwise ([retroarch-quest-perf.md](retroarch-quest-perf.md) §2.1); this reads
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
1. Which board identities do timecrs2 / crszone actually accept at the subcpu handshake, and is any
   already-emulated accepted board cheaper than the MB90242A (a possible one-line win)?
2. Can the HLE board deliver the light-gun analog aim (the H8 ADC path), or does HLE cost aim?
3. Does crszone's separately-noted "input issues" ([plan_system23.md](plan_system23.md) scope) interact
   with the I/O board choice — i.e. could Lever 1 fix *or* worsen it?
4. After Lever 1, is `:subcpu` still 20%, and does threading it (Lever 2) actually free a big core, or does
   the JVS serial coupling to the H8 serialize them again?
