# P5 — internal resolution, as built (2026-07-28)

**The core renders into a framebuffer of the player's choosing and hands the frontend that
framebuffer.** Nine resolutions, `496x384` (the hardware's own) to `2848x2136`, live from the options
menu.

This file is as-built, not a plan. Read it with [res-baselines.md](res-baselines.md), which measures
the *other* feature that shares this code path and is easy to mistake for this one.

---

## 1. What was wrong

`model2_internal_res` shipped earlier the same day as a menu on top of `M2VK_SS`. That switch was
built for P4 step 2 as a **measurement**: draw the frame into an n× attachment, then **resolve it back
down to 496×384**, because the accuracy harness compares against MAME's software rasteriser and MAME
cannot draw at anything else.

As a player option that is an antialiasing setting wearing an internal-resolution label. It worked —
1×/2×/3×/4×, live in both directions, switch-beats-option precedence, all verified — and it was still
the wrong feature. Measured on vf2's last frame: **782 unique colours at 1×, 11172 at 4×**. Every one
of those extra colours is a blend produced by throwing the resolution away.

🚨 **The two features still share one render path and the difference between them is the resolve
pass.** Anything written about "the internal resolution" that predates 2026-07-28 is about the
downscale. `M2VK_SS` is unchanged and is still that.

## 2. The resolutions

Native is 496×384 — one `set_raw` for the whole driver (`src/mame/sega/model2.cpp:2538`), so there is
no per-game variation to design around.

| option value | pixels vs native | ring cost (colour+depth × 3 slots) |
|---|---|---|
| `496x384` (default) | 1× | 4.4 MiB |
| `640x480` | 1.6× | 7.0 MiB |
| `1024x768` | 4.1× | 18.0 MiB |
| `1280x960` | 6.5× | 28.1 MiB |
| `1440x1080` | 8.2× | 35.6 MiB |
| `1600x1200` | 10.1× | 43.9 MiB |
| `1920x1440` | 14.5× | 63.3 MiB |
| `2560x1920` | 25.8× | 112.5 MiB |
| `2848x2136` | 31.9× | 139.2 MiB |

8 bytes a pixel a slot (B8G8R8A8 colour + D32_SFLOAT depth) over a ring of three. **Confirmed against
the core's own log**: `2848x2136` reports `142578 KiB of attachments`, which is that 139.2 MiB.

The list is absolute resolutions rather than multipliers because Flycast's is
(`../flycast-aoj/shell/libretro/libretro_core_options.h:250`) and matching it was the instruction. It
also removes the question of what a multiplier means when the source is 1.2917 and the target is 4:3.

🚨 **Every entry above native is 4:3 and native is 1.2917, so the scale is non-uniform and
fractional** — 640×480 is 1.290× across and 1.250× down. That is the single fact that made this more
than a size change; §4 is what it cost.

⚠️ **`aspect_ratio` is unchanged at every resolution** and stays the OSD's `vis.width()/vis.height()`.
A bigger buffer is a denser sample grid for the same image, not a wider picture. It also means the
core still presents Model 2 as **1.29:1 rather than the 4:3 a real cabinet was** — existing behaviour,
deliberately not touched here, and worth its own decision one day.

## 3. The shape of it

Two extents where there was one:

| | |
|---|---|
| **picture** (`s_width`/`s_height`) | MAME's visible area. The 2D layer textures and their staging, the polygon stream's coordinates, the vertex shader's half-extent, the reticle's aim, the scissor's units. |
| **output** (`s_out_width`/`s_out_height`) | The chosen resolution. Ring image, depth attachment, framebuffer, render pass, viewport, `video_cb`. |

Two mutually exclusive modes, resolved in `read_resolution()` and nowhere else:

- **`M2VK_SS=n` set** → supersample and resolve back; output stays 496×384. Byte-for-byte what
  existed before. `res.sh` and `res-baselines.md` are untouched.
- **otherwise** → the ring image *is* the chosen size. No oversized attachment, no resolve pass, no
  `downsample.frag`.

`M2VK_SS` winning over the option is what keeps a remembered menu value from disturbing a harness run.

**The frontend handshake is Flycast's** (`libretro.cpp:686-765`), copied deliberately:
`base_width`/`base_height` stay at the **picture** so the frontend does not open a 2848-pixel-wide
window at startup; `max_width`/`max_height` are a high-water mark that only grows;
**`SET_GEOMETRY`** announces a size inside the max and **`SET_SYSTEM_AV_INFO`** only when the max has
to grow. Both branches were exercised — see §5.

**Files:** `vk_present.{h,cpp}` (the substance), `vk_geom.{h,cpp}` + `poly.frag` + `poly.vert` (the
scissor and the stipple), `reticle.frag` (one line), `retro_entry.cpp` (the handshake),
`retro_options.{h,cpp}` (the value list and its parser), `devnotes/retrohost.c`. **No new file, no
upstream file — the diff against mame0288 is still 30 lines.**

## 4. What a fractional scale actually cost

- **`poly.vert`: nothing.** It maps picture pixels to NDC against the visible half-extent, and NDC
  fills whatever viewport it is given. Non-uniform scaling is automatic and was already correct.
- **`geom_draw`'s per-polygon scissor: integer → two floats, rounded outward.**
  `floor(left * sx)` and `ceil((right + 1) * sx) - 1`. Outward can only keep a boundary pixel, never
  drop one — the safe direction, and safe here for a measured reason: P3 step 6 found this scissor
  exactly redundant with the geometry engine's own frustum clip in 23 of 25 games, worst excess one
  float ULP. ⚠️ The right/bottom edges are computed in **double**, because an unscissored batch
  carries `INT32_MAX` and the multiply has to survive the trip to the clamp.
- **The reticle** takes the aim scaled per axis and the cross's size scaled by **one** factor (the
  vertical). Scaling the shape by both would leave a 4:3 target's crosshair arms visibly longer across
  than down. Measured in §5.
