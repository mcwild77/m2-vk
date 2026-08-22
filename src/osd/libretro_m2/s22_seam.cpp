// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Namco (Super) System 22 renderer seam — the S1 diagnostic tap.

    See s22_seam.h for the seam and the data shape. This file is the whole of the plumbing plus one
    consumer: a diagnostic tap that counts the primitive stream and never draws anything. It exists to
    answer the S1 question — is the seam actually wired? — because a passthrough that draws nothing
    passes the byte-identical test vacuously if the hooks are never reached. Run with M2VK_S22TAP=1 and
    the per-frame counts prove the quad, sprite and frame hooks fire; run with it unset and the seam is
    inert (g_active stays false, every submit() returns at the active() gate) and the output is
    byte-identical to the S0 software baseline.

    Everything here runs on the emulation thread: poly_manager farms scanlines out to workers, but
    render_scene() and the drawquad/render_sprite calls it makes are only reached from the driver's own
    screen update.

    Environment variables (the tap is attached only if one is set):
      M2VK_S22TAP=1          attach with the defaults below
      M2VK_S22TAP_EVERY=N    print a summary line every N scenes that carry geometry (default 1)

*********************************************************************************************************************************/

#include "s22_seam.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace s22 {

namespace detail {

bool g_active = false;
bool g_sw_owns_3d = true;

} // namespace detail

namespace {

// The two independent reasons active() is true: the S1 diagnostic tap (env) and the S2 GPU path.
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

// The S1 tap. One object with static storage duration, so it always exists and its destructor emits a
// run-level summary at process exit (retrohost exits normally, so the destructor runs). The per-frame
// lines are the real S1 evidence; the summary is a convenience.
class tap
{
public:
	void begin(int variant)
	{
		ensure_init();
		if (!m_attached)
			return;

		m_variant = variant;
		m_quads = m_sprites = m_sprite_tiles = 0;
		m_quads_tex = m_quads_alpha = m_quads_ss22 = 0;
		m_quads_fog = m_quads_zfog = 0; m_max_fog = 0;
		m_min_x = m_min_y = m_min_ooz = 1e30f;
		m_max_x = m_max_y = m_max_ooz = -1e30f;
	}

	void on_quad(quad const &q)
	{
		m_quads++;
		if (q.textured)      m_quads_tex++;
		if (q.alpha_enabled) m_quads_alpha++;
		if (q.ss22)          m_quads_ss22++;
		if (q.fogfactor)     m_quads_fog++;
		if (q.zfog_enabled)  m_quads_zfog++;
		if (q.fogfactor > m_max_fog) m_max_fog = q.fogfactor;

		const int n = (q.num_verts < 6) ? q.num_verts : 6;
		for (int i = 0; i < n; i++)
		{
			if (q.x[i]   < m_min_x)   m_min_x   = q.x[i];
			if (q.x[i]   > m_max_x)   m_max_x   = q.x[i];
			if (q.y[i]   < m_min_y)   m_min_y   = q.y[i];
			if (q.y[i]   > m_max_y)   m_max_y   = q.y[i];
			if (q.ooz[i] < m_min_ooz) m_min_ooz = q.ooz[i];
			if (q.ooz[i] > m_max_ooz) m_max_ooz = q.ooz[i];
		}
	}

	void on_sprite(sprite const &s)
	{
		m_sprites++;
		m_sprite_tiles += s.tiles;
	}

	void end()
	{
		if (!m_attached)
			return;

		m_scenes++;
		m_run_quads += m_quads;
		m_run_sprites += m_sprites;
		m_run_sprite_tiles += m_sprite_tiles;
		if (m_quads > m_max_quads)       m_max_quads = m_quads;
		if (m_sprites > m_max_sprites)   m_max_sprites = m_sprites;
		if (m_quads || m_sprites)        m_scenes_with_geom++;

		if (m_every != 0 && (m_scenes % m_every) == 0 && (m_quads || m_sprites))
		{
			std::fprintf(stderr,
					"[s22tap] scene %6u  %s  quads %4u (tex %4u alpha %4u ss22 %4u fog %4u/zfog %4u max %3u)"
					"  sprites %3u (tiles %4u)  x %.0f..%.0f  y %.0f..%.0f  1/z %.5g..%.5g\n",
					m_scenes, m_variant ? "ss22" : "s22 ",
					m_quads, m_quads_tex, m_quads_alpha, m_quads_ss22,
					m_quads_fog, m_quads_zfog, m_max_fog,
					m_sprites, m_sprite_tiles,
					double(m_min_x), double(m_max_x), double(m_min_y), double(m_max_y),
					double(m_min_ooz), double(m_max_ooz));
		}
	}

