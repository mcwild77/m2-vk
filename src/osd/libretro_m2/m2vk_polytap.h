// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Sega Model 2 polygon tap — diagnostic instrumentation for the hardware renderer port.

    One consumer behind the sink (m2vk_sink.h), which is where the polygon stream is intercepted;
    this file only reports on what arrives. It knows nothing about the driver: everything it prints
    comes out of m2vk::poly.

    What it reports:
      - per-frame polygon counts, split by the renderer class the hardware would select
        (solid/textured x opaque/translucent), plus a vertex-count histogram
      - screen-space and depth ranges, sort-bucket range, window count
      - the submitting thread, so that single-threaded submission can be relied upon
      - a run-level feature survey at teardown, one key=value per line, so a sweep across the ROM
        set can aggregate it
      - optionally, a full record of every polygon in one frame, dumped to a text file

    Environment variables. The tap is attached only if at least one of them is set, so a normal
    core run costs nothing; M2VK_POLYTAP=0 forces it off whatever else is set.
      M2VK_POLYTAP=1             attach with the defaults below
      M2VK_POLYTAP_EVERY=N       print a summary line every N rendered frames (default 1, 0 = never)
      M2VK_POLYTAP_DUMP=N        dump every polygon of rendered frame N (1-based) to a file
      M2VK_POLYTAP_DUMP_FILE=P   path for that dump (default "polytap_frame<N>.txt")
      M2VK_POLYTAP_SUMMARY=P     path for the run-level survey (default stderr)
      M2VK_POLYTAP_TAG=S         label carried in the survey's first line

    Dump format, one record per line, tab-separated key=value fields:
      P  poly-level fields (draw order, sort bucket, window, texture header, resolved parameters)
      V  one line per vertex, following its P line: screen x/y, 1/z, u/z, v/z

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_POLYTAP_H
#define MAME_OSD_LIBRETRO_M2_M2VK_POLYTAP_H

#pragma once

#include "m2vk_sink.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>

namespace m2vk {

class polytap : public consumer
{
public:
	// Nothing is attached unless the run is being observed; see the environment variables above.
	static bool wanted()
	{
		char const *const on = std::getenv("M2VK_POLYTAP");
		if (on != nullptr)
			return std::strtoul(on, nullptr, 10) != 0;

		static char const *const vars[] = {
				"M2VK_POLYTAP_EVERY", "M2VK_POLYTAP_DUMP", "M2VK_POLYTAP_DUMP_FILE",
				"M2VK_POLYTAP_SUMMARY", "M2VK_POLYTAP_TAG" };
		for (char const *const var : vars)
		{
			if (std::getenv(var) != nullptr)
				return true;
		}
		return false;
	}

	polytap()
	{
		char const *const every = std::getenv("M2VK_POLYTAP_EVERY");
		if (every != nullptr)
			m_every = uint32_t(std::strtoul(every, nullptr, 10));

		char const *const dump = std::getenv("M2VK_POLYTAP_DUMP");
		if (dump != nullptr)
			m_dump_frame = uint32_t(std::strtoul(dump, nullptr, 10));

		std::fprintf(stderr, "[polytap] active; summary every %u frame(s), dump frame %u\n",
				m_every, m_dump_frame);
	}

	virtual ~polytap()
	{
		if (m_dump != nullptr)
			std::fclose(m_dump);
	}

	// submitted = raster->poly_list_index, the number of polygons the geometry engine put in the
	// list this frame. Comparing it against the number of tapped polygons proves the tap sees the
	// whole stream and not a subset.
	virtual void frame_begin(uint32_t submitted) override
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
		m_pages_frame.clear();
		m_prev_key = ~uint32_t(0);

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