- **The 2D layers get an uneven magnification.** They are uploaded at the picture's size and magnified
  by the existing `VK_FILTER_NEAREST` sampler, so at 1.29× some source columns double and some do not.
  Kept NEAREST: LINEAR would soften HUD text that is supposed to be crisp. **This is the one place a
  non-integer target looks worse than an integer one.**
- **Texture sharpness does not improve, and that is correct.** `poly.frag` takes its mip level from
  MAME's own per-polygon formula, not from screen-space derivatives, so a bigger frame magnifies the
  same mip. **Geometry gets sharper; texels get bigger.** See the screenshots — it is very visible and
  it is what makes the result MAME's picture rather than a remaster.

### The `checker` stipple is now a fine dither, by decision

`pc.scale` in `poly.frag` became `pc.stipple_div` — how many **attachment** pixels one square of the
screen door spans — and the renderer sets it **per frame** rather than deriving it from the
resolution, because the two things it can mean are different pictures:

- **`M2VK_SS=n` → n.** The frame is about to be averaged down, so the door must be one *picture* pixel
  per square or the resolve turns it into a flat 50 % blend. That is P4 step 2's measured bug and
  `res-baselines.md` rests on the fix.
- **internal resolution → 1.** Nothing is averaged. One square per *output* pixel is the finest dither
  the picture can carry and reads as smooth translucency.

**This answers the question `res-baselines.md` handed to P5.** Measured on P4 step 2's own fixture
(vcop2, `M2VK_ONLY_POLY=114`): the polygon covers **57.401 %** of the frame at native and **57.422 %**
at 1440×1080 — still a half-covering screen door, at three times the frequency. Pictures in
`screenshots/2026-07-28-p5-vcop2-stipple-*.png`.

## 5. Verification

**1 — native is a proven no-op.** `ab.sh vf2 2500` reproduces `16af05bb8d02a9a5` (software) and
`55da761fecca5c01` (vulkan) **byte-exactly**, with same colour 95.059 % and SSIM covered 0.9963.
`POINT=1 res.sh vf2 2500 3` reproduces `res-baselines.md`'s row **to the digit** — 106396 / 106397 /
A only 0 / B only 1 / interior 0 / 99.273 % / agreement 1.0000 / SSIM 0.9997 — so the `M2VK_SS` path
is untouched.

**2 — the output is really the chosen size.** `M2OPT_model2_internal_res=1440x1080` writes a
1440×1080 PPM, and retrohost's new `presented:` line says so.

**3 — equivalence with the already-measured path, and this is the one that carries the claim.**
No menu entry is an integer multiple of 496×384, so `M2VK_RES=1488x1152` (exactly 3×) exists for the
harness. Point-downsampling that frame in numpy — centre subpixel of each 3×3 block — and comparing
against `M2VK_SS=3 M2VK_SS_POINT=1`'s 496×384 output gives **0 differing pixels of 190464 on all ten
`res-baselines.md` fixtures**: vf2, vcop2, srallyc, sgt24h, desert, waverunr, dynabb97, dynamcop,
overrev, schamp.

That is exact, not statistical, and it holds **including checkered polygons** even though the two runs
use different stipple divisors: for odd n the centre subpixel `(3x+1, 3y+1)` has parity `x+y` either
way. The real path renders what the verified path renders; the resolve is the only difference.

⚠️ **`overrev` and `schamp` failed on the first attempt and both are on the frame-parity bistable
list.** Re-run: all 9 pairings of 3 runs each agree on overrev, all 4 on schamp. Exactly the trap
CLAUDE.md documents — two samples cannot tell bistable from broken.

