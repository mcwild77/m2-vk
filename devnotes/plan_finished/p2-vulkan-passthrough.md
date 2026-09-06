# P2 — Vulkan HW context, passthrough

**Status: P2 is DONE.** Steps 1–5 committed 2026-07-25 (HEAD `c18e41c536b`); steps 6–8 done
2026-07-26 and changed no core code at all. **The exit criterion is met**: `renderer=software` and
`renderer=vulkan` produce byte-identical pictures over four cases. This file has been rewritten to
as-built — where a decision was made differently from the plan, the plan text is kept and the
correction sits under it, because the reasoning that turned out to be wrong is usually the part worth
knowing.

Plan of record for P2 as scoped in `../Polydiver/PDDocs/model2/model2_libretro_core.md` §3. Read
[p1-libretro-core.md](p1-libretro-core.md) first — P2 changes exactly one thing about the core it
describes: how the finished frame reaches the frontend. Everything else (the run loop, the baton,
audio, input, options, the seam) is untouched. What the *device* turned out to be able to do is in
[vulkan-target.md](../reference/vulkan-target.md), which is the file P3 should read before writing renderer code.

## What P2 delivers

The **same picture as P1, drawn by Vulkan**. MAME's software-rendered frame is uploaded as a texture
and drawn as a fullscreen triangle into an image the frontend consumes through
`RETRO_HW_RENDER_INTERFACE_VULKAN`. No polygons are rendered — the tapped stream keeps going to the
diagnostic sink and nowhere else. This is `pdvk`'s checkerboard with a real image in it.

The point is not the picture. It is that after P2 we own a live `VkDevice`, a queue we may submit on,
an image ring the frontend presents, and a `context_reset`/`context_destroy` lifecycle that survives
a frontend that tears the context down mid-run. P3 then has somewhere to put geometry.

**Exit criterion: pixel-exact against P1.** Passthrough is the one phase where "identical to the
software renderer" is actually achievable — nothing is being reinterpreted, only moved. So P2 also
**calibrates the A/B harness**: if the SSIM rig reports anything other than a bit-exact match here,
the rig is wrong, not the renderer. Getting that for free is worth more than the picture.

**Met at step 7.** vf2 (4500 frames), vcop2 (2500), srallyc (2500) and an input-scripted vf2 run
(2500), each run twice through `retrohost` with only `model2_renderer` changed: last-frame PPM,
whole-run picture digest, per-frame polytap stream, frame and audio accounting, and MAME's NVRAM/cfg
tree identical every time. The harness is `retrohost --vk`, and it is calibrated: **a bit-exact match
is a thing this rig can actually report**, which is the claim that had never been tested before.

Explicit non-goals: polygon rendering, depth buffer, texture decode, internal-res scaling, filtering,
widescreen, and any device-feature selection beyond what passthrough needs.

## Frontend decision — RetroArch (settled 2026-07-25)

Verified on this machine before committing to it:

| | |
|---|---|
| `/Applications/RetroArch.app` | 1.22.2 (git 69a4f0ea, Nov 2025), arm64 |
| `video_driver` | already `vulkan` in `~/Library/Application Support/RetroArch/config/retroarch.cfg` |
| Vulkan implementation | **MoltenVK 1.2.7**, bundled in `Contents/Frameworks` — no system Vulkan install involved |
| HW-render paths present | binary carries `GET_PREFERRED_HW_RENDER: RETRO_HW_CONTEXT_VULKAN`, `VK_KHR_swapchain` |
| Already exercised | P1 booted vf2 in it |

RetroArch **is** the compatibility target: its implementation defines what the negotiation and
`set_image` semantics actually mean in practice, spec text notwithstanding. So P2 is developed
against it first, and only then ported into our own headless host (problem 5 below). Doing it the
other way round means inventing semantics in `retrohost` and discovering the divergence in P3, with
the polygon renderer in the way of the diagnosis.

## The five problems, and how each is handled

### 1. Headers and a shader compiler, without adding a build dependency

Nothing Vulkan exists in the tree: MAME's `3rdparty/` has no Vulkan headers, there is no Vulkan SDK
installed, and no `glslc`/`glslangValidator` on `PATH`.

```sh
brew install vulkan-headers shaderc     # headers + glslc; both arm64-native
brew install molten-vk                  # ONLY for the headless host (step 7), not for the core
```

**As built:** all three are still the whole dependency list, and `molten-vk` really is only the
headless host's — `retrohost --vk` `dlopen`s it at run time, and the *core* has never resolved a
Vulkan symbol from anywhere but the frontend. Note this means **two different MoltenVK builds are in
play**: Homebrew's under `retrohost --vk`, RetroArch's bundled one under RetroArch. They are not the
same version and the difference is measured in [vulkan-target.md](../reference/vulkan-target.md).

Three rules that fall out of this, and they are load-bearing:

- **The core links no Vulkan library at all.** Every entry point is resolved from the
  `get_instance_proc_addr` the frontend hands us, then `vkGetDeviceProcAddr` for device-level calls.
  We need *headers*, never a library. This is not tidiness — on macOS there is no Vulkan loader
  unless someone ships one, and RetroArch is talking directly to its own bundled MoltenVK. Linking
  anything of our own would either fail to load or, worse, load a *second* implementation. It also
  keeps `model2_libretro.dylib` loadable on a machine with no Vulkan whatsoever, which is what makes
  the software fallback honest.
- **Shaders are compiled offline and committed as SPIR-V headers**, exactly as
  `PDTooling/vulkan_plugin/build_android.sh` does it: `glslc` → `.spv` → `xxd -i` → `*_spv.h`, checked
  in. No genie custom build rule (they are fragile and would make `glslc` a build requirement for
  anyone who touches the tree), and the shaders are two files that change roughly never during P2.
- **The header include path is configurable, not a hard-coded Homebrew path.** Default
  `/opt/homebrew/include`, overridable, with a clear error if `vulkan/vulkan.h` isn't there. If CI
  later wants hermeticity, the fallback is vendoring `Vulkan-Headers` under `3rdparty/` — new files
  only, so it stays inside the mergeability rule — but not before CI actually exists.
  **As built:** the `M2VK_VULKAN_INCLUDEDIR` environment variable, read at the top of
  `scripts/src/osd/libretro_m2.lua`, and *not* a genie `--option` as first planned — genie options
  arrive only through the `PARAMS` list in the top-level `makefile`, which is an upstream file, and
  a command-line `PARAMS=` overrides every entry already in it. Env var, zero upstream edits.

