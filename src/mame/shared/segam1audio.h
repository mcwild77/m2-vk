// license:BSD-3-Clause
// copyright-holders:R. Belmont
#ifndef MAME_SHARED_SEGAM1AUDIO_H
#define MAME_SHARED_SEGAM1AUDIO_H

#include "cpu/m68000/m68000.h"
#include "machine/i8251.h"
#include "sound/multipcm.h"
#include "sound/ymopn.h"

#include <queue>
#include <utility>

#pragma once

#define M1AUDIO_TAG "m1audio"
#define M1AUDIO_CPU_REGION "m1audio:sndcpu"
#define M1AUDIO_MPCM1_REGION "m1audio:pcm1"
#define M1AUDIO_MPCM2_REGION "m1audio:pcm2"


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

class m2vk_baud_device;

class segam1audio_device : public device_t
{
public:
	// construction/destruction
	segam1audio_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	// configuration
	auto rxd_handler() { return m_rxd_handler.bind(); }

	void m1_snd_mpcm_bnk1_w(uint16_t data);
	void m1_snd_mpcm_bnk2_w(uint16_t data);

	void write_txd(int state);

	void mpcm1_map(address_map &map) ATTR_COLD;
	void mpcm2_map(address_map &map) ATTR_COLD;
	void segam1audio_map(address_map &map) ATTR_COLD;
protected:
	// device-level overrides
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;

private:
	required_device<cpu_device> m_audiocpu;
	required_device<multipcm_device> m_multipcm_1;
	required_device<multipcm_device> m_multipcm_2;
	required_device<ym3438_device> m_ym;
	required_device<i8251_device> m_uart;

	required_memory_region m_multipcm1_region;
	required_memory_region m_multipcm2_region;

	required_memory_bank m_mpcmbank1;
	required_memory_bank m_mpcmbank2;

	devcb_write_line   m_rxd_handler;

	// Demand-gated baud clock for this board's UART, when the machine config installed one in place of
	// the CLOCK feeding its TxC/RxC (src/osd/libretro_m2/m2vk_baud.h). The board's RxD has to arrive
	// through it or a sleeping receiver is never woken. Null on the untouched path, and in any build
	// whose driver project does not compile the generator. Resolved in device_start().
	m2vk_baud_device *m_baud = nullptr;

	void output_txd(int state);

	// Stage-0 de-risk: optional fixed delay on the sound->main serial reply line
	// (m_rxd_handler), single-threaded and order-preserving. Enabled by the desktop
	// env var M2VK_SOUND_DELAY (microseconds); zero/unset = immediate delivery with no
	// behaviour change (getenv returns null on device, so shipping builds are unaffected).
	TIMER_CALLBACK_MEMBER(rxd_delay_tick);
	emu_timer *m_rxd_delay_timer = nullptr;
	std::queue<std::pair<attotime, int>> m_rxd_delay_fifo;
	attotime m_rxd_delay = attotime::zero;
};


// device type definition
DECLARE_DEVICE_TYPE(SEGAM1AUDIO, segam1audio_device)

#endif  // MAME_SHARED_SEGAM1AUDIO_H