**4 — a fractional scale draws the same scene.** Coverage against each resolution's own
`M2VK_NO_3D=1` background: vf2 **55.861 % native → 55.839 % at 640×480**, srallyc **71.465 % →
71.362 %**. Edge sampling, not missing geometry — a wrongly-rounded scissor would take whole columns.

**5 — the live change and both handshake branches, in one run.** Loading at native and changing to
1440×1080 at frame 2000 exceeds the max declared at load, so it takes **`SET_SYSTEM_AV_INFO`**;
loading *at* 1440×1080 seeds the max from the option and takes **`SET_GEOMETRY`**. The live-changed
run's last frame is **byte-identical** to the run that started there.

**6 — RetroArch 1.22.2 accepts it.** `SET_GEOMETRY` logged by the frontend, ring at 1440×1080,
`Content ran for a total of: 24 seconds` for 1400 frames (so the run was real), **Average speed
104.46 %** — against 104.50 % at native. Keeping up with realtime at 8.2× the pixels.

**7 — the reticle.** Isolated by differencing against `M2VK_NO_RETICLE=1` on `vcop --gun 0` with the
aim scripted at (0.25, 0.75):

| | reticle px | centre | box |
|---|---|---|---|
| 496×384 | **124** | (0.2500, 0.7500) | 18×18 |
| 640×480 | 208 | (0.2500, 0.7500) | 22×22 |
| 1440×1080 | 1020 | (0.2500, 0.7500) | 50×50 |

The centre is exact at every resolution (the per-axis aim), the box stays **square in pixels** at
every resolution (the single shape scale), and native reproduces lightgun step 4's **124 pixels**.

**8 — the guards.** `MODE=M2VK_FORCE_SOLID=2 ab.sh vcop2 2500`: 154203 covered, A only 0, B only 0,
the two renderers **byte-identical**. `renderer=software` ignores the option entirely — presents
496×384 and digests `16af05bb8d02a9a5` with `model2_internal_res=1920x1440` set. The standalone
`OSD=sdl3` binary still builds.

**9 — cost.** vf2, 1000 frames after 1500 skipped:

| | native | 640×480 | 1440×1080 | 2848×2136 |
|---|---|---|---|---|
| `core` (emulation) | 4.381 | 4.417 | 4.440 | 4.458 ms |
| `gpuwait` | 0.743 | 0.760 | 1.428 | 4.253 ms |
| `pipe%` | 396.8 | 393.6 | 391.6 | **390.0 %** |

**The pipelined speed barely moves**, because the emulation thread is still the long pole: 32× the
pixels costs 5.7× the GPU time and the GPU is still under `core` even at 2848×2136 — that resolution
is roughly where the two cross. ⚠️ `serial%` falls hard (339 → 200 %) and quoting it would be wrong
for the reason performance.md §2a gives: the harness reads every frame back and hashes it, which is
its cost and not the core's, and it scales with the pixel count.

## 6. Refusals, clamps, and one thing that is not clamped

- An option value the parser does not recognise resolves to **native**, and the log says `native`
  rather than the `0x0` the parser produces — `model2_internal_res=0x0` reads as a broken option.
- ⚠️ **A `.opt` file holding `1x` or `2x` from the option set this replaced falls back to native**,
  which is what `1x` meant. `2x` silently loses its supersampling; there is no way to honour it,
  because it named a feature the option no longer offers.
- `M2VK_RES` that is not a usable `<w>x<h>` logs and falls back to **the option's** value, not to
  native.
- The device's `maxImageDimension2D` and `maxFramebufferWidth/Height` are asked once per context and
  clamp the request. 🚨 **`ensure_limits()` must run before the ring-rebuild test, not merely before
  the build** — both go through `wanted_resolution()`, and a clamp appearing between them would make
  them disagree for ever and rebuild the ring on every frame of the run.
- 🚨 **The clamp is not a sanity bound and `M2VK_RES` is not safe against typos.** This device reports
  16384, so `M2VK_RES=16000x16000` was accepted, allocated **5.7 GiB** of attachments and ran. The
  option's value list is the only real bound, and it is the only thing a player can reach. Left as is
  deliberately: the switch is the harness's, and the device limit is the bound that will actually
  matter on a Quest 3.

## 7. Left open

- **4:3 vs 1.29:1.** The core reports MAME's pixel-derived aspect. A real cabinet was 4:3. Now that
  the option's own list is 4:3, the mismatch is easier to notice — but changing it moves every
  existing picture and is not this step's to take.
- **The 2D layers' uneven magnification** at a non-integer scale (§4). A player-visible artefact with
  three possible answers — leave it, filter it, or offer integer-only resolutions — and no measurement
  yet saying which is preferred.
- **`M2VK_SS` and the option cannot be combined.** Supersampling *above* the internal resolution would
  be genuine antialiasing at a chosen output size, and the code is nearly there. Not built: it doubles
  the verification matrix, and nobody has asked.
