/* Minimal libretro host for testing model2_libretro.dylib without RetroArch.
 *
 * Loads the core, boots a ROM, runs N frames as fast as it can, and writes the last frame as a
 * PPM. Enough to prove the whole libretro path — load, av_info, the per-frame baton, video and
 * audio — and to be the thing the A/B harness grows out of, since it can dump any frame by number
 * and P0 measured runs to be bit-repeatable.
 *
 *   devnotes/build-retrohost.sh          (cc line, include paths, and why)
 *   ./devnotes/retrohost ./model2_libretro.dylib devnotes/roms/vf2.zip 1200 /tmp/f.ppm
 *
 * With --vk it is also a Vulkan frontend: it dlopens MoltenVK, creates an instance and a device of
 * its own, implements retro_hw_render_interface_vulkan, and reads the image the core hands to
 * set_image straight back off the GPU into the very same frame buffer the software path fills. So
 * both renderers converge on one code path for the PPM, the digest and the frame accounting, and
 * the A/B comparison is a cmp of two files this program wrote the same way:
 *
 *   ./devnotes/retrohost --vk ./model2_libretro.dylib devnotes/roms/vf2.zip 1200 /tmp/vk.ppm
 *
 * There is no window, no swapchain and no surface extension, which makes this a *smaller* job than
 * RetroArch's, not a larger one. What it must get exactly right is the contract in
 * libretro_vulkan.h — the sync index, the mask, and the queue lock — and those are the parts worth
 * reading below. See devnotes/p2-vulkan-passthrough.md §5.
 *
 * The optional trailing argument is a control script — a comma-separated list of
 * frame:control[:frames-held][:port], e.g. "600:select,660:start,700:b:30,800:lx+:200" — which is
 * how the input path gets exercised without a real frontend. A control is either a RetroPad button
 * name, a half-axis (lx±, ly±, rx±, ry±) held at full deflection, or one of the lightgun controls;
 * see CONTROLS below. A half-axis takes an optional fraction — "lx+=0.35" is 35 % of full
 * deflection — which is how an analogue sweep is written.
 *
 * The gun is the reason for --gun and for the aim payload:
 *
 *   ./devnotes/retrohost --gun 0 ./model2_libretro.dylib devnotes/roms/vcop.zip 1400 /tmp/f.ppm \
 *       "1200:gun=0.0/0.5:20,1220:gun=0.5/0.5:20,1240:gun=1.0/0.5:20,1260:trigger:10"
 *
 * --gun <port> is retro_set_controller_port_device for that port; "gun=<x>/<y>" aims at normalised
 * screen coordinates and "trigger", "reload" and "offscreen" are the gun's buttons. What the aim
 * came out as on the other side is M2VK_GUN_LOG=<n> in the core (src/osd/libretro_m2/m2vk_gunlog.h),
 * which prints the resolved port values — the only read-out there is, since this OSD draws no
 * crosshair. See devnotes/lightgun.md.
 *
 * --modern <port> is the same call with the core's second pad layout, which is a subclass of
 * RETRO_DEVICE_JOYPAD rather than a class of its own: nothing about the script changes, only which
 * MAME button each RetroPad control ends up as. It is how the layout swap is measured — press "r"
 * with and without it and compare what the game does.
 *
 * ⚠ Whatever is being measured, run these SEQUENTIALLY. Several retrohost processes at once make
 * renderer=software non-deterministic: six identical vf2 runs launched together produced two
 * different whole-run digests, and the same command run four times in a row produced one. MAME's
 * scanline rasteriser is multi-threaded and its work queue is sized off the core count, so under
 * contention the runs stop agreeing. It reads exactly like the change under test.
 *
 * Core options take their declared defaults unless overridden by an environment variable named
 * M2OPT_<key>, e.g. M2OPT_model2_service_buttons=enabled. MAME's writable directories go under
 * ./retrohost-save, or $M2_SAVE_DIR; $M2_SYSTEM_DIR is added to the rompath if set.
 *
 * Vulkan-side environment variables, all optional, all for exercising paths RetroArch cannot reach:
 *
 *   M2VK_HOST_MOLTENVK=<path>      which libMoltenVK to dlopen (default: Homebrew's, then the one
 *                                  inside RetroArch.app)
 *   M2VK_HOST_SYNC_MASK=0x7        the sync-index mask reported to the core; 0x7 is what RetroArch
 *                                  reports on this machine, so it is the default
 *   M2VK_HOST_MASK_AT=<frame>:<mask>[,...]   change the mask mid-run. The one ring-rebuild trigger
 *                                  RetroArch never produces, because its mask is always 0x7.
 *   M2VK_HOST_RESET_AT=<frame>[,...]         destroy the device and build a new one, i.e. what a
 *                                  RetroArch fullscreen toggle does to the core.
 *   M2VK_HOST_SKIP_DESTROY=1       do the above *without* calling context_destroy first, which is
 *                                  the ordering the core's abandon path exists for.
 *
 * And three that have nothing to do with Vulkan:
 *
 *   M2VK_HOST_RSS=<n>              print the process's resident size every n frames, and the peak
 *                                  at the end. The core is supposed to reach a steady state and
 *                                  stop allocating; this is what says whether it did.
 *   M2VK_HOST_PERF=1               time the run and print `speed:` — the unthrottled headroom
 *                                  figure, which is the one number RetroArch's 104.5 % is not.
 *                                  See the block comment above the timers below; the two host
 *                                  costs are measured separately and taken *out* of the headline,
 *                                  because neither is the core's work.
 *   M2VK_HOST_PERF_SKIP=<n>        ignore the first n frames in the headline. Every game boots
 *                                  through a stretch that renders nothing (vf2: ~990 frames), and
 *                                  averaging that in measures the boot, not the renderer. The
 *                                  per-bucket table prints regardless, so the choice is visible.
 */

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach/mach.h>
#include <time.h>

/* The real headers, not local approximations of them. retro_hw_render_callback and
 * retro_hw_render_interface_vulkan are far too big to retype by hand, and a field at the wrong
 * offset in either would be silent memory corruption across a dylib boundary rather than a
 * compile error. libretro_vulkan.h pulls in libretro.h and vulkan/vulkan.h itself. */
#include <libretro_vulkan.h>

/* Nothing here links against a Vulkan library: every entry point below is a pointer resolved at
 * run time from the dlopened MoltenVK, exactly as the core resolves its own from the interface. A
 * stray direct call to vkSomething() is therefore a link error, which is the point. */

/* A value chosen for the run overrides the core's default. Set one per option with an environment
 * variable named for the key, e.g. M2OPT_model2_service_buttons=enabled. */
static struct { const char *key, *value; } g_options[16];
static unsigned g_option_count;

/* A core option changed WHILE CONTENT RUNS -- see parse_opt_changes() below.  Declared here rather
 * than beside its parser because the environment callback above reads the flag. */
static struct { long frame; const char *key, *value; } g_opt_changes[8];
static unsigned g_opt_change_count;
static bool g_opt_updated;

static const char *option_value(const char *key)
{
	char name[128];
	const char *from_env;
	unsigned n;

	snprintf(name, sizeof name, "M2OPT_%s", key);
	from_env = getenv(name);
	if (from_env)
		return from_env;

	for (n = 0; n < g_option_count; n++)
		if (!strcmp(g_options[n].key, key))
			return g_options[n].value;
	return NULL;
}

static const uint32_t *g_fb;
static unsigned g_w, g_h;
static size_t g_pitch;
static unsigned long g_frames_with_video, g_audio_samples;
static bool g_shutdown;

/* Resident size, for M2VK_HOST_RSS. getrusage's ru_maxrss is a peak and a peak cannot show a slow
 * leak flattening out, so ask the kernel for the current figure instead. Reads as 0 rather than
 * failing loudly: this is a diagnostic and it must never be the reason a run stops. */
static size_t resident_bytes(void)
{
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
		return 0;
	return (size_t)info.resident_size;
}

/* --- the perf timers, for M2VK_HOST_PERF -----------------------------------------------------
 *
 * The point of this is one number that RetroArch cannot give: how fast the core would run if
 * nothing were pacing it. RetroArch's `Average speed: 104.5 %` means "keeps up with the declared
 * 57.52 Hz" and nothing more — it did not move when the entire textured path landed on the GPU,
 * which is the proof it was never a headroom measurement. See performance.md §2.
 *
 * Three costs are separated because only the first is the core's:
 *
 *   core     wall time inside retro_run, minus everything below. The emulation thread (two
 *            interpreted i960s, the copro DSPs, a 68000) plus, on the Vulkan path, the renderer's
 *            own recording and submit. This is what an optimisation has to move.
 *   gpuwait  the read-back's vkWaitForFences. The core ends its frame with a submit and no fence,
 *            so this wait is where its GPU work is actually observed to finish. It is a *lower
 *            bound* on GPU time — anything the GPU did while the CPU was still busy is already
 *            hidden and does not appear here.
 *   host     the read-back's command recording and 762 KB memcpy, plus the whole-run digest. Pure
 *            harness overhead: no frontend hashes every frame or stalls the pipe to copy it back
 *            to system memory. It is measured so that it can be taken out rather than assumed
 *            small — at 496x384 it is not.
 *
 * Speed is reported as a **bracket**, because this harness cannot overlap CPU and GPU and a real
 * frontend does. The read-back waits on a fence immediately after the core's submit, so the two
 * never run at once here:
 *
 *   serial      core + gpuwait, the lower bound — what this harness actually achieves
 *   pipelined   max(core, gpuwait), the upper bound — what a frontend that lets the GPU run into
 *               the next frame's emulation would get, which is every real one
 *
 * The truth is between them and much nearer `pipelined`, because `gpuwait` is itself an
 * *over*-estimate of the core's GPU time: the wait covers our own image→buffer copy of the whole
 * frame as well as the core's rendering. Quoting only the serial figure made the Vulkan path look
 * slower than the software rasterizer on vcop2, which it is not. **Do not quote one number.**
 *
 * The wall figure is against all three, so the distortion the harness introduces is printed rather
 * than argued about. None of it is a claim about the Quest 3, where §1 says the memory bus alone
 * invalidates the desktop picture.
 *
 * All of it is nanoseconds from CLOCK_MONOTONIC. The timers are always compiled in and cost two
 * clock reads per frame when M2VK_HOST_PERF is off, which is below the noise of a 17 ms frame. */
static bool g_perf;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Accumulated over the whole run, and again per bucket so that a game which boots for 990 frames
 * before it renders anything shows that as a shape rather than as a lower average. */
