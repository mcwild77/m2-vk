# The Vulkan target — what the device actually is

**Read this before writing renderer code.** Everything here was measured by the core's own probe
(`renderer_vk/vk_context.cpp`, logged once per process on the first `context_reset`), not read out of
a header. The headers this core builds against are **1.4.350** and will happily compile anything up
to Vulkan 1.4; the device admits to **1.1**. The headers are not evidence.

Two probe logs exist because there are **two MoltenVK builds in play**, and they are not the same
one. Both are quoted in full in [worklog.md](worklog.md) (step 2 for RetroArch, step 7 for the host).

| | RetroArch 1.22.2 | `retrohost --vk` |
|---|---|---|
| MoltenVK | **1.3.0** (`driverVersion` 0x283c), bundled in `Contents/Frameworks` | **1.4.2** (0x28a2), Homebrew's `libMoltenVK.dylib`, `dlopen`ed |
| instance `apiVersion` | 1.3.313 | 1.4.357 |
| **device `apiVersion`** | **1.1.0** | **1.1.357** |
| `deviceID` | 0x1a050209 | 0x1a05020a |
| device extensions offered | 110 | 131 |

MoltenVK clamps what a *device* admits to to the API version the **instance** asked for. RetroArch
asks for 1.1, so the device reports 1.1 — and `retrohost --vk` asks for `VK_API_VERSION_1_1`
deliberately, to mirror that. **The ceiling is core Vulkan 1.1 on both**, and a device with a
different ceiling under one host would make the two non-comparable, which is the whole reason the
host does not ask for more than it needs.

## The device

`Apple M5`, integrated GPU, vendor 0x106b (Apple).

- **Four queue families**, each with a single queue, each `graphics|compute|transfer`, 64 timestamp
  bits. The frontend gives us **family 0** and the interface documents that the family and queue are
  constant for the context's lifetime.
- **One 32 GiB device-local heap**, three memory types: `device-local`; `device-local | host-visible
  | host-coherent | host-cached`; `device-local | lazily-allocated`. Unified memory — **a staging
  buffer costs no extra footprint**, and type 1 is both the fast path and the mappable one.
- Timestamps are available on every family, at 1.0 ns — profiling in P5 has somewhere to start.

## Formats

| format | verdict |
|---|---|
| `B8G8R8A8_UNORM` | sampled, storage, colour-attachment, blend, blit both ways, transfer both ways, linear filter. **This is the passthrough format** and MAME's `bitmap_rgb32` pixel maps onto it with no swizzle |
| `R8G8B8A8_UNORM` | identical support |
| **`D24_UNORM_S8_UINT`** | **does not exist.** `optimalTilingFeatures` is literally zero, on both hosts |
| `D32_SFLOAT` | sampled, depth-stencil, blit both ways, transfer both ways, linear filter |
| `D32_SFLOAT_S8_UINT` | the same |

**P3 must pick `D32_SFLOAT` or `D32_SFLOAT_S8_UINT`.** The 24-bit combined format is the reflex
choice on desktop and it is not available on Apple GPUs at all. Take the stencil variant only if
stencil is actually wanted — shadows are planned as a dedicated pipeline keyed on `det(3×3)≈0`
(`../Polydiver/PDDocs/model2/model2_shadows.md`), not as a stencil pass, so `D32_SFLOAT` is the
default until something argues otherwise.

**Two unnamed bits show up in `optimalTilingFeatures`, and neither is a problem.** `0x80000000` on
both colour formats is `VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT` — a *flags2* value
leaking into a flags1 field, which has no name in the 32-bit enum. `0x10000` (host only) is
`VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT`, a real 1.2 feature the newer MoltenVK
advertises. The probe prints unnamed bits as hex rather than dropping them, which is why they were
noticed at all.

## Features

Identical on both hosts. **Availability, not enablement** — see the caveat below.

**Present:** `depthBiasClamp`, `depthClamp`, `fillModeNonSolid`, `samplerAnisotropy` (max 16),
`independentBlend`, `dualSrcBlend`, `alphaToOne`, `largePoints`, `multiViewport`, `imageCubeArray`,
`textureCompressionBC`, `occlusionQueryPrecise`, `fragmentStoresAndAtomics`, `shaderClipDistance`,
`shaderSampledImageArrayDynamicIndexing`.