`libretro_vulkan.h` is **not** in our vendored `libretro.h` (only the enum values are:
`RETRO_HW_CONTEXT_VULKAN = 6` at `libretro.h:5251`, `RETRO_HW_RENDER_INTERFACE_VULKAN = 0` at
`:3297`). It is a separate header in libretro-common and must be vendored alongside `libretro.h`,
same permissive licence.
**As built:** copied byte-for-byte from libretro-common `23d82a25841350e7b7db93905ee1fc3ec09ac9d2`,
with no banner added — `libretro.h` next to it is pristine upstream too, and provenance lives in
[worklog.md](../worklog.md) rather than in the file.

### 2. Declaring HW render without losing the software path

The software path is not legacy — it is **the A/B ground truth**, and it lives in the same binary
behind `model2_renderer`. P2 must not weaken it.

**As built (step 2):** all of it lives in `renderer_vk/vk_context.{h,cpp}`, so `retro_entry.cpp`
gained four lines and a presentation branch and nothing else. One correction to the sketch below: a
core that has declared HW render may pass **only** `RETRO_HW_FRAME_BUFFER_VALID` or null to
`video_cb` ([libretro.h:946](../../src/osd/libretro_m2/libretro.h#L946)) — handing over a software
pixel pointer is not allowed. So `renderer=vulkan` cannot show the software picture as a stopgap; it
dupes until the renderer exists. Verified harmless: RetroArch runs 400 duped frames with audio and
no complaint.

- `RETRO_ENVIRONMENT_SET_HW_RENDER` is issued from `retro_load_game`, with `context_type =
  RETRO_HW_CONTEXT_VULKAN`, `version_major/minor` = 1.0 (see the MoltenVK note below),
  `cache_context = false`, and `context_reset`/`context_destroy` callbacks. Depth/stencil and
  `bottom_left_origin` are GL-era fields and are ignored for Vulkan; leave them zero rather than
  guessing.
- **Declared only when the option says `vulkan`.** `model2_renderer` is already resolved at load
  ([p1-libretro-core.md:297](p1-libretro-core.md#L297)), which is exactly the right time.
  `renderer=software` must not declare HW render at all, so the P1 presentation path stays
  byte-for-byte the path that generated the goldens.
- **If the environment call fails, fall back to software and keep running** — log it once, at
  `RETRO_LOG_WARN`, in the same voice as today's "the Vulkan renderer is not built into this core
  yet" message. This path is not hypothetical: `retrohost` is precisely a frontend with no HW
  render, and it stays that way until step 7.
- The P1 "not built into this core yet" warning (it was in `retro_entry.cpp`, not `retro_options.cpp`)
  is gone as of step 2: it is replaced by a warning on the *refusal* path only, plus a one-shot info
  line saying the context is up and nothing is drawn through it. That second line goes away in step 4.

### 3. The image ring and the frontend's sync model

`RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE` is only valid **after** `context_reset` has fired — ask
earlier and it fails or hands back stale state. Check `interface_version` before touching anything;
a mismatch is a fallback-to-software, not an abort.

There is no swapchain on our side. We own N images — **step 2 measured N: RetroArch reports 3
swapchain images on this machine, so size the ring off `get_sync_index_mask` rather than the "start
at 2" this plan first assumed** — and:

- `get_sync_index` / `get_sync_index_mask` say which of our per-frame resource sets is safe to write;
  `wait_sync_index` blocks until it is. Command pools, staging buffers and images are indexed by that
  sync index, never shared across frames.
- **The queue is shared with the frontend.** Every `vkQueueSubmit` is bracketed by
  `lock_queue`/`unlock_queue`. Forgetting this produces corruption that looks like a driver bug and
  is not one.
- `set_image(handle, &image, num_semaphores, semaphores, queue_family_index)`, where
  `retro_vulkan_image::image_layout` is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` — we transition it
  ourselves before handing it over. Then `video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0)`.
- `context_destroy` can arrive **mid-run** (window/driver events), and must destroy everything. This
  is safe here for a structural reason worth writing down: the baton means the emulation thread is
  parked inside `update()` whenever the libretro thread is running, and **no Vulkan call is ever made
  from the emulation thread**. Teardown therefore cannot race the emulator. With `cache_context =
  false` the next `context_reset` rebuilds from nothing.

**As built (steps 3–5, and stress-tested at step 7).** All of the above stands, with three things the
plan could only guess at now measured. The ring is **3** and is sized from `get_sync_index_mask`
every frame, not once — and the mask really can change: `retrohost --vk` drives it 3 → 1 → 4 mid-run
and the rebuild is bit-exact in both directions, which is the first time that path has run against a
genuine change rather than a throwaway patch. The `retro_vulkan_image` handed to `set_image` lives in
a fixed-size array, never a `std::vector`, because the frontend keeps the pointer across duped frames
(step 5). And a `context_reset` arriving with no `context_destroy` before it **abandons** the ring
rather than destroying it, because no observable fact can prove the old device is still alive —
see the handle-recycling gotcha below.

**The startup ordering trap.** `retro_load_game` blocks until the first emulated frame because
`retro_get_system_av_info` needs real geometry ([retro_entry.cpp:23](../../src/osd/libretro_m2/retro_entry.cpp#L23)),
but `context_reset` does not fire until *after* `retro_load_game` returns. So the renderer is
guaranteed absent for at least the first frame, and possibly a few. Handling: if the renderer is not
ready, `video_cb(nullptr, 0, 0, 0)` — a duped frame, which `retro_run` already does today for the
no-picture case. Do **not** make `retro_load_game` wait for a context; that deadlocks.

### 4. Getting the pixels in, exactly

Bit-exactness is the exit criterion, so the format chain gets stated rather than assumed:

- MAME's `bitmap_rgb32` pixel is `0xAARRGGBB` in a native `uint32_t`; on little-endian that is the
  byte sequence **B, G, R, A** → **`VK_FORMAT_B8G8R8A8_UNORM`** with no swizzle, no shader fixup, no
  row-order change. `capture_frame()` already tightly packs the visible 496×384 rectangle
  ([libretro_m2_osd.cpp](../../src/osd/libretro_m2/libretro_m2_osd.cpp), `capture_frame`), so the
  upload is one `memcpy`-shaped blit with a known row pitch.
- **Alpha is garbage** (it is X, not A). The fragment shader writes `1.0` explicitly rather than
  trusting the frontend to ignore the channel.
- Staging buffer (`HOST_VISIBLE | HOST_COHERENT`) → `vkCmdCopyBufferToImage` → `OPTIMAL`-tiled image.
  Not a linear-tiled image sampled directly: MoltenVK's linear-tiling feature set is narrow and this
  is the portable shape anyway.
- **Nearest** sampler, `CLAMP_TO_EDGE`, one output pixel per source pixel. Filtering is P5.
- **One fullscreen triangle, generated from `gl_VertexIndex`** — no vertex buffer, no index buffer,
  no vertex input state. A quad's diagonal is a real source of interpolation seams and there is
  nothing to gain from it.
- 496×384×4 = 762 KB per frame. At 57.52 Hz that is ~44 MB/s over PCIe-equivalent — irrelevant.

One optimization deliberately **not** taken: `capture_frame()` could write straight into the mapped
staging buffer and skip `m_fb` entirely. That would put Vulkan-owned memory in the emulation thread's
write path and couple the emulator's frame timing to the sync index. Not for a phase whose job is to
establish a baseline. Revisit when there is a reason.

**As built: every line of the chain above survived contact, and the format claim is now proved in
both directions.** `B8G8R8A8_UNORM` with no swizzle was the assumption the whole exit criterion rests
on, and it holds twice over — the core's own `M2VK_VK_DUMP` read-back matches the software frame that
went into staging (step 4), and `retrohost --vk` reads the image back off the GPU into a buffer it
then treats as MAME's own `0xAARRGGBB` pixels with no conversion whatsoever (step 7). If the byte
order were wrong anywhere, the second of those could not produce a PPM identical to the software
path's. Alpha is still written as `1.0` by the fragment shader rather than trusted.

### 5. `retrohost` cannot follow us — and the A/B harness needs it to (it can now: step 7)

[retrohost.c](../retrohost.c) is software-only: it sets `retro_set_video_refresh` and dumps a PPM, and
knows nothing of `SET_HW_RENDER`. But §5 of the plan needs deterministic headless captures **of the
Vulkan path** from P3 onward, and a screenshot out of RetroArch is not a fixture.

So P2 ends with **`retrohost-vk`**: the same host, plus a Vulkan side that
`dlopen`s `libMoltenVK.dylib` directly (Homebrew's), creates an instance and device, implements
`retro_hw_render_interface_vulkan` (`set_image`, `get_sync_index`, `wait_sync_index`,
`lock_queue`/`unlock_queue`, `get_device_proc_addr`), and reads the received image back to a PPM.
No window, no swapchain, no surface extension — which is *simpler* than RetroArch's job, not harder.

Whether that is a second binary or a `-vk` flag on the existing one is a step-7 decision; a flag is
likely, since every non-video behaviour (options, saves, input script, frame counting) must stay
identical for A/B to mean anything.

**As built (step 7): a `--vk` flag,** for exactly that reason — the two paths do not merely stay in
sync, they run the same code, right down to the buffer the picture lands in. The read-back is
`B8G8R8A8_UNORM`, which is `0xAARRGGBB` little-endian, which is MAME's own pixel, so the host points
its frame pointer at the read-back and the PPM writer, the digest and the frame counter cannot tell
the renderers apart. Without `--vk` the host refuses `SET_HW_RENDER`, which is what it did before it
could do Vulkan at all — so the software side of every A/B is untouched by construction, not by
care. Build with [build-retrohost.sh](../build-retrohost.sh).

Four things about it are worth knowing before changing it:

| | |
|---|---|
| **real headers** | it now `#include`s the core's own `libretro.h` and `libretro_vulkan.h` instead of the hand-rolled struct approximations it had carried since P1. `retro_hw_render_interface_vulkan` is too big to retype and a field at the wrong offset would be silent corruption across a `dlopen` boundary, not a compile error. Verified rather than assumed: the software path through the new binary is byte-identical to the pre-step-7 binary |
| **the digest** | FNV-1a over every frame's visible RGB in the PPM writer's byte order, printed by both paths. The last frame's PPM proves one frame out of 4500; this proves all of them for a pass over a buffer that is already hot, and it is what P5's SSIM rig gets calibrated against |
| **when it reads back** | inside `video_cb`, per frame — when a real frontend would sample the image. Not once at the end: it keeps the digest honest, and by the time the final PPM is written that path has already run three thousand times. Costs ~20 % throughput (534 % of real time software, 406 % under Vulkan) |
| **hard failures, not no-ops** | `set_command_buffers` and `set_image`-with-semaphores both `exit(1)`. The core uses neither. If it ever does, a host that quietly ignored them would read an unfinished frame — and an A/B comparison that silently stopped being bit-exact is the worst failure this harness could have |

It also owns what RetroArch does not, which is the point of having a second host: the sync-index mask
(`M2VK_HOST_SYNC_MASK`, `M2VK_HOST_MASK_AT=<frame>:<mask>`), context loss without a window
(`M2VK_HOST_RESET_AT=<frame>`), and the *ordering* of context loss (`M2VK_HOST_SKIP_DESTROY=1` omits
`context_destroy`, driving the core's abandon path). Those three retired the last paths step 5 could
only reach with throwaway patches.

## Where the code goes

```
src/osd/libretro_m2/
├── libretro_vulkan.h              ← vendored, alongside libretro.h
└── renderer_vk/
    ├── vk_funcs.h / .cpp          ← the function-pointer table, instance- and device-level
    ├── vk_context.h / .cpp        ← declare HW render, the two callbacks, the device probe log
    ├── vk_present.h / .cpp        ← image ring, staging upload, pipeline, the frame's submit,
    │                                 and the M2VK_VK_DUMP read-back
    └── shaders/
        ├── fullscreen.vert        ← gl_VertexIndex triangle
        ├── passthrough.frag       ← sample, alpha = 1
        ├── build_shaders.sh       ← glslc → spv → uint32_t header, run by hand, output committed
        └── *_spv.h                ← generated, committed

devnotes/                          ← local-only, never committed
├── retrohost.c                    ← the host, software-only and (with --vk) a Vulkan frontend
└── build-retrohost.sh             ← two include paths, no libraries
```

**Status as built:** every file above exists. `build_shaders.sh` emits `uint32_t` words rather than
`xxd -i` bytes (alignment, see step 4), and the `.vert`/`.frag`/`.sh` are deliberately *not* listed
in `libretro_m2.lua` — genie has no rule for them and none is wanted. Only the two `_spv.h` are.

**The upstream diff is still 16 lines**, and no file under `src/mame/` was touched by any of P2.
Steps 6, 7 and 8 changed no core code whatever: step 6 was a measurement, step 7 lives entirely in
`devnotes/`, step 8 is this file. Everything the phase produced in the tree is inside the five commits
listed at the top, and `git status` at the end of P2 is clean.

Edits to existing files: `retro_entry.cpp` (declare HW render, the two context callbacks, the branch
in `retro_run`), `retro_options.cpp` (drop the not-built-yet warning), `scripts/src/osd/libretro_m2.lua`
(the new sources and the header include path). **Zero new upstream edits — the `model2_v.cpp` diff
stays at 16 lines**, and it should still be 16 lines at the end of P6.

## Order of work

Each step ends in something observable; nothing is "done" on the strength of it compiling.

1. ~~**Toolchain.** `brew install vulkan-headers shaderc molten-vk`; vendor `libretro_vulkan.h`; genie
   wiring for the include path. *Verify:* a `renderer_vk` TU that includes both headers compiles into
   the existing dylib and the P1 software path still runs unchanged through `retrohost`.~~
   **Done 2026-07-25.** Headers 1.4.350 against MoltenVK 1.2.7 — newer, deliberately, see the
   gotcha below. `vk_funcs.{h,cpp}` is the TU; `retro_init()` logs `vk_build_info()`, which is what
   proves it is *linked* and not merely compiled. `otool -L` names no Vulkan library.
2. ~~**Context, and nothing else.** Declare HW render; `context_reset` fetches the interface and logs
   everything: interface version, API version, physical device name and type, queue family index,
   enabled features, memory heaps. *Verify:* run in RetroArch, read the log. **This is the first real
   evidence that MoltenVK + our core work at all** — do not build anything on top of it until the log
   is in hand.~~
   **Done 2026-07-25.** `vk_context.{h,cpp}`; the log is quoted in full in [worklog.md](../worklog.md).
   MoltenVK + this core work. What the log settled, all of it load-bearing for steps 3–4 and P3:

   | | |
   |---|---|
   | interface | v5, every entry point present including the v5 `set_signal_semaphore` |
   | device | `Apple M5`, integrated GPU, **`apiVersion` 1.1.0** (instance 1.3.313, driver 0x283c = MoltenVK 1.3.0) |
   | queue | family 0 of 4, `graphics\|compute\|transfer`, 1 queue each; ours is 0 |
   | memory | one 32 GiB device-local heap; type 1 is `device-local\|host-visible\|host-coherent\|host-cached` — unified memory, so the staging buffer costs nothing extra |
   | `B8G8R8A8_UNORM` | optimal tiling: sampled, colour attachment, blend, blit both ways, transfer both ways, linear filter. **Step 4's format chain is confirmed.** |
   | depth | **`D24_UNORM_S8_UINT` is not supported at all**; `D32_SFLOAT` and `D32_SFLOAT_S8_UINT` are. P3 uses one of those. |
   | copy alignment | `optimalBufferCopyOffsetAlignment` 16, row pitch **1**, `nonCoherentAtomSize` 16 — a 496×384 tightly-packed upload needs no padding |
   | features present | `depthBiasClamp` (the decal fix), `depthClamp`, `fillModeNonSolid`, `samplerAnisotropy`, `independentBlend`, `dualSrcBlend`, `alphaToOne`, `multiViewport`, `fragmentStoresAndAtomics`, `shaderClipDistance` |
   | features absent | `geometryShader`, `logicOp`, `wideLines` (`lineWidthRange` is 1.0–1.0) — Apple GPUs, and none of the three is in the plan |
3. ~~**Image ring + clear.** Images, command pools, sync-index bookkeeping, `set_image`, a solid-colour
   clear. *Verify:* RetroArch shows a flat colour at the right geometry and aspect, and survives being
   left running for minutes (sync bugs surface as drift, not as an immediate crash).~~
   **Done 2026-07-25.** `vk_present.{h,cpp}`; `vk_funcs` grew its device-level half. Ring of **3**,
   `get_sync_index_mask` = `0x7` exactly as step 2 predicted. Three things this settled:

   | | |
   |---|---|
   | ring shape | one `VkImage`+memory+view+`VkCommandPool`+`VkCommandBuffer`+`VkFence` per sync index; nothing shared between slots |
   | the frame | `wait_sync_index` → wait our own fence → reset pool → barrier → `vkCmdClearColorImage` → barrier to `SHADER_READ_ONLY_OPTIMAL` → submit under `lock_queue` → `set_image` |
   | synchronisation | **no semaphores.** The closing layout transition is the synchronisation, which the interface documents as the preferred of the two; `src_queue_family` is `VK_QUEUE_FAMILY_IGNORED` because we submit on the frontend's own family |

   Two decisions worth keeping when the clear is replaced in step 4:
   - **`oldLayout` is `UNDEFINED` every frame**, not the layout we left the image in. The frontend is
     explicitly allowed to transition an image it holds, so its current layout is not ours to know —
     and discarding contents costs nothing when the frame overwrites every pixel anyway.
   - **The clear colour is not static.** A single unchanging colour cannot distinguish "the ring is
     advancing and each slot is being presented" from "the frontend is showing one stale image
     forever", which is the exact failure this step exists to rule out. Brightness walks a slow
     triangle (~2 s at 57.5 Hz), so a frozen picture is visible as one. Orange, because a red/blue
     swizzle would read as blue and be unmissable.
4. ~~**The picture.** Staging upload, sampler, pipeline, fullscreen triangle. Retire the P1 fallback
   warning. *Verify:* vf2 attract renders in RetroArch on the Vulkan path, ~16 emulated seconds in.~~
   **Done 2026-07-25.** vf2's attract renders in RetroArch on the Vulkan path, and the read-back is
   **bit-identical to the software frame that went in**. (Nothing was left to retire: the P1 warning
   went at step 2 and the placeholder info line this plan promised was never written.)

   **"The picture" does not mean the GPU is drawing the game.** The software rasteriser is still
   drawing every polygon on the CPU; the GPU blits one triangle. That is restated here because the
   step's own title reads like acceleration and the question was asked. The bit-exactness above is the
   *proof* of passthrough — a GPU rasteriser could not match MAME bit-for-bit — which is the same point
   "What P2 delivers" opens with.

   | | |
   |---|---|
   | shared per ring | render pass, NEAREST/CLAMP_TO_EDGE sampler, descriptor set layout + pool, pipeline layout, pipeline. Built and destroyed **with the ring**, not with the context — the descriptor pool is sized off the ring and the framebuffers need the render pass, so one lifetime is one build path and one teardown path |
   | per slot | ring image + view + framebuffer, a **persistently mapped** HOST_COHERENT staging buffer, an OPTIMAL-tiled texture + view, a descriptor set. 2232 KiB of staging for a ring of 3 |
   | the frame | `wait_sync_index` → wait our fence → one `memcpy` into staging → barrier → `vkCmdCopyBufferToImage` → barrier to `SHADER_READ_ONLY_OPTIMAL` → render pass → `vkCmdDraw(3)` → submit under `lock_queue` → `set_image` |
   | synchronisation | still no semaphores. The **render pass's `finalLayout`** now does what the clear's closing barrier did, with both subpass dependencies stated explicitly — the implicit ones are `TOP_OF_PIPE` in and `BOTTOM_OF_PIPE`/no-access out, and the latter is no dependency at all as far as the frontend's fragment shader read is concerned |
   | `loadOp` | `CLEAR` to black, not `DONT_CARE`, even though the triangle covers every pixel. Diagnostic, not correctness: a draw that fails to happen then reads as black rather than as whatever was last in that memory, which would read as a picture |
   | shaders | `--target-env=vulkan1.0 -O`, committed as `uint32_t` arrays. **Bytes would not do** — `vkCreateShaderModule` takes a `const uint32_t*` and an `unsigned char[]` carries no alignment guarantee |
   | MoltenVK noise | `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart`. Harmless, and **not ours** — see the correction below |

   **Correction, measured at step 6: the primitive-restart warnings are entirely RetroArch's.** This
   step attributed them to our pipeline, once per creation. A 300-frame vf2 run emits **36 of them on
   both paths** — including `renderer=software`, where the core declares no context and creates no
   pipeline at all — and the count does not move by one when our ring (and with it our pipeline) is
   built. They come from RetroArch's own stock shaders. Nothing to fix; worth knowing before an hour
   is spent chasing a warning that would still be there with the core removed.

   **The read-back diagnostic, and why it exists.** `M2VK_VK_DUMP=<prefix>` (with optional
   `M2VK_VK_DUMP_FRAME=<n>`, default 600) writes `<prefix>-src.ppm` — the software picture as it
   went into the staging buffer — and `<prefix>-vk.ppm`, the ring image read straight back off the
   GPU in the same command buffer, after the draw. Then `cmp` is the test.

   This was not in the plan; it is here because the plan's verification method turned out not to
   exist on this machine (see the screenshot gotcha below), and because it is *better* evidence than
   the screenshot would have been — it is the core's own pixels, with the frontend's presentation
   nowhere in the picture. It cost one extra entry point (`vkCmdCopyImageToBuffer`) and one buffer
   that only exists when the variable is set.

   It does **not** retire step 7. What it compares is source-against-read-back inside one Vulkan
   run; the phase's exit criterion is software-against-Vulkan across two renderers, and that still
   needs `retrohost-vk`. But it makes the step-7 result predictable: the pipeline demonstrably does
   not touch a bit.
5. ~~**Lifecycle.** Force `context_destroy`/`context_reset` (toggle fullscreen, change the video driver
   and back, resize). *Verify:* no leak, no crash, picture returns, and the emulator has not lost a
   beat.~~
   **Done 2026-07-25.** All four verifications pass over **six forced cycles in one run**, and the
   ring-rebuild path — written at step 3, never executed until now — has been run deliberately.

   **The lever: `FULLSCREEN_TOGGLE` over the network command port.** RetroArch's response to it is a
   complete video-driver teardown and rebuild: a new `VkInstance`, a new `VkDevice`, a new swapchain,
   a new interface pointer, and a `context_destroy`/`context_reset` pair around all of it. That makes
   it strictly stronger than the "change the video driver and back" this plan asked for — the core
   sees everything replaced either way — and it changes the window size (1395×1080 ↔ 2940×1846), so
   the resize case comes free with it. `SCREENSHOT` and `GET_STATUS` remain broken (see the gotchas);
   `FULLSCREEN_TOGGLE` works, which is worth recording because it is the only one of the three that
   does.

   | verification | evidence |
   |---|---|
   | no crash | six cycles, exit 0, no error or warning from the core in the whole log |
   | picture returns | `picture resumed at frame N` after every reset, and the read-back taken **in the fourth context** is still bit-identical to the software frame |
   | the emulator has not lost a beat | frame accounting is continuous across every handover: 1078 → 1416 → 1765 → 2058 → 2409 → 2745. Each context resumes at exactly the frame the previous one ended on. The polygon tap — which is not the Vulkan side — keeps counting straight through |
   | no leak | MoltenVK's `still allocated` at each device teardown alternates 20/63 MB strictly with the window size and is *identical* at cycles 1, 3, 5 after intervening teardowns; RSS settles flat. See below |

   **The 59 MB line from the step-4 play session is explained and is not ours.** MoltenVK's
   `Destroyed VkPhysicalDevice … with N MB of GPU memory still allocated` tracks the *window*: 20–21 MB
   windowed, 58–63 MB fullscreen, repeatably, in whatever order the toggles put them. Across nine
   device lifetimes it does not accumulate. It is RetroArch's swapchain, reported after our
   `context_destroy` has already run. Our own ring is ~7 MB (3 × 496×384×4 × two images, plus 2232 KiB
   of staging) — a figure worth remembering, because it is exactly the step this number moves by when
   the ring *is* leaked, which is how the abandon path below was confirmed.

   **Three defects found by reading and fixed, none of which RetroArch's own ordering exposes:**

   - **The slots had to stop being a `std::vector`.** `vk_present.h` already stated the rule — the
     `retro_vulkan_image` given to `set_image` must stay at a stable address, because the frontend
     keeps the pointer and reads it again on any duped frame — and the vector broke it: a ring rebuild
     frees the old storage *before* the new ring is handed over, so a dupe in that window reads freed
     memory. Now a `std::array<frame_slot, MAX_RING_SLOTS>`, zeroed in place on teardown, so the worst
     a stale pointer finds is a null handle.
   - **`M2VK_VK_DUMP_FRAME` counted from the wrong zero.** The frame counter restarted at every
     `context_reset`, so a read-back armed for frame 1500 fired 1500 frames after the *last* reset —
     silently a different frame, in a phase whose whole premise is that a fixture is `(rom, frame)`.
     The counter now survives context loss; a per-context count is kept alongside it only so the log
     can say `picture resumed at frame N`. This is not cosmetic: the step-5 verification *needs* a
     dump that lands after a reset, and under the old counter no context in these runs lasted long
     enough to reach one.
   - **A `context_reset` with no `context_destroy` before it would have destroyed the ring against a
     dead device.** The header called this ordering legal and the code claimed to handle it; it
     actually ran `vkDeviceWaitIdle` and `vkDestroyImage` on whatever the old handles were. Now the
     ring is *abandoned* — dropped without a single Vulkan call, leaking ~7 MB — because nothing
     observable can tell the two cases apart. See the MoltenVK handle-recycling gotcha below, which is
     the reason the obvious fix does not work.

   **How the two unreachable paths were actually exercised**, since neither can be reached through
   RetroArch and "it compiles" is not evidence. Both by throwaway one-line patches, run, then reverted
   — deliberately *not* left in the tree as test hooks, since they exist to break the lifecycle:
   - the **in-place ring rebuild**: `|| (s_frames == 900) || …` added to the rebuild condition in
     `present_frame`. Result: `rebuilding the ring: 496x384 mask 0x7 -> 496x384 mask 0x7` three times,
     picture uninterrupted, frame count continuous.
   - the **abandon path**: an early `return` in `context_destroy_cb` behind an env var, so the ring
     survived into the next `context_reset`. Result:
     `the context was replaced without being destroyed; 3 slots abandoned`, picture resumes, no crash.
   Still *not* exercised by any route: a ring rebuild triggered by a genuine mask or geometry change.
   The mask was `0x7` in every context of every run, and 496×384 never varies. `retrohost-vk` (step 7)
   owns the mask and can force it properly.
6. ~~**`renderer=software` regression.** *Verify:* flip the option; identical behaviour to the P1 build,
   including under `retrohost`, which never sees HW render at all.~~
   **Done 2026-07-26, and nothing had to change.** The phase's one hard requirement on the software
   path — that it stay byte-for-byte the path the goldens were generated with — holds without a fix.

   **What "the P1 build" means here, since the comparison needs a real binary.** P2 modified exactly
   two files the build compiles, `retro_entry.cpp` and `libretro_m2.lua`; everything else it added is
   new files. So `git checkout 00a245ac219 --` those two yields a tree whose *compiled inputs* are
   P1's exactly, the `renderer_vk` sources falling out of the project along with the lua. Verified
   rather than assumed: `nm` finds no `declare_hw_render` or `vk_build_info` in the result and the
   dylib is 37 KB smaller. A worktree build was the obvious route and turned out not to be needed —
   the object directory is shared, so each direction of the switch relinks in about two seconds.

   Three configurations, through `retrohost`, each with a private empty save directory (credits live
   in battery RAM, so a stale one changes the emulated timeline):

   | | |
   |---|---|
   | `p1` | the P1 build at its own default (`vulkan`, which P1 answers with "not built into this core yet") |
   | `head-sw` | HEAD with `model2_renderer=software` |
   | `head-vkdefault` | HEAD at its default `vulkan` — declared, refused by retrohost, falls back |

   Across `vf2` (4500 frames), `vcop2` and `srallyc` (2500 each), and a `vf2` run driven by the input
   script `600:select,660:start,900:b:30,1200:lx+:200`, **every artifact is byte-identical**: the last
   frame as a PPM, the per-polygon dump, the run summary, the entire per-frame polytap stream (3191
   rendered frames for vf2), retrohost's own frame and audio accounting, and the NVRAM and `.cfg`
   trees MAME wrote.

   The vf2 rendered-frame-800 dump is also byte-identical to the **committed P0 fixture**
   `fixtures/vf2-frame800-polytap.txt`, which makes this an absolute anchor rather than a HEAD-vs-P1
   comparison: the stream is what it was at P0, two phases ago.

   **The one difference from P1, recorded so it is not discovered later as a surprise:** `retro_init`
   logs `vulkan headers 1.4.350, libretro vulkan interface v5 …` on every path, software included.
   It is a log line and nothing downstream of it differs.

   **Under RetroArch**, flipped both ways, 1200 frames each:

   | | `renderer=software` | `renderer=vulkan` |
   |---|---|---|
   | core log lines | 3 | 31 |
   | of which `vk:` | **0** | 28 |
   | RetroArch's own log | **no `SET_HW_RENDER` at all** | `SET_HW_RENDER, context type: vulkan` |
   | the run | 1200 frames, 20 s of content, exit 0 | the same |

   `M2VK_VK_DUMP` was confirmed inert under `renderer=software` — no file, no log line, no change to
   any artifact — since it is armed from an environment variable a user could have left set.
7. ~~**`retrohost-vk`.** Headless MoltenVK host, image read-back to PPM.
   *Verify — the exit criterion:* same ROM, same frame number, `renderer=software` vs
   `renderer=vulkan`, **PPMs are bit-identical**. `cmp` is the whole test; SSIM is not needed to tell
   you that.~~
   **Done 2026-07-26, and the exit criterion is met.** A `--vk` flag on `retrohost`, not a second
   binary — see below. Four cases (vf2 4500, vcop2 2500, srallyc 2500, vf2 + input script 2500):
   PPM, whole-run picture digest, polytap stream, frame/audio accounting and save tree identical on
   both paths, and the Vulkan run's rendered-frame-800 dump byte-identical to the committed P0
   fixture. The two orderings RetroArch cannot produce — a genuine sync-mask change (3 → 1 → 4
   slots) and a device loss with and without `context_destroy` — are now scripted from environment
   variables and are bit-exact too. **The core needed no change.** Full account, including the one
   bug this found (in the host: slot resources sized from the initial mask) and how Homebrew's
   MoltenVK 1.4.2 differs from RetroArch's bundled 1.3.0, is in [worklog.md](../worklog.md).
8. **Docs.** Update this file to as-built, append to [worklog.md](../worklog.md), add the P2 row to the
   README index, and record whatever the step-2 log said about MoltenVK, because P3 will need it.

## Gotchas known in advance

Each of these is cheap to respect now and expensive to diagnose later.

- **The ceiling is core Vulkan 1.1, and the step-2 log is the reason.** The guess this plan opened
  with ("MoltenVK 1.2.7") was wrong in the detail and right in the conclusion. What is actually there:
  RetroArch's bundled MoltenVK reports `driverVersion` 0x283c — MoltenVK's own encoding
  (major·10000 + minor·100 + patch), i.e. **1.3.0**, built against headers 1.3.313 — and the instance
  it creates reports 1.3.313, but the **physical device reports `apiVersion` 1.1.0**, because MoltenVK
  clamps what a device admits to to the instance's requested API version and RetroArch asks for 1.1.
  So: target core **1.1**. `VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2` and
  `VK_KHR_timeline_semaphore` are all *available* on the device — but we did not create the device,
  cannot know which extensions RetroArch enabled, and must not call into them. They become reachable
  only if the negotiation interface lands. And the **headers are 1.4.350**, so anything up to 1.4 will
  compile and declare itself perfectly happy. The headers are not evidence.
- **MoltenVK reports one format-feature bit with no name in the 32-bit enum.** `0x80000000` shows up
  in `optimalTilingFeatures` for both 8-bit colour formats; there is no such bit in
  `VkFormatFeatureFlagBits`, and it matches `VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT` from
  the *flags2* enum. A flags2 value leaking into a flags1 field, in other words. Harmless — we ask for
  none of it — but it is the reason the probe prints unnamed bits as hex instead of dropping them.
- **Without a negotiation interface, RetroArch picks the GPU and creates the device**, and we get
  only what it chose to enable. For passthrough that is fine and it removes an entire failure surface
  from P2. Device ownership (`SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE`, `libretro.h:1684`) is what
  the plan wants for the VR-path reuse, but it buys nothing until P3 needs a specific feature — so it
  is deferred, and `vk_present` is written to take an externally supplied device from day one so that
  adding negotiation later changes who *creates* the device and nothing else.
- **If/when we do create the device ourselves on macOS**, `VK_KHR_portability_subset` must be enabled
  on the device and the instance needs the portability-enumeration flag. Not P2's problem, but it is
  the first thing that will bite in P3 if negotiation lands.
- **Vulkan only ever on the frontend's thread.** Every call sits inside `retro_run`, `context_reset` or
  `context_destroy`. The emulation thread never touches it. (macOS is not special here; the baton is.)
- `video_shared_context` is a GL-era setting and does not apply — the `false` in the local config is
  not a problem to fix.
- **Screenshot before believing anything**, per the P1 gotchas: an attract text card is hundreds of
  textured quads and a passthrough bug can still look like a plausible picture.
- **`pause_nonactive = "false"` is mandatory in the `--appendconfig` file.** RetroArch's default is
  to pause the core whenever its window is not the focused one, and a run launched from a shell
  never gets focus. The symptom is not an error: RetroArch spins through `--max-frames` in
  milliseconds, reports `Content ran for a total of: 00 hours, 00 minutes, 00 seconds`, exits 0, and
  the core has advanced perhaps three frames. Every log line looks healthy. This cost most of an
  afternoon at step 4, chasing a read-back that "never fired" when the run had simply never reached
  the frame it was armed for. **`Content ran for a total of` is the line to read first** — if it
  does not roughly match `max-frames / 57.5`, nothing else in the run means anything.
- **After the machine has been idle a while, a shell-launched RetroArch run stops advancing — run it
  under `caffeinate -dsu`.** Found at step 6, ~40 minutes into an unattended session (`pmset -g
  assertions` showing `UserIsActive 0`, `HIDIdleTime` ~2400 s): a `--max-frames=1200` run that should
  take 21 s was still going at five minutes, with **zero** rendered frames in 75 s of `M2VK_POLYTAP`
  output — not slow, stopped. macOS throttles the unfocused window's presentation and RetroArch's
  main loop stops calling `retro_run` with it. It reads exactly like the core deadlocking, and the
  detail that makes it look worse than it is: **short runs still pass** (300 frames completed in 5 s,
  600 did not), so it presents as "the core hangs after a few hundred frames".

  What told it apart from a real regression was **running the other renderer**: the Vulkan path
  stalled identically, and it had run 3400 frames the previous evening. A failure both renderers
  share is the environment's, not the path under test. Prefixing the command with `caffeinate -dsu`
  fixes it outright — the same 1200-frame run then finishes in 21.9 s — and it is cheap enough that
  every RetroArch invocation in this document should carry it. Note this is a *different* failure
  from `pause_nonactive` below: that one runs far too **fast**, this one never finishes.
- **Do not plan on screenshotting RetroArch from here. There is no working way.**
  - macOS `screencapture` fails with `could not create image from display` — it wants a Screen
    Recording permission this shell does not have.
  - RetroArch's network command interface *is* up (`bringing_up_command_interface_at_port 55355`)
    and commands *do* arrive, but `SCREENSHOT` produces no file, with `video_gpu_screenshot` either
    way, with the directory existing, and with the core unpaused. Nothing is logged. Step 3 recorded
    this as working; it does not reproduce at step 4 and the step-3 evidence should be treated as
    unexplained rather than as a recipe.
  - `GET_STATUS` **segfaults RetroArch 1.22.2** — `strlen(NULL)` inside its own `command_get_status`,
    reached from `command_network_poll` during input polling. Nothing to do with the core; it just
    happens to unwind through `retro_run`. Useful only as proof that UDP commands arrive at all.

  Use `M2VK_VK_DUMP` instead (step 4 above). It answers the better question anyway: a frontend
  screenshot is post-upscale and post-gamut, so it says what the user sees, never what the core
  produced.
- **MoltenVK recycles `VkDevice` handle values, so comparing them is not a liveness test.** Measured
  at step 5: RetroArch's fullscreen toggle destroys the device and creates a new one — MoltenVK logs
  both — and the handle the new interface carries compares **equal** to the destroyed one. The
  obvious implementation of the abandon path above ("if the incoming device is the same one, the old
  ring is still real, so destroy it properly") therefore does the exact thing it was written to
  prevent, and does it silently: the run did not crash, it just destroyed the old ring's handles
  against a device that had never owned them, and MoltenVK's `still allocated` figure went 20 → 27 MB
  to prove it. A handle is a value, not a reference. The only sound signal that a device is gone is
  `context_destroy` having been called.
- **RetroArch's `--max-frames` counter restarts at every video-driver reinit.** So a run with N
  fullscreen toggles runs for roughly (N+1) × `max-frames`, not `max-frames`. Confirmed exactly: the
  final context of a `--max-frames=3400` run reported `destroyed after 3400 frames in this context`
  with 6145 frames since load. **This partly retires the `Content ran for a total of` heuristic** —
  it still catches the `pause_nonactive` failure (far *too short*), but "longer than expected" is now
  normal rather than suspicious. The core's own `ring of N destroyed after M frames … (T since load)`
  is the reliable measure, and unlike RetroArch's counter it is the core's.
- **`config_save_on_exit` defaults to true, so `--appendconfig` values are written into the user's
  real `retroarch.cfg` on exit.** That is how this machine's `video_fullscreen` came to be `"false"`
  when CLAUDE.md still described it as on — a previous session's test harness edited the user's
  configuration as a side effect. Put `config_save_on_exit = "false"` in the appendconfig file; it is
  one line and it makes the runs stop mutating anything.
- **RetroArch remembers core options per core, and a shell run silently inherits them.** They live in
  `config/Model 2/Model 2.opt`. The first step-5 run measured the *software* path from beginning to
  end because an earlier interactive session had left `model2_renderer = "software"` there, and
  nothing about the run looked wrong — the log line saying which renderer was chosen is the only
  tell. Write that file as part of the harness rather than trusting it, and **read
  `[model2] options:` in the log before believing any result**.
- **Do not sample an animation at a round interval.** Step 3's five soak screenshots, taken 60 s
  apart, all came back at almost the same brightness and looked exactly like a frozen picture. They
  were not: RetroArch drives the core at ~60 fps, 60 s is 3600 frames, and 3600 mod the 120-frame
  colour cycle is 0 — perfect aliasing. Re-sampled 0.45 s apart the brightness swung across most of
  its range. Pick a sampling interval coprime with whatever is being animated, or the evidence is
  worthless in the direction that matters.
- **None of the RetroArch gotchas above apply to `retrohost --vk`**, and that is a reason to reach
  for it first. There is no window, so nothing loses focus, nothing gets throttled, no config file is
  inherited or mutated, and the frame count is the core's own. Two of its own instead:
  - **`M2VK_POLYTAP_DUMP=N` counts *rendered* frames, not host frames.** vf2 renders nothing for its
    first ~990 frames, so rendered frame 800 lands near host frame 1790 — ask for 1500 and no dump
    file appears at all, which reads as a broken flag rather than as a short run.
  - **`/dev/null` is a perfectly good PPM destination** when only the dump is wanted.
- **A host that sizes per-slot resources from the *initial* sync mask will segfault the first time the
  mask grows.** Found at step 7, in `retrohost` itself and not in the core: the mask was driven
  0x7 → 0x1 → 0xf, the core rebuilt its ring correctly at both steps and logged so, and the host then
  read slot 3 through handles it had never created. Slot resources belong to whoever owns the slots
  and not to any particular mask, so build them all up front. Worth stating because it is the same
  mistake the core could make in P3 and the log would look just as healthy right up to the crash.

## Risks

- ~~**MoltenVK + our core is unproven.**~~ **Settled at step 2**, and cheaply, exactly as intended:
  RetroArch hands over a v5 interface with every entry point, the device loader resolves device-level
  names, and the emulator runs undisturbed through a full context-reset/destroy cycle. The remaining
  unknown is not "does it work" but "what did RetroArch enable on the device", which only negotiation
  can answer.
- ~~**Bit-exactness may not survive a colour-space or sRGB assumption somewhere in RetroArch's
  presentation.**~~ **Confirmed at step 3, and it is real.** The clear writes UNORM bytes
  `(252, 113, 13)`; a RetroArch GPU screenshot of the presented window reads `(252, 131, 43)` — red
  exact, green and blue lifted. That is a colour-space/gamut conversion on presentation (this display
  is P3), not anything the core did. So **a RetroArch screenshot is not the core's output and can
  never be the A/B ground truth**, and if `cmp` fails at step 7 the frontend is not the place to
  look. Step 7 compares `retrohost-vk`'s *own* read-back, which bypasses RetroArch's presentation
  entirely — that was already the design, and this is the measurement that justifies it.
  **Step 7 closed it:** the read-back path is bit-exact across four ROMs and 12,000 frames, so no
  colour-space assumption survives anywhere between MAME's frame and the image the frontend is given.
  The gamut shift is real and is entirely on RetroArch's side of `set_image`.
- ~~**Two hosts to keep honest.** `retrohost-vk` and RetroArch will drift unless every non-video
  behaviour stays shared. Hence the flag-not-a-fork preference in problem 5.~~ **Resolved by
  construction, not by discipline.** One binary, one flag: there is no second copy of the option
  handling, the save paths, the input script, the frame loop or the PPM writer to drift *from*. The
  residual risk is not drift between the hosts but the gap between either host and RetroArch, and
  that one is now bounded and known — see the next entry.
- **The two hosts do not run the same MoltenVK, and that is permanent.** RetroArch bundles its own
  (1.3.0) and `retrohost --vk` `dlopen`s Homebrew's (1.4.2). Both clamp the device to Vulkan 1.1 —
  the host asks for `VK_API_VERSION_1_1` deliberately, to mirror RetroArch — and every capability P3
  depends on is identical on both, but the two probe logs are not interchangeable and the device
  `apiVersion` differs in its patch field. Measured side by side in
  [vulkan-target.md](../reference/vulkan-target.md). The failure mode to watch for in P3 is a feature that exists
  on one and not the other; the harness would call it a renderer bug.
- **Nothing has yet been rendered.** P2's bit-exactness is the bit-exactness of a `memcpy` through a
  fullscreen triangle. It says the plumbing is sound and says nothing at all about whether the
  renderer P3 writes will be right, and the temptation to read it as a stronger result than that is
  the main risk this phase hands forward.