	~tap()
	{
		if (!m_attached)
			return;

		std::fprintf(stderr,
				"[s22tap] run end: %u scenes (%u with geometry)  quads %llu (max/scene %u)"
				"  sprites %llu (tiles %llu, max/scene %u)\n",
				m_scenes, m_scenes_with_geom,
				(unsigned long long)m_run_quads, m_max_quads,
				(unsigned long long)m_run_sprites, (unsigned long long)m_run_sprite_tiles,
				m_max_sprites);
	}

private:
	void ensure_init()
	{
		if (m_inited)
			return;
		m_inited = true;

		char const *const every = std::getenv("M2VK_S22TAP_EVERY");
		m_attached = env_on("M2VK_S22TAP") || (every != nullptr);
		if (every != nullptr)
			m_every = uint32_t(std::strtoul(every, nullptr, 10));

		g_tap_attached = m_attached;
		recompute_active();
		if (m_attached)
			std::fprintf(stderr, "[s22tap] active; summary every %u scene(s)\n", m_every);
	}

	bool     m_inited = false;
	bool     m_attached = false;
	uint32_t m_every = 1;

	// Per-scene, reset in begin().
	int      m_variant = 0;
	uint32_t m_quads = 0, m_sprites = 0, m_sprite_tiles = 0;
	uint32_t m_quads_tex = 0, m_quads_alpha = 0, m_quads_ss22 = 0;
	uint32_t m_quads_fog = 0, m_quads_zfog = 0, m_max_fog = 0;
	float    m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0, m_min_ooz = 0, m_max_ooz = 0;

	// Run-level, never reset.
	uint32_t m_scenes = 0;
	uint32_t m_scenes_with_geom = 0;
	uint64_t m_run_quads = 0, m_run_sprites = 0, m_run_sprite_tiles = 0;
	uint32_t m_max_quads = 0, m_max_sprites = 0;
};

tap g_tap;

} // anonymous namespace

// The seam plumbing. Each site drives the S1 diagnostic tap (harmless when it is not attached) and,
// when the S2 GPU path is capturing, the record consumer in s22_geom.cpp. The tap and the record are
// independent: a run may have either, both, or (the common case) neither.
void frame_begin(int variant)
{
	g_tap.begin(variant);
	if (g_gpu_capture)
		record_begin(variant);

	// A fresh frame has no overlay until capture_over() runs at the end of screen_update. Cleared here
	// so a frame that captures none (nothing owns the 3D on the GPU) presents the passthrough, not a
	// stale overlay. capture_over() runs after frame_end(), so the reset never races the fill.
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

void submit(sprite const &s)
{
	g_tap.on_sprite(s);
	// Sprites are not on the GPU path yet (S2 does quads first; the ROMs present emit none).
}

void set_gpu(bool on)
{
	g_gpu_capture = on;
	detail::g_sw_owns_3d = !on;     // the GPU owns the 3D once capture is on, so software stops drawing
	recompute_active();
}

// The texture pointers the driver hands over. File-scope rather than in s22_geom so the tap build (no
// renderer) still links; the pointers are stable, so re-storing them every frame costs six writes.
namespace { texture_ram g_texram; }

void set_texture_ram(uint16_t const *ttmap, uint8_t const *ttattr, uint8_t const *ttdata,
		uint8_t const *ayx, uint32_t const *palette, uint8_t const *gamma)
{
	g_texram.ttmap = ttmap;
	g_texram.ttattr = ttattr;
	g_texram.ttdata = ttdata;
	g_texram.ayx = ayx;
	g_texram.palette = palette;
	g_texram.gamma = gamma;
}

texture_ram const &get_texture_ram()
{
	return g_texram;
}

// The per-frame globals of the SS22 shading tail. File-scope for the same reason g_texram is: the tap
// build has no renderer and must still link (inert). The czram pointers are stable, their contents
// change each frame; the scalars change each frame. The frontend reads this in geom_upload/geom_draw
// while the emulation thread is parked on the baton, so no lock is needed.
namespace { shading_globals g_shading; }

void set_shading_state(shading_globals const &g)
{
	g_shading = g;
}

shading_globals const &get_shading_globals()
{
	return g_shading;
}

// The 2D-over overlay buffer. File-scope rather than in s22_geom for the same reason g_texram is: the
// tap build has no renderer, and this must still link there (inert). The emulation thread fills it in
// capture_over(); the frontend reads it in present_frame while that thread is parked on the baton, the
// same guarantee the quad record and texture pointers rest on, so no lock is needed.
namespace {
std::vector<uint32_t> g_over;
int  g_over_w = 0;
int  g_over_h = 0;
bool g_over_valid = false;
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

} // namespace s22
