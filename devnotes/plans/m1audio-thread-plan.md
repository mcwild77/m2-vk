# M1 sound board threading — staged plan + desktop A/B recipe

**Goal:** run the SEGAM1AUDIO board (the 68000 sound board) on its own host thread, so heavy-frame
wall time approaches the slowest single device instead of the sum of all of them. On a Quest 3 the
Model 2 emulation is CPU-bound and the per-device profile is flat — five co-equal CPUs at 6–12 %
each — so parallelising a device is the right lever, not optimising one path. The sound 68000
(`:m1audio:sndcpu`) is co-largest (~12 %) and the loosest-coupled device, so it goes first. This is
the Model 1 sound-board / Daytona-class path (`model2o`); the later `model2a/b/c` SCSP path shares
`soundram` and is a separate, harder job.

## Why it's safe to split (feasibility spike, done)

- **Coupling is a serial cable + the audio mixer, nothing else.** SEGAM1AUDIO's only wires to the
  machine are the bidirectional i8251↔i8251 serial link (`write_txd` / `rxd_handler`) and its audio
  streams into the global speaker. **No shared RAM.** It's a genuinely separate board, exactly like
  the hardware.
- **The link tolerates ~1 frame of latency.** It's a MIDI-rate (31.25 kbps) fire-and-forget command
  stream, interrupt-driven both ways; the main never busy-waits on a byte-level reply inside a frame.
  A sound effect landing one frame late is inaudible.
- **The accuracy harness is a video-only guardrail.** `devnotes/ab.sh` / retrohost `hash_frame()`
  FNVs the frame buffer and nothing else. Threading the sound board doesn't touch the render path, so
  a **bit-identical pixel digest proves the main emulation is uncorrupted**; audio is ear-validated on
  device, and savestates must still round-trip.

## Two-box workflow (this is inherent, not a limitation)

Correctness and performance are measured on different machines and neither can do the other's half:

- **Mac (host core + `ab.sh`)** — the correctness gate. `ab.sh` is deterministic (scripted input,
  fixed frame count, software render) so its digest is bit-comparable. Every code change is validated
  here first: digest matches baseline ⇒ emulation not corrupted.
- **Quest 3 (Android `.so`, driven from the AoJ side)** — the performance verdict. The whole problem
  is a Quest-specific CPU ceiling; a desktop host proves nothing about the target `realtime`. And a
  live VR run has no per-frame digest and isn't bit-reproducible, so no accuracy gate is possible
  on-device.

So each change goes: **validate on Mac (digest) → build Android `.so` → measure on Quest.**

## Stage 0 — de-risk with NO threading (✅ PASSED 2026-08-31)

A fixed, single-threaded, order-preserving delay on the **sound→main serial reply** line, to answer
one question: *does the main gate its video on sound-reply timing?* If a ~1-frame delay leaves the
pixel digest bit-identical, the thread split is safe for video.

**Result — bit-identical, including under gameplay load.** Two passes, delay off vs 17400 µs (~1f) vs
34800 µs (~2f):
- `ab.sh daytona 2500` (boot→attract): `bg` and both `3d` digests identical across all three arms
  (`3d vulkan dadecf752e33441d`, `3d software 48bb93c7814cd3f4`).
- retrohost daytona 4200f driven into an actual race (coin → accel-jam past the selector → hold accel +
  weave, serial link flooded with engine/tyre sound; final frame confirmed on-track, LAP 1/8):
  `digest: 41c50490a932b852` identical across all three arms.

⇒ The main does not gate video on sound-reply timing, even mid-race. **Stage 1 is cleared to build.**

Implemented in `src/mame/shared/segam1audio.{h,cpp}`:

