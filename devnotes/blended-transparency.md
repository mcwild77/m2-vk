# `model2_transparency` — the `checker` screen door as a real blend

**Built 2026-07-28. Off by default. Vulkan only. An enhancement, not an accuracy fix.**

Model 2 has no alpha blender. A translucent surface is a **50 % screen door**: MAME's
`draw_scanline_*` steps `x` by two and starts at the first `x` where `(x ^ scanline) & 1` is 1
(`model2rd.ipp:84-85`), so one pixel in two is drawn and the rest are left for whatever is behind. The
renderer reproduces that literally as a `discard` in `poly.frag`.

The option replaces it with a genuine half-transparency. `stipple` (default) is unchanged and is what
every harness run measures; `blended` defers those polygons and blends them.

**All 29 games in [feature-survey.md](feature-survey.md) use the stipple**, several enormously —
`segawski` 381k polygons a run, `waverunr` 366k, `skytargt` 187k, `stcc` 147k, `indy500` 128k,
`vcop2` 123k. So this is not a corner-case option; it changes what most of the library looks like.

## 1. Why it cannot be `blendEnable = VK_TRUE` and nothing else

That was the first idea and it is wrong, for a reason that belongs to the *stream* rather than to the
polygon: **Model 2 submits front to back.** The software renderer walks `z` from `min_z` to `max_z`
with an occlusion mask and first-writer-wins, which the GPU path reproduces as the draw-order depth key
`1 - n/65536` with `GREATER` and depth writes on.

So at the moment a checkered polygon rasterises, **the geometry behind it — the thing it is supposed to
be seen through — has not been drawn yet.** The colour attachment holds the 2D under-layer and whatever
opaque polygons happened to be *nearer*. Blending in place composites against the wrong thing and is
then overwritten by the right thing.

Neither of the two obvious repairs works either:

- **Blend and keep writing depth.** The polygon then occludes everything behind it, which is the exact
  opposite of transparent.
- **Blend and stop writing depth.** The polygon behind draws afterwards, opaquely, straight over the
  blend. The result is the stippled polygon vanishing entirely.

## 2. What it does instead — a deferred second pass

Checkered polygons are held back in `geom_upload` and drawn after every other polygon, out of a third
pipeline (`PIPE_BLEND`). Everything else about them is untouched: same vertices, same parameter block,
**same draw-order depth key**. Only their indices are deferred.

**The depth buffer the first pass leaves behind already answers the only question they have.** The key
at a pixel belongs to the *lowest record index* that claimed it, and `GREATER` passes exactly where the
deferred polygon's own index is lower still — i.e. where nothing opaque is in front of it. That is the
same occlusion the screen door gets from first-writer-wins, read out of the same buffer, and it costs
**no sorting of the opaque stream at all**. This is the payoff from P3's decision that depth is draw
order: with an interpolated z-buffer the same trick would need a real depth compare and would inherit
every coplanar tie the phase avoided.

Pipeline state, and both halves are load-bearing:

| | test | write | blend |
|---|---|---|---|
| `PIPE_GENERAL` / `PIPE_EARLY` | `GREATER` | yes | off |
| `PIPE_BLEND` | `GREATER` | **no** | `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA` |

It **tests** so it is occluded correctly. It **does not write** because a transparent surface occludes
nothing — leaving the key unclaimed is what lets two overlapping deferred polygons both draw.

**The deferred pass is walked BACK TO FRONT**, which is the reverse of the record, because the record is
front to back. It is the only place in this renderer where draw order decides a colour: everywhere else
the depth key makes the picture order-independent, and it still does between these polygons and the
opaque ones — it is only *each other* they have to be ordered against. Two translucent surfaces over one
another only composite right if the far one is laid down first.

`PIPE_BLEND` takes the **general** fragment module, not the early-Z one: a deferred polygon is checkered
and may also carry the translucent texel cutout, so it can `discard` and must test late.

## 3. What it is not

- **Not a fix for the textured translucent cutout.** `FLAG_TRANSLUCENT` is a texel *index* (15 =
  transparent) thresholded at 50 %, and MAME's own filter interpolates the alpha lane only so that a
  transparent texel cannot drag colour out of the transparent region. Turning that into real per-texel
  alpha would antialias cutout edges, which is a separate feature with the same ordering problem and is
  **not** in this option.
- **Not resolution-dependent.** The stipple divisor (`pc.stipple_div`, P5) is irrelevant under the
  option because there is no stipple. Verified at `M2VK_RES=1488x1152`.
- **Not available on the software path.** MAME's rasteriser has nothing to blend with, so like
  `model2_internal_res` this cannot obey the both-renderers rule. An `ab.sh` or `res.sh` run must leave
  it alone — and by default does, because the default is the accurate path.

## 4. The switch

`M2VK_BLEND=0|1` overrides the option, in the standing direction: the harness sets the environment and
must win over whatever a frontend's `.opt` file remembers. It takes a **value** rather than a presence,
so a harness run can pin the accurate path ON as well as off. The core logs a line naming it when it is
overriding.

