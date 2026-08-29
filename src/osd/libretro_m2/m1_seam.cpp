// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 1 renderer seam — the M1-1 diagnostic tap.

    See m1_seam.h for the seam and the data shape. This file is the whole of the plumbing plus one
    consumer: a diagnostic tap that counts the quad stream and never draws anything. It exists to answer
    the M1-1 question — is the seam actually wired? — because a passthrough that draws nothing passes the
    byte-identical test vacuously if the hooks are never reached. Run with M2VK_M1TAP=1 and the per-frame
    counts prove the quad and frame hooks fire; run with it unset and the seam is inert (g_active stays
    false, submit_quad returns at the active() gate) and the output is byte-identical to the M1-0 software
    baseline.

    Everything here runs on the emulation thread: draw_quads and screen_update are only reached from the
    driver's own screen update.

    Environment variables (the tap is attached only if one is set):
      M2VK_M1TAP=1          attach with the defaults below
      M2VK_M1TAP_EVERY=N    print a summary line every N frames that carry geometry (default 1)

    M1-2 will add the GPU record consumer (m1_geom.cpp record_*) behind set_gpu()/g_gpu_capture, alongside
    the tap; the two are independent, exactly as in s21_seam.cpp.

*********************************************************************************************************************************/

#include "m1_seam.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace m1 {

namespace detail {

bool g_active = false;
bool g_sw_owns_3d = true;

} // namespace detail

// "No Lighting" (model2_flat_luma). Read on the frontend thread in m1_geom's geom_draw; a plain bool, set
// once per option-apply and never per primitive, so no synchronisation is needed.
namespace {
bool g_no_lighting = false;
}

void set_option_no_lighting(bool on) { g_no_lighting = on; }
bool no_lighting() { return g_no_lighting; }

namespace {

// The two independent reasons active() is true: the M1-1 diagnostic tap (env) and the M1-2 GPU path.
// active() must reflect either, decided at different times, so they are kept apart and active() is their OR.
bool g_tap_attached = false;
bool g_gpu_capture = false;

void recompute_active()
{
	detail::g_active = g_tap_attached || g_gpu_capture;
}

bool env_on(char const *name)
{
	char const *const v = std::getenv(name);
	return (v != nullptr) && (std::strtoul(v, nullptr, 10) != 0);
}

// The M1-1 tap. One object with static storage duration, so it always exists and its destructor emits a
// run-level summary at process exit (retrohost exits normally, so the destructor runs). The per-frame
// lines are the real M1-1 evidence; the summary is a convenience.
class tap
{
public:
	void begin()
	{
		ensure_init();
		if (!m_attached)
			return;

		m_quads = 0;
		m_min_x = m_min_y = 0x7fffffff;
		m_max_x = m_max_y = -0x7fffffff;
		m_min_z = 3.4e38f;
		m_max_z = -3.4e38f;
		m_moire = 0;
	}

	void on_quad(quad const &q)
	{
		m_quads++;
		for (int i = 0; i < 4; i++)
		{
			const int xi = int(q.x[i]), yi = int(q.y[i]);
			if (xi < m_min_x) m_min_x = xi;
			if (xi > m_max_x) m_max_x = xi;
			if (yi < m_min_y) m_min_y = yi;
			if (yi > m_max_y) m_max_y = yi;
		}
		if (q.z < m_min_z) m_min_z = q.z;
		if (q.z > m_max_z) m_max_z = q.z;
		if (q.col & COL_MOIRE) m_moire++;
	}

	void end()
	{
		if (!m_attached)
			return;

		m_frames++;
		m_run_quads += m_quads;
		if (m_quads > m_max_quads) m_max_quads = m_quads;
		if (m_quads) m_frames_with_geom++;

		if (m_every != 0 && (m_frames % m_every) == 0 && m_quads)
		{
			std::fprintf(stderr,
					"[m1tap] frame %6u  quads %5u  x %d..%d  y %d..%d  z %g..%g  moire %u\n",
					m_frames, m_quads,
					m_min_x, m_max_x, m_min_y, m_max_y,
					(double)m_min_z, (double)m_max_z, m_moire);
		}
	}

