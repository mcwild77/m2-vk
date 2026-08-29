// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 renderer sink — dispatch to whatever is watching the polygon stream.

    See m2vk_sink.h for the seam and the data shape. This file is the whole of the plumbing: one
    object with static storage duration, a list of consumers, and three forwarding calls.

    Everything here runs on the emulation thread. The tap verifies that claim every frame
    (multithreaded= in its survey) — poly_manager farms scanlines out to worker threads, but
    model2_3d_render() itself is only ever reached from the driver's own frame update.

*********************************************************************************************************************************/

#include "m2vk_sink.h"

#include "m2vk_polytap.h"

#include <cstdlib>
#include <memory>
#include <vector>

namespace m2vk {

namespace detail {

bool g_active = false;
bool g_rasterize = true;

uint8_t  g_force_solid = 0;
bool     g_flat_luma = false;
bool     g_smooth = false;
bool     g_opaque_only = false;
int32_t  g_only_poly = -1;
int32_t  g_only_frame = -1;
uint32_t g_frame_index = 0;
uint32_t g_poly_index = 0;
bool     g_poly_dropped = false;

} // namespace detail

namespace {

// The debug filter's whole configuration, read once per run. Numbers rather than flags because
// M2VK_FORCE_SOLID has two modes and the other two name a polygon; an unparseable value reads as
// "off" rather than as zero, which for M2VK_ONLY_POLY would otherwise silently mean "polygon 0".
int32_t env_index(char const *name)
{
	char const *const v = std::getenv(name);
	if ((v == nullptr) || (*v == '\0'))
		return -1;

	char *end = nullptr;
	const long n = std::strtol(v, &end, 10);
	if ((end == v) || (n < 0) || (n > 0x7fffffff))
		return -1;

	return int32_t(n);
}

// model2_flat_shading's value, parked here by retro_load_game before the run opens. The environment
// switch overrides it — see set_option_force_solid()'s comment in the header for why that direction
// and not the other.
uint8_t g_option_force_solid = 0;

// The flat-shading mode, from whichever source wins. Written once at sink_open() and again whenever
// the core option changes mid-run, which is why it is a function rather than two lines inside
// read_debug_filter(): the two callers must resolve it the same way or a live change would mean
// something different from a change at load.
//
// A negative from env_index() is "unset", which is the only case the option gets to answer. An
// explicit M2VK_FORCE_SOLID=0 is a request for "off" and beats an option asking for flat shading,
// which is what makes the switch a complete override rather than a one-way one.
void apply_force_solid()
{
	const int32_t solid = env_index("M2VK_FORCE_SOLID");
	detail::g_force_solid = (solid < 0)
			? g_option_force_solid
			: ((solid <= 0) ? 0 : uint8_t((solid == 1) ? 1 : 2));
}

// model2_flat_luma's value, parked by retro_load_game exactly as g_option_force_solid is, and
// resolved against the switch by the same rule — the switch wins in both directions, so an explicit
// M2VK_FLAT_LUMA=0 pins the lighting on against an option asking for it off.
bool g_option_flat_luma = false;

void apply_flat_luma()
{
	const int32_t flat = env_index("M2VK_FLAT_LUMA");
	detail::g_flat_luma = (flat < 0) ? g_option_flat_luma : (flat > 0);
}

// model2_smooth_shading's value (Model 2 only), parked by retro_load_game and resolved against the
// M2VK_M2_SMOOTH switch by the same switch-wins rule. Read on the frontend thread in vk_geom's
// geom_upload — an enhancement that welds a smooth per-vertex luma; off by default (Model 2 is
// flat-shaded hardware).
bool g_option_smooth = false;

void apply_smooth()
{
	const int32_t s = env_index("M2VK_M2_SMOOTH");
	detail::g_smooth = (s < 0) ? g_option_smooth : (s > 0);
}

void read_debug_filter()
{
	apply_force_solid();
	apply_flat_luma();
	apply_smooth();
	detail::g_opaque_only = (std::getenv("M2VK_OPAQUE_ONLY") != nullptr);
	detail::g_only_poly = env_index("M2VK_ONLY_POLY");
	detail::g_only_frame = env_index("M2VK_ONLY_FRAME");
	detail::g_frame_index = 0;
	detail::g_poly_index = 0;
	detail::g_poly_dropped = false;
}

class sink
{
public:
	// Consumers belong to a run, not to the process: they are built in open() and destroyed in
	// close(), so a second game loaded into the same process gets its own.
	~sink() { close(); }