	virtual void submit(poly const &poly) override
	{
		check_thread();

		m_polys++;
		m_by_renderer[poly.renderer & 3]++;
		if (poly.num_verts >= 3 && poly.num_verts <= 8)
			m_by_verts[poly.num_verts - 3]++;
		if (poly.bucket < m_min_bucket) m_min_bucket = poly.bucket;
		if (poly.bucket > m_max_bucket) m_max_bucket = poly.bucket;
		if (poly.window >= m_windows) m_windows = poly.window + 1;

		for (int i = 0; i < poly.num_verts; i++)
		{
			vertex const &v = poly.v[i];
			if (v.x < m_min_x) m_min_x = v.x;
			if (v.x > m_max_x) m_max_x = v.x;
			if (v.y < m_min_y) m_min_y = v.y;
			if (v.y > m_max_y) m_max_y = v.y;
			if (v.rz < m_min_rz) m_min_rz = v.rz;
			if (v.rz > m_max_rz) m_max_rz = v.rz;

			// A depth value the Vulkan path would have to clamp: not finite, or so large that z has
			// collapsed to ~0. The software rasterizer tolerates these silently.
			if (!std::isfinite(v.rz) || v.rz > 1e6f)
				m_run_bad_depth++;
		}

		// Run-level accumulation.
		m_run_polys++;
		m_run_by_renderer[poly.renderer & 3]++;
		if (poly.num_verts >= 3 && poly.num_verts <= 8)
			m_run_by_verts[poly.num_verts - 3]++;
		if (poly.num_verts > 5)
			m_run_verts_over5++;
		m_run_lumabase.insert(poly.lumabase);
		m_run_colorbase.insert(poly.colorbase);
		if (poly.checker)
			m_run_checker++;

		// The P4 metric: does this polygon share a sort bucket with the one drawn immediately before
		// it, inside the same window? If so a depth buffer has a tie to break, and only draw order
		// can break it correctly.
		uint32_t const key = (uint32_t(poly.window) << 16) | poly.bucket;
		if (key == m_prev_key)
			m_run_tie_polys++;
		m_prev_key = key;

		if (poly.renderer & 2)
		{
			if (poly.utex)
				m_run_utex++;
			uint64_t const page =
					(uint64_t(poly.sheet) << 60) |
					(uint64_t(poly.texx) << 44) | (uint64_t(poly.texy) << 28) |
					(uint64_t(poly.texwidth) << 14) | uint64_t(poly.texheight);
			m_pages_frame.insert(page);
			m_run_pages.insert(page);
		}

		if (m_dump == nullptr)
			return;

		std::fprintf(m_dump,
				"P\tseq=%u\tverts=%u\tbucket=%u\twindow=%u\trenderer=%u\ttex=%u\ttrans=%u"
				"\thdr=%04x,%04x,%04x,%04x\tluma=%u\tlumabase=%u\tcolorbase=%u\tchecker=%u\ttexlod=%d"
				"\tvp=%d,%d,%d,%d\tclip=%d,%d,%d,%d\tcenter=%d,%d"
				"\ttexsize=%ux%u\ttexpos=%u,%u\twrap=%u,%u\tmirror=%u,%u\tsheet1=%u"
				"\tutex=%u\tutexminlod=%u\tutexpos=%u,%u\n",
				m_polys - 1, poly.num_verts, poly.bucket, poly.window,
				poly.renderer, (poly.renderer >> 1) & 1, poly.renderer & 1,
				poly.texheader[0], poly.texheader[1], poly.texheader[2], poly.texheader[3],
				poly.luma, poly.lumabase, poly.colorbase, poly.checker, poly.texlod,
				poly.viewport[0], poly.viewport[1], poly.viewport[2], poly.viewport[3],
				poly.clip[0], poly.clip[1], poly.clip[2], poly.clip[3],
				poly.center[0], poly.center[1],
				poly.texwidth, poly.texheight, poly.texx, poly.texy,
				poly.texwrapx, poly.texwrapy, poly.texmirrorx, poly.texmirrory,
				poly.sheet, poly.utex, poly.utexminlod, poly.utexx, poly.utexy);

		for (int i = 0; i < poly.num_verts; i++)
		{
			vertex const &v = poly.v[i];
			std::fprintf(m_dump, "V\t%d\tx=%.6f\ty=%.6f\trz=%.9g\tuz=%.9g\tvz=%.9g\n",
					i, v.x, v.y, v.rz, v.uz, v.vz);
		}
	}

