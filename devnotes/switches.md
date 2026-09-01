# `M2VK_*` switch reference

Every runtime env switch the code actually reads, audited 2026-08-22 against the tree (not prose);
**+4 core switches added 2026-08-25** (`M2VK_POLYCOUNT`, `M2VK_S22_FOG`/`NOTEX`/`HUD` — the R3 option set).
**62 distinct switches**: 47 read by the shipped core, 15 host-only in `retrohost.c`
(**+1 2026-09-01**: `M2VK_LAZY_BAUD`). A matching
`M2VK_*` switch **overrides its core option** and the core logs a line when one is doing so.

Not switches (excluded from the count): the `*_H` include guards, the `M2VK_RESOLVE_DEVICE` and
`M2VK_PRINTF` macros in `renderer_vk/`, and `M2VK_VULKAN_INCLUDEDIR` (a build-time genie variable, set
by `build-android.sh`).

**New System 22 switches must justify themselves against this list** — most of it generalises to the
second driver, so prefer reusing a name to coining one.

---

## Core-option mirrors — the switch half of a shipped option (KEEP)
The override discipline: a harness run sets the switch to pin behaviour a `.opt` file can't be trusted
to hold. `DEFINITIONS[]` in `retro_options.cpp` is the option authority.

| Switch | Option | Value |
|---|---|---|
| `M2VK_FORCE_SOLID` | `model2_flat_shading` | `=2` flat (both renderers); `=1` diagnostic-only |
| `M2VK_FLAT_LUMA` | `model2_flat_luma` | `=0\|1`, pins lighting off/on |
| `M2VK_BLEND` | `model2_transparency` | `=0\|1`, value not presence — pins accurate path on too |
| `M2VK_RES` | `model2_internal_res` | `=<w>x<h>`, can name a size the menu can't (e.g. exact 3× `1488x1152`) |
| `M2VK_STEER_GAMMA` | `model2_steering_response` | γ float |
| `M2VK_STEER_DEADZONE` | `model2_steering_deadzone` | 0–1 |
| `M2VK_STEER_RANGE` | `model2_steering_range` | 0–1 |
| `M2VK_STEER_DAMP_DRIVE` | `model2_steering_damp_drive` | frames; `=0` pins OFF |
| `M2VK_STEER_DAMP_RETURN` | `model2_steering_damp_return` | frames; `=0` pins OFF |
| `M2VK_STEERBAR` | `model2_steering_display` | `=0\|1` |
| `M2VK_STEER_LINEAR` | — | `=1` bypasses the whole steer chain (curve+damping); the harness pin |
| `M2VK_POLYCOUNT` | `model2_poly_counter` | `=0\|1`; the top-right primitive-count HUD (all three families, Vulkan-only) |
| `M2VK_S22_FOG` | `system22_fog` | `=0\|1`, value not presence — `=0` pins fog OFF; default draws it |
| `M2VK_S22_NOTEX` | `system22_no_textures` | `=1` whitewashes S22 3D to greyscale (geometry + luma) |
| `M2VK_S22_HUD` | `system22_2d_overlay` | `=0` suppresses the S22 2D HUD/text over-layer |