static struct {
	uint64_t run, gpuwait, host;   /* this frame */
	uint64_t total_run, total_gpuwait, total_host;
	uint64_t skip_run, skip_gpuwait, skip_host; /* the same, from PERF_SKIP onward */
	uint64_t worst_core; long worst_frame;
} g_t;

static long g_perf_skip;

/* Per-bucket means. 32 rows is enough for a 3000-frame run at 100 frames a row and it never grows,
 * so a long run gets coarser buckets rather than an unbounded table. */
#define PERF_BUCKETS 32
static struct { uint64_t core, gpuwait, host; long frames; } g_bucket[PERF_BUCKETS];
static long g_bucket_size = 1;

/* A 64-bit FNV-1a over every frame's visible RGB, in the order the PPM writer emits them. The last
 * frame's PPM proves one frame; this proves all of them, for the price of a pass over a buffer
 * that is already hot. Both renderers feed it from the same place, so equal digests mean the two
 * paths produced identical pictures for the whole run and not merely at the end. */
static long g_frame; /* also read by the input script and by the digest window */

static uint64_t g_digest = 1469598103934665603ULL;

static void hash_frame(const uint32_t *fb, unsigned w, unsigned h, size_t pitch)
{
	unsigned x, y;
	for (y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)((const uint8_t *)fb + y * pitch);
		for (x = 0; x < w; x++) {
			const uint32_t p = row[x];
			const uint8_t rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
			unsigned i;
			for (i = 0; i < 3; i++) {
				g_digest ^= rgb[i];
				g_digest *= 1099511628211ULL;
			}
		}
	}
}

/* M2VK_HOST_DIGEST_FROM=<frame>: start hashing at that frame instead of frame 0.
 *
 * 🚨 This exists because the obvious savestate test is vacuous. "Save at 1500, load at 1500 in an
 * identical run, digests match" proves nothing — the loaded state is the state that was already
 * there, so a completely broken read_buffer that restored nothing would also pass. The test that
 * means something loads a state taken from a DIFFERENT machine history and asks whether the future
 * matches that history's future, which requires digesting only the frames after the load. See
 * devnotes/savestates.md §3 step 3. */
static long g_digest_from = 0;
static unsigned long g_digest_frames = 0;

/* M2VK_HOST_FRAME_HASH=<frame>: print a hash of EVERY frame from that frame on, one line each.
 *
 * 🚨 The whole-run digest answers "did these two runs produce the same pictures" and nothing else,
 * and for a savestate that is the wrong resolution: a load that is perfect from the tenth frame
 * onwards fails it exactly as loudly as one that never recovers. Those are different bugs. This
 * turns the answer into a sequence, so "diverges at k and reconverges at k+n" is readable — which is
 * what separates a stale display cache (the §9.3 species, transient, presentational) from a machine
 * that has genuinely taken a different path (permanent, and growing).
 *
 * Align two runs by subtracting each one's load point: frame L+k of a run that loaded at L is the
 * same emulated frame as L'+k of a run that loaded at L'. */
static long g_frame_hash_from = -1;

/* The one place a finished frame is recorded, whichever renderer produced it. */
static void note_frame(const uint32_t *fb, unsigned w, unsigned h, size_t pitch)
{
	g_fb = fb; g_w = w; g_h = h; g_pitch = pitch;
	g_frames_with_video++;
	if (g_frame_hash_from >= 0 && g_frame >= g_frame_hash_from) {
		const uint64_t save = g_digest;
		g_digest = 1469598103934665603ULL;
		hash_frame(fb, w, h, pitch);
		printf("fh %ld %016llx\n", g_frame, (unsigned long long)g_digest);
		g_digest = save;
	}
	if (g_frame < g_digest_from)
		return;
	g_digest_frames++;
	/* The digest is the harness's, not the core's, so it is charged to `host` and taken out of the
	 * headline. It is a pass over 190464 pixels with a multiply per channel and it is not free. */
	const uint64_t t0 = now_ns();
	hash_frame(fb, w, h, pitch);
	g_t.host += now_ns() - t0;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	fprintf(stderr, "[core] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* --- the Vulkan frontend ------------------------------------------------------------------- */

/* Off unless --vk was given, in which case SET_HW_RENDER is refused and the core falls back to its
 * software path — which is exactly what this host did before it could do Vulkan at all, and is why
 * the software A/B side is unaffected by any of this. */
static bool g_vk_mode;


#define VK_MAX_SLOTS 8

static struct {
	void *lib;

	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	PFN_vkGetDeviceProcAddr   get_device_proc_addr;

	PFN_vkCreateInstance      create_instance;
	PFN_vkDestroyInstance     destroy_instance;
	PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions;

	PFN_vkEnumeratePhysicalDevices      enumerate_physical_devices;
	PFN_vkGetPhysicalDeviceProperties   get_physical_device_properties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties;
	PFN_vkGetPhysicalDeviceMemoryProperties      get_memory_properties;
	PFN_vkEnumerateDeviceExtensionProperties     enumerate_device_extensions;
	PFN_vkCreateDevice                  create_device;
	PFN_vkDestroyDevice                 destroy_device;

	PFN_vkGetDeviceQueue          get_device_queue;
	PFN_vkDeviceWaitIdle          device_wait_idle;
	PFN_vkCreateCommandPool       create_command_pool;
	PFN_vkDestroyCommandPool      destroy_command_pool;
	PFN_vkAllocateCommandBuffers  allocate_command_buffers;
	PFN_vkResetCommandPool        reset_command_pool;
	PFN_vkBeginCommandBuffer      begin_command_buffer;
	PFN_vkEndCommandBuffer        end_command_buffer;
	PFN_vkCmdPipelineBarrier      cmd_pipeline_barrier;
	PFN_vkCmdCopyImageToBuffer    cmd_copy_image_to_buffer;
	PFN_vkQueueSubmit             queue_submit;
	PFN_vkCreateFence             create_fence;
	PFN_vkDestroyFence            destroy_fence;
	PFN_vkWaitForFences           wait_for_fences;
	PFN_vkResetFences             reset_fences;
	PFN_vkCreateBuffer            create_buffer;
	PFN_vkDestroyBuffer           destroy_buffer;
	PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
	PFN_vkAllocateMemory          allocate_memory;
	PFN_vkFreeMemory              free_memory;
	PFN_vkBindBufferMemory        bind_buffer_memory;
	PFN_vkMapMemory               map_memory;
	PFN_vkUnmapMemory             unmap_memory;

	VkInstance       instance;
	VkPhysicalDevice gpu;
	VkDevice         device;
	VkQueue          queue;
	uint32_t         queue_family;

	/* The queue is shared with the core, which submits its own frames on it, so every use of it on
	 * this side sits between lock_queue and unlock_queue too. Single-threaded today; the mutex is
	 * here because the contract is, not because this host races itself. */
	pthread_mutex_t queue_lock;

	/* Per sync index: what the core last handed us, and the command buffer and fence with which we
	 * read it back. Nothing is shared between slots, for the same reason the core shares nothing. */
	struct {
		struct retro_vulkan_image image;
		bool                      have_image;
		VkCommandPool             pool;
		VkCommandBuffer           cmd;
		VkFence                   fence;
		bool                      pending;
	} slot[VK_MAX_SLOTS];

	uint32_t mask;   /* what get_sync_index_mask reports */
	uint32_t slots;  /* indices 0..slots-1 */
	uint32_t index;  /* what get_sync_index reports; advanced once per retro_run */

	/* One read-back buffer, sized on first use. The read-back is synchronous — submit, wait, copy —
	 * so there is never more than one in flight and a buffer per slot would buy nothing. */
	VkBuffer       readback;
	VkDeviceMemory readback_memory;
	void          *readback_mapped;
	unsigned       readback_w, readback_h;

	/* Where a read-back frame lands. Owned by this host and outliving the device, so that the last
	 * frame's PPM can still be written after teardown. */
	uint32_t *frame;
	size_t    frame_bytes;

	retro_hw_context_reset_t reset_cb, destroy_cb;
	bool declared;   /* the core asked for Vulkan and we said yes */
	bool live;       /* a device exists and context_reset has been delivered */

	struct retro_hw_render_interface_vulkan iface;
} vk;

#define VK_CHECK(expr, what) do { \
	const VkResult vk_result_ = (expr); \
	if (vk_result_ != VK_SUCCESS) { \
		fprintf(stderr, "[host] %s failed: VkResult %d\n", (what), (int)vk_result_); \
		return false; \
	} \
} while (0)

/* Bit N set means get_sync_index() may return N. Every frontend in practice reports a contiguous
 * (1<<n)-1, and the core sizes its ring off the highest bit, so treat it that way and say so if it
 * is ever anything else. */
static uint32_t slots_for_mask(uint32_t mask)
{
	uint32_t n = 0, i;
	for (i = 0; i < 32; i++)
		if (mask & (1u << i))
			n = i + 1;
	if (n == 0 || n > VK_MAX_SLOTS) {
		fprintf(stderr, "[host] mask 0x%x is not usable; falling back to 3 slots\n", mask);
		n = 3;
	}
	return n;
}

static bool vk_load_lib(void)
{
	static const char *const CANDIDATES[] = {
		"/opt/homebrew/lib/libMoltenVK.dylib",
		"/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
		"/usr/local/lib/libMoltenVK.dylib",
		"/Applications/RetroArch.app/Contents/Frameworks/MoltenVK.framework/MoltenVK",
		"libMoltenVK.dylib",
		"libvulkan.so.1",
	};
	const char *from_env = getenv("M2VK_HOST_MOLTENVK");
	unsigned n;

	if (from_env) {
		vk.lib = dlopen(from_env, RTLD_NOW | RTLD_LOCAL);
		if (!vk.lib) {
			fprintf(stderr, "[host] M2VK_HOST_MOLTENVK=%s: %s\n", from_env, dlerror());
			return false;
		}
	} else {
		for (n = 0; n < sizeof CANDIDATES / sizeof CANDIDATES[0] && !vk.lib; n++)
			vk.lib = dlopen(CANDIDATES[n], RTLD_NOW | RTLD_LOCAL);
		if (!vk.lib) {
			fprintf(stderr, "[host] no Vulkan library found. brew install molten-vk, or set "
			                "M2VK_HOST_MOLTENVK.\n");
			return false;
		}
	}

	*(void **)&vk.get_instance_proc_addr = dlsym(vk.lib, "vkGetInstanceProcAddr");
	if (!vk.get_instance_proc_addr) {
		fprintf(stderr, "[host] the Vulkan library exports no vkGetInstanceProcAddr\n");
		return false;
	}
	return true;
}

#define GIPA(field, name) do { \
	*(PFN_vkVoidFunction *)&vk.field = vk.get_instance_proc_addr(inst, name); \
	if (!vk.field) { fprintf(stderr, "[host] missing %s\n", name); return false; } \
} while (0)

