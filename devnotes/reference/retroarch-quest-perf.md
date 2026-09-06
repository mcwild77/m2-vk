# RetroArch on the Quest 3 — the performance iteration loop

**Status, 2026-08-31: the plan of record for measuring CPU speed.** The per-frame optimisation work
is CPU-bound (see `performance.md` §2a/§6 and the device profile behind `m1audio-thread-plan.md`), and
the iteration cost of measuring it on-device was the whole friction: every change meant rebuilding the
`.so`, repackaging it into the deployment APK, sideloading, and then driving to a heavy scene by hand
in VR. This retires that inner loop. The perf leg runs the **same Android core** in **RetroArch on the
Quest 3 itself**, deployed by `deploy-android.sh` and launched by intent — a fraction of the turnaround,
and it can be driven from the same box that runs the correctness harness.

The full APK run does not go away: it stays as the once-per-milestone **absolute** acceptance gate (§7).
RetroArch is the fast **relative** dyno for every change in between.

---

## 1. Why RetroArch is a faithful stand-in — the load is entirely on the CPU

The one fact that makes this substitution sound: **the GPU is idle and the renderer is not on the
critical path.** The heavy-scene device profile has `gpu_busy` at 8–53 %, and the GPU *down-clocks
itself* to 492–545 MHz of an available 690 while the frame is late — it finishes early and coasts,
`thermal_pwrlevel = 0`. Nothing that a different frontend does to the *present* path can move a number
that is set by the interpreter finishing its frame. The whole ~13 % we are chasing is CPU: five
interpreted MAME CPUs at 6–12 % each, no DRC (`--NOASM=1`).

That cost lives inside the core, so it is **frontend-independent**:

- The core links no Vulkan library and resolves every entry point at run time from the frontend's
  `vkGetInstanceProcAddr` (`android.md` §1, `renderer_vk/vk_funcs.h`). RetroArch's Vulkan context and
  the deployment frontend's are the same contract — the core cannot tell them apart.
- It declares a stock `RETRO_HW_CONTEXT_VULKAN` with **no negotiation interface**
  (`renderer_vk/vk_context.cpp:532`) — it consumes whatever `VkDevice` the frontend hands it. RetroArch
  is the reference implementation of exactly that path.
- The per-device profiler that produced the diagnosis is a core-side hook (`m2vk_profile.h`, built with
  `PROFILER=1`) that writes straight to logcat. It reads identically no matter who is hosting the core.

So RetroArch runs the same instructions on the same big cores against the same 1.92 GHz cap. What it
does *not* reproduce is the deployment frontend's own CPU load — that is the reason §7 keeps the APK run
as the final word.

✅ **The context-negotiation question is already answered.** `vf2` and `daytona` boot, render and play
under RetroArch's Vulkan driver on an **Adreno 740** (`android.md` — Odin 2 Portal, the Quest 3's GPU
silicon family). The "does RetroArch drive our negotiation" unknown in `android.md` §6.2 is retired for
this GPU. Smoke-test it once on the Quest 3 (a different OS image and driver revision) before trusting a
number, then stop worrying about it.

---

## 2. What is already built vs. what is Quest-specific

The install and launch mechanics are done and documented in **`android.md` §7** and **`deploy-android.sh`**
— push the stripped `.so` to RetroArch's *downloads* directory (the cores directory is not adb-writable
on a production build), let RetroArch copy it into place, launch by intent. Do not re-derive any of that;
it works. Only the deltas below are Quest-3-specific.

