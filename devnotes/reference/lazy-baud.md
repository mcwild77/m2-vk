# The demand-gated i8251 baud clock (`M2VK_LAZY_BAUD`)

Built 2026-09-01, off the finding in [plan_model2_quantum.md](../plan_finished/plan_model2_quantum.md): every Model 2
machine carries one or two `clock_device`s at 500 kHz feeding `i8251_device::write_txc` + `write_rxc`.
`clock_device` fires on **both** edges, so each is 1,000,000 timer callbacks per emulated second —
99.87 % of all timer callbacks in the machine, and ~1 M forced scheduler breaks/s at ~85 ns each. The
callbacks are cheap; the **break points** are the cost.

**Result: −37 % to −57 % of desktop core ms/frame, essentially at the plan's `NOUART` upper bound.**

Code: `src/osd/libretro_m2/m2vk_baud.{h,cpp}` (all the logic). Guarded hooks only in `model2.cpp`,
`segam1audio.cpp`/`.h` and `i8251.h`.

**Shipped as a core option: `model2_lazy_baud` — "Fast Sound-Link Timing", default ON, reload-gated.**
Visible on the Model 2 and Model 1 menus (the two families whose machines carry a 500 kHz `CLOCK` into
an i8251); hidden from System 22/21/23, which have no such device. `M2VK_LAZY_BAUD` still overrides the
option in both directions, per the M2VK_*-wins rule — verified all four ways. The option exists because
**`getenv` is dead on Android**, and the Quest is the target where this matters most.

⚠️ **When A/B-ing it by ear, disable `model2_sound_thread` first** — that feature has an open bug that
silences daytona's SFX ~11 s in regardless of this one
([m1audio-thread-plan.md](../plans/m1audio-thread-plan.md)), and it confounded two rounds of hand-checking here
before it was spotted.

## What it does

Same edges, same i8251, same emulated instants — on the same grid a free-running `clock_device` would
have produced (edge *k* at *k*/(2·clock), anchored at machine time 0). What changes is that only an
edge the UART can **act** on gets a timer.

| case | why it is skippable | saving |
|---|---|---|
| TX between bit boundaries | `transmit_clock()` is a pure `m_txc_count++` until it reaches `m_br_factor`. The bulk add **stops short of the boundary** — see the bug below | 500 k/s → 31.25 k/s |
| RX mid-character | `receive_clock()` only does `--m_rxc_count` until the sample point. Same rule: the bulk subtract stops short of it | 16× |
| RX waiting for a start bit | the line is steady and the shift register's top bit already equals it, so every sample sees no transition and does nothing. Sleeps outright and wakes on `write_rxd` | ~all of it |

Nothing is **batched**: start-bit detection still runs at the full 16× rate, so it cannot quantise to a
whole bit. Two things make the skips exact rather than approximate:

- **The idle replay.** A skipped idle edge still shifts the 16-bit receive register. On waking, up to
  16 real edges are replayed — 16 shifts fully replace the register, so it ends bit-identical however
  long the sleep was. Without at least one, a start bit after power-on is missed: the register starts
  at 0 while the line idles at 1, so `previous_bit` never becomes 1 and no 1→0 transition is ever seen.
- **The wake points.** The generator sleeps, so everything that can change the UART goes through it:
  the register window (`uart_r`/`uart_w` — a mode byte rewrites `m_br_factor` and zeroes both dividers,
  a command byte can enable the receiver) and RxD (`rxd_w`). Reads are forwarded untouched: `data_r`
  and `status_r` touch neither divider nor the receive register, and the CPU polls status hard.

**One deviation from the plan's design.** It proposed that a fully idle transmitter needs no timer at
all until `data_w`. It does. An idle bit boundary still calls `update_tx_ready()`/`update_tx_empty()`,
which reach `model2_state::sound_ready_w`, which **re-asserts** IRQ bit 10 whenever TxRDY is still
high — so if the CPU has cleared `m_intreq` in between, the next idle boundary raises it again. That
is observable, so TX keeps its 31.25 kHz boundary unconditionally. It costs almost nothing: 31.25 k/s
against the 1 M/s removed is under 4 % of the break points, and the measured numbers land at the
`NOUART` floor anyway.

Synchronous mode is not skipped at all — `sync1_rxc`/`sync2_rxc` act on every edge — so `arm_rx()`
never sleeps there. Model 2's MIDI link is async 8N1, so this costs nothing in practice.

## 🚨 The bug the first cut shipped with — read this before touching `sync_tx`/`sync_rx`

