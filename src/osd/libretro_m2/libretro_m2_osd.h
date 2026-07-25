// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro OSD — the osd_common_t subclass.

    Subclasses osd_common_t the way sdl_osd_interface does, but owns no window, no monitor and no
    platform backend. The frontend owns the display; this class exists to
      - hold a render_target so the emulated screen keeps rendering (video_manager only invokes
        SCREEN_UPDATE for a screen something is looking at),
      - hand the finished frame and the frame's audio to the libretro entry points, and
      - act as the per-frame rendezvous: update() is called once per emulated frame, which is
        exactly the granularity retro_run() has to advance.

    On not needing to patch osdobj_common.h: init_subsystems() is overridden so the private module
    manager is never touched. Every pointer this class has to populate is already protected, and the
    one private member (m_font_module) is reachable only from font_alloc() and get_font_families(),
    both of which are overridden here. Modules that are needed come straight from their exported
    factories — a module_type is just a std::unique_ptr<osd_module>(*)(), so it can be called
    directly without the manager. See devnotes/p1-libretro-core.md.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_OSD_H
#define MAME_OSD_LIBRETRO_M2_OSD_H

#pragma once

#include "modules/lib/osdobj_common.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class libretro_m2_input;
class render_target;


// no extra options yet; the core's own knobs arrive as libretro core options and are
// translated into a MAME command line by retro_entry.cpp
class libretro_m2_options : public osd_options
{
public:
	libretro_m2_options() : osd_options() { }
};


class libretro_m2_osd_interface : public osd_common_t
{
public:
	libretro_m2_osd_interface(libretro_m2_options &options);
	virtual ~libretro_m2_osd_interface();

	// general overridables
	virtual void init(running_machine &machine) override;
	virtual void update(bool skip_redraw) override;
	virtual void input_update(bool relative_reset) override;
	virtual void check_osd_inputs() override;

	// osd_common_t leaves these pure; there is no event pump and the core is always "focused"
	virtual void process_events() override { }
	virtual bool has_focus() const override { return true; }

	// we want MAME's rendered audio — it is what gets handed to audio_batch_cb
	virtual bool no_sound() override { return false; }

	virtual libretro_m2_options &options() override { return m_options; }

	// --- audio: implemented here rather than as a sound_module ---
	// One less module to construct, one less symbol to register. The sink is a single stereo
	// node whose samples are pushed into the frame's audio buffer.
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual uint32_t sound_get_generation() override { return 1; }
	virtual osd::audio_info sound_get_information() override;
	virtual uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override;
	virtual uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) override { return 0; }
	virtual void sound_stream_close(uint32_t id) override;
	virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override;
	virtual void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) override { }
	virtual void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) override { }
	virtual void sound_begin_update() override { }
	virtual void sound_end_update() override { }

	// --- the pointers osd_common_t would delegate to modules we do not construct ---
	virtual void init_debugger() override { }
	virtual void wait_for_debugger(device_t &device, bool firststop) override { }
	virtual void debugger_update() override { }
	virtual osd_font::ptr font_alloc() override;
	virtual bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result) override;
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override { return nullptr; }
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override { return nullptr; }
	virtual std::vector<osd::midi_port_info> list_midi_ports() override { return std::vector<osd::midi_port_info>(); }
	virtual std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) override { return nullptr; }
	virtual std::vector<osd::network_device_info> list_network_devices() override { return std::vector<osd::network_device_info>(); }

	virtual void init_subsystems() override;
	virtual void osd_exit() override;

	// --- the libretro side of the per-frame rendezvous (called from retro_entry.cpp) ---

	// Block until the emulation thread has finished a frame and is parked in update().
	// Returns false if the machine exited or failed to start.
	bool wait_for_frame();
	// Release the emulation thread to run the next frame.
	void release_frame();
	// Ask the emulation thread to exit; honored on that thread in update().
	void request_exit();
	// Called by the emulation thread as it unwinds, so a waiter is never left hanging.
	void signal_died();

	// Valid between wait_for_frame() and release_frame().
	const uint32_t *framebuffer(int &width, int &height) const;
	const int16_t *frame_audio(int &samples_this_frame) const;

	// Ask the emulation thread for a soft reset; honored on that thread in update().
	void request_reset();

	// The input module, so retro_run() can publish the frontend's pad state into it. Null until
	// the machine has started, and again once it has exited.
	libretro_m2_input *input() const { return m_input.get(); }

	bool machine_started() const { return m_started.load(std::memory_order_acquire); }
	uint32_t audio_rate() const { return m_audio_rate; }

	// The model2_service_buttons core option. Set from retro_load_game() before the emulation
	// thread starts; read once, when the input devices are configured at machine start.
	void set_service_buttons(bool enable) { m_service_buttons = enable; }

	// Screen geometry and timing for retro_get_system_av_info. Valid once a frame has been
	// produced; sampled on the emulation thread, read while it is parked.
	double refresh_rate() const { return m_refresh_rate; }
	double aspect_ratio() const { return m_aspect_ratio; }

protected:
	// constructs the one input module and registers its RetroPad devices
	virtual bool input_init() override;

private:
	void capture_frame();
	void alloc_screen_target();
	void free_screen_target();

	libretro_m2_options &m_options;

	// Modules taken straight from their exported factories. Only the two that osd_common_t
	// genuinely dereferences are constructed; everything else is overridden above.
	std::unique_ptr<osd_module> m_render_module;
	std::unique_ptr<osd_module> m_output_module;

	// The one real module written here rather than taken from upstream: a RetroPad input_module.
	// osd_common_t's four input pointers are all aimed at it, but input_init()/input_update() are
	// overridden so it is initialised and polled once, not four times.
	std::unique_ptr<libretro_m2_input> m_input;

	// keeps the emulated screen "visible" so its SCREEN_UPDATE callback keeps firing
	render_target *m_target = nullptr;

	bool m_service_buttons = false;

	// the frame, written on the emulation thread while the libretro thread is blocked, so
	// no locking is needed around it — the baton below is the synchronisation
	std::vector<uint32_t> m_fb;
	int m_fb_w = 0;
	int m_fb_h = 0;

	double m_refresh_rate = 60.0;
	double m_aspect_ratio = 4.0 / 3.0;

	std::vector<int16_t> m_audio;   // interleaved stereo, this frame only
	uint32_t m_audio_stream_id = 0;
	uint32_t m_audio_next_id = 1;
	uint32_t m_audio_rate = 0;

	// --- the baton ---
	// update() posts m_frame_ready and waits on m_go; retro_run() does the mirror image.
	std::mutex m_baton;
	std::condition_variable m_cv;
	bool m_frame_ready = false;
	bool m_go = false;
	bool m_died = false;
	// sticky once the machine is on its way out; emulation thread only. See update().
	bool m_exiting = false;

	std::atomic<bool> m_exit_requested{ false };
	std::atomic<bool> m_reset_requested{ false };
	std::atomic<bool> m_started{ false };
};

#endif // MAME_OSD_LIBRETRO_M2_OSD_H
