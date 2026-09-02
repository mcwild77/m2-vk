# Optimization work — who does what next

Written 2026-09-01, at the end of the baud-clock session. This is the short list. The detail lives in
[lazy-baud.md](lazy-baud.md), [m1audio-thread-plan.md](m1audio-thread-plan.md),
[plan_model2_quantum.md](plan_model2_quantum.md) and [performance.md](performance.md).

**State of play in one line:** the demand-gated baud clock is built, verified and shipping ON as a core
option (−35 to −48 % of emulation-thread time on desktop); the sound thread — the *other* big lever, and
the one that bought 50→57.5 fps on the Quest — is **working and validated on hardware**. The two stack.

---

## 1. YOURS — the checks only a person can make

### 1.1 Ear test: Fast Sound-Link Timing (desktop) — **blocking**
The one piece of evidence that cannot be produced here. Every digest, savestate and byte-stream check
passes; two earlier rounds still turned up faults that only came out by ear.

**Threaded Sound now works** (validated on Quest, §1.3) so you may leave it on. To keep this a clean
test of the baud clock *alone*, hold Threaded Sound at one setting across both arms — change one thing
at a time.

| # | Game | What to listen for | If it is wrong |
|---|---|---|---|
| 1 | **daytona** | Coin up and race. Engine pitch tracking the throttle, other cars, crashes, tyre squeal, the announcer. | Engine stuck at one cadence, no SFX after the opening = the failure mode already seen twice. |
| 2 | **vf2** | Attract music, then a round: hit/impact and voice samples. | ⚠️ You reported "volume very soft, music normal" — **still unexplained**. Its RMS is ~2062 in every arm including stock, so it may be pre-existing. This test decides that. |
| 3 | **vf** (Model 1) | Attract music plus in-round effects. | This family went fully silent once (fixed); confirm it stayed fixed. |
| 4 | **Negative control** | Repeat whichever sounded wrong with the option set to **disabled**. | Same both ways = pre-existing, not this feature. Right only when disabled = a real bug, tell me. |

Toggle is **Core Options → Fast Sound-Link Timing** (`model2_lazy_baud`), reload-gated.

### 1.2 Hardware test: the Quest 3 — the case this was built for
Desktop cannot show the win (the core already runs 400 %+ of real time there); a CPU-bound device can.

1. Core is pushed to the headset. **Load Core → Install or Restore a Core →
   `libmodelizer_libretro_android.so`** — re-do this copy-into-place after every rebuild or RetroArch
   keeps the old one.
2. Threaded Sound **on** (validated, §1.3), Fast Sound-Link Timing **on**, then a heavy daytona race.
3. Same again with Fast Sound-Link Timing **off**. Report the fps difference and whether the sound
   holds up in both.

The prediction on record: a scheduler break costs 2.5–4× more on Adreno than the ~85 ns measured on
desktop, so the win should be *larger* there. That is a prediction, not a measurement — this test is
what turns it into one.

### 1.3 Threaded Sound — working
Validated on the Quest (daytona ~50→57.5 fps, sound holds). It is enabled in your live
`config/m2-vk/m2-vk.opt` and fine to leave on. The serial-bridge byte-drop that was §2.1 is fixed.

### 1.4 Two option defaults that will not reach your install
RetroArch persists chosen values, so a key already named in `m2-vk.opt` keeps its old value whatever the
core now defaults to. Both are one line in the core options menu:

- **Self-Paced Timing** (`model2_self_throttle`) — now defaults ON, yours says `disabled`. Without it,
  RetroArch paces to a 60 Hz panel against a 57.5242 Hz machine and the game runs **~4.3 % fast**.
  (RetroArch's own "Sync to Exact Content Framerate" fixes it too.)
- **Threaded Sound** (`model2_sound_thread`) — yours says `enabled`; that is the intended setting (§1.3).

---

## 2. MINE — next steps here

### 2.1 ~~Fix the sound-thread serial bridge~~ — **RESOLVED**
Fixed and validated on the Quest (daytona ~50→57.5 fps, sound holds through a race). The old symptom —
the board receiving 96 of 1622 bytes and stopping at ~11 s while the main CPU transmitted to 104 s — is
gone; the byte stream now tracks the main CPU. It stacks with the baud clock, and no longer blocks clean
sound testing. Background in [m1audio-thread-plan.md](m1audio-thread-plan.md).

### 2.2 Re-measure the per-device profile before committing to the MB86233 (TGP) recompiler
The 2026-09-01 worklog picked the DRC as the next big job. That argument was made when the scheduler was
eating ~40 % of the time on break points that are now gone. A recompiler makes each dispatch cheaper
without making them fewer, so the ratio it has to beat has changed. Re-take the profile — on the Quest,
not on desktop — before spending that effort.

### 2.3 Model 1's own baud clock
`model1.cpp` has a second 500 kHz `CLOCK` (`m1uart_clock`) feeding its own i8251, still stock. That is
why Model 1 only gains 2–6 % where Model 2 gains 35–48 %. Left alone because `model1.cpp` compiles in the
`mame_model1` project, which scopes on `M1VK`, not `M2VK` — a small build-script decision, then the same
one-line hook. Cheap, and only worth doing if Model 1 performance ever matters.

### 2.4 Standing change to how this gets verified
🚨 **Attract-mode digests cannot verify a sound path.** A transmitter that was completely dead passed 14
`ab.sh` fixtures, 8 savestate fixtures and an audio digest, because daytona moves 48 bytes in 43 s of
attract and the fault first fires 56 s into a race. Anything touching the serial link, the sound boards
or device timing now gets a **scripted-gameplay byte-stream diff** as well — the method is written up in
lazy-baud.md's verification section. Cost: one extra run per arm.

---

## Closed this session, for the record
- Demand-gated baud clock built, shipping as `model2_lazy_baud` (default ON, Model 2 + Model 1 menus).
  −35 to −48 % desktop. `ab.sh` 14/14, savestates 16/16, UART byte streams identical through a race.
- Fixed a transmitter stall of my own making (a bulk counter advance stepping over a bit boundary the
  CPU had overshot); permanent guard added.
- Fixed Model 1 going silent (the board's RxD bypassed the generator).
- `retrohost` now prints an `audio:` digest + RMS beside the picture digest.
- Self-Paced Timing now defaults ON everywhere; `retrohost` pins it off for itself so sweeps stay at 4×.
- **Dead end, do not re-propose:** `set_maximum_quantum` as a way to buy back the old interleave. It
  fixes daytona and breaks srallyc.