#define GDPA(field, name) do { \
	*(PFN_vkVoidFunction *)&vk.field = vk.get_device_proc_addr(vk.device, name); \
	if (!vk.field) { fprintf(stderr, "[host] missing %s\n", name); return false; } \
} while (0)

static bool vk_load_global(void)
{
	const VkInstance inst = VK_NULL_HANDLE;
	GIPA(create_instance, "vkCreateInstance");
	GIPA(enumerate_instance_extensions, "vkEnumerateInstanceExtensionProperties");
	return true;
}

static bool vk_load_instance(void)
{
	const VkInstance inst = vk.instance;
	GIPA(destroy_instance, "vkDestroyInstance");
	GIPA(get_device_proc_addr, "vkGetDeviceProcAddr");
	GIPA(enumerate_physical_devices, "vkEnumeratePhysicalDevices");
	GIPA(get_physical_device_properties, "vkGetPhysicalDeviceProperties");
	GIPA(get_queue_family_properties, "vkGetPhysicalDeviceQueueFamilyProperties");
	GIPA(get_memory_properties, "vkGetPhysicalDeviceMemoryProperties");
	GIPA(enumerate_device_extensions, "vkEnumerateDeviceExtensionProperties");
	GIPA(create_device, "vkCreateDevice");
	return true;
}

static bool vk_load_device(void)
{
	GDPA(destroy_device, "vkDestroyDevice");
	GDPA(get_device_queue, "vkGetDeviceQueue");
	GDPA(device_wait_idle, "vkDeviceWaitIdle");
	GDPA(create_command_pool, "vkCreateCommandPool");
	GDPA(destroy_command_pool, "vkDestroyCommandPool");
	GDPA(allocate_command_buffers, "vkAllocateCommandBuffers");
	GDPA(reset_command_pool, "vkResetCommandPool");
	GDPA(begin_command_buffer, "vkBeginCommandBuffer");
	GDPA(end_command_buffer, "vkEndCommandBuffer");
	GDPA(cmd_pipeline_barrier, "vkCmdPipelineBarrier");
	GDPA(cmd_copy_image_to_buffer, "vkCmdCopyImageToBuffer");
	GDPA(queue_submit, "vkQueueSubmit");
	GDPA(create_fence, "vkCreateFence");
	GDPA(destroy_fence, "vkDestroyFence");
	GDPA(wait_for_fences, "vkWaitForFences");
	GDPA(reset_fences, "vkResetFences");
	GDPA(create_buffer, "vkCreateBuffer");
	GDPA(destroy_buffer, "vkDestroyBuffer");
	GDPA(get_buffer_memory_requirements, "vkGetBufferMemoryRequirements");
	GDPA(allocate_memory, "vkAllocateMemory");
	GDPA(free_memory, "vkFreeMemory");
	GDPA(bind_buffer_memory, "vkBindBufferMemory");
	GDPA(map_memory, "vkMapMemory");
	GDPA(unmap_memory, "vkUnmapMemory");
	return true;
}

static bool vk_create_instance(void)
{
	/* 1.1, deliberately, because that is what RetroArch asks for — and MoltenVK clamps what a
	 * physical device admits to to the instance's requested version, so asking for more here would
	 * hand the core a device with a different apiVersion than the one it sees in RetroArch and
	 * quietly make the two hosts non-comparable. */
	const VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "retrohost",
		.applicationVersion = 1,
		.pEngineName = "retrohost",
		.engineVersion = 1,
		.apiVersion = VK_API_VERSION_1_1,
	};
	const char *enabled[2];
	uint32_t enabled_count = 0;
	VkInstanceCreateFlags flags = 0;
	VkExtensionProperties *exts = NULL;
	uint32_t count = 0, i;

	/* Portability enumeration is the loader's rule, not MoltenVK's, so going straight at MoltenVK
	 * usually does not need it — but asking for it when it is advertised costs nothing and keeps
	 * this working if the dlopened library is ever a real loader instead. */
	if (vk.enumerate_instance_extensions(NULL, &count, NULL) == VK_SUCCESS && count) {
		exts = calloc(count, sizeof *exts);
		if (exts && vk.enumerate_instance_extensions(NULL, &count, exts) == VK_SUCCESS) {
			for (i = 0; i < count; i++) {
				if (!strcmp(exts[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
					enabled[enabled_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
					flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
				}
			}
		}
		free(exts);
	}

	const VkInstanceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.flags = flags,
		.pApplicationInfo = &app,
		.enabledExtensionCount = enabled_count,
		.ppEnabledExtensionNames = enabled_count ? enabled : NULL,
	};
	VK_CHECK(vk.create_instance(&info, NULL, &vk.instance), "vkCreateInstance");
	return vk_load_instance();
}

static bool vk_create_device(void)
{
	VkPhysicalDevice gpus[8];
	uint32_t count = sizeof gpus / sizeof gpus[0], i;
	VkQueueFamilyProperties families[16];
	uint32_t family_count = sizeof families / sizeof families[0];
	VkExtensionProperties *exts = NULL;
	uint32_t ext_count = 0;
	const char *enabled[2];
	uint32_t enabled_count = 0;
	VkPhysicalDeviceProperties props;

	const VkResult enumerated = vk.enumerate_physical_devices(vk.instance, &count, gpus);
	if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || count == 0) {
		fprintf(stderr, "[host] no physical device\n");
		return false;
	}
	vk.gpu = gpus[0];

	vk.get_queue_family_properties(vk.gpu, &family_count, families);
	vk.queue_family = UINT32_MAX;
	for (i = 0; i < family_count; i++)
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.queue_family = i; break; }
	if (vk.queue_family == UINT32_MAX) {
		fprintf(stderr, "[host] no graphics queue family\n");
		return false;
	}

	/* VK_KHR_portability_subset is not optional: the spec requires it to be enabled on any device
	 * that advertises it, and every MoltenVK device does. */
	if (vk.enumerate_device_extensions(vk.gpu, NULL, &ext_count, NULL) == VK_SUCCESS && ext_count) {
		exts = calloc(ext_count, sizeof *exts);
		if (exts && vk.enumerate_device_extensions(vk.gpu, NULL, &ext_count, exts) == VK_SUCCESS) {
			for (i = 0; i < ext_count; i++)
				if (!strcmp(exts[i].extensionName, "VK_KHR_portability_subset"))
					enabled[enabled_count++] = "VK_KHR_portability_subset";
		}
		free(exts);
	}

	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = vk.queue_family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	/* No features enabled at all. The passthrough needs none, and a feature turned on here that
	 * RetroArch does not turn on would be a difference between the two hosts that only shows up
	 * when some later phase starts depending on it. */
	const VkDeviceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = enabled_count,
		.ppEnabledExtensionNames = enabled_count ? enabled : NULL,
	};
	VK_CHECK(vk.create_device(vk.gpu, &info, NULL, &vk.device), "vkCreateDevice");
	if (!vk_load_device())
		return false;
	vk.get_device_queue(vk.device, vk.queue_family, 0, &vk.queue);

	vk.get_physical_device_properties(vk.gpu, &props);
	printf("vk host: '%s' api %u.%u.%u driver 0x%08x, queue family %u, %u slots (mask 0x%x)\n",
	       props.deviceName,
	       VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	       VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion,
	       vk.queue_family, vk.slots, vk.mask);
	return true;
}

/* All VK_MAX_SLOTS of them, not vk.slots: the mask can change mid-run (M2VK_HOST_MASK_AT), and a
 * slot's pool and fence belong to this host rather than to any particular mask. Building only as
 * many as the current mask needs is a segfault waiting for the first mask that grows — which is
 * exactly what the first M2VK_HOST_MASK_AT run did, at the 0x1 -> 0xf step. */
static bool vk_create_slots(void)
{
	uint32_t i;
	for (i = 0; i < VK_MAX_SLOTS; i++) {
		const VkCommandPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = vk.queue_family,
		};
		VK_CHECK(vk.create_command_pool(vk.device, &pool_info, NULL, &vk.slot[i].pool),
		         "vkCreateCommandPool");

		const VkCommandBufferAllocateInfo cmd_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk.slot[i].pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VK_CHECK(vk.allocate_command_buffers(vk.device, &cmd_info, &vk.slot[i].cmd),
		         "vkAllocateCommandBuffers");

		const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VK_CHECK(vk.create_fence(vk.device, &fence_info, NULL, &vk.slot[i].fence), "vkCreateFence");
	}
	return true;
}

