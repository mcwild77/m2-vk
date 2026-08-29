// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco System 23 renderer seam — the 23-1 diagnostic tap.

    See s23_seam.h for the seam and the data shape. This file is the whole of the plumbing plus one
    consumer: a diagnostic tap that counts the primitive stream and never draws anything. It exists to
    answer the 23-1 question — is the seam actually wired? — because a passthrough that draws nothing
    passes the byte-identical test vacuously if the hooks are never reached. Run with M2VK_S23TAP=1 and
    the per-frame counts prove the frame and submit hooks fire; run with it unset and the seam is inert
    (g_active stays false) and the output is byte-identical to the 23-0 software baseline.

    Everything here runs on the emulation thread: poly_manager farms scanlines out to workers, but
    render_flush() and the submit() calls it makes are only reached from the driver's own screen update.

    Environment variables (the tap is attached only if one is set):
      M2VK_S23TAP=1          attach with the defaults below
      M2VK_S23TAP_EVERY=N    print a summary line every N scenes that carry geometry (default 1)

*********************************************************************************************************************************/

#include "s23_seam.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace s23 {

namespace detail {

bool g_active = false;
bool g_sw_owns_3d = true;

} // namespace detail

namespace {

// The two independent reasons active() is true: the 23-1 diagnostic tap (env) and the 23-2 GPU path.
// active() must reflect either, and both are decided at different times (the tap on its first
// frame_begin, the GPU path at load), so they are kept apart and active() is their OR.
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

// The 23-1 tap. One object with static storage duration, so it always exists and its destructor emits
// a run-level summary at process exit (retrohost exits normally, so the destructor runs). The per-frame
// lines are the real 23-1 evidence; the summary is a convenience.
class tap
{
public:
	void begin(int variant)
	{
		ensure_init();
		if (!m_attached)
			return;

		m_variant = variant;
		m_polys = m_model = m_direct = m_immediate = m_sprite = 0;
		m_stencil = m_shade = m_pfade = m_colorfade = m_blend = m_alpha = 0;
		m_min_x = m_min_y = 1e30f;
		m_max_x = m_max_y = -1e30f;
		m_min_zkey = INT32_MAX;
		m_max_zkey = INT32_MIN;
	}

	void on_poly(poly const &p)
	{
		if (!m_attached)
			return;

		m_polys++;
		if (p.model)           m_model++;
		if (p.direct)          m_direct++;
		if (p.immediate)       m_immediate++;
		if (p.sprite)          m_sprite++;
		if (p.stencil_enabled) m_stencil++;
		if (p.shade_enabled)   m_shade++;
		if (p.pfade_enabled)   m_pfade++;
		if (p.colorfade)       m_colorfade++;
		if (p.blend_enabled)   m_blend++;
		if (p.poly_alpha)      m_alpha++;

		if (p.zkey < m_min_zkey) m_min_zkey = p.zkey;
		if (p.zkey > m_max_zkey) m_max_zkey = p.zkey;

		const int n = (p.num_verts < 16) ? p.num_verts : 16;
		for (int i = 0; i < n; i++)
		{
			if (p.x[i] < m_min_x) m_min_x = p.x[i];
			if (p.x[i] > m_max_x) m_max_x = p.x[i];
			if (p.y[i] < m_min_y) m_min_y = p.y[i];
			if (p.y[i] > m_max_y) m_max_y = p.y[i];
		}
	}

	void end()
	{
		if (!m_attached)
			return;

		m_scenes++;
		m_run_polys += m_polys;
		if (m_polys > m_max_polys) m_max_polys = m_polys;
		if (m_polys) m_scenes_with_geom++;

		if (m_every != 0 && (m_scenes % m_every) == 0 && m_polys)
		{
			std::fprintf(stderr,
					"[s23tap] scene %6u  polys %4u (model %4u direct %4u imm %4u spr %4u)"
					"  shade[st %3u sh %3u pf %3u cf %3u bl %3u al %3u]"
					"  x %.0f..%.0f  y %.0f..%.0f  zkey %d..%d\n",
					m_scenes, m_polys, m_model, m_direct, m_immediate, m_sprite,
					m_stencil, m_shade, m_pfade, m_colorfade, m_blend, m_alpha,
					double(m_min_x), double(m_max_x), double(m_min_y), double(m_max_y),
					m_min_zkey, m_max_zkey);
		}
	}

	~tap()
	{
		if (!m_attached)
			return;

		std::fprintf(stderr,
				"[s23tap] run end: %u scenes (%u with geometry)  polys %llu (max/scene %u)\n",
				m_scenes, m_scenes_with_geom,
				(unsigned long long)m_run_polys, m_max_polys);
	}

private:
	void ensure_init()
	{
		if (m_inited)
			return;
		m_inited = true;

		char const *const every = std::getenv("M2VK_S23TAP_EVERY");
		m_attached = env_on("M2VK_S23TAP") || (every != nullptr);
		if (every != nullptr)
			m_every = uint32_t(std::strtoul(every, nullptr, 10));

		g_tap_attached = m_attached;
		recompute_active();
		if (m_attached)
			std::fprintf(stderr, "[s23tap] active; summary every %u scene(s)\n", m_every);
	}