| | Odin 2 (`android.md`) | Quest 3 |
| --- | --- | --- |
| RetroArch package | `com.retroarch.aarch64` | same (arm64 → aarch64 build) |
| ROM location | SD card, found by `fsLabel` | **no SD card** — internal storage `/storage/emulated/0/Roms/model2`, set `M2VK_ANDROID_ROMDIR` |
| Input | built-in gamepad | **pair a Bluetooth pad** (RetroArch's touch overlay is unusable in the 2D panel) |
| Video driver | `vulkan` (already was) | ⚠️ shipped as `gl` here — **had to flip to `vulkan`** in `retroarch.cfg`, or the core silently fails to load |

`deploy-android.sh` errors out looking for the SD card when there isn't one, so name the internal ROM
directory explicitly:

```sh
export M2VK_ANDROID_ROMDIR=/sdcard/Roms/model2      # /sdcard = internal, adb-writable.
                                                    # NOTE the folder is `Roms` (capital R only), see §2.1;
                                                    # deploy-android.sh's own default is all-caps ROMS, so this
                                                    # override is mandatory — do not let it fall through.
./devnotes/build-android.sh
./devnotes/deploy-android.sh daytona                 # strip + push core to downloads, ROM to ROMDIR
```

Prerequisite: install the aarch64 RetroArch APK on the headset and launch it once, so
`/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg` exists — the deploy script
reads its paths out of that file rather than assuming them.

RetroArch runs as an ordinary flat 2D Android app inside the Quest's panel. That is what we want: it
replaces the entire deployment-frontend CPU load with a thin host, leaving the emulation thread the run
of the big cores. (The system compositor still owns cores 6–7, same as always.)

### 2.1 Quest 3 first light — verified 2026-08-31 (and the gotchas that cost the session)

daytona built, deployed, and **ran on the Quest 3** under RetroArch's Vulkan driver. Concrete facts and
traps found on the way:

- **The ROM folder is `/storage/emulated/0/Roms` (capital R), not `ROMS`** — it already held a `VB`
  (Virtual Boy) subfolder. Dropped the sets in a new `model2/` beside it. `retroarch.cfg` here has
  `rgui_browser_directory = "default"`, so nothing pins model2 as the browse root — navigate to it.
- **On-device ROM copy, no Mac round-trip.** The sets were already on the headset in AoJ's download
  cache; copy them straight across rather than pull+push:
  `adb shell "cp -r /sdcard/Android/data/com.curif.ageofjoy/downloads/modelizer/. /storage/emulated/0/Roms/model2/"`
  (91 entries, 1.38 GB, verified by `du -sb` on both sides; the loose-file `manxttc/` + `overrev/` dirs
  survive the `/.` copy).
- **`video_driver` shipped as `gl`.** With `am force-stop` first (RA rewrites the file on exit):
  `sed -i 's/^video_driver = "gl"/video_driver = "vulkan"/'`. A GL driver + a
  `RETRO_HW_CONTEXT_VULKAN` core = a load that fails with nothing useful on screen.
- 🚨 **"Install or Restore a Core" is not optional and the installed copy goes stale silently.** The
  first launch after deploy **hung at ~0 CPU / ANR** — the core in the private cores dir was stale. A
  fresh *Install or Restore a Core* on the headset fixed it instantly (CPU jumped to ~90 %, TIME+
  climbing). If a launch hangs, reinstall the core before debugging anything else. Same class of trap as
  the desktop symlink-reverts-to-copy note in `CLAUDE.md`.
- **Launch by intent works, but the core only runs while the panel is focused in the headset.** An
  unfocused panel sits at `0:00.xx` CPU and reads exactly like a hang; take the headset off and
  wakefulness goes `Asleep` and daytona **pauses (not killed)** — pid and banked CPU-time survive, it
  resumes on wake. Verdict tool while blind: `adb shell "top -b -n1 -p <pid>"` — climbing TIME+ = really
  emulating, flat = unfocused/paused/deadlocked.
- **Pin the clock before any number:** `adb shell cmd power set-fixed-performance-mode-enabled true`
  (persists across headset sleep). §4 already says this; confirmed it takes on the Quest 3.
- First live baseline (no optimisation, plain build): daytona **choppy audio** (emulation missing the
  frame budget, as expected — CPU-bound) with OVR Metrics reporting ~57. This is the number Stage 1 is
  meant to move.

The Stage 0 gate for the first lever (thread the M1 sound 68000) **passed on the host** the same day —
sound→main reply delayed up to 2 frames leaves the pixel digest bit-identical, boot→attract *and* driven
into an actual race with the serial link flooded. Detail and digests in `m1audio-thread-plan.md`
(Stage 0). Stage 1 (`M2VK_SOUND_THREAD`) is the next build; its Stage 2 measure is this loop.

---

## 3. The loop, per change

1. **Build** the Android core: `./devnotes/build-android.sh` (`REGENIE=1` after any `scripts/` change).
2. **Deploy**: `./devnotes/deploy-android.sh daytona` — with `M2VK_ANDROID_ROMDIR` set as above. On the
   headset, `Load Core → Install or Restore a Core → libmodelizer_libretro_android.so` the first time
   after each rebuild (RetroArch copies it into its cores directory; the UI ANRs briefly on the ~57 MB
   copy — that is not a failure, `android.md` §7.1).
3. **Launch by intent** (no menu navigation — `android.md` §7.2), pointing `ROM` at the internal path:

   ```sh
   adb shell am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture \
     -e ROM      /sdcard/Roms/model2/daytona.zip \
     -e LIBRETRO /data/user/0/com.retroarch.aarch64/cores/libmodelizer_libretro_android.so \
     -e CONFIGFILE /storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg \
     -e DATADIR  /data/user/0/com.retroarch.aarch64
   ```

   (First launch after a rebuild: point `LIBRETRO` at the *downloads* copy to trigger the install, the
   *cores* copy thereafter — `android.md` §7.2.)
4. **Measure** on a heavy scene (§4).

The whole loop is one box: build the `.so`, `adb` it to the tethered Quest, launch, read the log. No
APK repackage, no VR walk.

---

## 4. Reading speed without the deployment `[speed]` line

The deployment frontend prints a `realtime` / `retro_run avg/max` line; RetroArch does not. The signal
is the same, read three ways:

- **Real-time verdict — audio + FPS.** RetroArch gates emulation to audio (`audio_sync = "true"`). A core
  that misses the frame budget crackles and underruns — the *same* audible symptom the deployment build
  has — and the on-screen counter (`fps_show = "true"`) falls below the driver's target refresh. This is
  the pass/fail: sustained target FPS on a heavy scene with clean audio.
- **Per-device split — the core's own profiler, and it is the authoritative one.** Build `PROFILER=1`
  (`m2vk_profile.h` enables MAME's `g_profiler` and dumps per-device % to logcat, tag `m2prof`). This is
  a direct `__android_log` call from inside the core, so — unlike RetroArch's own logging, which is
  near-silent to logcat and needs file-logging turned on (`android.md` §7.3) — it appears regardless of
  frontend config:

  ```sh
  adb logcat -c && adb logcat -s m2prof:V
  ```

  The profiler roughly doubles frame time, so read the **ranking**, not the absolute ms — exactly as on
  the deployment side. Ship builds default `PROFILER=0` to a dummy `g_profiler`, so this costs nothing
  when off.
- **`retro_run` timing.** Not exposed by RetroArch directly; the profiler split plus RetroArch's frametime
  counter cover it. If a per-frame `retro_run` number is wanted, the `m2prof` hook is the place to add a
  cheap logcat timer.

**Two things that must match the deployment measurement or the number lies:**

- **Heavy scene is mandatory** — a full-grid race, not attract. Light scenes already hit 1.00× and prove
  nothing. Drive it with the paired Bluetooth pad.
- **Pin the clock.** `adb shell cmd power set-fixed-performance-mode-enabled true` before measuring, so
  DVFS does not dip below the 1.92 GHz cap mid-run (worth ~7 % on its own, and it is system-wide, so it
  applies to RetroArch identically). It cannot exceed the cap — there is no clock headroom on this device.

### 4.1 First on-device ranking — daytona, heavy race, 2026-08-31

The PROFILER=1 core (throwaway build; ship builds stay PROFILER=0) run on the Quest 3 under RetroArch's
Vulkan driver, clock pinned, driven into a sustained full-grid race. The per-device split was **rock
steady across f=2640–3060** (`:copro_tgp` risen to 8–9 % confirms real 3D geometry load, not attract):

| Bucket | % | Note |
| --- | --- | --- |
| `:maincpu` (i960) | 12 % | |
| `:m1audio:sndcpu` (sound 68000) | **12 %** | **co-largest — the Stage 1 target** |
| `:copro_tgp` (TGP geometry) | 8–9 % | the one bucket that scales with scene density |
| `:ioboard:iocpu` | 8 % | |
| `:drivecpu` (drive-board Z80) | 6 % | force feedback we never use on a pad |
| *Timer Callbacks* | 13–14 % | |
| *Unaccounted/Overhead* | 28 % | inflated by the profiler's own tick reads — read the ranking, not this |
| *Video Update / Sound Generation* | 2 % / 1 % | GPU does the raster; confirms §1 |

**Three more titles, same session, same story — `srallyc` (Sega Rally), `motoraid` (Motor Raid) and
`dynamcop` (Dynamite Cop / Dynamite Deka 2), heavy scenes.** All steady across ~f=2100–2900. The sound
CPU enumerates as `:audiocpu` on these three (their own 68000 board) rather than daytona's
`:m1audio:sndcpu`, but it is the same device role:

| Bucket | daytona | Sega Rally | Motor Raid | Dynamite Cop |
| --- | --- | --- | --- | --- |
| `:maincpu` | 12 % | 14 % | 15–16 % | 17 % |
| sound 68000 (`:audiocpu` / `:m1audio:sndcpu`) | 12 % | 12–13 % | **17–18 %** | **18 %** |
| sound rank | tied #1 | #2 | **#1** | **#1** |
| `:copro_tgp` | 8–9 % | 6–8 % | 6 % | 5 % |
| `:drivecpu` (drive board) | 6 % | 8–9 % | — | — |
| `Video Update` | 2 % | 4–5 % | 4 % | 5 % |

The sound 68000 is the **outright largest device in two of four titles and never below #2** — and
`dynamcop` is a beat-'em-up, not a racer, so this is a Model-2-wide property, not a driving-game one.
Stage 1 (thread it) is backed by a genre-spanning sample. The drive-board Z80 is present only on the
wheel cabs (daytona/Sega Rally), absent on the bike (Motor Raid) and the brawler (Dynamite Cop), and
*higher* in Sega Rally than daytona; size that lever at ~6–9 % on the games that have it, zero elsewhere.

Three conclusions, the first two feeding `m1audio-thread-plan.md`:

1. **The sound 68000 is co-largest (12 %, tied with `:maincpu`), so Stage 1 is justified.** The decisive
   detail is that it held 12 % in *both* attract and the heavy race, while `:maincpu` eased 13→12 and
   `:copro_tgp` climbed 0→9 under load — the sound CPU is a **fixed** cost on the critical path, so it
   *becomes* the co-top bucket precisely when the frame is tightest. Best possible case for threading it off.
2. **The drive-board Z80 (`:drivecpu`) is 6 %** — the smallest CPU bucket, but nonzero and pure
   force-feedback with no effect on a pad. A clean ~6 % for zero gameplay cost → worth a **second lever**
   (gate/park `:drivecpu` when no wheel is bound) once Stage 1 lands.
3. **Interpreter hot-path work is the *other* class of lever, and the flat profile is exactly why.**
   Every bucket here is a plain switch-interpreter with no DRC (`:maincpu`/i960, the sound 68000, `:copro_tgp`,
   the two Z80s), so each has real local headroom — decode caching / computed-goto dispatch / fast-pathing
   the common opcodes typically 2–3× the *device*. But Amdahl caps the whole-frame payoff at the device's
   share: 3× on the i960 removes only `12 % × 2/3 ≈ 8 %` of the frame, not the "insane" whole-frame
   multiplier a hot-path buys on a machine with one dominant CPU (e.g. Model 3's PowerPC). That is not a
   reason to skip it — the gap being chased is only ~13 % (§2), so ~6–8 % off the i960 is a real chunk of
   it, it **stacks with the sound thread** (parallelism + faster interpreter compose), and on a flat profile
   several such ~6–8 % wins (i960, then `:copro_tgp`, then the I/O Z80) add up. It also carries **none of the
   cross-thread-latency risk** threading does. Order of attack: land the sound thread (Stage 2 measures it),
   then decide hot-path vs the harder copro thread by what the number says.

Method notes for a re-run: the private cores dir is not adb-writable on this non-rooted headset and
Android's linker namespace refuses to `dlopen` a core from `/storage/emulated/0/RetroArch/downloads`
(`namespace "clns-6"`), so the in-headset **Load Core → Install or Restore a Core** step is genuinely
mandatory — there is no shell path around it. After it, launch by intent against
`/data/user/0/com.retroarch.aarch64/cores/…` (not the downloads copy).

### 4.2 The pacing findings that invalidate §4's first bullet — 2026-09-01

**RetroArch cannot pace this core on the Quest, at all.** Established live (full chain in the
worklog entry of this date):

- `audio_sync = "true"` is **inert** — opensl writes never block here; a free-run reached 90+ fps.
  Audio crackle is therefore NOT a reliable real-time verdict on this device.
- The vrr_runloop timer undershoots ~6% (54 of 57.5); the unthreaded vsync path quantizes to 90 Hz
  vblank pairs (45-54); and a 60 Hz `video_refresh_rate` claim inside the default 5%
  `audio_max_timing_skew` makes RA time-warp 57.5 Hz content to video timing.
- Working config: `video_threaded=true`, `video_refresh_rate=90`, `audio_max_timing_skew=0.01`,
  `vrr_runloop_enable=false`, `video_vsync=false` — every RA limiter off — plus the
  **`model2_self_throttle` core option (default enabled on Android)**: MAME's own sleep+spin
  throttle paces the core exactly. Attract/select hold 57.5 flat with it.
- **The verdict instrument is now the core itself**: the on-screen fps counter
  (`model2_fps_display`) in-headset, and `m2vk_stallmeter.h` → `adb logcat -s m2stall:V`, which
  splits each emu-thread frame into cpu / park (frontend round-trip) / other (throttle + sched).
- Same session: big-core pins with periodic re-assert (`m2vk_affinity.h` — **Android wipes thread
  affinity on app-state transitions**, cpu0-1 little @1.38 / cpu2-5 big @1.92, and the 2.36 GHz in
  the freq table is never granted — no clock headroom), frame pipelining on Android (+1 frame
  latency, savestates dropped there by user decision), and the `model2_drive_board` park.
  Net: heavy-race worst 50 → ~55.5-56.7; the remaining gap and levers are in
  [plan_model2_quantum.md](../plan_finished/plan_model2_quantum.md).

---

## 5. A/B a core change on one binary

Every speed lever is gated so one build A/Bs cleanly (`M2VK_SOUND_THREAD`, `M2VK_SOUND_DELAY`,
`M2VK_ASYNC_PRESENT`, …). 🚨 **On Android `getenv` is null, so `M2VK_*` env vars DO NOT reach the core
here** ([retro_entry.cpp:908](../../src/osd/libretro_m2/retro_entry.cpp#L908), and the read site in
`m2vk_soundthread.cpp` / `vk_present.cpp`). Passing the switch on the app's environment before the intent
is a **no-op on the Quest** — it works only on the host harness. So:

- **Levers with a core option** (the sound thread → `model2_sound_thread`) A/B on-device through the
  option, i.e. RetroArch's core-options file, not the env. Set it there (or in the menu) between arms and
  confirm the core's own `[model2] model2_sound_thread=on|off` log line, then diff the `m2prof` ranking
  and the sustained FPS.
- **Env-only levers** (`M2VK_ASYNC_PRESENT`, `M2VK_SOUND_DELAY` — no backing core option) **cannot be
  toggled on the Quest at all**; they run at their built-in default. A/B those on the host, or add a core
  option first if an on-device sweep is needed.

Always re-run the correctness harness (`ab.sh` digests, on the host — §6) before believing a speed win is
free of an accuracy cost.

---

## 6. What this does NOT replace — correctness stays on the host

RetroArch-on-Quest is a **performance** instrument only. It has no per-frame digest and is not
bit-reproducible, so it cannot gate accuracy — same limitation any live device run has (`android.md`
§6, "none of the harness travels"). Correctness stays where it belongs: `ab.sh` on the host core, whose
FNV pixel digest is deterministic and bit-comparable to `ab-baselines.md`. The flow per change is
unchanged in principle — **validate on the host (digest matches baseline) → build the Android `.so` →
measure on the Quest** — only the last leg is faster now.

---

## 7. The absolute gate stays the deployment APK

RetroArch reads **optimistically** relative to the shipping build: it is a thin host, so it leaves the
emulation thread more of the big cores than the real frontend does (which runs its own main and render
threads alongside — ~29 % on the main thread in the device profile). A change that just reaches target
FPS in RetroArch has *not* been proven to hold once that load is added back, and thermals are gentler
without the VR render path on top.

So the milestone gate is unchanged: build the Android `.so`, repackage into the deployment APK, drive a
heavy race in VR, confirm the real `realtime` line clears the bar. Run it **once per milestone**, not
per change. RetroArch tells you which changes are worth taking that far.

---

## See also

- `android.md` — the install/intent mechanics this loop rides on (§7), and the first-light proof on
  Adreno 740.
- `m1audio-thread-plan.md` — the first speed lever (thread the sound 68000); its Stage 2 "measure on
  device" step is this loop.
- `performance.md` §2a/§6 — the desktop analysis that sized the CPU ceiling and established the GPU is
  not the constraint.
- `build-android.sh`, `deploy-android.sh` — the build and push.