	void open()
	{
		close();

		read_debug_filter();

		if (polytap::wanted())
			m_consumers.push_back(std::make_unique<polytap>());

		for (auto &c : m_consumers)
			c->run_begin();

		m_open = true;
		detail::g_active = !m_consumers.empty();
	}

	void close()
	{
		if (!m_open)
			return;

		m_open = false;
		detail::g_active = false;

		for (auto &c : m_consumers)
			c->run_end();
		m_consumers.clear();
	}

	void frame_begin(uint32_t submitted, frame_tables const &tables)
	{
		// A build whose OSD does not bracket the run (the plain SUBTARGET=model2 binary, which has no
		// libretro OSD to call sink_open) still gets one run, opened here and closed by ~sink at
		// process exit. Such a run reports nothing if it never renders — there is nowhere left to
		// report it from.
		if (!m_open)
			open();

		for (auto &c : m_consumers)
			c->frame_begin(submitted, tables);
	}

	void submit(poly const &p)
	{
		for (auto &c : m_consumers)
			c->submit(p);
	}

	void frame_end()
	{
		for (auto &c : m_consumers)
			c->frame_end();
	}

private:
	std::vector<std::unique_ptr<consumer> > m_consumers;
	bool m_open = false;
};

sink g_sink;

} // anonymous namespace


void set_rasterize(bool on) { detail::g_rasterize = on; }

// Clamped rather than trusted: the only caller resolves it from a fixed value list, but a mode past 2
// would reach submit()'s `else if (g_force_solid != 0)` branch and behave as 2 by accident rather than
// by decision.
//
// Applied immediately as well as stored, so that a mid-run change from the options menu takes effect
// on the next frame rather than at the next content load. Safe from the frontend thread because
// retro_run calls this at the same point it publishes input — with the emulation thread parked on the
// baton — and g_force_solid is read on that thread in submit().
void set_option_force_solid(unsigned mode)
{
	g_option_force_solid = (mode > 2) ? 2 : uint8_t(mode);
	apply_force_solid();
}

// Stored and applied immediately, for the reason above and with the same thread-safety argument:
// g_flat_luma is a plain global that submit() reads per polygon on the emulation thread, and the one
// caller writes it from retro_run with that thread parked on the baton. Takes effect on the next
// frame — there is nothing to rebuild, because the luma is resolved per polygon as it crosses.
void set_option_flat_luma(bool on)
{
	g_option_flat_luma = on;
	apply_flat_luma();
}

// Model 2 Smooth Shading (model2_smooth_shading). Unlike the two above, g_smooth is read on the FRONTEND
// thread in geom_upload, not per-polygon at the seam — it decides whether that frame's vertices carry a
// welded per-vertex luma or the flat poly luma. Same store-and-apply shape; takes effect next frame with
// no rebuild (the smooth luma rides an existing vertex attribute).
void set_option_smooth(bool on)
{
	g_option_smooth = on;
	apply_smooth();
}

void sink_open() { g_sink.open(); }
void sink_close() { g_sink.close(); }

// The frame record is not a consumer: it is the renderer's half of the seam rather than something
// watching it, it is the only thing that needs the tables, and it must not depend on a consumer list
// that a diagnostic environment variable can empty. So it is called first and directly, and the
// consumers get whatever is left.
void frame_begin(uint32_t submitted, frame_tables const &tables)
{
	// Kept in step with the record's own ordering: this counts frames that reach the seam, and
	// g_poly_index counts polygons within one of them. Both are what M2VK_ONLY_FRAME/POLY name, and
	// both have to be maintained whether or not anything is watching.
	detail::g_poly_index = 0;
	detail::g_poly_dropped = false;

	// An empty display list is a frame as far as the record is concerned and not a frame as far as
	// anything else is. render_polygons calls this and then returns, so there is no stream to bracket
	// and no frame_end() to come — geometry_none() therefore opens and closes the record in one go.
	//
	// The consumers are deliberately not told. "Rendered frame" in the polytap means a frame carrying
	// polygons: it is what M2VK_POLYTAP_DUMP=N counts, what the committed vf2 frame-800 fixture is keyed
	// on, and what g_frame_index — M2VK_ONLY_FRAME's numbering — has to stay in step with. VF2 alone
	// queues nothing for its first ~990 frames, so counting those would renumber every one of them.
	if (submitted == 0)
	{
		geometry_none();
		return;
	}

	geometry_begin(submitted, tables);
	g_sink.frame_begin(submitted, tables);
}

void submit(poly const &p)
{
	geometry_submit(p);
	g_sink.submit(p);
}

void frame_end()
{
	detail::g_frame_index++;
	detail::g_poly_dropped = false;

	geometry_end();
	g_sink.frame_end();
}

} // namespace m2vk