	~tap()
	{
		if (!m_attached)
			return;

		std::fprintf(stderr,
				"[m1tap] run end: %u frames (%u with geometry)  quads %llu (max/frame %u)\n",
				m_frames, m_frames_with_geom,
				(unsigned long long)m_run_quads, m_max_quads);
	}

private:
	void ensure_init()
	{
		if (m_inited)
			return;
		m_inited = true;

		char const *const every = std::getenv("M2VK_M1TAP_EVERY");
		m_attached = env_on("M2VK_M1TAP") || (every != nullptr);
		if (every != nullptr)
			m_every = uint32_t(std::strtoul(every, nullptr, 10));

		g_tap_attached = m_attached;
		recompute_active();
		if (m_attached)
			std::fprintf(stderr, "[m1tap] active; summary every %u frame(s)\n", m_every);
	}

	bool     m_inited = false;
	bool     m_attached = false;
	uint32_t m_every = 1;

	// Per-frame, reset in begin().
	uint32_t m_quads = 0;
	int      m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0;
	float    m_min_z = 0, m_max_z = 0;
	uint32_t m_moire = 0;

	// Run-level, never reset.
	uint32_t m_frames = 0;
	uint32_t m_frames_with_geom = 0;
	uint64_t m_run_quads = 0;
	uint32_t m_max_quads = 0;
};

tap g_tap;

} // anonymous namespace

// The seam plumbing. Each site drives the diagnostic tap (harmless when it is not attached) and, when
// the GPU path is capturing, the record consumer in m1_geom.cpp. The tap and the record are independent:
// a run may have either, both, or (the common case) neither.
void frame_begin()
{
	g_tap.begin();
	if (g_gpu_capture)
		record_begin();

	// A fresh frame has no overlay until the driver fills one at the end of screen_update. Cleared here so
	// a frame that captures none presents the passthrough, not a stale overlay.
	over_forget();
}

void frame_end()
{
	g_tap.end();
	if (g_gpu_capture)
		record_end();
}

void submit(quad const &q)
{
	g_tap.on_quad(q);
	if (g_gpu_capture)
		record_quad(q);
}

void set_gpu(bool on)
{
	g_gpu_capture = on;
	detail::g_sw_owns_3d = !on;     // the GPU owns the 3D once capture is on, so software stops drawing
	recompute_active();
}

bool gpu_owns_3d()
{
	return g_gpu_capture;
}

void set_no_3d()
{
	// M2VK_NO_3D: neither draws. Capture off (the GPU draws nothing) AND sw_owns_3d off (the driver skips
	// the 3D), leaving just the 2D tile layers — the background reference.
	g_gpu_capture = false;
	detail::g_sw_owns_3d = false;
	recompute_active();
}

// The 2D-over overlay buffer. File-scope so a build with no renderer still links. The emulation thread
// fills it in the driver's screen_update; the frontend reads it in present while that thread is parked on
// the baton, the same guarantee the record rests on, so no lock is needed.
namespace {
std::vector<uint32_t> g_over;
int  g_over_w = 0;
int  g_over_h = 0;
bool g_over_valid = false;
}

uint32_t *over_begin(int width, int height)
{
	g_over_valid = false;
	// Gated on gpu_owns_3d(), not merely !sw_owns_3d(): screen_update takes this same site under M2VK_NO_3D
	// too, and an overlay that existed there would draw over what is meant to be a pure 2D background
	// reference the coverage/exact harness differences against.
	if (!gpu_owns_3d() || (width <= 0) || (height <= 0))
		return nullptr;
	g_over.resize(size_t(width) * size_t(height));
	g_over_w = width;
	g_over_h = height;
	return g_over.data();
}

void over_end()
{
	g_over_valid = true;
}

uint32_t const *over_pixels(int &width, int &height)
{
	if (!g_over_valid)
		return nullptr;
	width = g_over_w;
	height = g_over_h;
	return g_over.data();
}

void over_forget()
{
	g_over_valid = false;
}

} // namespace m1