static bool vk_create_readback(unsigned w, unsigned h)
{
	VkMemoryRequirements reqs;
	VkPhysicalDeviceMemoryProperties mem;
	uint32_t type = UINT32_MAX, i;
	const VkDeviceSize size = (VkDeviceSize)w * h * 4;

	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VK_CHECK(vk.create_buffer(vk.device, &info, NULL, &vk.readback), "vkCreateBuffer");

	vk.get_buffer_memory_requirements(vk.device, vk.readback, &reqs);
	vk.get_memory_properties(vk.gpu, &mem);
	for (i = 0; i < mem.memoryTypeCount; i++) {
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if ((reqs.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		fprintf(stderr, "[host] no host-visible coherent memory type\n");
		return false;
	}

	const VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	VK_CHECK(vk.allocate_memory(vk.device, &alloc, NULL, &vk.readback_memory), "vkAllocateMemory");
	VK_CHECK(vk.bind_buffer_memory(vk.device, vk.readback, vk.readback_memory, 0), "vkBindBufferMemory");
	VK_CHECK(vk.map_memory(vk.device, vk.readback_memory, 0, VK_WHOLE_SIZE, 0, &vk.readback_mapped),
	         "vkMapMemory");

	if (vk.frame_bytes < (size_t)size) {
		free(vk.frame);
		vk.frame = malloc((size_t)size);
		if (!vk.frame) { fprintf(stderr, "[host] out of memory\n"); return false; }
		vk.frame_bytes = (size_t)size;
	}

	vk.readback_w = w;
	vk.readback_h = h;
	return true;
}

static void vk_destroy_readback(void)
{
	if (vk.readback_mapped) vk.unmap_memory(vk.device, vk.readback_memory);
	if (vk.readback)        vk.destroy_buffer(vk.device, vk.readback, NULL);
	if (vk.readback_memory) vk.free_memory(vk.device, vk.readback_memory, NULL);
	vk.readback_mapped = NULL;
	vk.readback = VK_NULL_HANDLE;
	vk.readback_memory = VK_NULL_HANDLE;
	vk.readback_w = vk.readback_h = 0;
}

/* Tears the device down the way a frontend does when its video driver is reinitialised: everything
 * this host owns goes, and the core is expected to have already dropped its ring in context_destroy
 * (or, if M2VK_HOST_SKIP_DESTROY says otherwise, to abandon it at the next reset). */
static void vk_destroy_device(void)
{
	uint32_t i;
	if (!vk.device)
		return;

	pthread_mutex_lock(&vk.queue_lock);
	vk.device_wait_idle(vk.device);
	pthread_mutex_unlock(&vk.queue_lock);

	vk_destroy_readback();
	for (i = 0; i < VK_MAX_SLOTS; i++) {
		if (vk.slot[i].fence) vk.destroy_fence(vk.device, vk.slot[i].fence, NULL);
		if (vk.slot[i].pool)  vk.destroy_command_pool(vk.device, vk.slot[i].pool, NULL);
		memset(&vk.slot[i], 0, sizeof vk.slot[i]);
	}
	vk.destroy_device(vk.device, NULL);
	vk.device = VK_NULL_HANDLE;
	vk.queue = VK_NULL_HANDLE;
}

static void vk_destroy_all(void)
{
	vk_destroy_device();
	if (vk.instance) {
		vk.destroy_instance(vk.instance, NULL);
		vk.instance = VK_NULL_HANDLE;
	}
	vk.live = false;
}

/* --- the interface the core is handed ------------------------------------------------------- */

static void vkif_set_image(void *handle, const struct retro_vulkan_image *image,
                           uint32_t num_semaphores, const VkSemaphore *semaphores,
                           uint32_t src_queue_family)
{
	(void)handle; (void)semaphores; (void)src_queue_family;
	if (num_semaphores) {
		/* The core is documented to use a layout transition instead, and does. If that ever changes
		 * this host has to wait on them before reading the image, so fail loudly rather than
		 * silently read a frame that is not finished. */
		fprintf(stderr, "[host] set_image passed %u semaphores; this host waits on none of them\n",
		        num_semaphores);
		exit(1);
	}
	vk.slot[vk.index].image = *image;
	vk.slot[vk.index].have_image = true;
}

static uint32_t vkif_get_sync_index(void *handle)      { (void)handle; return vk.index; }
static uint32_t vkif_get_sync_index_mask(void *handle) { (void)handle; return vk.mask; }

static void vkif_wait_sync_index(void *handle)
{
	/* "The frontend has finished with this slot." Our read-back is synchronous, so the fence is
	 * normally already clear by the time the core asks — but the wait is what the contract says
	 * happens here, and it is what would keep this host honest if the read-back ever went async. */
	(void)handle;
	if (vk.slot[vk.index].pending) {
		vk.wait_for_fences(vk.device, 1, &vk.slot[vk.index].fence, VK_TRUE, UINT64_MAX);
		vk.reset_fences(vk.device, 1, &vk.slot[vk.index].fence);
		vk.slot[vk.index].pending = false;
	}
}

static void vkif_set_command_buffers(void *handle, uint32_t num_cmd, const VkCommandBuffer *cmd)
{
	/* The alternative to the core submitting for itself. It submits, so this is never called; if it
	 * ever is, silently dropping the work would look like a rendering bug rather than a missing
	 * feature. */
	(void)handle; (void)num_cmd; (void)cmd;
	fprintf(stderr, "[host] set_command_buffers is not implemented by this host\n");
	exit(1);
}

static void vkif_lock_queue(void *handle)   { (void)handle; pthread_mutex_lock(&vk.queue_lock); }
static void vkif_unlock_queue(void *handle) { (void)handle; pthread_mutex_unlock(&vk.queue_lock); }

static void vkif_set_signal_semaphore(void *handle, VkSemaphore semaphore)
{
	/* v5. Nothing here presents, so there is nothing for the core to signal to. */
	(void)handle; (void)semaphore;
}

static void vk_fill_interface(void)
{
	memset(&vk.iface, 0, sizeof vk.iface);
	vk.iface.interface_type    = RETRO_HW_RENDER_INTERFACE_VULKAN;
	vk.iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	vk.iface.handle            = &vk;
	vk.iface.instance          = vk.instance;
	vk.iface.gpu               = vk.gpu;
	vk.iface.device            = vk.device;
	vk.iface.get_device_proc_addr   = vk.get_device_proc_addr;
	vk.iface.get_instance_proc_addr = vk.get_instance_proc_addr;
	vk.iface.queue             = vk.queue;
	vk.iface.queue_index       = vk.queue_family;
	vk.iface.set_image           = vkif_set_image;
	vk.iface.get_sync_index      = vkif_get_sync_index;
	vk.iface.get_sync_index_mask = vkif_get_sync_index_mask;
	vk.iface.set_command_buffers = vkif_set_command_buffers;
	vk.iface.wait_sync_index     = vkif_wait_sync_index;
	vk.iface.lock_queue          = vkif_lock_queue;
	vk.iface.unlock_queue        = vkif_unlock_queue;
	vk.iface.set_signal_semaphore = vkif_set_signal_semaphore;
}

/* Everything from the instance down. Called once at startup and again for every forced context
 * loss, which is the whole point of it being a function. */
static bool vk_bring_up(void)
{
	if (!vk.instance && !vk_create_instance())
		return false;
	if (!vk_create_device())
		return false;
	if (!vk_create_slots())
		return false;
	vk_fill_interface();
	vk.index = vk.slots - 1; /* so the first advance lands on 0 */
	vk.live = true;
	return true;
}

/* Reads the image the core just handed over, straight off the GPU, into the same 0xAARRGGBB layout
 * MAME's software frame already has — B8G8R8A8_UNORM is B,G,R,A in memory, which little-endian
 * reads back as exactly that word. So there is no conversion anywhere in this path, and the PPM
 * this produces is comparable to the software one byte for byte or not at all. */
static bool vk_read_back(unsigned w, unsigned h)
{
	const uint32_t i = vk.index;
	const struct retro_vulkan_image *const img = &vk.slot[i].image;

	if (!vk.slot[i].have_image) {
		fprintf(stderr, "[host] frame %ld reported a valid image but set_image was never called\n", g_frame);
		return false;
	}

	if (vk.readback_w != w || vk.readback_h != h) {
		vk_destroy_readback();
		if (!vk_create_readback(w, h))
			return false;
	}

	VK_CHECK(vk.reset_command_pool(vk.device, vk.slot[i].pool, 0), "vkResetCommandPool");

	const VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vk.begin_command_buffer(vk.slot[i].cmd, &begin), "vkBeginCommandBuffer");

	/* The core submitted this frame on the same queue and ended it with a layout transition rather
	 * than a semaphore. A barrier's scopes reach back through submission order on one queue, so
	 * ALL_COMMANDS here picks up that submit without a semaphore of our own — and srcQueueFamily is
	 * IGNORED because the core told set_image there was no ownership transfer to make. */
	VkImageMemoryBarrier to_src = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = img->image_layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img->create_info.image,
		.subresourceRange = img->create_info.subresourceRange,
	};
	vk.cmd_pipeline_barrier(vk.slot[i].cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

	const VkBufferImageCopy region = {
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.imageExtent = { w, h, 1 },
	};
	vk.cmd_copy_image_to_buffer(vk.slot[i].cmd, img->create_info.image,
	                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback, 1, &region);

	/* Put it back the way the core left it. The core happens to transition from UNDEFINED every
	 * frame and so would not notice, but a frontend that hands an image back in a layout other than
	 * the one it was given is a bug waiting for the first core that does notice. */
	VkImageMemoryBarrier restore = to_src;
	restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	restore.newLayout = img->image_layout;
	vk.cmd_pipeline_barrier(vk.slot[i].cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &restore);

	VK_CHECK(vk.end_command_buffer(vk.slot[i].cmd), "vkEndCommandBuffer");

	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &vk.slot[i].cmd,
	};
	pthread_mutex_lock(&vk.queue_lock);
	const VkResult submitted = vk.queue_submit(vk.queue, 1, &submit, vk.slot[i].fence);
	pthread_mutex_unlock(&vk.queue_lock);
	VK_CHECK(submitted, "vkQueueSubmit (read-back)");
	vk.slot[i].pending = true;

	/* This wait, and only this wait, is charged to the GPU. Everything either side of it is the
	 * harness copying a frame nobody would otherwise copy — see the perf-timer comment above. */
	const uint64_t t_wait = now_ns();
	VK_CHECK(vk.wait_for_fences(vk.device, 1, &vk.slot[i].fence, VK_TRUE, UINT64_MAX),
	         "vkWaitForFences (read-back)");
	g_t.gpuwait += now_ns() - t_wait;
	VK_CHECK(vk.reset_fences(vk.device, 1, &vk.slot[i].fence), "vkResetFences (read-back)");
	vk.slot[i].pending = false;

	memcpy(vk.frame, vk.readback_mapped, (size_t)w * h * 4);
	return true;
}