```sh
M2VK_BLEND=1 M2OPT_model2_renderer=vulkan ./devnotes/retrohost --vk ./model2_libretro.dylib \
  devnotes/roms/vcop2.zip 2500 /tmp/blend.ppm
```

The run reports `geometry: N of M drawn polygons were checkered and drawn blended in a deferred pass`,
and **warns if the option was on and nothing was checkered** — which the survey says cannot happen, so
that line means the option stopped reaching the geometry.

## 5. Measured

### The default is a proven no-op

Four fixtures' whole-run `renderer=vulkan` digests reproduce
[ab-baselines.md](ab-baselines.md) byte-exactly with the option at its default, and `vf2` reproduces
`renderer=software` too:

| | recorded | measured |
|---|---|---|
| `vf2` 2500 software | `16af05bb8d02a9a5` | `16af05bb8d02a9a5` |
| `vf2` 2500 vulkan | `55da761fecca5c01` | `55da761fecca5c01` |
| `vcop2` 2500 vulkan | `ccd47e79f2722f86` | `ccd47e79f2722f86` |
| `sgt24h` 2500 vulkan | `c8f12a6a197619c0` | `c8f12a6a197619c0` |
| `waverunr` 2500 vulkan | `a3ac5e6e50bf0353` | `a3ac5e6e50bf0353` |

`M2VK_BLEND=0` with `model2_transparency=blended` set is byte-identical to the accurate run, which is
the override working in the direction that protects a baseline.

### It blends against the right thing, and the evidence is the MEAN

🚨 **This is the check that matters, and it is not the screenshot.** A 50 % screen door and a 50 % blend
have the *same average* over any region larger than a texel — the door draws the polygon at half the
pixels and what is behind it at the other half. So if the blend partner were wrong (the 2D under-layer
instead of the geometry behind), the mean brightness would move a long way while the picture still
looked plausible.

| | mean, stipple | mean, blended | delta | pixels differing | max delta |
|---|---|---|---|---|---|
| `vcop2` | 99.372 | 99.457 | **+0.085** | 74585 | 128 |
| `waverunr` | 75.540 | 75.541 | **+0.000** | 1065 | 122 |
| `sgt24h` | 84.416 | 84.417 | **+0.001** | 595 | 91 |

74585 pixels move by up to 128 of 255 and the frame's mean moves by 0.085. That is the dither being
removed and nothing else being disturbed.

### Coverage

`vcop2`, last frame of 2500, against the `M2VK_NO_3D=1` background:

```
covered by A (stipple)   154196      covered by B (blended)   157945
both 154196 · same colour 83360 (54.061 %) · A only 0 · B only 3749 (all on an edge)
```

`B only 3749` is the screen door's holes being filled, and it is the same 3749 that
[res-baselines.md](res-baselines.md) records for the pre-fix 2× box resolve — the same geometry, seen
from the other side. **`A only` is 0**, which is the assertion that matters: nothing the accurate path
draws is lost. `sgt24h` is coverage-identical (agreement 1.0000, zero disagreement) with 595 pixels
recoloured — its stipple holes were already being filled by geometry behind, so only the colour moves.

### The option, and live

- Set as a core option with no switch present: byte-identical PPM to the `M2VK_BLEND=1` run.
- `M2VK_HOST_OPT_AT=1200:model2_transparency=blended`: the core logs `core options changed: …
  applied now`, the blended count drops from 36439 to 27129 over the run (it only started at 1200), and
  **the last frame is byte-identical to the always-blended run**. It applies live, which
  [user-options.md](user-options.md) and `CLAUDE.md` both say is mandatory for anything a player plays
  with.

### Pictures

`devnotes/screenshots/2026-07-28-transparency-*.png`, three games as pairs plus a magnified crop. The
`vcop2` crop is the one to look at: its attract mode lays a translucent panel over the whole scene, so
the stipple version is a visible checkerboard across the entire street and the blended version is a
clean fade.

## 6. Left open

- **Ordering among deferred polygons is back-to-front by record index, which is a proxy for depth**, and
  it is the same proxy the whole renderer uses. P4 measured 0–23 pairs a frame where a later polygon is
  strictly nearer, i.e. where the proxy is wrong; that is as true here as it is for the opaque stream,
  and it is not worth a second sort to fix a handful of pairs at 50 % opacity.
- **Two overlapping stippled surfaces are not equivalent between the two modes and cannot be.** They
  share the parity, so under the screen door the farther one is drawn *nowhere* — it lands on pixels the
  nearer one already claimed. Blended, it contributes 25 %. This is a difference in the feature, not a
  defect in the port, and it is where the two modes' means could legitimately diverge.
- **Not measured on GPU cost.** The deferred pass adds draws (a batch break on the pipeline, as P4 step
  1's split does) and adds blended fragments. On desktop this is inside the noise for
  [performance.md](performance.md) §2a's reason — the emulation thread is the long pole. On an Adreno
  740 blending is not free and this is a Quest 3 question, alongside every other item in §4.
