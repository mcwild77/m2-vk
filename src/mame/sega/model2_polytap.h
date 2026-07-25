// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 polygon tap — diagnostic instrumentation for the hardware renderer port.

    Header-only, and compiled in only when M2VK_POLYTAP is defined; the stock build is untouched.
    Build with: make SOURCES=src/mame/sega/model2.cpp SUBTARGET=model2 ARCHOPTS_CXX=-DM2VK_POLYTAP

    Purpose: observe the exact polygon stream the hardware renderer will consume. The tap sits at
    the point in model2_renderer::model2_3d_render() where the geometry engine has finished
    projecting to screen space and every texture/lighting parameter has been resolved into
    m2_poly_extra_data, i.e. immediately before the software scanline rasterizer is dispatched.

    What it reports:
      - per-frame polygon counts, split by the renderer class the hardware would select
        (solid/textured x opaque/translucent), plus a vertex-count histogram
      - screen-space and 1/z ranges, sort-bucket range, window count
      - the calling thread, so that single-threaded submission can be relied upon
      - optionally, a full record of every polygon in one frame, dumped to a text file

    Environment variables (all optional):
      M2VK_POLYTAP_EVERY=N      print a summary line every N rendered frames (default 1, 0 = never)
      M2VK_POLYTAP_DUMP=N       dump every polygon of rendered frame N (1-based) to a file
      M2VK_POLYTAP_DUMP_FILE=P  path for that dump (default "polytap_frame<N>.txt")

    Dump format, one record per line, tab-separated key=value fields:
      P  poly-level fields (draw order, sort bucket, window, texture header, resolved parameters)
      V  one line per vertex, following its P line: screen x/y, 1/z, u/z, v/z

*********************************************************************************************************************************/
#ifndef MAME_SEGA_MODEL2_POLYTAP_H
#define MAME_SEGA_MODEL2_POLYTAP_H