- `output_txd()` (the sound board's UART TXD → `m_rxd_handler` → main UART RXD) now, when the delay is
  enabled, enqueues `(deliver_time, state)` and delivers it `m_rxd_delay` later on a single
  `emu_timer` (`rxd_delay_tick`). Constant delay + monotonic enqueue ⇒ FIFO order == delivery order,
  so one timer armed on the empty→non-empty edge is exact.
- Delay is read once in `device_start` from the env var **`M2VK_SOUND_DELAY`** (microseconds; µs
  because Daytona's frame rate is odd). Unset / non-positive ⇒ zero delay ⇒ the old immediate path,
  byte-for-byte. `getenv` returns null on device, so shipping / Android builds are unaffected.
- Reply-only: the main→sound direction (`write_txd`) is untouched — it can't affect main video, and
  delaying replies is the faithful model of "the sound board runs a frame behind".

### Desktop A/B recipe (Mac)

```sh
# from the m2-vk repo root
make SUBTARGET=modelizer OSD=libretro_m2 REGENIE=1 NOWERROR=1 -j10   # → ./modelizer_libretro.dylib
./devnotes/build-retrohost.sh

ROMS=/path/to/Model2            # the folder holding daytona.zip

# baseline (delay OFF) vs ~1 frame delay (17400 µs ≈ 1 frame; also bracket with 34800 = ~2 frames)
                          ROMS=$ROMS ./devnotes/ab.sh daytona 2500 /tmp/ab-base
MODE="M2VK_SOUND_DELAY=17400" ROMS=$ROMS ./devnotes/ab.sh daytona 2500 /tmp/ab-delay

# compare the 3D video digests across the two runs
grep '^3d ' /tmp/ab-base/daytona.txt /tmp/ab-delay/daytona.txt
```

Boot→attract already runs the sound board and its reply path, so it's a valid first cut. For a harder
test, drive gameplay via retrohost's control script (`frame:control[:held][:port]`, e.g.
`600:select,660:start,...`) so engine sounds flood the link.

**Reading:**
- **Digests bit-identical** across baseline and both delay brackets ⇒ the main doesn't gate video on
  sound-reply timing ⇒ **Stage 1 is safe → proceed.**
- **Digests move** ⇒ **STOP.** The main gates video on reply timing; reassess the boundary before
  building any thread.

## Stage 1 — thread the board (behind `M2VK_SOUND_THREAD=0/1`, default OFF) — ✅ BUILT 2026-08-31

**Approach chosen: a second `running_machine`.** The m1audio subtree can't be peeled off MAME's single
global `device_scheduler`, so the board runs in its own headless machine, on a worker thread, stepped
~1 frame behind the main. All new logic is in **`src/osd/libretro_m2/m2vk_soundthread.{h,cpp}`**; the
only upstream edit is a **+29-line, `#ifdef M2VK`-guarded hook in `model2o_state::model2o()`**
(`model2.cpp`) — off by default, byte-identical when off.

How it fits together:

- **Second machine.** A synthetic `GAME(m1snd)` (internal linkage, never in the driver list) hosts one
  `SEGAM1AUDIO` tagged `m1audio`. Its ROM regions are declared empty and **memcpy'd from the main
  machine's already-loaded `m1audio:*` regions** in the driver's `machine_start` (before the 68000's
  first fetch) — so it needs no per-game ROM_START. Driven by a bare `machine_manager` shim (returns a
  no-op `ui_manager`) + a direct-`osd_interface` stub (`sound_osd`); `machine.run(true)` on the worker.
  Gotchas that each cost a debug cycle: `start_http_server()` must be called (run() derefs http()),
  `create_ui` must return a real (no-op) `ui_manager`, and one non-hidden `render().target_alloc()` is
  needed so exit-time config-save doesn't deref a null `m_ui_target`. Do NOT `set_system_name` (it
  validates against the driver list → "Unknown system").
- **Serial bridge, both directions.** Both machines share the t=0 emulated-time origin, so each
  direction is a time-tagged transition queue replayed on the RECEIVING machine's own scheduler
  (`serial_line`), preserving inter-bit spacing — the Stage-0 delay line generalised across threads.
  main→sound is scheduled at the tag's absolute time (worker lags, so it's future); sound→main clamps
  to "now" and preserves deltas (a real ~1-frame reply latency). **The sound→main timer must be
  allocated during the main machine's start (from `osd().init()`), while save registration is open** —
  allocating it post-start is refused by MAME.