## A/B & resolution harness — the renderer no-op/differential guards (KEEP)
| Switch | Role |
|---|---|
| `M2VK_SW_3D` | `=1` puts MAME's rasteriser back in charge; bit-exact vs `renderer=software`. The "rendering or timing?" isolator |
| `M2VK_NO_3D` | background-only reference — the `ppmdiff.py coverage` baseline both renderers produce bit-identically. Suppresses both the GPU and software 3D on Model 2 **and** System 22 (the S22 case fixed 2026-08-22 via `s22::set_no_3d()`; before that it left the S22 rasteriser drawing). |
| `M2VK_OPAQUE_ONLY` | rewrite every translucent poly to a class neither renderer draws — opaque-path regression guard |
| `M2VK_NO_EARLY_Z` | the one *pure no-op* switch: must not move a pixel on either renderer. Equality on/off is its whole purpose |
| `M2VK_NO_SCISSOR` | collapse every per-poly clip window to full-screen; the scissor A/B, on **both** the Model 2 and System 22 GPU passes (on S22 it is the tokyowar-letterbox attribution switch) |
| `M2VK_NO_RETICLE` | remove the lightgun cross from both paths so a gun game can go through `ab.sh` |
| `M2VK_RETICLE` | force the reticle on a non-gun game (for testing it); `M2VK_NO_RETICLE` still wins |
| `M2VK_SS` / `M2VK_SS_POINT` | render at n× and resolve back to 496×384 — resolution-invariance harness (`res.sh`). `POINT=1` + odd scale carries the claim |
| `M2VK_ONLY_POLY` / `M2VK_ONLY_FRAME` | draw one polygon (in the run's last rendered frame) and nothing else — single-polygon A/B |
| `M2VK_LAZY_BAUD` | the demand-gated i8251 baud clock — the switch half of the `model2_lazy_baud` option ("Fast Sound-Link Timing", default ON) and worth 36–48 % of core ms/frame ([lazy-baud.md](lazy-baud.md)). `=0` restores the stock 500 kHz `CLOCK`, which is the A/B arm. `=2` eager (this device, a timer for every edge — reproduces stock digests exactly, so it separates "the mechanism is wrong" from "the machine noticed the missing scheduler breaks"); `=3`/`=4` gate only TX / only RX |

## Diagnostic read-outs (KEEP — cheap, and reused by the System 22 port)
| Switch | Role |
|---|---|
| `M2VK_POLYTAP` | enable the polygon tap (off unless a `POLYTAP*` var is set) |
| `M2VK_POLYTAP_EVERY` | print a summary every n frames |
| `M2VK_POLYTAP_DUMP` [`_FILE`] | dump per-polygon detail of one *rendered* frame (counts rendered, not host, frames) |
| `M2VK_POLYTAP_SUMMARY` | one summary line: frame/poly counts |
| `M2VK_POLYTAP_TAG` | label lines in the tap output — candidate for removal (labelling only) |
| `M2VK_GEOM_LOG` | the geometry record from the renderer's side, one line/frame |
| `M2VK_VK_DUMP` [`_FRAME`] | write the core's own output PPM — the only screenshot path under RetroArch |
| `M2VK_INPUT_DUMP` | the machine's own ioport list incl. every `PORT_NAME`. Reach for this before scripting input |
| `M2VK_GUN_LOG` | lightgun read-out: axis ranges, resolved port values, offscreen bit |
| `M2VK_STEER_LOG` | steering read-out: raw axis, shaped axis, resolved `IPT_PADDLE` value |

## Savestate diagnostics (KEEP — S4 reuses the savestate framework)
On any savestate failure, reach for the per-frame picture hash (`M2VK_HOST_FRAME_HASH`) before a
registry diff. The receiver of a load is by construction in no state file — `M2VK_SAVE_PROBE` is the
only way to see it.

| Switch | Role |
|---|---|
| `M2VK_SAVE_PROBE` [`_FROM`/`_TO`] | the machine's *live* condition (FIFO occupancy, suspend masks, HALT, phase) each frame over a window |
| `M2VK_SAVE_DIFF` [`_DIFF_MAX`] | registry-entry diff after a load; `_DIFF_MAX=1000000` to see late-sorting timer entries |
| `M2VK_SAVE_VERIFY` | re-serialise immediately after load and report which registered items churn |
| `M2VK_SAVE_LOG` | verbose savestate logging |
| `M2VK_SAVE_DUMP` | print serialised bytes of matching registry entries — candidate for removal (code comment notes it's strictly weaker than `PROBE`) |

## retrohost host-side — frontend simulation (KEEP; not in the shipped core)
These live only in `devnotes/retrohost.c`, so they never ship. They reproduce frontend behaviours
RetroArch can't be scripted into.

| Switch | Role |
|---|---|
| `M2VK_HOST_SYNC_MASK` / `M2VK_HOST_MASK_AT` | the sync-image mask, and changing it mid-run |
| `M2VK_HOST_RESET_AT` / `M2VK_HOST_SKIP_DESTROY` | force a `context_destroy`/`context_reset` pair, or the abandon path |
| `M2VK_HOST_OPT_AT` | a core option changing *while content runs* — the liveness test. Pick a change point where the two static arms differ or it passes vacuously |
| `M2VK_HOST_DESCRIPTORS` | print what the frontend is told about inputs — the static input read-out |
| `M2VK_HOST_FRAME_HASH` | per-frame picture hash — tells "diverged at k and recovered" from "never recovered" |
| `M2VK_HOST_DIGEST_FROM` | whole-run digest starting at a frame |
| `M2VK_HOST_SAVE_AT` / `M2VK_HOST_LOAD_AT` | serialize/unserialize to/from a file at a frame |
| `M2VK_HOST_ROUNDTRIP_AT` | serialize then immediately unserialize the same bytes — candidate for removal (a determinism check `state.sh`'s E-run now covers) |
| `M2VK_HOST_PERF` [`_SKIP`] | the `perf.sh` timers; `_SKIP` must land past the boot plateau |
| `M2VK_HOST_RSS` | resident size every n frames + peak |
| `M2VK_HOST_MOLTENVK` | path to the MoltenVK dylib to dlopen |

## `M2OPT_<key>` — generic core-option override
Not a fixed name: `M2OPT_model2_renderer=vulkan` etc. sets any core option from the environment, and
**beats the option file**. `M2OPT_<key>` also beats an `M2VK_HOST_OPT_AT` scripting the same key, so
don't pin a key you're scripting live.

---

## Removal candidates (flagged, NOT removed)
Three low-value one-offs, kept for now because removing a `getenv` touches the committed OSD and the
saving isn't worth the risk mid-port: **`M2VK_POLYTAP_TAG`** (labelling only), **`M2VK_SAVE_DUMP`**
(strictly weaker than `M2VK_SAVE_PROBE`, per its own code comment), **`M2VK_HOST_ROUNDTRIP_AT`**
(a determinism check `state.sh`'s E-run now does). Pull them in a batch if/when the OSD is next edited
for other reasons. Everything else earns its place — the poly tap, the savestate probes, and the whole
host-side harness are what the System 22 port will lean on from S1 onward.