/* --- the libretro callbacks ------------------------------------------------------------------ */

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		g_shutdown = true;
		return true;
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
		const struct retro_core_options_v2 *o = data;
		unsigned n;
		for (n = 0; o->definitions[n].key && g_option_count < 16; n++) {
			g_options[g_option_count].key = o->definitions[n].key;
			g_options[g_option_count].value = o->definitions[n].default_value;
			g_option_count++;
			printf("option: %s = %s (default)\n", o->definitions[n].key,
			       o->definitions[n].default_value);
		}
		return true;
	}
	/* The frontend's remap labels, printed under M2VK_HOST_DESCRIPTORS=1.
	 *
	 * This exists because there was no read-out at all for them: the core sends the array and
	 * RetroArch shows it in Quick Menu -> Controls, which is an interactive screen and cannot be
	 * checked from a shell. Per-game labels are the user-facing half of the layout work, so "the
	 * labels are right" needs to be a command and not a screenshot — the same argument that built
	 * M2VK_GUN_LOG when MAME's crosshair turned out to be undrawable here.
	 *
	 * Handled unconditionally (returning true either way, as a frontend that does not care would),
	 * so a run without the variable set is unchanged. */
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
		const struct retro_input_descriptor *d = data;
		unsigned n = 0;
		if (getenv("M2VK_HOST_DESCRIPTORS") == NULL)
			return true;
		for (; d && d->description; d++, n++)
			printf("descriptor: port=%u device=%u index=%u id=%-2u  %s\n",
			       d->port, d->device, d->index, d->id, d->description);
		printf("descriptor: %u entr%s\n", n, n == 1 ? "y" : "ies");
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		v->value = option_value(v->key);
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		/* Raised by M2VK_HOST_OPT_AT and cleared as it is read, which is what the environment call
		 * is specified to do -- a flag that stayed raised would make the core re-apply every frame. */
		*(bool *)data = g_opt_updated;
		g_opt_updated = false;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY: {
		/* The core changing the size of its picture inside the max it already declared -- which is
		 * what the internal-resolution option does when the player moves it mid-run. Nothing here has
		 * to be resized: the read-back buffer is keyed on the size each frame arrives with, and the
		 * PPM writer and the digest both take w/h per frame. Recorded and printed so a harness run
		 * cannot silently measure a resolution nobody asked for. */
		const struct retro_game_geometry *g = data;
		printf("geometry: %ux%u aspect %.4f (SET_GEOMETRY)\n",
		       g->base_width, g->base_height, g->aspect_ratio);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
		/* The same, but the core needs a bigger max than it declared at load. A real frontend may
		 * reinitialise its video driver here, which for this core means a context_destroy/reset pair;
		 * this host does not, deliberately -- M2VK_HOST_RESET_AT is how that path gets exercised, and
		 * conflating the two would make a resolution change untestable without one. */
		const struct retro_system_av_info *av = data;
		printf("av: %ux%u (max %ux%u) aspect %.4f  fps %.4f  rate %.0f (SET_SYSTEM_AV_INFO)\n",
		       av->geometry.base_width, av->geometry.base_height,
		       av->geometry.max_width, av->geometry.max_height,
		       av->geometry.aspect_ratio, av->timing.fps, av->timing.sample_rate);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = getenv("M2_SYSTEM_DIR");
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		/* Defaults to a directory of its own so a test run does not scatter NVRAM and cfg through
		 * the working directory — and so it can be deleted between fixture runs, which credits
		 * being kept in battery RAM makes necessary. */
		*(const char **)data = getenv("M2_SAVE_DIR") ? getenv("M2_SAVE_DIR") : "retrohost-save";
		return true;

	case RETRO_ENVIRONMENT_SET_HW_RENDER: {
		struct retro_hw_render_callback *hw = data;
		/* Without --vk this is refused, which is what every earlier version of this host did by
		 * having no case for it at all — the core then logs the fallback and runs its software path
		 * exactly as before. That equivalence is the reason the software A/B side is untouched. */
		if (!g_vk_mode)
			return false;
		if (hw->context_type != RETRO_HW_CONTEXT_VULKAN) {
			fprintf(stderr, "[host] the core asked for context type %u, not Vulkan\n",
			        (unsigned)hw->context_type);
			return false;
		}
		vk.reset_cb = hw->context_reset;
		vk.destroy_cb = hw->context_destroy;
		vk.declared = true;
		printf("hw render: vulkan %u.%u requested, cache_context %s\n",
		       hw->version_major, hw->version_minor, hw->cache_context ? "yes" : "no");
		return true;
	}
	case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
		if (!vk.live)
			return false;
		*(const struct retro_hw_render_interface **)data =
			(const struct retro_hw_render_interface *)&vk.iface;
		return true;

	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data)
		return; /* duped frame */

	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		/* The core has rendered into the ring slot this frame owns and told us so. Reading it here
		 * rather than at the end of the run is deliberate: it is when a real frontend would sample
		 * the image, it keeps the digest honest over every frame, and it means the last frame's PPM
		 * is written from a read-back that has already been proven to work 3000 times. */
		/* Charge the read-back to `host` except for its fence wait, which vk_read_back has already
		 * added to `gpuwait` — so subtract that back out rather than double-counting it. */
		const uint64_t t0 = now_ns(), w0 = g_t.gpuwait;
		const bool ok = vk_read_back(width, height);
		g_t.host += (now_ns() - t0) - (g_t.gpuwait - w0);
		if (ok)
			note_frame(vk.frame, width, height, (size_t)width * 4);
		return;
	}

	note_frame(data, width, height, pitch);
}

static void audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; g_audio_samples++; }
static size_t audio_batch_cb(const int16_t *data, size_t frames) { (void)data; g_audio_samples += frames; return frames; }

/* --- the button script ------------------------------------------------------------------- */

/* Control names accepted in a script. The first sixteen are the RetroPad digital buttons, in
 * RETRO_DEVICE_ID_JOYPAD_* order; then the half-axes, which report a full deflection of the named
 * analogue stick for as long as they are "held"; then the lightgun.
 *
 * The gun is the one control that carries a value, because an absolute pointer is the one thing a
 * name cannot express: "gun=<x>/<y>" aims at normalised screen coordinates, 0.0..1.0 with the origin
 * top left, and is reported as RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X/Y. So a sweep is a series of them:
 *
 *   1200:gun=0.0/0.5:20,1220:gun=0.1/0.5:20,...
 *
 * Note that both axes are always reported together, which is what a real gun does — there is no way
 * to aim in X only. A press whose frame range has elapsed stops reporting entirely, and the core
 * then sees 0, i.e. the centre of the screen rather than the last aim. */
static const struct control {
	const char *name;
	unsigned dev, index, id;
	int16_t value;
} CONTROLS[] = {
	{ "b",      RETRO_DEVICE_JOYPAD, 0, 0,  1 }, { "y",     RETRO_DEVICE_JOYPAD, 0, 1,  1 },
	{ "select", RETRO_DEVICE_JOYPAD, 0, 2,  1 }, { "start", RETRO_DEVICE_JOYPAD, 0, 3,  1 },
	{ "up",     RETRO_DEVICE_JOYPAD, 0, 4,  1 }, { "down",  RETRO_DEVICE_JOYPAD, 0, 5,  1 },
	{ "left",   RETRO_DEVICE_JOYPAD, 0, 6,  1 }, { "right", RETRO_DEVICE_JOYPAD, 0, 7,  1 },
	{ "a",      RETRO_DEVICE_JOYPAD, 0, 8,  1 }, { "x",     RETRO_DEVICE_JOYPAD, 0, 9,  1 },
	{ "l",      RETRO_DEVICE_JOYPAD, 0, 10, 1 }, { "r",     RETRO_DEVICE_JOYPAD, 0, 11, 1 },
	{ "l2",     RETRO_DEVICE_JOYPAD, 0, 12, 1 }, { "r2",    RETRO_DEVICE_JOYPAD, 0, 13, 1 },
	{ "l3",     RETRO_DEVICE_JOYPAD, 0, 14, 1 }, { "r3",    RETRO_DEVICE_JOYPAD, 0, 15, 1 },
	{ "lx-", RETRO_DEVICE_ANALOG, 0, 0, -32767 }, { "lx+", RETRO_DEVICE_ANALOG, 0, 0, 32767 },
	{ "ly-", RETRO_DEVICE_ANALOG, 0, 1, -32767 }, { "ly+", RETRO_DEVICE_ANALOG, 0, 1, 32767 },
	{ "rx-", RETRO_DEVICE_ANALOG, 1, 0, -32767 }, { "rx+", RETRO_DEVICE_ANALOG, 1, 0, 32767 },
	{ "ry-", RETRO_DEVICE_ANALOG, 1, 1, -32767 }, { "ry+", RETRO_DEVICE_ANALOG, 1, 1, 32767 },
	/* The gun. "gun" takes a value and reports both screen axes; the other three are ordinary
	 * buttons on the gun device, which is a different device number from the pad's and so does not
	 * collide with anything above. */
	{ "gun",       RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X,     0 },
	{ "trigger",   RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER,      1 },
	{ "reload",    RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD,       1 },
	{ "offscreen", RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN, 1 },
};
#define CONTROL_COUNT (sizeof CONTROLS / sizeof CONTROLS[0])
#define CONTROL_IS_GUN_AIM(c) (CONTROLS[c].dev == RETRO_DEVICE_LIGHTGUN && \
                               CONTROLS[c].id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X)

/* x/y carry the gun aim; v is what every other control reports, which is the control table's
 * constant unless the script scaled it (see parse_script). */
struct press { long frame, until; unsigned port, control; int16_t x, y, v; };
static struct press g_presses[64];
static unsigned g_press_count;

/* one bit per port given --gun */
static uint32_t g_gun_ports;

/* one bit per port given --modern: RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1), the core's
 * second pad layout. A port in neither set is left alone, i.e. it is the frontend default, which
 * for this core is the first layout. */
static uint32_t g_modern_ports;

/* one bit per port given --cabinet: plain RETRO_DEVICE_JOYPAD, which for this core means "the best
 * layout this set has" — its cabinet row where it has one, the generic Classic layout otherwise.
 * That is also what a port with no call at all gets, so this switch changes no behaviour; it exists
 * because it is the call RetroArch makes when a player picks the first Controls entry, and a harness
 * that can only reach a state by NOT acting cannot tell "selected it" from "forgot to". */
static uint32_t g_cabinet_ports;

/* one bit per port given --classic: RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2), the generic
 * layout as an explicit opt-out. On a set with a cabinet row this is the ONLY way to reach the old
 * behaviour, which makes it the negative control for every per-game layout check — see
 * devnotes/per-game-input.md §5.3. On a set without one it is the same thing the default already is. */
static uint32_t g_classic_ports;

static void input_poll_cb(void) { }

static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{
	unsigned n;
	for (n = 0; n < g_press_count; n++) {
		const struct control *c = &CONTROLS[g_presses[n].control];
		if (g_presses[n].port != p || c->dev != d ||
		    g_frame < g_presses[n].frame || g_frame >= g_presses[n].until)
			continue;
		if (CONTROL_IS_GUN_AIM(g_presses[n].control)) {
			if (id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X) return g_presses[n].x;
			if (id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) return g_presses[n].y;
			continue;
		}
		if (c->id == id && (d != RETRO_DEVICE_ANALOG || c->index == i))
			return g_presses[n].v;
	}
	return 0;
}

/* 0.0..1.0 across the screen -> the full SCREEN_X/Y range, which the libretro header defines as
 * -0x8000..0x7fff. No clamping to a smaller window anywhere: PORT_MINMAX is the cabinet's own
 * calibration and doing any of it here is exactly the mistake devnotes/lightgun.md §5 warns about. */
static int16_t gun_coord(double v)
{
	long scaled = lround(v * 65535.0) - 32768;
	if (scaled < -32768) scaled = -32768;
	if (scaled >  32767) scaled =  32767;
	return (int16_t)scaled;
}

