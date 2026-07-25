// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro core — the Vulkan context's lifecycle.

    See vk_context.h for the rules this keeps. The other half of the file is the probe: on the first
    context_reset it logs what the frontend's Vulkan implementation actually is and what it can
    actually do. That log is not decoration. The headers this core is built against are 1.4, and on
    this machine the physical device RetroArch hands over reports apiVersion 1.1.0 — MoltenVK clamps
    a device to the instance's requested API version, whatever the driver itself could do — so
    anything above core 1.1 compiles perfectly happily and does not exist at run time. Every
    decision the renderer makes from here — depth format, staging alignment, whether depthBiasClamp
    is available for the decal fix, whether dynamic rendering or synchronization2 can be used at all
    — is answered by this log rather than by the headers.

*********************************************************************************************************************************/

#include "renderer_vk/vk_context.h"

#include "renderer_vk/vk_present.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace m2vk {

namespace {

//============================================================
//  state
//============================================================

retro_environment_t s_environ_cb = nullptr;

// Set once SET_HW_RENDER has been accepted; cleared when the machine goes away. A context_reset
// that arrives while this is false belongs to a load that has already been torn down.
bool s_declared = false;

// Owned by the frontend, valid from context_reset to context_destroy, and never copied: the struct
// carries function pointers and an opaque handle, and the frontend is entitled to keep it live only
// for the context's lifetime.
const retro_hw_render_interface_vulkan *s_iface = nullptr;

vk_funcs s_funcs;

bool s_probe_logged = false;


//============================================================
//  formatting helpers
//============================================================

struct flag_name { uint32_t bit; char const *name; };

template <size_t N>
std::string flags_to_string(uint32_t value, flag_name const (&names)[N])
{
	std::string s;
	uint32_t rest = value;
	for (size_t i = 0; i < N; i++)
	{
		if ((value & names[i].bit) != 0)
		{
			if (!s.empty())
				s += '|';
			s += names[i].name;
			rest &= ~names[i].bit;
		}
	}

	// Anything we have no name for still gets reported — an unnamed bit is exactly the sort of thing
	// worth noticing in this log.
	if (rest != 0)
	{
		char buf[24];
		std::snprintf(buf, sizeof(buf), "0x%x", unsigned(rest));
		if (!s.empty())
			s += '|';
		s += buf;
	}

	return s.empty() ? std::string("none") : s;
}

std::string version_string(uint32_t v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u",
			VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
	return std::string(buf);
}

char const *device_type_name(VkPhysicalDeviceType type)
{
	switch (type)
	{
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete GPU";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual GPU";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
	case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return "other";
	default:                                     return "unknown";
	}
}


//============================================================
//  the probe
//============================================================

void log_interface(retro_hw_render_interface_vulkan const &iface)
{
	vk_log(RETRO_LOG_INFO, "interface v%u (this core built against v%u)\n",
			iface.interface_version, unsigned(RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION));
	vk_log(RETRO_LOG_INFO, "handles: instance %p gpu %p device %p queue %p (family %u), frontend handle %p\n",
			static_cast<void const *>(iface.instance), static_cast<void const *>(iface.gpu),
			static_cast<void const *>(iface.device), static_cast<void const *>(iface.queue),
			iface.queue_index, iface.handle);

	// Which of the interface's entry points the frontend actually filled in. We need everything up
	// to unlock_queue; set_command_buffers is the alternative to submitting ourselves (we submit),
	// and set_signal_semaphore is the v5 tail field, read only when the frontend claims v5.
	std::string present;
	auto const note = [&present](char const *name, bool have)
	{
		if (!present.empty())
			present += ' ';
		present += have ? "+" : "-";
		present += name;
	};
	note("set_image", iface.set_image != nullptr);
	note("get_sync_index", iface.get_sync_index != nullptr);
	note("get_sync_index_mask", iface.get_sync_index_mask != nullptr);
	note("wait_sync_index", iface.wait_sync_index != nullptr);
	note("set_command_buffers", iface.set_command_buffers != nullptr);
	note("lock_queue", iface.lock_queue != nullptr);
	note("unlock_queue", iface.unlock_queue != nullptr);
	if (iface.interface_version >= 5)
		note("set_signal_semaphore", iface.set_signal_semaphore != nullptr);
	vk_log(RETRO_LOG_INFO, "entry points: %s\n", present.c_str());
}

void log_device(retro_hw_render_interface_vulkan const &iface, vk_funcs const &fns)
{
	uint32_t instance_version = 0;
	if (fns.enumerate_instance_version != nullptr)
		fns.enumerate_instance_version(&instance_version);
	vk_log(RETRO_LOG_INFO, "instance api %s\n",
			(instance_version != 0) ? version_string(instance_version).c_str() : "1.0 (no vkEnumerateInstanceVersion)");

	VkPhysicalDeviceProperties props{};
	fns.get_physical_device_properties(iface.gpu, &props);

	// apiVersion is the ceiling — the highest core version this device will honour. Everything the
	// renderer may assume follows from this number and not from the headers.
	vk_log(RETRO_LOG_INFO, "device '%s' (%s), api %s, driver 0x%08x (%s), vendor 0x%04x device 0x%04x\n",
			props.deviceName, device_type_name(props.deviceType),
			version_string(props.apiVersion).c_str(),
			props.driverVersion, version_string(props.driverVersion).c_str(),
			props.vendorID, props.deviceID);

	VkPhysicalDeviceLimits const &l = props.limits;
	vk_log(RETRO_LOG_INFO, "limits: max 2D image %u, array layers %u, viewports %u, colour attachments %u, samplers/stage %u, sampled images/stage %u\n",
			l.maxImageDimension2D, l.maxImageArrayLayers, l.maxViewports,
			l.maxColorAttachments, l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages);
	vk_log(RETRO_LOG_INFO, "limits: push constants %u B, bound descriptor sets %u, vertex attributes %u, max anisotropy %.1f, max sampler LOD bias %.1f\n",
			l.maxPushConstantsSize, l.maxBoundDescriptorSets, l.maxVertexInputAttributes,
			double(l.maxSamplerAnisotropy), double(l.maxSamplerLodBias));
	// The three that decide how the frame gets uploaded in step 4.
	vk_log(RETRO_LOG_INFO, "limits: optimal buffer copy offset %llu, row pitch %llu, non-coherent atom %llu, buffer-image granularity %llu, map alignment %llu\n",
			(unsigned long long)l.optimalBufferCopyOffsetAlignment,
			(unsigned long long)l.optimalBufferCopyRowPitchAlignment,
			(unsigned long long)l.nonCoherentAtomSize,
			(unsigned long long)l.bufferImageGranularity,
			(unsigned long long)l.minMemoryMapAlignment);
	vk_log(RETRO_LOG_INFO, "limits: max allocations %u, line width %.1f-%.1f, point size %.1f-%.1f, timestamps %s (%.1f ns)\n",
			l.maxMemoryAllocationCount,
			double(l.lineWidthRange[0]), double(l.lineWidthRange[1]),
			double(l.pointSizeRange[0]), double(l.pointSizeRange[1]),
			l.timestampComputeAndGraphics ? "yes" : "no", double(l.timestampPeriod));

	// Supported, not enabled. Without the negotiation interface the frontend created the device and
	// chose what to turn on, and there is no way to ask it which; a feature listed here is one we
	// could get if we took device creation over in a later phase, not one we may use today.
	VkPhysicalDeviceFeatures feats{};
	fns.get_physical_device_features(iface.gpu, &feats);
	static struct { char const *name; VkBool32 VkPhysicalDeviceFeatures::*field; } const WATCHED_FEATURES[] = {
			{ "depthBiasClamp",             &VkPhysicalDeviceFeatures::depthBiasClamp },
			{ "depthClamp",                 &VkPhysicalDeviceFeatures::depthClamp },
			{ "fillModeNonSolid",           &VkPhysicalDeviceFeatures::fillModeNonSolid },
			{ "samplerAnisotropy",          &VkPhysicalDeviceFeatures::samplerAnisotropy },
			{ "independentBlend",           &VkPhysicalDeviceFeatures::independentBlend },
			{ "dualSrcBlend",               &VkPhysicalDeviceFeatures::dualSrcBlend },
			{ "logicOp",                    &VkPhysicalDeviceFeatures::logicOp },
			{ "alphaToOne",                 &VkPhysicalDeviceFeatures::alphaToOne },
			{ "wideLines",                  &VkPhysicalDeviceFeatures::wideLines },
			{ "largePoints",                &VkPhysicalDeviceFeatures::largePoints },
			{ "geometryShader",             &VkPhysicalDeviceFeatures::geometryShader },
			{ "multiViewport",              &VkPhysicalDeviceFeatures::multiViewport },
			{ "imageCubeArray",             &VkPhysicalDeviceFeatures::imageCubeArray },
			{ "textureCompressionBC",       &VkPhysicalDeviceFeatures::textureCompressionBC },
			{ "occlusionQueryPrecise",      &VkPhysicalDeviceFeatures::occlusionQueryPrecise },
			{ "fragmentStoresAndAtomics",   &VkPhysicalDeviceFeatures::fragmentStoresAndAtomics },
			{ "shaderClipDistance",         &VkPhysicalDeviceFeatures::shaderClipDistance },
			{ "shaderSampledImageArrayDynamicIndexing", &VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing } };
	std::string features;
	for (auto const &f : WATCHED_FEATURES)
	{
		if (!features.empty())
			features += ' ';
		features += (feats.*(f.field)) ? "+" : "-";
		features += f.name;
	}
	vk_log(RETRO_LOG_INFO, "device supports (not necessarily enabled — the frontend created the device): %s\n", features.c_str());
}

void log_queues(retro_hw_render_interface_vulkan const &iface, vk_funcs const &fns)
{
	static flag_name const QUEUE_FLAGS[] = {
			{ VK_QUEUE_GRAPHICS_BIT,       "graphics" },
			{ VK_QUEUE_COMPUTE_BIT,        "compute" },
			{ VK_QUEUE_TRANSFER_BIT,       "transfer" },
			{ VK_QUEUE_SPARSE_BINDING_BIT, "sparse" },
			{ VK_QUEUE_PROTECTED_BIT,      "protected" } };

	uint32_t count = 0;
	fns.get_physical_device_queue_family_properties(iface.gpu, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	if (count != 0)
		fns.get_physical_device_queue_family_properties(iface.gpu, &count, families.data());

	for (uint32_t i = 0; i < count; i++)
	{
		vk_log(RETRO_LOG_INFO, "queue family %u:%s %u queue%s, %s, %u timestamp bits\n",
				i, (i == iface.queue_index) ? " [ours]" : "",
				families[i].queueCount, (families[i].queueCount == 1) ? "" : "s",
				flags_to_string(families[i].queueFlags, QUEUE_FLAGS).c_str(),
				families[i].timestampValidBits);
	}

	if (iface.queue_index >= count)
		vk_log(RETRO_LOG_WARN, "the frontend's queue family %u is outside the %u this device reports\n", iface.queue_index, count);
}

void log_memory(retro_hw_render_interface_vulkan const &iface, vk_funcs const &fns)
{
	static flag_name const HEAP_FLAGS[] = {
			{ VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,   "device-local" },
			{ VK_MEMORY_HEAP_MULTI_INSTANCE_BIT, "multi-instance" } };
	static flag_name const TYPE_FLAGS[] = {
			{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,     "device-local" },
			{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,     "host-visible" },
			{ VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,    "host-coherent" },
			{ VK_MEMORY_PROPERTY_HOST_CACHED_BIT,      "host-cached" },
			{ VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazily-allocated" },
			{ VK_MEMORY_PROPERTY_PROTECTED_BIT,        "protected" } };

	VkPhysicalDeviceMemoryProperties mem{};
	fns.get_physical_device_memory_properties(iface.gpu, &mem);

	for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
	{
		vk_log(RETRO_LOG_INFO, "memory heap %u: %llu MiB, %s\n", i,
				(unsigned long long)(mem.memoryHeaps[i].size / (1024 * 1024)),
				flags_to_string(mem.memoryHeaps[i].flags, HEAP_FLAGS).c_str());
	}
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
	{
		vk_log(RETRO_LOG_INFO, "memory type %u: heap %u, %s\n", i,
				mem.memoryTypes[i].heapIndex,
				flags_to_string(mem.memoryTypes[i].propertyFlags, TYPE_FLAGS).c_str());
	}
}

void log_formats(retro_hw_render_interface_vulkan const &iface, vk_funcs const &fns)
{
	static flag_name const FORMAT_FEATURES[] = {
			{ VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,                "sampled" },
			{ VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,                "storage" },
			{ VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,             "colour-attachment" },
			{ VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,       "blend" },
			{ VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,     "depth-stencil" },
			{ VK_FORMAT_FEATURE_BLIT_SRC_BIT,                     "blit-src" },
			{ VK_FORMAT_FEATURE_BLIT_DST_BIT,                     "blit-dst" },
			{ VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,                 "transfer-src" },
			{ VK_FORMAT_FEATURE_TRANSFER_DST_BIT,                 "transfer-dst" },
			{ VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,  "filter-linear" } };

	// B8G8R8A8_UNORM is the one P2 depends on: MAME's bitmap_rgb32 pixel is 0xAARRGGBB in a native
	// uint32_t, which on little-endian is B,G,R,A in memory, so it uploads with no swizzle at all.
	// The depth formats are P3's, asked now because the answer costs nothing here.
	static struct { char const *name; VkFormat format; } const WATCHED_FORMATS[] = {
			{ "B8G8R8A8_UNORM",   VK_FORMAT_B8G8R8A8_UNORM },
			{ "R8G8B8A8_UNORM",   VK_FORMAT_R8G8B8A8_UNORM },
			{ "D32_SFLOAT",       VK_FORMAT_D32_SFLOAT },
			{ "D24_UNORM_S8_UINT",VK_FORMAT_D24_UNORM_S8_UINT },
			{ "D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT } };

	for (auto const &f : WATCHED_FORMATS)
	{
		VkFormatProperties fp{};
		fns.get_physical_device_format_properties(iface.gpu, f.format, &fp);
		vk_log(RETRO_LOG_INFO, "format %s: optimal %s\n", f.name,
				flags_to_string(fp.optimalTilingFeatures, FORMAT_FEATURES).c_str());
	}
}

void log_extensions(retro_hw_render_interface_vulkan const &iface, vk_funcs const &fns)
{
	// Available on the device, which is not the same as enabled on it. The frontend created the
	// device, so the extensions it turned on are its business and unqueryable from here; this is a
	// list of what could be had if a later phase takes device creation over.
	static char const *const WATCHED[] = {
			"VK_KHR_portability_subset",
			"VK_KHR_swapchain",
			"VK_KHR_maintenance1",
			"VK_KHR_dynamic_rendering",
			"VK_KHR_synchronization2",
			"VK_KHR_timeline_semaphore",
			"VK_KHR_push_descriptor",
			"VK_KHR_image_format_list",
			"VK_EXT_descriptor_indexing",
			"VK_EXT_memory_budget",
			"VK_EXT_metal_objects" };

	uint32_t count = 0;
	if (fns.enumerate_device_extension_properties(iface.gpu, nullptr, &count, nullptr) != VK_SUCCESS)
	{
		vk_log(RETRO_LOG_WARN, "device extensions could not be enumerated\n");
		return;
	}
	std::vector<VkExtensionProperties> exts(count);
	if ((count != 0) && (fns.enumerate_device_extension_properties(iface.gpu, nullptr, &count, exts.data()) != VK_SUCCESS))
		return;

	std::string found;
	for (char const *const want : WATCHED)
	{
		bool have = false;
		for (auto const &e : exts)
		{
			if (std::strcmp(e.extensionName, want) == 0)
			{
				have = true;
				break;
			}
		}
		if (!found.empty())
			found += ' ';
		found += have ? "+" : "-";
		found += want;
	}
	vk_log(RETRO_LOG_INFO, "device offers %u extensions (availability, not enablement): %s\n", count, found.c_str());
}


//============================================================
//  taking delivery
//============================================================

// Everything read below sits inside the interface as it was first published, with the single
// exception of set_signal_semaphore, which is the v5 tail field and is only touched when the
// frontend says v5. So the version check is what makes reading the rest legal, not a formality.
bool interface_usable(retro_hw_render_interface_vulkan const &iface)
{
	if (iface.interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
	{
		vk_log(RETRO_LOG_ERROR, "the frontend handed over interface type %u, not Vulkan\n", unsigned(iface.interface_type));
		return false;
	}
	if (iface.interface_version < 1)
	{
		vk_log(RETRO_LOG_ERROR, "the frontend reports interface v%u, which this core cannot use\n", iface.interface_version);
		return false;
	}
	if (iface.interface_version > RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
	{
		// Newer than we were built against. The struct only ever grows at the end, so everything we
		// read is still where we expect it; worth a line rather than a refusal.
		vk_log(RETRO_LOG_WARN, "the frontend reports interface v%u, newer than the v%u this core was built against\n",
				iface.interface_version, unsigned(RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION));
	}

	if ((iface.instance == VK_NULL_HANDLE) || (iface.gpu == VK_NULL_HANDLE)
			|| (iface.device == VK_NULL_HANDLE) || (iface.queue == VK_NULL_HANDLE))
	{
		vk_log(RETRO_LOG_ERROR, "the frontend's Vulkan handles are incomplete\n");
		return false;
	}

	// The set the renderer will actually call. lock_queue/unlock_queue are in that set because the
	// queue is shared with the frontend and submitting without them corrupts in ways that look like
	// a driver bug.
	if ((iface.set_image == nullptr) || (iface.get_sync_index == nullptr) || (iface.get_sync_index_mask == nullptr)
			|| (iface.wait_sync_index == nullptr) || (iface.lock_queue == nullptr) || (iface.unlock_queue == nullptr))
	{
		vk_log(RETRO_LOG_ERROR, "the frontend's Vulkan interface is missing entry points this core needs\n");
		return false;
	}

	return true;
}

void context_reset_cb()
{
	if (!s_declared)
	{
		// A reset for content that has already been unloaded. Nothing to build on.
		vk_log(RETRO_LOG_WARN, "context_reset arrived with no content loaded; ignored\n");
		return;
	}

	// cache_context is false, so a reset means everything from a previous one is gone: drop what we
	// hold before taking delivery, and do it while the old handles are still the ones the ring was
	// built against. A second context_reset with no intervening context_destroy is legal and lands
	// here.
	present_shutdown();
	s_iface = nullptr;
	s_funcs = vk_funcs{};

	retro_hw_render_interface const *base = nullptr;
	if ((s_environ_cb == nullptr) || !s_environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &base) || (base == nullptr))
	{
		vk_log(RETRO_LOG_ERROR, "context_reset fired but the frontend offers no HW render interface\n");
		return;
	}

	auto const *const iface = reinterpret_cast<retro_hw_render_interface_vulkan const *>(base);
	if (!interface_usable(*iface))
		return;

	// This resolves the device-level half of the table as well as the instance-level one, which
	// doubles as proof that the device loader works before anything relies on it. A device-level
	// name the instance loader would happily resolve and the device loader would not is the failure
	// it catches, and catching it here is far cheaper than catching it mid-frame.
	if (!load_funcs(s_funcs, iface->get_instance_proc_addr, iface->get_device_proc_addr, iface->instance, iface->device))
	{
		s_funcs = vk_funcs{};
		return;
	}

	s_iface = iface;

	if (!s_probe_logged)
	{
		log_interface(*iface);
		log_device(*iface, s_funcs);
		log_queues(*iface, s_funcs);
		log_memory(*iface, s_funcs);
		log_formats(*iface, s_funcs);
		log_extensions(*iface, s_funcs);
		s_probe_logged = true;
	}
	else
	{
		VkPhysicalDeviceProperties props{};
		s_funcs.get_physical_device_properties(iface->gpu, &props);
		vk_log(RETRO_LOG_INFO, "context reset: '%s', api %s, queue family %u\n",
				props.deviceName, version_string(props.apiVersion).c_str(), iface->queue_index);
	}
}

void context_destroy_cb()
{
	if (s_iface == nullptr)
		return;

	// Safe against the emulator by construction: the baton has the emulation thread parked inside
	// update() for the whole of retro_run, and no Vulkan call is ever made from it. So teardown here
	// cannot race a frame in flight.
	//
	// The ring goes first, while the device is still alive — this callback is the last moment at
	// which it is — and only then are the handles let go.
	present_shutdown();
	vk_log(RETRO_LOG_INFO, "context destroyed\n");
	s_iface = nullptr;
	s_funcs = vk_funcs{};
}

} // anonymous namespace


//============================================================
//  m2vk — the public surface
//============================================================

bool declare_hw_render(retro_environment_t environ_cb, retro_log_printf_t log_cb)
{
	set_log(log_cb);
	s_environ_cb = environ_cb;
	s_iface = nullptr;
	s_funcs = vk_funcs{};
	s_declared = false;

	if (environ_cb == nullptr)
		return false;

	// version_major/minor are 1.0: the floor, not a request for anything. Vulkan's own version
	// negotiation happens at instance creation, which is the frontend's business while we have no
	// negotiation interface — and MoltenVK 1.2.7 is the real ceiling regardless of what our 1.4
	// headers would let us ask for.
	//
	// depth, stencil and bottom_left_origin are GL-era fields with no Vulkan meaning; they stay zero
	// rather than being guessed at. cache_context is false so that a lost context is always
	// rebuilt from nothing, which is the only lifecycle we test.
	retro_hw_render_callback hw{};
	hw.context_type = RETRO_HW_CONTEXT_VULKAN;
	hw.version_major = 1;
	hw.version_minor = 0;
	hw.context_reset = &context_reset_cb;
	hw.context_destroy = &context_destroy_cb;
	hw.cache_context = false;

	if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw))
		return false;

	s_declared = true;
	return true;
}

void forget_hw_render()
{
	// If the frontend fired context_destroy first — it normally does — this is a no-op. If it did
	// not, this is where the ring gets destroyed while its device is still alive.
	present_shutdown();
	s_iface = nullptr;
	s_funcs = vk_funcs{};
	s_declared = false;
}

bool have_context()
{
	return s_iface != nullptr;
}

const retro_hw_render_interface_vulkan *context_interface()
{
	return s_iface;
}

const vk_funcs &context_funcs()
{
	return s_funcs;
}

} // namespace m2vk