**Absent:** `geometryShader`, `logicOp`, `wideLines` (`lineWidthRange` is 1.0–1.0 exactly).

What that means for the plan as written:

- **`depthBiasClamp` is there**, which is the one the decal/z-fight fix needs — per-primitive depth
  bias keyed on display-list order, the Unity vertex-alpha solution ported
  (`../Polydiver/PDDocs/model1/model1_sortorder.md`).
- **No geometry shader.** Nothing in P3–P6 asks for one, but it is the usual reflex for expanding
  points or quads and it is not available.
- **No wide lines.** `lineWidthRange` is 1.0–1.0, so anything wanting a thick line has to draw
  geometry.
- `fillModeNonSolid` is present, so wireframe debug views are possible.

## Limits worth knowing

| | RetroArch | host | matters to |
|---|---|---|---|
| max 2D image | 16384 | 32768 | internal-res scaling, atlases (P5) |
| max image array layers | 2048 | 2048 | texture atlasing |
| sampled images per stage | 128 | 256 | bindless-ish texture arrays |
| samplers per stage | 16 | 16 | — |
| max sampler LOD bias | 4.0 | 16.0 | microtexture LOD (`texlod` in the poly tap) |
| push constants | 4096 B | 4096 B | generous; per-poly params can go here |
| bound descriptor sets | 8 | 8 | — |
| vertex input attributes | 31 | 31 | — |
| viewports | 16 | 16 | Model 2's per-window viewports |
| colour attachments | 8 | 8 | — |
| max memory allocations | 2³⁰ | 2³⁰ | effectively unbounded |

**Take the smaller of each column.** Two of these differ between the hosts and both differences are
in the direction that would let a renderer work under `retrohost --vk` and fail under RetroArch —
`maxSamplerLodBias` 16 vs 4 is the one most likely to bite, since microtexture LOD is a real Model 2
feature and 4.0 is not a lot of headroom.

**Copy alignment, which is why the passthrough upload needs no padding:**
`optimalBufferCopyOffsetAlignment` 16, `optimalBufferCopyRowPitchAlignment` **1**,
`nonCoherentAtomSize` 16, `bufferImageGranularity` 16, `minMemoryMapAlignment` 64. A tightly packed
496×384×4 upload satisfies all of them as-is.

## The caveat that governs all of the above

**We did not create the device.** Without the context-negotiation interface the frontend chose the
GPU, created the `VkDevice`, and decided which features and extensions to enable — and there is no
way to ask it which. So everything in this file is what the device *could* do, not what is *turned
on*. Calling into a feature listed here without negotiation is undefined behaviour that will look
like it works right up until it does not.

The extensions below are **available on the device and unreachable from the core today** for exactly
that reason: `VK_KHR_portability_subset`, `VK_KHR_swapchain`, `VK_KHR_maintenance1`,
`VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2`, `VK_KHR_timeline_semaphore`,
`VK_KHR_push_descriptor`, `VK_KHR_image_format_list`, `VK_EXT_descriptor_indexing`,
`VK_EXT_memory_budget`, `VK_EXT_metal_objects` (all `+` on both hosts).

Dynamic rendering and synchronization2 in particular would simplify P3 considerably and **must not be
used** until negotiation lands. `SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE`
([libretro.h:1684](../src/osd/libretro_m2/libretro.h#L1684)) is the mechanism; it was deliberately
deferred in P2 because it buys nothing for a passthrough and removes an entire failure surface. The
core is written to take an externally supplied device from day one, so adding negotiation later
changes who *creates* the device and nothing else.

**When we do create the device ourselves on macOS**, `VK_KHR_portability_subset` must be enabled on
it — the spec requires it of any device that advertises it, and every MoltenVK device does — and the
instance needs `VK_KHR_portability_enumeration` plus the enumeration flag if it goes through a real
loader. `retrohost --vk` already does both and is the working reference for it.

## One rule that is not about capabilities

**Vulkan is only ever touched on the frontend's thread** — inside `retro_run`, `context_reset` or
`context_destroy`. The emulation thread is parked on the baton throughout and never makes a Vulkan
call. This is what makes mid-run teardown safe by construction rather than by luck, and it is a
property to preserve in P3, where the temptation to build command buffers on the emulation thread
will be much stronger.