#pragma once

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace model2_polytap {

// Vertex parameter slots, as used by model2_v.cpp. Spelled out rather than relying on that file's
// pz/pu/pv macros so this header stands alone.
enum : int { PARAM_Z = 0, PARAM_U = 1, PARAM_V = 2 };

class tap
{
public:
	static tap &instance()
	{
		static tap s_tap;
		return s_tap;
	}

	// submitted = raster->poly_list_index, the number of polygons the geometry engine put in the
	// list this frame. Comparing it against the number of tapped polygons proves the tap sees the
	// whole stream and not a subset.
	void frame_begin(u32 submitted)
	{
		m_frame++;
		m_polys = 0;
		m_submitted = submitted;
		std::fill(std::begin(m_by_renderer), std::end(m_by_renderer), 0);
		std::fill(std::begin(m_by_verts), std::end(m_by_verts), 0);
		m_min_x = m_min_y = 1e30f;
		m_max_x = m_max_y = -1e30f;
		m_min_rz = 1e30f;
		m_max_rz = -1e30f;
		m_min_bucket = 0xffff;
		m_max_bucket = 0;
		m_windows = 0;

		if (m_dump_frame != 0 && m_frame == m_dump_frame)
		{
			char path[256];
			char const *const env = std::getenv("M2VK_POLYTAP_DUMP_FILE");
			if (env != nullptr)
				std::snprintf(path, sizeof(path), "%s", env);
			else
				std::snprintf(path, sizeof(path), "polytap_frame%u.txt", m_dump_frame);
			m_dump = std::fopen(path, "w");
			if (m_dump != nullptr)
				std::fprintf(m_dump, "# Model 2 polygon dump, rendered frame %u\n", m_frame);
			else
				std::fprintf(stderr, "[polytap] could not open %s for writing\n", path);
		}
	}

	// Called with the polygon exactly as the software rasterizer is about to receive it: vertices
	// projected to screen space, and for textured polygons p[0] already reciprocated to 1/z with
	// u,v premultiplied by it.
	void poly(model2_state::polygon const &poly, m2_poly_extra_data const &extra, u8 renderer, rectangle const &vp)
	{
		check_thread();

		m_polys++;
		m_by_renderer[renderer & 3]++;
		if (poly.num_vertices >= 3 && poly.num_vertices <= 8)
			m_by_verts[poly.num_vertices - 3]++;
		if (poly.z < m_min_bucket) m_min_bucket = poly.z;
		if (poly.z > m_max_bucket) m_max_bucket = poly.z;
		if (poly.window >= m_windows) m_windows = poly.window + 1;

		for (int i = 0; i < poly.num_vertices; i++)
		{
			poly_vertex const &v = poly.v[i];
			if (v.x < m_min_x) m_min_x = v.x;
			if (v.x > m_max_x) m_max_x = v.x;
			if (v.y < m_min_y) m_min_y = v.y;
			if (v.y > m_max_y) m_max_y = v.y;
			float const rz = v.p[PARAM_Z];
			if (rz < m_min_rz) m_min_rz = rz;
			if (rz > m_max_rz) m_max_rz = rz;
		}

		if (m_dump == nullptr)
			return;

		std::fprintf(m_dump,
				"P\tseq=%u\tverts=%u\tbucket=%u\twindow=%u\trenderer=%u\ttex=%u\ttrans=%u"
				"\thdr=%04x,%04x,%04x,%04x\tluma=%u\tlumabase=%u\tcolorbase=%u\tchecker=%u\ttexlod=%d"
				"\tvp=%d,%d,%d,%d\tclip=%d,%d,%d,%d\tcenter=%d,%d"
				"\ttexsize=%ux%u\ttexpos=%u,%u\twrap=%u,%u\tmirror=%u,%u\tsheet1=%u"
				"\tutex=%u\tutexminlod=%u\tutexpos=%u,%u\n",
				m_polys - 1, poly.num_vertices, poly.z, poly.window,
				renderer, (renderer >> 1) & 1, renderer & 1,
				poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3],
				poly.luma, extra.lumabase, extra.colorbase, extra.checker, extra.texlod,
				poly.viewport[0], poly.viewport[1], poly.viewport[2], poly.viewport[3],
				vp.left(), vp.top(), vp.right(), vp.bottom(),
				poly.center[0], poly.center[1],
				(renderer & 2) ? extra.texwidth : 0u, (renderer & 2) ? extra.texheight : 0u,
				(renderer & 2) ? extra.texx : 0u, (renderer & 2) ? extra.texy : 0u,
				(renderer & 2) ? extra.texwrapx : 0u, (renderer & 2) ? extra.texwrapy : 0u,
				(renderer & 2) ? extra.texmirrorx : 0u, (renderer & 2) ? extra.texmirrory : 0u,
				(renderer & 2) ? u32((poly.texheader[2] & 0x1000) ? 1 : 0) : 0u,
				(renderer & 2) ? extra.utex : 0u, (renderer & 2) ? extra.utexminlod : 0u,
				(renderer & 2) ? extra.utexx : 0u, (renderer & 2) ? extra.utexy : 0u);

		for (int i = 0; i < poly.num_vertices; i++)
		{
			poly_vertex const &v = poly.v[i];
			std::fprintf(m_dump, "V\t%d\tx=%.6f\ty=%.6f\trz=%.9g\tuz=%.9g\tvz=%.9g\n",
					i, v.x, v.y, v.p[PARAM_Z], v.p[PARAM_U], v.p[PARAM_V]);
		}
	}

	void frame_end()
	{
		if (m_dump != nullptr)
		{
			std::fprintf(m_dump, "# %u polygons tapped, %u submitted by the geometry engine\n",
					m_polys, m_submitted);
			std::fclose(m_dump);
			m_dump = nullptr;
			std::fprintf(stderr, "[polytap] dumped frame %u (%u polygons)\n", m_frame, m_polys);
		}

		if (m_every != 0 && (m_frame % m_every) == 0)
		{
			std::fprintf(stderr,
					"[polytap] frame %6u  polys %5u/%5u sent  solid %4u/%4u trans  tex %4u/%4u trans"
					"  verts 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u"
					"  x %.1f..%.1f  y %.1f..%.1f  1/z %.6g..%.6g  bucket %u..%u  windows %u%s\n",
					m_frame, m_polys, m_submitted,
					m_by_renderer[0], m_by_renderer[1], m_by_renderer[2], m_by_renderer[3],
					m_by_verts[0], m_by_verts[1], m_by_verts[2], m_by_verts[3], m_by_verts[4], m_by_verts[5],
					m_min_x, m_max_x, m_min_y, m_max_y, m_min_rz, m_max_rz,
					m_min_bucket, m_max_bucket, m_windows,
					m_multithreaded ? "  *** MULTIPLE SUBMITTING THREADS ***" : "");
		}
	}

private:
	tap()
	{
		char const *const every = std::getenv("M2VK_POLYTAP_EVERY");
		if (every != nullptr)
			m_every = u32(std::strtoul(every, nullptr, 10));

		char const *const dump = std::getenv("M2VK_POLYTAP_DUMP");
		if (dump != nullptr)
			m_dump_frame = u32(std::strtoul(dump, nullptr, 10));

		std::fprintf(stderr, "[polytap] active; summary every %u frame(s), dump frame %u\n",
				m_every, m_dump_frame);
	}

	void check_thread()
	{
		std::thread::id const self = std::this_thread::get_id();
		if (!m_have_thread)
		{
			m_thread = self;
			m_have_thread = true;
		}
		else if (self != m_thread)
		{
			m_multithreaded = true;
		}
	}

	u32              m_frame = 0;
	u32              m_polys = 0;
	u32              m_submitted = 0;
	u32              m_every = 1;
	u32              m_dump_frame = 0;
	u32              m_by_renderer[4] = { 0, 0, 0, 0 };
	u32              m_by_verts[6] = { 0, 0, 0, 0, 0, 0 };
	float            m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0;
	float            m_min_rz = 0, m_max_rz = 0;
	u32              m_min_bucket = 0, m_max_bucket = 0;
	u32              m_windows = 0;
	FILE *           m_dump = nullptr;
	std::thread::id  m_thread;
	bool             m_have_thread = false;
	bool             m_multithreaded = false;
};

inline void frame_begin(u32 submitted) { tap::instance().frame_begin(submitted); }
inline void frame_end() { tap::instance().frame_end(); }
inline void submit(model2_state::polygon const &poly, m2_poly_extra_data const &extra, u8 renderer, rectangle const &vp)
{
	tap::instance().poly(poly, extra, renderer, vp);
}

} // namespace model2_polytap

#endif // MAME_SEGA_MODEL2_POLYTAP_H
