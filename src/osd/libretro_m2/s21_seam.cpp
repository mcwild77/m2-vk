// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 21 renderer seam — the T1 diagnostic tap.

    See s21_seam.h for the seam and the data shape. This file is the whole of the plumbing plus one
    consumer: a diagnostic tap that counts the quad stream and never draws anything. It exists to answer
    the T1 question — is the seam actually wired? — because a passthrough that draws nothing passes the
    byte-identical test vacuously if the hooks are never reached. Run with M2VK_S21TAP=1 and the
    per-frame counts prove the quad and frame hooks fire; run with it unset and the seam is inert
    (g_active stays false, submit_quad returns at the active() gate) and the output is byte-identical to
    the T0 software baseline (digest 8ae63fbb7bd812fa at 4500 frames on starblad).

    Everything here runs on the emulation thread: blit_single_quad and the framebuffer swap are only
    reached from the DSP's render path, which the driver drives from its own screen update.

    Environment variables (the tap is attached only if one is set):
      M2VK_S21TAP=1          attach with the defaults below
      M2VK_S21TAP_EVERY=N    print a summary line every N frames that carry geometry (default 1)

*********************************************************************************************************************************/

#include "s21_seam.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace s21 {

namespace detail {

bool g_active = false;
bool g_sw_owns_3d = true;

} // namespace detail

namespace {

// The two independent reasons active() is true: the T1 diagnostic tap (env) and the T2 GPU path.
// active() must reflect either, and both are decided at different times (the tap on its first
// frame_begin, the GPU path at load), so they are kept apart and active() is their OR.
bool g_tap_attached = false;
bool g_gpu_capture = false;

void recompute_active()
{
	detail::g_active = g_tap_attached || g_gpu_capture;
}

// The palette pointer the CLUT reads. File-scope so a build with no renderer still links; the pointer
// is stable, so re-storing it every frame costs two writes.
palette_ram g_palette;

bool env_on(char const *name)
{
	char const *const v = std::getenv(name);
	return (v != nullptr) && (std::strtoul(v, nullptr, 10) != 0);
}

// The T1 tap. One object with static storage duration, so it always exists and its destructor emits a
// run-level summary at process exit (retrohost exits normally, so the destructor runs). The per-frame
// lines are the real T1 evidence; the summary is a convenience.
class tap
{
public:
	void begin()
	{
		ensure_init();
		if (!m_attached)
			return;

		m_quads = 0;
		m_min_x = m_min_y = m_min_z = 0x7fffffff;
		m_max_x = m_max_y = m_max_z = -0x7fffffff;
		m_min_color = 0xffff;
		m_max_color = 0;
	}

	void on_quad(quad const &q)
	{
		m_quads++;
		for (int i = 0; i < 4; i++)
		{
			if (q.x[i] < m_min_x) m_min_x = q.x[i];
			if (q.x[i] > m_max_x) m_max_x = q.x[i];
			if (q.y[i] < m_min_y) m_min_y = q.y[i];
			if (q.y[i] > m_max_y) m_max_y = q.y[i];
		}
		if (q.zsort < m_min_z) m_min_z = q.zsort;
		if (q.zsort > m_max_z) m_max_z = q.zsort;
		if (q.color < m_min_color) m_min_color = q.color;
		if (q.color > m_max_color) m_max_color = q.color;
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
					"[s21tap] frame %6u  quads %5u  x %d..%d  y %d..%d  z %d..%d  pen %04x..%04x\n",
					m_frames, m_quads,
					m_min_x, m_max_x, m_min_y, m_max_y, m_min_z, m_max_z,
					m_min_color, m_max_color);
		}
	}

	~tap()
	{
		if (!m_attached)
			return;

		std::fprintf(stderr,
				"[s21tap] run end: %u frames (%u with geometry)  quads %llu (max/frame %u)\n",
				m_frames, m_frames_with_geom,
				(unsigned long long)m_run_quads, m_max_quads);
	}

private:
	void ensure_init()
	{
		if (m_inited)
			return;
		m_inited = true;

		char const *const every = std::getenv("M2VK_S21TAP_EVERY");
		m_attached = env_on("M2VK_S21TAP") || (every != nullptr);
		if (every != nullptr)
			m_every = uint32_t(std::strtoul(every, nullptr, 10));

		g_tap_attached = m_attached;
		recompute_active();
		if (m_attached)
			std::fprintf(stderr, "[s21tap] active; summary every %u frame(s)\n", m_every);
	}

	bool     m_inited = false;
	bool     m_attached = false;
	uint32_t m_every = 1;

	// Per-frame, reset in begin().
	uint32_t m_quads = 0;
	int      m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0, m_min_z = 0, m_max_z = 0;
	uint16_t m_min_color = 0, m_max_color = 0;

	// Run-level, never reset.
	uint32_t m_frames = 0;
	uint32_t m_frames_with_geom = 0;
	uint64_t m_run_quads = 0;
	uint32_t m_max_quads = 0;
};

tap g_tap;

} // anonymous namespace

// The seam plumbing. Each site drives the T1 diagnostic tap (harmless when it is not attached) and,
// when the T2 GPU path is capturing, the record consumer in s21_geom.cpp. The tap and the record are
// independent: a run may have either, both, or (the common case) neither.
void frame_begin()
{
	g_tap.begin();
	if (g_gpu_capture)
		record_begin();

	// A fresh frame has no overlay until the driver fills one at the end of screen_update. Cleared here so
	// a frame that captures none presents the passthrough, not a stale overlay.
	over_forget();
	mix_forget();
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
	// the 3D), leaving just the 2D layers — the background reference.
	g_gpu_capture = false;
	detail::g_sw_owns_3d = false;
	recompute_active();
}

void set_palette(uint32_t const *pens, uint32_t count)
{
	g_palette.pens = pens;
	g_palette.count = count;
}

palette_ram const &get_palette()
{
	return g_palette;
}

// The 2D-over overlay buffer. File-scope so a build with no renderer still links. The emulation thread
// fills it in the driver's screen_update; the frontend reads it in present while that thread is parked
// on the baton, the same guarantee the record rests on, so no lock is needed.
namespace {
std::vector<uint32_t> g_over;
int  g_over_w = 0;
int  g_over_h = 0;
bool g_over_valid = false;
}

uint32_t *over_begin(int width, int height)
{
	g_over_valid = false;
	// Gated on gpu_owns_3d(), not merely !sw_owns_3d(): the driver's screen_update takes this same call
	// site under M2VK_NO_3D too (there is no third branch for "capture nothing"), and an overlay that
	// existed there would draw over what is supposed to be a pure two-layer background reference.
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

// The T2b layer-0 z-mix overlay buffer. Same shape and the same threading guarantee as g_over above.
namespace {
std::vector<uint32_t> g_mix;
int  g_mix_w = 0;
int  g_mix_h = 0;
bool g_mix_valid = false;
}

uint32_t *mix_begin(int width, int height)
{
	g_mix_valid = false;
	// Same gate as over_begin() and for the same reason: capture_mix_sprites fires under M2VK_NO_3D too.
	if (!gpu_owns_3d() || (width <= 0) || (height <= 0))
		return nullptr;
	g_mix.resize(size_t(width) * size_t(height));
	g_mix_w = width;
	g_mix_h = height;
	return g_mix.data();
}

void mix_end()
{
	g_mix_valid = true;
}

uint32_t const *mix_pixels(int &width, int &height)
{
	if (!g_mix_valid)
		return nullptr;
	width = g_mix_w;
	height = g_mix_h;
	return g_mix.data();
}

void mix_forget()
{
	g_mix_valid = false;
}

} // namespace s21