	virtual void frame_end() override
	{
		m_run_frames++;
		if (m_polys > m_run_max_polys) m_run_max_polys = m_polys;
		if (m_polys < m_run_min_polys) m_run_min_polys = m_polys;
		if (m_polys != m_submitted) m_run_count_mismatch++;
		if (m_pages_frame.size() > m_run_max_pages_frame) m_run_max_pages_frame = uint32_t(m_pages_frame.size());
		if (m_min_bucket < m_run_min_bucket) m_run_min_bucket = m_min_bucket;
		if (m_max_bucket > m_run_max_bucket) m_run_max_bucket = m_max_bucket;
		if (m_windows > m_run_max_windows) m_run_max_windows = m_windows;
		if (m_min_rz < m_run_min_rz) m_run_min_rz = m_min_rz;
		if (m_max_rz > m_run_max_rz) m_run_max_rz = m_max_rz;

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

	// Run-level feature survey, emitted once as the machine exits. Written to M2VK_POLYTAP_SUMMARY
	// if set, otherwise stderr. One key=value per line so a sweep across the ROM set can aggregate
	// it. A game that reaches no 3D at all says so rather than saying nothing — the sink exists for
	// the whole run, so this is reached either way.
	virtual void run_end() override
	{
		FILE *out = stderr;
		char const *const path = std::getenv("M2VK_POLYTAP_SUMMARY");
		if (path != nullptr)
		{
			FILE *const f = std::fopen(path, "w");
			if (f != nullptr)
				out = f;
		}

		char const *const tag = std::getenv("M2VK_POLYTAP_TAG");
		std::fprintf(out, "tag=%s\n", (tag != nullptr) ? tag : "unknown");
		std::fprintf(out, "frames=%u\n", m_run_frames);
		if (m_run_frames == 0)
		{
			// No rendered frames at all: the game never reached 3D in the time allowed, or it is
			// sitting on a screen the rasterizer is not involved in.
			std::fprintf(out, "no_3d=1\n");
			if (out != stderr)
				std::fclose(out);
			return;
		}

		std::fprintf(out, "polys_total=%llu\n", (unsigned long long)m_run_polys);
		std::fprintf(out, "polys_min=%u\n", m_run_min_polys);
		std::fprintf(out, "polys_max=%u\n", m_run_max_polys);
		std::fprintf(out, "polys_mean=%.1f\n", double(m_run_polys) / double(m_run_frames));
		std::fprintf(out, "solid=%llu\n", (unsigned long long)m_run_by_renderer[0]);
		std::fprintf(out, "solid_trans=%llu\n", (unsigned long long)m_run_by_renderer[1]);
		std::fprintf(out, "tex=%llu\n", (unsigned long long)m_run_by_renderer[2]);
		std::fprintf(out, "tex_trans=%llu\n", (unsigned long long)m_run_by_renderer[3]);
		for (int i = 0; i < 6; i++)
			std::fprintf(out, "verts%d=%llu\n", i + 3, (unsigned long long)m_run_by_verts[i]);
		std::fprintf(out, "verts_over5=%llu\n", (unsigned long long)m_run_verts_over5);
		std::fprintf(out, "microtexture=%llu\n", (unsigned long long)m_run_utex);
		std::fprintf(out, "checker=%llu\n", (unsigned long long)m_run_checker);
		std::fprintf(out, "tie_polys=%llu\n", (unsigned long long)m_run_tie_polys);
		std::fprintf(out, "tie_pct=%.1f\n", 100.0 * double(m_run_tie_polys) / double(m_run_polys));
		std::fprintf(out, "bad_depth=%llu\n", (unsigned long long)m_run_bad_depth);
		std::fprintf(out, "rz_min=%.9g\n", m_run_min_rz);
		std::fprintf(out, "rz_max=%.9g\n", m_run_max_rz);
		std::fprintf(out, "bucket_min=%u\n", m_run_min_bucket);
		std::fprintf(out, "bucket_max=%u\n", m_run_max_bucket);
		std::fprintf(out, "windows_max=%u\n", m_run_max_windows);
		std::fprintf(out, "pages_run=%u\n", uint32_t(m_run_pages.size()));
		std::fprintf(out, "pages_frame_max=%u\n", m_run_max_pages_frame);
		std::fprintf(out, "lumabase_distinct=%u\n", uint32_t(m_run_lumabase.size()));
		std::fprintf(out, "colorbase_distinct=%u\n", uint32_t(m_run_colorbase.size()));
		std::fprintf(out, "count_mismatch_frames=%u\n", m_run_count_mismatch);
		std::fprintf(out, "multithreaded=%u\n", m_multithreaded ? 1u : 0u);

		if (out != stderr)
			std::fclose(out);
	}

private:
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

	uint32_t         m_frame = 0;
	uint32_t         m_polys = 0;
	uint32_t         m_submitted = 0;
	uint32_t         m_every = 1;
	uint32_t         m_dump_frame = 0;
	uint32_t         m_by_renderer[4] = { 0, 0, 0, 0 };
	uint32_t         m_by_verts[6] = { 0, 0, 0, 0, 0, 0 };
	float            m_min_x = 0, m_max_x = 0, m_min_y = 0, m_max_y = 0;
	float            m_min_rz = 0, m_max_rz = 0;
	uint32_t         m_min_bucket = 0, m_max_bucket = 0;
	uint32_t         m_windows = 0;
	FILE *           m_dump = nullptr;
	std::thread::id  m_thread;
	bool             m_have_thread = false;
	bool             m_multithreaded = false;

	// Per-frame, reset in frame_begin.
	std::set<uint64_t> m_pages_frame;
	uint32_t         m_prev_key = ~uint32_t(0);

	// Run-level, never reset.
	uint32_t         m_run_frames = 0;
	uint64_t         m_run_polys = 0;
	uint32_t         m_run_min_polys = ~uint32_t(0);
	uint32_t         m_run_max_polys = 0;
	uint64_t         m_run_by_renderer[4] = { 0, 0, 0, 0 };
	uint64_t         m_run_by_verts[6] = { 0, 0, 0, 0, 0, 0 };
	uint64_t         m_run_verts_over5 = 0;
	uint64_t         m_run_utex = 0;
	uint64_t         m_run_checker = 0;
	uint64_t         m_run_tie_polys = 0;
	uint64_t         m_run_bad_depth = 0;
	float            m_run_min_rz = 1e30f;
	float            m_run_max_rz = -1e30f;
	uint32_t         m_run_min_bucket = 0xffff;
	uint32_t         m_run_max_bucket = 0;
	uint32_t         m_run_max_windows = 0;
	uint32_t         m_run_max_pages_frame = 0;
	uint32_t         m_run_count_mismatch = 0;
	std::set<uint64_t> m_run_pages;
	std::set<uint32_t> m_run_lumabase;
	std::set<uint32_t> m_run_colorbase;
};

} // namespace m2vk

#endif // MAME_OSD_LIBRETRO_M2_M2VK_POLYTAP_H