/* frame:control[:frames-held][:port], comma separated */
static int parse_script(char *s)
{
	char *item, *save = NULL;
	for (item = strtok_r(s, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
		char *fields[4] = { NULL, NULL, NULL, NULL };
		char *fsave = NULL, *f = strtok_r(item, ":", &fsave);
		unsigned nf = 0, c;
		char *payload;
		for (; f && nf < 4; f = strtok_r(NULL, ":", &fsave))
			fields[nf++] = f;
		if (nf < 2 || g_press_count >= 64) {
			fprintf(stderr, "bad script item\n");
			return -1;
		}
		/* A control may carry "=<payload>": coordinates for the gun aim, and a deflection fraction
		 * for a half-axis, which is what makes a *sweep* expressible rather than three points. A
		 * digital button has nothing to scale and says so rather than ignoring it. */
		payload = strchr(fields[1], '=');
		if (payload)
			*payload++ = '\0';
		for (c = 0; c < CONTROL_COUNT; c++)
			if (!strcmp(fields[1], CONTROLS[c].name))
				break;
		if (c == CONTROL_COUNT) {
			fprintf(stderr, "unknown control '%s'\n", fields[1]);
			return -1;
		}
		g_presses[g_press_count].x = g_presses[g_press_count].y = 0;
		g_presses[g_press_count].v = CONTROLS[c].value;
		if (CONTROL_IS_GUN_AIM(c)) {
			char *slash = payload ? strchr(payload, '/') : NULL;
			if (!slash) {
				fprintf(stderr, "gun wants coordinates: gun=<x>/<y>, 0.0..1.0 each\n");
				return -1;
			}
			*slash = '\0';
			g_presses[g_press_count].x = gun_coord(atof(payload));
			g_presses[g_press_count].y = gun_coord(atof(slash + 1));
		} else if (payload && CONTROLS[c].dev == RETRO_DEVICE_ANALOG) {
			g_presses[g_press_count].v = (int16_t)lround(atof(payload) * CONTROLS[c].value);
		} else if (payload) {
			fprintf(stderr, "control '%s' takes no value\n", fields[1]);
			return -1;
		}
		g_presses[g_press_count].frame = strtol(fields[0], NULL, 10);
		g_presses[g_press_count].until = g_presses[g_press_count].frame +
			(fields[2] ? strtol(fields[2], NULL, 10) : 10);
		g_presses[g_press_count].port = fields[3] ? (unsigned)strtoul(fields[3], NULL, 10) : 0;
		g_presses[g_press_count].control = c;
		if (CONTROL_IS_GUN_AIM(c))
			printf("script: port %u gun %d,%d frames %ld..%ld\n", g_presses[g_press_count].port,
			       g_presses[g_press_count].x, g_presses[g_press_count].y,
			       g_presses[g_press_count].frame, g_presses[g_press_count].until - 1);
		else
			printf("script: port %u %s=%d frames %ld..%ld\n", g_presses[g_press_count].port,
			       CONTROLS[c].name, g_presses[g_press_count].v, g_presses[g_press_count].frame,
			       g_presses[g_press_count].until - 1);
		g_press_count++;
	}
	return 0;
}

/* --- the two scripted context events ---------------------------------------------------------- */

/* Both exist because RetroArch cannot produce them. Its mask is 0x7 in every context of every run,
 * so the core's ring-rebuild-on-mask-change has never executed against a real change; and a
 * fullscreen toggle is the only context loss it offers, which is slow and needs a window. */
static struct { long frame; uint32_t mask; } g_mask_changes[8];
static unsigned g_mask_change_count;
static long g_resets[8];
static unsigned g_reset_count;

static void parse_mask_changes(char *s)
{
	char *item, *save = NULL;
	for (item = strtok_r(s, ",", &save); item && g_mask_change_count < 8; item = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(item, ':');
		if (!colon) continue;
		*colon = '\0';
		g_mask_changes[g_mask_change_count].frame = strtol(item, NULL, 10);
		g_mask_changes[g_mask_change_count].mask = (uint32_t)strtoul(colon + 1, NULL, 0);
		printf("vk host: mask becomes 0x%x at frame %ld\n",
		       g_mask_changes[g_mask_change_count].mask, g_mask_changes[g_mask_change_count].frame);
		g_mask_change_count++;
	}
}

static void parse_resets(char *s)
{
	char *item, *save = NULL;
	for (item = strtok_r(s, ",", &save); item && g_reset_count < 8; item = strtok_r(NULL, ",", &save)) {
		g_resets[g_reset_count] = strtol(item, NULL, 10);
		printf("vk host: context loss at frame %ld\n", g_resets[g_reset_count]);
		g_reset_count++;
	}
}

/* A core option changing WHILE CONTENT RUNS -- what a player does from the options menu, and the one
 * frontend behaviour this harness could not produce.  It is why "the option is read at load" went
 * unnoticed as a usability bug: every check here set the option before the run and so could never
 * tell "applied at load" apart from "applied live".
 *
 * M2VK_HOST_OPT_AT=<frame>:<key>=<value>[,...].  The value replaces g_options[]'s entry and the
 * update flag is raised once, exactly as RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE is specified.
 *
 * NB the matching M2OPT_<key> environment variable, if set, still wins in option_value() -- so a
 * scripted change must not also be pinned from the environment. */
static void parse_opt_changes(char *s)
{
	char *item, *save = NULL;
	for (item = strtok_r(s, ",", &save); item && g_opt_change_count < 8; item = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(item, ':');
		char *eq = colon ? strchr(colon + 1, '=') : NULL;
		if (!colon || !eq) continue;
		*colon = '\0';
		*eq = '\0';
		g_opt_changes[g_opt_change_count].frame = strtol(item, NULL, 10);
		g_opt_changes[g_opt_change_count].key = colon + 1;
		g_opt_changes[g_opt_change_count].value = eq + 1;
		printf("option: %s becomes %s at frame %ld\n",
		       g_opt_changes[g_opt_change_count].key, g_opt_changes[g_opt_change_count].value,
		       g_opt_changes[g_opt_change_count].frame);
		g_opt_change_count++;
	}
}

static void apply_opt_changes(long frame)
{
	unsigned i, n;
	for (i = 0; i < g_opt_change_count; i++) {
		if (g_opt_changes[i].frame != frame) continue;
		for (n = 0; n < g_option_count; n++) {
			if (strcmp(g_options[n].key, g_opt_changes[i].key)) continue;
			g_options[n].value = g_opt_changes[i].value;
			g_opt_updated = true;
			printf("option: %s = %s (changed at frame %ld)\n",
			       g_opt_changes[i].key, g_opt_changes[i].value, frame);
		}
	}
}

/*==========================================================================================
 * Savestates.  devnotes/savestates.md §3 step 3 is what these are for.
 *
 *   M2VK_HOST_SAVE_AT=<frame>:<path>      retro_serialize at the END of that frame, to a file
 *   M2VK_HOST_SAVE_AT2=<frame>:<path>     a SECOND save point in the same run — so the load-state (N)
 *                                         and the reference future (N+1) come from ONE history, which a
 *                                         contamination-free carrier diff needs (savestates.md §10.1)
 *   M2VK_HOST_LOAD_AT=<frame>:<path>      retro_unserialize at the END of that frame, from a file
 *   M2VK_HOST_ROUNDTRIP_AT=<frame>        serialize, then immediately unserialize the same bytes
 *   M2VK_HOST_DIGEST_FROM=<frame>         hash only from that frame on (see note at g_digest_from)
 *
 * 🚨 END of the frame, not the start, and both events use the same boundary — otherwise a state
 * saved at N and loaded at N describe different instants and the comparison is off by a frame in a
 * way that looks exactly like an incomplete registry.
 *
 * ROUNDTRIP is the weaker guard and it is deliberately kept: it cannot detect a missing save_item,
 * but it is the only thing that detects a save that PERTURBS the machine — dispatch_presave() and
 * dispatch_postload() run real device callbacks, and a run that saves must be identical to one that
 * does not.
 *==========================================================================================*/
static long  g_save_at = -1, g_load_at = -1, g_roundtrip_at = -1;
static const char *g_save_path, *g_load_path;
/* A SECOND save point in the same run. This is what makes a carrier diff clean: the load-state (frame
 * N) and the reference future (frame N+1) must come from ONE history, or an uninitialised-but-saved
 * member (device_rom_interface::m_bank_count is one) differs run-to-run and masquerades as the carrier.
 * Same trap savestates.md §9.1c fixed for the digest; the state diff needs it too. */
static long  g_save_at2 = -1;
static const char *g_save_path2;
static size_t (*g_serialize_size)(void);
static bool   (*g_serialize)(void *, size_t);
static bool   (*g_unserialize)(const void *, size_t);

/* "<frame>:<path>" */
static long parse_state_at(const char *env, const char **path_out)
{
	const char *v = getenv(env);
	const char *colon;
	if (!v) return -1;
	colon = strchr(v, ':');
	if (!colon || !colon[1]) {
		fprintf(stderr, "%s wants <frame>:<path>\n", env);
		return -1;
	}
	*path_out = colon + 1;
	return strtol(v, NULL, 10);
}

static void do_state_events(long frame)
{
	size_t size;
	if (frame != g_save_at && frame != g_save_at2 && frame != g_load_at && frame != g_roundtrip_at)
		return;
	if (!g_serialize_size || (size = g_serialize_size()) == 0) {
		fprintf(stderr, "state: core reports no savestate support (size 0)\n");
		return;
	}

	if (frame == g_save_at2) {
		void *buf = malloc(size);
		if (buf && g_serialize(buf, size)) {
			FILE *f = fopen(g_save_path2, "wb");
			if (!f || fwrite(buf, 1, size, f) != size)
				fprintf(stderr, "state: could not write %s\n", g_save_path2);
			else
				printf("state: saved %zu bytes at frame %ld -> %s\n", size, frame, g_save_path2);
			if (f) fclose(f);
		} else {
			fprintf(stderr, "state: retro_serialize (2) failed at frame %ld\n", frame);
		}
		free(buf);
	}

	if (frame == g_save_at || frame == g_roundtrip_at) {
		void *buf = malloc(size);
		if (!buf) { fprintf(stderr, "state: out of memory for %zu bytes\n", size); return; }
		if (!g_serialize(buf, size)) {
			fprintf(stderr, "state: retro_serialize failed at frame %ld\n", frame);
			free(buf);
			return;
		}
		if (frame == g_save_at) {
			FILE *f = fopen(g_save_path, "wb");
			if (!f || fwrite(buf, 1, size, f) != size)
				fprintf(stderr, "state: could not write %s\n", g_save_path);
			else
				printf("state: saved %zu bytes at frame %ld -> %s\n", size, frame, g_save_path);
			if (f) fclose(f);
		}
		if (frame == g_roundtrip_at) {
			/* Straight back in, same bytes, same instant. Anything this changes was changed by the
			 * act of saving. */
			if (!g_unserialize(buf, size))
				fprintf(stderr, "state: round-trip unserialize failed at frame %ld\n", frame);
			else
				printf("state: round-tripped %zu bytes in place at frame %ld\n", size, frame);
		}
		free(buf);
	}

	if (frame == g_load_at) {
		FILE *f = fopen(g_load_path, "rb");
		void *buf;
		size_t got;
		if (!f) { fprintf(stderr, "state: could not open %s\n", g_load_path); return; }
		buf = malloc(size);
		if (!buf) { fclose(f); fprintf(stderr, "state: out of memory\n"); return; }
		got = fread(buf, 1, size, f);
		fclose(f);
		if (got != size)
			fprintf(stderr, "state: %s is %zu bytes, core wants %zu — loading anyway\n",
			        g_load_path, got, size);
		if (!g_unserialize(buf, got))
			fprintf(stderr, "state: retro_unserialize FAILED at frame %ld\n", frame);
		else
			printf("state: loaded %zu bytes at frame %ld <- %s\n", got, frame, g_load_path);
		free(buf);
	}
}

/* Applied between frames, never inside one, which is the only ordering a frontend can produce. */
static bool apply_scripted_events(long frame)
{
	unsigned n;

	for (n = 0; n < g_mask_change_count; n++) {
		if (g_mask_changes[n].frame != frame)
			continue;
		/* The spec's promise when the mask changes is that the device is idle, so make it true
		 * rather than assume it: the core is about to destroy a ring built for the old mask. */
		pthread_mutex_lock(&vk.queue_lock);
		vk.device_wait_idle(vk.device);
		pthread_mutex_unlock(&vk.queue_lock);
		vk.mask = g_mask_changes[n].mask;
		vk.slots = slots_for_mask(vk.mask);
		if (vk.index >= vk.slots)
			vk.index = vk.slots - 1;
		/* Every image we were holding belongs to a ring the core is about to destroy, so none of
		 * them may be read again until set_image says otherwise. */
		for (uint32_t s = 0; s < VK_MAX_SLOTS; s++)
			vk.slot[s].have_image = false;
		printf("vk host: sync index mask now 0x%x (%u slots) at frame %ld\n", vk.mask, vk.slots, frame);
	}

	for (n = 0; n < g_reset_count; n++) {
		if (g_resets[n] != frame)
			continue;
		if (getenv("M2VK_HOST_SKIP_DESTROY")) {
			/* A reset with no destroy before it. Not an ordering RetroArch produces, and the reason
			 * the core abandons its ring instead of destroying it: the device those handles belong
			 * to is about to stop existing without the core ever being told. */
			printf("vk host: replacing the context at frame %ld *without* context_destroy\n", frame);
		} else {
			printf("vk host: context lost at frame %ld\n", frame);
			if (vk.destroy_cb)
				vk.destroy_cb();
		}
		vk_destroy_device();
		vk.live = false;
		if (!vk_bring_up()) {
			fprintf(stderr, "[host] could not rebuild the Vulkan context\n");
			return false;
		}
		if (vk.reset_cb)
			vk.reset_cb();
	}
	return true;
}

/* --- main ------------------------------------------------------------------------------------ */

#define SYM(v, n) do { *(void **)(&v) = dlsym(h, n); if (!v) { fprintf(stderr, "missing %s\n", n); return 1; } } while (0)

int main(int argc, char **argv)
{
	int arg = 1;

	/* The core defaults the frame-rate read-out (model2_fps_display) ON for players, but it draws
	 * wall-clock digits that change every run -- poison for a digest comparison. This is the A/B
	 * harness, so pin it OFF unless the caller explicitly set M2VK_FPS, exactly as the poly counter is
	 * off by default here. Overwrite=0 leaves an explicit M2VK_FPS=1 (eyeballing the overlay) alone. */
	setenv("M2VK_FPS", "0", 0);

	for (; arg < argc && argv[arg][0] == '-' && argv[arg][1] == '-'; arg++) {
		if (!strcmp(argv[arg], "--vk"))
			g_vk_mode = true;
		else if (!strcmp(argv[arg], "--gun")) {
			/* Selects the lightgun device for a port, i.e. what a frontend's input menu does. The
			 * gun script controls report on RETRO_DEVICE_LIGHTGUN whether or not this is passed —
			 * a frontend does not stop sending a device's state because the core did not select it
			 * — so this is a separate switch on purpose: it is how "the core ignores a device it
			 * was not told to use" gets tested. */
			if (arg + 1 >= argc) {
				fprintf(stderr, "--gun wants a port number\n");
				return 2;
			}
			g_gun_ports |= 1u << (strtoul(argv[++arg], NULL, 10) & 31);
		} else if (!strcmp(argv[arg], "--modern")) {
			/* The second pad layout, which is a subclass of RETRO_DEVICE_JOYPAD rather than a class
			 * of its own — so unlike --gun there is no separate set of controls to send, and the
			 * only thing this changes is which RetroPad button the core reads into each MAME button.
			 * The script still says "l" and "r"; what they come out as is the point of the switch. */
			if (arg + 1 >= argc) {
				fprintf(stderr, "--modern wants a port number\n");
				return 2;
			}
			g_modern_ports |= 1u << (strtoul(argv[++arg], NULL, 10) & 31);
		} else if (!strcmp(argv[arg], "--cabinet")) {
			/* The per-game layout, on the same terms as --modern — except that this one is what a
			 * port already is, so passing it asserts the default rather than changing it. */
			if (arg + 1 >= argc) {
				fprintf(stderr, "--cabinet wants a port number\n");
				return 2;
			}
			g_cabinet_ports |= 1u << (strtoul(argv[++arg], NULL, 10) & 31);
		} else if (!strcmp(argv[arg], "--classic")) {
			/* The generic layout, explicitly. On a set with a cabinet row this is the negative
			 * control: the only way back to what every set used to do. */
			if (arg + 1 >= argc) {
				fprintf(stderr, "--classic wants a port number\n");
				return 2;
			}
			g_classic_ports |= 1u << (strtoul(argv[++arg], NULL, 10) & 31);
		} else {
			fprintf(stderr, "unknown option %s\n", argv[arg]);
			return 2;
		}
	}

	if (argc - arg < 3) {
		fprintf(stderr, "usage: %s [--vk] [--gun <port>] [--modern <port>] [--classic <port>] [--cabinet <port>] <core.dylib> <content> <frames> [out.ppm] [control-script]\n"
		                "  --vk: act as a Vulkan frontend and read the core's image back off the GPU\n"
		                "  --gun <port>: select RETRO_DEVICE_LIGHTGUN for that port (repeatable)\n"
		                "  --modern <port>: select the second generic pad layout (repeatable)\n"
		                "  --classic <port>: select the first generic pad layout, i.e. opt out of the\n"
		                "                    set's cabinet layout — the negative control (repeatable)\n"
		                "  --cabinet <port>: select the set's cabinet layout, which is already the default\n"
		                "  control-script: frame:control[:held][:port],...  e.g. 600:select,660:start,800:lx+:200\n"
		                "                  the gun aims: 1200:gun=0.25/0.5:60 (normalised, 0.0..1.0)\n",
		        argv[0]);
		return 2;
	}
	const char *corepath = argv[arg], *content = argv[arg + 1];
	const long want = strtol(argv[arg + 2], NULL, 10);
	const char *out = (argc - arg > 3) ? argv[arg + 3] : NULL;

	if (argc - arg > 4 && parse_script(argv[arg + 4]) != 0)
		return 2;

	if (g_vk_mode) {
		char *at;
		pthread_mutex_init(&vk.queue_lock, NULL);
		vk.mask = (at = getenv("M2VK_HOST_SYNC_MASK")) ? (uint32_t)strtoul(at, NULL, 0) : 0x7;
		vk.slots = slots_for_mask(vk.mask);
		if ((at = getenv("M2VK_HOST_MASK_AT")) != NULL) parse_mask_changes(strdup(at));
		if ((at = getenv("M2VK_HOST_RESET_AT")) != NULL) parse_resets(strdup(at));
		if (!vk_load_lib() || !vk_load_global())
			return 1;
	}

	{
		const char *at = getenv("M2VK_HOST_OPT_AT");
		if (at != NULL) parse_opt_changes(strdup(at));
	}

	{
		const char *at = getenv("M2VK_HOST_FRAME_HASH");
		if (at != NULL) g_frame_hash_from = strtol(at, NULL, 10);
	}
	{
		const char *at = getenv("M2VK_HOST_DIGEST_FROM");
		g_save_at = parse_state_at("M2VK_HOST_SAVE_AT", &g_save_path);
		g_save_at2 = parse_state_at("M2VK_HOST_SAVE_AT2", &g_save_path2);
		g_load_at = parse_state_at("M2VK_HOST_LOAD_AT", &g_load_path);
		g_roundtrip_at = (at = getenv("M2VK_HOST_ROUNDTRIP_AT")) ? strtol(at, NULL, 10) : -1;
		if ((at = getenv("M2VK_HOST_DIGEST_FROM")) != NULL) {
			g_digest_from = strtol(at, NULL, 10);
			printf("state: digest covers frames %ld and later only\n", g_digest_from);
		}
	}

	void *h = dlopen(corepath, RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*init_fn)(void);
	bool (*load_game)(const struct retro_game_info *);
	void (*run_fn)(void);
	void (*unload_game)(void);
	void (*deinit_fn)(void);

	SYM(set_environment, "retro_set_environment");
	SYM(set_video_refresh, "retro_set_video_refresh");
	SYM(set_audio_sample, "retro_set_audio_sample");
	SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
	SYM(set_input_poll, "retro_set_input_poll");
	SYM(set_input_state, "retro_set_input_state");
	SYM(set_controller_port_device, "retro_set_controller_port_device");
	SYM(api_version, "retro_api_version");
	SYM(get_system_info, "retro_get_system_info");
	SYM(get_system_av_info, "retro_get_system_av_info");
	SYM(init_fn, "retro_init");
	SYM(load_game, "retro_load_game");
	SYM(run_fn, "retro_run");
	SYM(unload_game, "retro_unload_game");
	SYM(deinit_fn, "retro_deinit");
	SYM(g_serialize_size, "retro_serialize_size");
	SYM(g_serialize, "retro_serialize");
	SYM(g_unserialize, "retro_unserialize");

	struct retro_system_info si;
	memset(&si, 0, sizeof si);
	get_system_info(&si);
	printf("core: %s %s (api %u) exts=%s\n", si.library_name, si.library_version,
	       api_version(), si.valid_extensions ? si.valid_extensions : "?");

	set_environment(env_cb);
	set_video_refresh(video_cb);
	set_audio_sample(audio_sample_cb);
	set_audio_sample_batch(audio_batch_cb);
	set_input_poll(input_poll_cb);
	set_input_state(input_state_cb);
	init_fn();

	struct retro_game_info gi;
	memset(&gi, 0, sizeof gi);
	gi.path = content;
	if (!load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }

	/* After load, which is where RetroArch does it too: the core has to have a machine before a
	 * device selection can mean anything. Ports not named here are left alone rather than set to
	 * RETRO_DEVICE_JOYPAD, so a run without --gun makes no call at all and cannot be the reason a
	 * pad regression appears. */
	for (unsigned port = 0; port < 32; port++) {
		if (g_gun_ports & (1u << port)) {
			printf("input: port %u -> RETRO_DEVICE_LIGHTGUN\n", port);
			set_controller_port_device(port, RETRO_DEVICE_LIGHTGUN);
		} else if (g_modern_ports & (1u << port)) {
			printf("input: port %u -> RetroPad (Modern)\n", port);
			set_controller_port_device(port, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));
		} else if (g_classic_ports & (1u << port)) {
			printf("input: port %u -> RetroPad (Classic)\n", port);
			set_controller_port_device(port, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2));
		} else if (g_cabinet_ports & (1u << port)) {
			printf("input: port %u -> RetroPad (Cabinet), i.e. the default\n", port);
			set_controller_port_device(port, RETRO_DEVICE_JOYPAD);
		}
	}

	struct retro_system_av_info av;
	memset(&av, 0, sizeof av);
	get_system_av_info(&av);
	printf("av: %ux%u (max %ux%u) aspect %.4f  fps %.4f  rate %.0f\n",
	       av.geometry.base_width, av.geometry.base_height,
	       av.geometry.max_width, av.geometry.max_height,
	       av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

	/* After retro_load_game, never before: the core cannot be given a context until it has one to
	 * be given a context for, and it must not be made to wait for one inside load — its first
	 * emulated frame runs there, which is where the geometry comes from. */
	if (vk.declared) {
		if (!vk_bring_up())
			return 1;
		vk.reset_cb();
	}

	const char *rss_env = getenv("M2VK_HOST_RSS");
	long rss_every = rss_env ? strtol(rss_env, NULL, 10) : 0;
	size_t rss_peak = 0;

	g_perf = getenv("M2VK_HOST_PERF") != NULL;
	const char *skip_env = getenv("M2VK_HOST_PERF_SKIP");
	g_perf_skip = skip_env ? strtol(skip_env, NULL, 10) : 0;
	g_bucket_size = (want + PERF_BUCKETS - 1) / PERF_BUCKETS;
	if (g_bucket_size < 1) g_bucket_size = 1;

	/* The clock starts after retro_load_game and after the context is up. Loading a ROM set and
	 * building a Vulkan device are one-off costs that no amount of renderer work will change, and
	 * folding them into a per-frame average is how a short run comes out slower than a long one. */
	const uint64_t t_run_start = now_ns();

	long n = 0;
	for (; n < want && !g_shutdown; n++) {
		g_frame = n;
		/* Before the frame and outside the vk.live gate: an option change is a frontend action, not
		 * a Vulkan one, and it must reach the software path too. */
		if (g_opt_change_count) apply_opt_changes(n);
		if (vk.live && !apply_scripted_events(n))
			break;
		/* A frontend rotates its sync index once per frame, before the core draws. */
		if (vk.live)
			vk.index = (vk.index + 1) % vk.slots;

		g_t.gpuwait = g_t.host = 0;
		const uint64_t t_frame = now_ns();
		run_fn();
		g_t.run = now_ns() - t_frame;

		{
			const uint64_t core = g_t.run > g_t.gpuwait + g_t.host
			                    ? g_t.run - g_t.gpuwait - g_t.host : 0;
			g_t.total_run += g_t.run;
			g_t.total_gpuwait += g_t.gpuwait;
			g_t.total_host += g_t.host;
			if (n >= g_perf_skip) {
				g_t.skip_run += g_t.run;
				g_t.skip_gpuwait += g_t.gpuwait;
				g_t.skip_host += g_t.host;
				if (core > g_t.worst_core) { g_t.worst_core = core; g_t.worst_frame = n; }
			}
			const long b = n / g_bucket_size;
			if (b < PERF_BUCKETS) {
				g_bucket[b].core += core;
				g_bucket[b].gpuwait += g_t.gpuwait;
				g_bucket[b].host += g_t.host;
				g_bucket[b].frames++;
			}
		}

		/* At the END of the frame, after run_fn() and after the frame was hashed: a state saved
		 * here is "the machine as it stands having completed frame n", which is the same instant a
		 * load at n restores to. */
		do_state_events(n);

		if (rss_every > 0 && (n % rss_every) == 0) {
			size_t rss = resident_bytes();
			if (rss > rss_peak) rss_peak = rss;
			printf("rss: frame %6ld  %8.2f MiB\n", n, (double)rss / (1024.0 * 1024.0));
			fflush(stdout);
		}
	}

	const uint64_t t_wall = now_ns() - t_run_start;

	printf("ran %ld frames, %lu with video, %lu audio sample frames (%.1f/frame)\n",
	       n, g_frames_with_video, g_audio_samples, n ? (double)g_audio_samples / n : 0.0);
	/* The size the LAST frame arrived at, which is the size the PPM was written at. Printed because
	 * the internal-resolution option can change it mid-run and every other number in this summary is
	 * silent about which resolution produced it. */
	printf("presented: %ux%u\n", g_w, g_h);
	printf("digest: %016llx over %lu frames\n", (unsigned long long)g_digest,
	       g_digest_from ? g_digest_frames : g_frames_with_video);
	if (rss_every > 0) {
		size_t rss = resident_bytes();
		if (rss > rss_peak) rss_peak = rss;
		printf("rss: final %.2f MiB, peak %.2f MiB over %ld frames\n",
		       (double)rss / (1024.0 * 1024.0), (double)rss_peak / (1024.0 * 1024.0), n);
	}

	if (g_perf && n > 0) {
		const double hz = av.timing.fps > 0.0 ? av.timing.fps : 60.0;
		const long counted = n > g_perf_skip ? n - g_perf_skip : n;
		const long from = n > g_perf_skip ? g_perf_skip : 0;
		/* Over the counted window only, so PERF_SKIP means what it says. */
		const uint64_t run = from ? g_t.skip_run : g_t.total_run;
		const uint64_t gpu = from ? g_t.skip_gpuwait : g_t.total_gpuwait;
		const uint64_t hst = from ? g_t.skip_host : g_t.total_host;
		const uint64_t core = run > gpu + hst ? run - gpu - hst : 0;
		const double ms = 1.0e-6;
		/* Both bounds of the bracket — see the timer comment. Neither excludes the other's error;
		 * they exclude the harness's read-back copy and whole-run digest, which is what `host` is
		 * for. `wall` includes everything, so the three together say how much of the wall figure is
		 * the measuring apparatus. */
		const uint64_t pipe = core > gpu ? core : gpu;
		const double serial_s = (double)(core + gpu) * 1.0e-9;
		const double pipe_s = (double)pipe * 1.0e-9;
		const double wall_s = (double)t_wall * 1.0e-9;
		const double real_s = (double)counted / hz;

		printf("perf: %ld frames at %.2f Hz declared = %.2fs of emulated time\n", counted, hz, real_s);
		printf("perf: speed %.1f %% serial .. %.1f %% pipelined  (%.3f .. %.3f ms/frame)\n",
		       serial_s > 0.0 ? 100.0 * real_s / serial_s : 0.0,
		       pipe_s > 0.0 ? 100.0 * real_s / pipe_s : 0.0,
		       (double)(core + gpu) / counted * ms, (double)pipe / counted * ms);
		printf("perf:   core %.3f ms/frame   gpuwait %.3f ms/frame   host %.3f ms/frame\n",
		       (double)core / counted * ms, (double)gpu / counted * ms, (double)hst / counted * ms);
		printf("perf:   worst core frame %ld at %.3f ms\n", g_t.worst_frame, (double)g_t.worst_core * ms);
		printf("perf: wall %.3fs over the whole %ld-frame loop = %.1f %% including harness overhead\n",
		       wall_s, n, wall_s > 0.0 ? 100.0 * ((double)n / hz) / wall_s : 0.0);
		if (from)
			printf("perf: first %ld frames excluded (M2VK_HOST_PERF_SKIP)\n", from);

		/* The bucket table is what says whether the headline describes the game or its boot. A game
		 * that renders nothing for its first third shows up here as two plateaus, and if it does,
		 * the headline needs a PERF_SKIP past the first one to mean anything. */
		printf("perf: per-%ld-frame means, ms — frame  core  gpuwait  host\n", g_bucket_size);
		for (long b = 0; b < PERF_BUCKETS && g_bucket[b].frames; b++)
			printf("perf:   %6ld  %7.3f  %7.3f  %7.3f\n", b * g_bucket_size,
			       (double)g_bucket[b].core / g_bucket[b].frames * ms,
			       (double)g_bucket[b].gpuwait / g_bucket[b].frames * ms,
			       (double)g_bucket[b].host / g_bucket[b].frames * ms);
	}

	if (out && g_fb) {
		FILE *f = fopen(out, "wb");
		if (f) {
			fprintf(f, "P6\n%u %u\n255\n", g_w, g_h);
			for (unsigned y = 0; y < g_h; y++) {
				const uint32_t *row = (const uint32_t *)((const uint8_t *)g_fb + y * g_pitch);
				for (unsigned x = 0; x < g_w; x++) {
					uint32_t p = row[x];
					fputc((p >> 16) & 0xff, f);
					fputc((p >> 8) & 0xff, f);
					fputc(p & 0xff, f);
				}
			}
			fclose(f);
			printf("wrote %s (%ux%u)\n", out, g_w, g_h);
		}
	}

	/* The order a frontend uses, and the order the core's teardown assumes: the context goes while
	 * the device is still alive, then the device, then the content. */
	if (vk.live && vk.destroy_cb)
		vk.destroy_cb();
	vk_destroy_all();

	unload_game();
	deinit_fn();
	return 0;
}
