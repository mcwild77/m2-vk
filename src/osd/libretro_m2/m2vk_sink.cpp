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

#include <memory>
#include <vector>

namespace m2vk {

namespace detail {

bool g_active = false;
bool g_rasterize = true;

} // namespace detail

namespace {

class sink
{
public:
	// Consumers belong to a run, not to the process: they are built in open() and destroyed in
	// close(), so a second game loaded into the same process gets its own.
	~sink() { close(); }

	void open()
	{
		close();

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

	void frame_begin(uint32_t submitted)
	{
		// A build whose OSD does not bracket the run (the plain SUBTARGET=model2 binary, which has no
		// libretro OSD to call sink_open) still gets one run, opened here and closed by ~sink at
		// process exit. Such a run reports nothing if it never renders — there is nowhere left to
		// report it from.
		if (!m_open)
			open();

		for (auto &c : m_consumers)
			c->frame_begin(submitted);
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

void frame_begin(uint32_t submitted) { g_sink.frame_begin(submitted); }
void submit(poly const &p) { g_sink.submit(p); }
void frame_end() { g_sink.frame_end(); }

} // namespace m2vk