The first version bulk-added **all** elapsed TX edges on a wake, on the stated invariant that "the
boundary edge is always the one we scheduled, so a wake can never step over it." **That invariant is
false.** MAME lets a CPU overshoot a pending timer by up to one instruction inside a timeslice, so a
register access can land *after* a bit boundary whose callback has not run yet. Measured on daytona:
at 55.929,940,040 s the game wrote to the UART **40 ns** after a boundary due at .940,000.

The bulk add then walked `m_txc_count` onto exactly `m_br_factor` without `transmit_clock()` ever
running. `transmit_clock()` only resets the counter when it **equals** the factor, so the next tick
made it factor+1 and **the transmitter never fired again for the rest of the run**. Symptom: music and
voice (sent early) play, every later command is gone — a stuck engine drone and no SFX at all.

The fix is that a bulk advance never steps onto or past a boundary: it stops one short and leaves the
boundary to the timer, which arms at zero delay and delivers it just after the register access — the
same order stock MAME produces when the CPU overshoots. Delivery therefore accepts an edge whose time
has **passed** (`edge_time(next) <= now`), not only one landing exactly on `now`, and `arm()` clamps a
target already in the past. The RX sample point had the identical hazard and the identical fix.

`arm_tx()` now carries a permanent guard: if the TxC divider ever overtakes the factor it logs once and
names the failure. One comparison per bit. It is the check that would have found this in seconds.

⚠️ **The reason it survived every digest**: all of them ran attract mode, where daytona moves 48 bytes
in 43 s. The stall needs sustained traffic *and* a write inside that narrow window; it first fires 56 s
into a race. `ab.sh`, the audio digests and the savestate sweep were all measuring a path that never
exercised it. See the verification section — the gameplay byte-stream comparison is the check that can.

## What it does NOT preserve: scheduler interleaving

⚠️ **This is the one behaviour change, and it is real.** Removing ~1 M break points/s changes where
`timeslice()` cuts the CPU, so devices interleave differently. Three of five perf fixtures hold their
picture digest bit-exactly; **vf2 and vcop2 move**, and they move by a sub-pixel geometry phase, not a
rendering defect — 0 to 0.06 % of pixels on the frames sampled, final frame bit-identical on vf2.

⚠️ **"The dummy-clock control passes" is not on its own a licence to call a difference benign.** It was
passing while the transmitter-stall bug above was live: the control genuinely isolates the UART from
interleaving, but it says nothing about a path the run never reaches. Quote it for what it shows and
check the traffic separately.

The control, run on both: with the lazy clock **on** plus a dummy `CLOCK` that restores the break
points and drives nothing,

| game | stock | lazy | lazy + dummy 500 kHz |
|---|---|---|---|
| vf2 video | `3fe1c65ec124e202` | `da9e9028e18b7936` | `3fe1c65ec124e202` |
| vcop2 video | `959289e28ea8f11e` | `7b91faad8f284c39` | `959289e28ea8f11e` |
| daytona **audio** | `d0d75dbf470ad402` rms 3258.5 | `4aaa2910521c07b2` rms 3319.0 | `d0d75dbf470ad402` rms 3258.5 |

Bit-identical in every restored arm. `M2VK_LAZY_BAUD=2` (eager: this device, a timer for every edge)
also reproduces the stock digests exactly, which separates "the mechanism is wrong" from "the machine
noticed the missing breaks". Modes 3 and 4 are TX-lazy-only and RX-lazy-only; both hold stock digests,
so neither half is individually responsible.

## Measured

`retrohost --vk`, 2500 frames, second boot, own `M2_SAVE_DIR` per arm. `perf: core ms/frame`.

**Final numbers, taken on an idle machine 2026-09-01** (every earlier table in this file's history was
measured with sweeps running underneath it):

| game | board | LB=0 | LB=1 | saving | plan's `NOUART` floor | video digest | audio digest |
|---|---|---|---|---|---|---|---|
| daytona | 2O | 3.646 | **2.283** | −37 % | 2.018 | same | interleave |
| vf2 | 2A | 3.821 | **2.336** | −39 % | 2.047 | interleave | interleave (rms 2062.2→2062.0) |
| srallyc | 2A | 3.979 | **2.084** | −48 % | 1.570 | same | **same** |
| vcop2 | 2A | 3.426 | **2.074** | −39 % | 1.968 | interleave | interleave (rms 211.8→211.5) |
| fvipers | 2B | 4.771 | **3.078** | −35 % | 2.985 | same | **same** |
| vf | M1 | 3.274 | **3.075** | −6 % | — | **same** | **same** |
| swa | M1 | 3.082 | 3.011 | −2 % | — | **same** | **same** |
| vr | M1 | 3.550 | 3.391 | −4 % | — | **same** | **same** |
| ridgerac | S22 | 1.283 | 1.291 | — | — | same | same |
| starblad | S21 | 4.956 | 4.914 | — | — | same | same |

