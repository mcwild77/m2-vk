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

void read_debug_filter()
{
	const int32_t solid = env_index("M2VK_FORCE_SOLID");
	detail::g_force_solid = (solid <= 0) ? 0 : uint8_t((solid == 1) ? 1 : 2);
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