	bool     m_inited = false;
	bool     m_attached = false;
	uint32_t m_every = 1;

	// Per-scene, reset in begin().
	int      m_variant = 0;
	uint32_t m_polys = 0, m_model = 0, m_direct = 0, m_immediate = 0, m_sprite = 0;
	uint32_t m_stencil = 0, m_shade = 0, m_pfade = 0, m_colorfade = 0, m_blend = 0, m_alpha = 0;
	float    m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0;
	int32_t  m_min_zkey = 0, m_max_zkey = 0;

	// Run-level, never reset.
	uint32_t m_scenes = 0;
	uint32_t m_scenes_with_geom = 0;
	uint64_t m_run_polys = 0;
	uint32_t m_max_polys = 0;
};

tap g_tap;

} // anonymous namespace

// The seam plumbing. Each site drives the 23-1 diagnostic tap (harmless when it is not attached) and,
// when the 23-2 GPU path is capturing, the record consumer in s23_geom.cpp. The tap and the record are
// independent: a run may have either, both, or (the common case) neither.
void frame_begin(int variant)
{
	g_tap.begin(variant);
	if (g_gpu_capture)
		record_begin(variant);

	// A fresh frame has no overlay until capture_over() runs at the end of screen_update. Cleared here so
	// a frame that captures none (nothing owns the 3D on the GPU) presents the passthrough, not a stale
	// overlay. capture_over() runs after frame_end(), so the reset never races the fill.
	over_forget();
}

void frame_end()
{
	g_tap.end();
	if (g_gpu_capture)
		record_end();
}

void submit(poly const &p)
{
	g_tap.on_poly(p);
	if (g_gpu_capture)
		record_poly(p);
}

void set_gpu(bool on)
{
	g_gpu_capture = on;
	detail::g_sw_owns_3d = !on;     // the GPU owns the 3D once capture is on, so software stops drawing
	recompute_active();
}

void set_no_3d()
{
	// M2VK_NO_3D: neither draws. Capture off (the GPU draws nothing) AND sw_owns_3d off (render_flush
	// skips its 3D walk), leaving just the 2D layers — the background reference.
	g_gpu_capture = false;
	detail::g_sw_owns_3d = false;
	recompute_active();
}

bool gpu_owns_3d()
{
	return g_gpu_capture;
}

// The texture ROM pointers (23-3). The driver hands these over from the render_flush bracket; the
// consumer (s23_geom.cpp) reads them through get_texture_rom(). A plain latched struct — the ROM-derived
// pointers are stable, the palette contents change but its pointer does not.
namespace {
texture_rom g_texture_rom;
} // anonymous namespace

void set_texture_rom(texture_rom const &t)
{
	g_texture_rom = t;
}

texture_rom const &get_texture_rom()
{
	return g_texture_rom;
}

// The 2D-over overlay buffer (23-5). File-scope, mirroring s22_seam.cpp: the emulation thread fills it in
// capture_over(); the frontend reads it in present_frame while that thread is parked on the baton, the
// same guarantee the poly record and texture pointers rest on, so no lock is needed.
namespace {
std::vector<uint32_t> g_over;
int  g_over_w = 0;
int  g_over_h = 0;
bool g_over_valid = false;

// system23_2d_overlay ("2D Overlay"), deferred to 23-7's option set. On by default (the accurate
// sandwich); off leaves the 3D above the 2D background. M2VK_S23_HUD overrides it (0 = force off). Read in
// over_pixels() below, which returns nullptr when off, collapsing the whole OVER draw.
bool g_option_hud = true;
int  g_env_hud = -2;   // -2 = not read; -1 = no switch; 0/1 = pinned
bool hud_enabled()
{
	if (g_env_hud == -2)
	{
		char const *const env = std::getenv("M2VK_S23_HUD");
		g_env_hud = (env == nullptr) ? -1 : ((std::atoi(env) != 0) || (*env == '\0')) ? 1 : 0;
	}
	return (g_env_hud < 0) ? g_option_hud : (g_env_hud != 0);
}
}

void set_option_hud(bool on)
{
	g_option_hud = on;
}

uint32_t *over_begin(int width, int height)
{
	g_over_valid = false;
	if ((width <= 0) || (height <= 0))
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
	if (!g_over_valid || !hud_enabled())
		return nullptr;
	width = g_over_w;
	height = g_over_h;
	return g_over.data();
}

void over_forget()
{
	g_over_valid = false;
}

} // namespace s23