Model 1's small gain is expected and is the obvious follow-up: `segam1audio`'s board clock is gated,
but `model1.cpp`'s own `m1uart_clock` is still a stock `CLOCK` at 500 kHz. It was left alone because
`model1.cpp` compiles in the `mame_model1` project, which scopes on `M1VK`, not `M2VK`.
System 22/21 have no i8251 baud clock and are unaffected (they are the no-op control).

### Dead end: a machine-wide quantum is not a fix

Now that the baud clock no longer forces one, `set_maximum_quantum` actually binds — so it looks like a
way to buy interleave back. It is not. Measured (daytona audio digest returns to stock at 2 µs, core
2.970 vs stock 3.611), but the same 2 µs quantum **breaks srallyc's audio digest**, which is
bit-identical under plain lazy, and gives vcop2 a *third* video digest belonging to neither arm. It is
just another arbitrary interleave, not a restoration of the old one. Do not re-propose it.

## Verification run

🚨 **Attract mode cannot verify this feature.** The transmitter-stall bug above passed every
attract-mode measurement in this section. The check that catches it is a **scripted-gameplay byte-stream
comparison**: run both arms with an input script that coins up and starts a race, log every `data_w`
and `receive_character` on both UARTs with `machine().time()`, and diff the per-UART sequences.

```sh
# throwaway probe in i8251.cpp data_w + receive_character, env-gated; do not commit
SCRIPT="600:select:20,900:start:20,1300:start:20,1700:start:20"
env M2_SAVE_DIR=$(mktemp -d) M2VK_LAZY_BAUD=$lb M2VK_UARTLOG=1 M2OPT_model2_renderer=vulkan \
  ./devnotes/retrohost --vk ./modelizer_libretro.dylib devnotes/roms/daytona.zip 6000 /tmp/x.ppm "$SCRIPT"
```

daytona is the fixture for it: its **video digest is identical in both arms** under that script, so the
i960 provably ran the same code and any byte-stream difference is the link. Result after the fix —
**3246 bytes vs 3246**, and each UART's own sequence byte-for-byte identical (1623 each direction).
Before the fix it was 3246 vs 1196. For the SCSP games the games themselves diverge under script, so
compare lazy+dummy-clock against stock instead: vf2 402/402 and fvipers 492/492, sequences identical.


- **`ab.sh` across 14 fixtures, re-run on the final binary** — every one passes exit criterion 1,
  `white = 0`, zero interior disagreement. `dynabb97` and `waverunr` sit off [ab-baselines.md](ab-baselines.md) at LB=1
  (90.442 / 93.216 vs 92.11 / 91.33) purely because frame 2500 lands on a different moment of the
  scene; **LB=0 reproduces the baselines exactly** (92.119 / 91.334), and coverage agreement is
  1.0000 in all four.
- **Savestates 16/16 PASS** on the final binary — all 8 fixtures in *both* arms (`state.sh`, 2000
  frames / save point 1500, all four boards).
- **Sound thread** (`model2_sound_thread=enabled`, daytona): same video digest, no crash. ⚠️ Its audio
  ring reads rms 0.0 under `retrohost` in **both** arms — pre-existing, not this change.
- 🚨 **A green `ab.sh` cannot see sound.** That is why `retrohost` now prints an `audio:` line — an
  FNV digest and RMS over every sample — next to the picture digest. It caught Model 1 going
  **silent**, because `model1.cpp` feeds the board's RxD through `segam1audio_device::write_txd`,
  which was not routed through the generator. Fixed inside the board device so every caller
  (model1.cpp, manxttdx, the sound-thread worker) is covered by one path.
- ⚠️ **And an `audio:` digest that merely *differs* is not a verdict either.** It read "different, same
  RMS" for the transmitter stall — which was total TX death — because attract mode never triggered it.
  RMS within 2 % is not evidence of correctness.

## Traps this work hit

1. **`subdevice<T>()` is a `downcast`, not a `dynamic_cast`.** With `M2VK_LAZY_BAUD=0` it hands back
   the stock `clock_device` reinterpreted as a generator, and the next call segfaults. Use
   `dynamic_cast` on the untyped `subdevice()`. Cost: one crash in the sound-thread bridge.
2. **The audio path needs its own digest.** Video held while Model 1's sound was completely dead.
3. First boot writes NVRAM and digests differently from every run after — every arm runs twice with
   its own `M2_SAVE_DIR`.
4. **A CPU can be past a timer that has not fired.** Any "the scheduled event must come first" argument
   about emulated ordering is wrong by up to one instruction. This cost the transmitter-stall bug.
5. **Attract mode is not the emulator.** The user found in one race what a 14-fixture digest sweep,
   an 8-fixture savestate sweep and an audio digest all missed.