- **Audio.** The board's mixed stereo lands in a mutex-guarded ring; `frame_audio()` presents it in
  place of the main machine's now-silent mix. The main machine gets a bare marker speaker
  (`m2vk_snd_null`) so validity passes — that speaker's presence is also how the OSD detects "this is a
  split model2o machine" (a config-time flag can't be used: the validity checker builds every model2.cpp
  driver's config, incl. daytona's, before the real machine, and would leak the decision into e.g. vf2).
- **Pacing.** The worker blocks in its `update()` until the main is ≥1 frame ahead, so it never runs
  ahead of the main (which would put main→sound transitions in its past).

**Verified on desktop (retrohost):**
- `M2VK_SOUND_THREAD=0` (default): daytona 2500f `48bb93c7814cd3f4` — **bit-identical to baseline**.
- `M2VK_SOUND_THREAD=1`: daytona 2500f **also `48bb93c7814cd3f4`** — the split doesn't perturb the main
  video at all (Stage 0's hypothesis, now fully confirmed); audio flows from the worker; clean shutdown.
- Non-model2o safety: vf2 (model2a/SCSP) with the flag on is unchanged — worker never engages, real SCSP
  audio intact (834/frame).
- Works under both `software` and `--vk`.

**Open items:**
1. **Savestate round-trip across the thread boundary — ✅ WIRED + verified 2026-08-31.** The OSD's
   `state_size/save/load` now length-prefix both machines into one buffer when `m2vk_snd::running()`:
   `[u32 main_len][main bytes][u32 worker_len][worker bytes]`. `state_size()` reports
   `8 + main + worker` (an exact upper bound; libretro sizes the frontend buffer from it), and load
   parses the prefixes rather than assuming offsets — main's trailer is variable-length, so a fixed
   offset would be wrong. When the worker is not running (flag off, or a non-model2o game) all three
   forward to the main machine byte-for-byte as before, so the seven non-threaded fixtures are untouched
   (re-verified: daytona/vf2/vcop2 digests identical to the pre-change baseline). Worker size is cached
   (each `m2vk_snd::state_size()` parks the worker) and dropped in `osd_exit()`.
   `M2VK_SOUND_THREAD=1 ./devnotes/state.sh daytona` → **C==D && N!=D, 5/5 runs**.

   Wiring the save exposed **two pre-existing bugs in the threaded path**, both fixed:
   - **Worker-park deadlock (intermittent save failure).** A savestate parks the MAIN emulation, which
     freezes `g_main_time`. A worker sitting in its *pace* wait (`g_main_time >= self+LAG`) could then
     never satisfy its predicate and never reach the park check, so `with_worker_parked` timed out and
     the WORKER half of the save failed at random. Fix: the pace wait now also wakes on `g_park_request`
     and loops back to park instead of running a frame; and `with_worker_parked` now blocks on a proper
     CV handshake (worker announces parked under `g_park_mutex` + `notify`) instead of a 100k-yield
     busy-spin that lost the race under load. (`m2vk_soundthread.cpp` `update()` + `with_worker_parked`.)
   - **Lua PANIC (intermittent, even with NO savestate — 2/8 plain runs).** The worker `running_machine`
     drives the global `emulator_info` frame/periodic/sound/UI hooks, which reach
     `mame_machine_manager::instance()->lua()` — the PRIMARY machine's shared `sol::state` — from the
     worker thread, concurrently with the main thread. Fix: a `thread_local` suppressor in
     `src/frontend/mame/mame.cpp` (`mame_suppress_frontend_hooks(bool)`, always linked; only the
     libretro worker flips it, so other OSDs are unaffected) no-ops the four `emulator_info` hooks on the
     worker thread. **0/12 plain runs crash after the fix.** ⚠️ New upstream-file edit: `mame.cpp`
     (self-contained — one thread_local, one setter, four one-line guards).
2. **Worker throughput.** On the unthrottled desktop harness the worker underruns (main runs ~5.5×
   realtime and starves it); the worker's absolute rate here is ~1.15× realtime. This is a **harness
   artifact** — on a realtime-throttled device the worker isn't starved, and the Quest profile predicts
   the board needs ~15% of a frame (≈6× headroom on a dedicated core). **This is exactly what Stage 2
   measures on-device; the desktop number is not predictive.**
3. Optional core option (menu toggle) — the env var drives it today; the menu entry is a shippable-pass nicety.

## Stage 2 — measure on the Quest — ✅ VALIDATED 2026-09-01 (RetroArch loop, not AoJ)

Measured on the Quest 3 under RetroArch's Vulkan driver, clock pinned, daytona driven into a heavy
full-grid race. The on-device enable is the **`model2_sound_thread` core option** (env is dead on
Android — `am start` does not propagate it — so the option is the only gate; wired 2026-09-01, see
`user-options.md`). A/B by editing `.../RetroArch/config/m2-vk/m2-vk.opt` and relaunching by intent
(RetroArch **Restart** = `retro_reset`, which does NOT rebuild the machine, so toggling the reload-gated
option then hitting Restart ANRs — switch arms by a full relaunch, never Restart).

| Arm | Present FPS | Process CPU |
| --- | --- | --- |
| Thread **OFF** (baseline) | **49.96** — below the 57.5 Hz target, i.e. choppy | 103 % (one core maxed, spilling) |
| Thread **ON** (Stage 1) | **57.86** — at target, realtime | 81 % main + 22 % worker; ~274 % of 6 cores idle |

**The sound thread takes daytona from ~50 fps to the 57.5 Hz target — from missing the frame budget to
hitting it, ~15 % throughput recovered, matching the profile's 12 % sound-CPU share plus the freed
scheduling slack.** The user's ear-test agreed decisively ("way freaking worse" on thread-OFF). Worker
engagement confirmed three ways: a second busy emulation thread (~22 %) in `top -H`, the savestate
growing 463 B (worker machine serialised, host-side), and the FPS delta itself.

Two measurement notes for a re-run (there is **no RetroArch FPS counter** in this build's HUD, and the
plain build's `[model2]` log is near-silent to logcat — only a `PROFILER=1` build's `m2prof` writes
directly):
- **Present FPS via SurfaceFlinger**, no in-app counter needed:
  `dumpsys SurfaceFlinger --latency-clear '<layer>'`, wait, `--latency '<layer>'`, then FPS from the
  actual-present-time column deltas. The live layer is the app-uid `…RetroActivityFuture#<n>` row with
  non-zero present rows; **its `#<n>` changes on every relaunch**, so re-discover it each arm.
- **AudioFlinger underruns are NOT the tell here** — both arms read `underruns=2` (startup only), because
  `audio_rate_control=true` resamples to dodge hard underruns, trading them for the slowdown/pitch-warble
  the ear hears. The present-FPS delta is the hard number; the ear is the perceptual one.

**Residual, separate from the thread:** even at 57.86 fps there is display judder — a 57.5 Hz core on a
fixed 90 Hz panel (90/57.5 non-integer → each frame shows for 1 or 2 vsyncs) — worsened by
`vrr_runloop_enable=true` + `audio_rate_control=true`. That is a RetroArch sync-config artifact, present
thread-on or -off, not a core problem. Not chased here; a config-tuning pass (vrr off / rate-control off)
is the lever if it bothers a player.

**Next lever** (Stage 1's win banked): the drive-board Z80 park (~6–9 % on the wheel cabs, zero on a pad)
and/or the broadened interpreter hot-path work — order by what a fresh `m2prof` ranking says now that the
sound bucket is off the main thread.

---

## ✅ FIXED (2026-09-03): the worker machine's construction was undefined behaviour

**Symptom on Windows:** every model2o set (daytona, desert, vcop) killed RetroArch with
`0xC0000005` — an access violation — the moment the sound thread was on. Never reached a frame.

**Cause, and it was never a Windows bug.** `worker_main()` built the worker machine from
`GAME_NAME(m1snd)`, and `driver_device`'s constructor caches a search path by walking its own clone
chain:

```
driver_list::clone(m_system) -> { index = find(m_system); assert(index >= 0); return clone(index); }
```

`m1snd` is deliberately absent from the generated driver list — that is the entire point of the
anonymous namespace — so `find()` returns **-1**, the `assert` is compiled out of a release build,
and the call proceeds to `driver(std::size_t(-1))`: an out-of-bounds index into `s_drivers_sorted`.
macOS and Android read a survivable value off the end of that array and carried on, which is why
this shipped and why Stage 2 validated on the Quest. Windows reads a bad pointer and segfaults
inside the constructor, before the worker machine exists.

**Fix** (`m2vk_soundthread.cpp`, no upstream file touched): the worker is built from a **copy** of
the m1snd descriptor whose `name` is the main system's short name — which is in the list by
construction, so `find()` succeeds and the ancestor walk is the real set's. `game_driver::name` is an
inline `char` array, so the copy owns its string, and the copy is declared before the config and the
machine so it outlives both. `start()` checks the name resolves before spawning the worker, on the
main thread, where declining is still an option.

**Verified** (`retrohost --vk`, 1500 frames, Windows):

| set | threaded | unthreaded |
|---|---|---|
| `daytona` | `4e127a7a92c659de` | `4e127a7a92c659de` |
| `desert` | `44b335a236d4407f` | `44b335a236d4407f` |
| `vcop` | `dc3602ff58e3316c` | `8f63cd7961a07b6c` |
| `vf2` (model2a control, never split) | `7afdc41b80d8c5f9` | `7afdc41b80d8c5f9` |

daytona and desert are bit-identical with the thread on or off; vcop's digest moves between arms,
which is the board split shifting device interleave — the lazy-baud class, not this fix.
User hand-check on Windows/RetroArch: daytona "looks and sounds correct".

⚠️ **This does not touch the serial-bridge bug above.** Different failure, different place: that one
is a live worker that stops delivering, this one was a worker that never got built.


## 🚨 OPEN BUG (found 2026-09-01): the serial bridge dies ~11 s in

**Symptom, heard by the user in RetroArch:** daytona plays its opening music and voice, then the engine
sticks at one cadence and no SFX, crashes or car sounds ever arrive. Reported twice, once as "stuck"
and once as "delayed by multiple seconds".

**Measured** (`retrohost --vk`, daytona, 6000 frames, scripted coin-up
`600:select:20,900:start:20,1300:start:20,1700:start:20`, a throwaway log in `i8251_device::data_w`
and `receive_character`):

| `model2_sound_thread` | main CPU transmits | board receives | last byte the board saw |
|---|---|---|---|
| **enabled** | 1622 bytes, out to 104.2 s | **96** | **10.93 s** |
| disabled | 1622 bytes | **1622** | 104.2 s |

So `g_to_sound` stops delivering about eleven seconds in and never resumes: **94 % of the command
stream never reaches the board.** Everything after that point is silence, which is exactly what the
game sounds like.

**It is not the demand-gated baud clock.** The numbers above are identical at `M2VK_LAZY_BAUD=0` and
`=1`, and `m2vk_soundthread.cpp`'s only edit for that feature is null when it is off (the
`dynamic_cast` returns null and the lambda falls through to the original `g_main_uart->write_rxd`).
This is a pre-existing fault in the bridge itself.

**Not yet diagnosed.** Prime suspects, in order: `serial_line`'s time-tagged replay queue (both
machines start at t=0, but if the worker's pacing point stops advancing past the main machine's time,
`pump()` would stop dispatching), and `pump_main`/the worker's pacing loop stalling while the worker
machine keeps running. Start by logging the queue depth and the worker's published time each frame and
find what stops moving at ~11 s.

⚠️ **The Quest validation did not cover this.** Worklog 2026-09-01 records Stage 2 "VALIDATED on Quest
(daytona ~50→57.5 fps)" — that was a *frame-rate* measurement. Nothing in it checked that the sound
board was still receiving, and the video digest cannot see the serial link at all. The check that finds
it is the byte-stream comparison above.
