// license:BSD-3-Clause
// copyright-holders:R. Belmont
/***************************************************************************

  Sega Model 1 sound board (68000 + 2x 315-5560 "MultiPCM")

  used for Model 1 and early Model 2 games

***************************************************************************/

#include "emu.h"
#include "segam1audio.h"

#include "machine/clock.h"
#include "speaker.h"

#ifdef M2VK
#include "libretro_m2/m2vk_baud.h"
#endif

#include <cstdlib>

void segam1audio_device::segam1audio_map(address_map &map)
{
	map(0x000000, 0x03ffff).rom();
	map(0x080000, 0x09ffff).rom().region("sndcpu", 0x20000); // mirror of upper ROM socket
#ifdef M2VK
	// M2VK: through the demand-gated baud clock when it is on — a mode/command byte has to re-phase
	// the generator. See src/osd/libretro_m2/m2vk_baud.h.
	m2vk_baud::map_uart(map, 0xc20000, 0xc20003, m_uart).umask16(0x00ff);
#else
	map(0xc20000, 0xc20003).rw(m_uart, FUNC(i8251_device::read), FUNC(i8251_device::write)).umask16(0x00ff);
#endif
	map(0xc40000, 0xc40007).rw(m_multipcm_1, FUNC(multipcm_device::read), FUNC(multipcm_device::write)).umask16(0x00ff);
	map(0xc40012, 0xc40013).nopw();
	map(0xc50000, 0xc50001).w(FUNC(segam1audio_device::m1_snd_mpcm_bnk1_w));
	map(0xc60000, 0xc60007).rw(m_multipcm_2, FUNC(multipcm_device::read), FUNC(multipcm_device::write)).umask16(0x00ff);
	map(0xc70000, 0xc70001).w(FUNC(segam1audio_device::m1_snd_mpcm_bnk2_w));
	map(0xd00000, 0xd00007).rw(m_ym, FUNC(ym3438_device::read), FUNC(ym3438_device::write)).umask16(0x00ff);
	map(0xf00000, 0xf0ffff).ram(); // real PCB actually has 2x 8kBx8-bit SRAMs (16kB total)
}

void segam1audio_device::mpcm1_map(address_map &map)
{
	map(0x000000, 0x0fffff).rom();
	map(0x100000, 0x1fffff).bankr(m_mpcmbank1);
}

void segam1audio_device::mpcm2_map(address_map &map)
{
	map(0x000000, 0x0fffff).rom();
	map(0x100000, 0x1fffff).bankr(m_mpcmbank2);
}

//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

DEFINE_DEVICE_TYPE(SEGAM1AUDIO, segam1audio_device, "segam1audio", "Sega Model 1 Sound Board")

//-------------------------------------------------
// device_add_mconfig - add device configuration
//-------------------------------------------------

void segam1audio_device::device_add_mconfig(machine_config &config)
{
	M68000(config, m_audiocpu, 20_MHz_XTAL / 2);  // verified on real h/w
	m_audiocpu->set_addrmap(AS_PROGRAM, &segam1audio_device::segam1audio_map);

	SPEAKER(config, "speaker", 2).front();

	YM3438(config, m_ym, 16_MHz_XTAL / 2);
	m_ym->add_route(0, "speaker", 0.30, 0);
	m_ym->add_route(1, "speaker", 0.30, 1);

	MULTIPCM(config, m_multipcm_1, 20_MHz_XTAL / 2);
	m_multipcm_1->set_addrmap(0, &segam1audio_device::mpcm1_map);
	m_multipcm_1->add_route(0, "speaker", 0.5, 0);
	m_multipcm_1->add_route(1, "speaker", 0.5, 1);

	MULTIPCM(config, m_multipcm_2, 20_MHz_XTAL / 2);
	m_multipcm_2->set_addrmap(0, &segam1audio_device::mpcm2_map);
	m_multipcm_2->add_route(0, "speaker", 0.5, 0);
	m_multipcm_2->add_route(1, "speaker", 0.5, 1);

	I8251(config, m_uart, 16_MHz_XTAL / 2); // T82C51
	m_uart->rxrdy_handler().set_inputline(m_audiocpu, M68K_IRQ_2);
	m_uart->txd_handler().set(FUNC(segam1audio_device::output_txd));

#ifdef M2VK
	// M2VK: the demand-gated baud clock in place of a 500 kHz CLOCK that pokes TxC/RxC a million times
	// an emulated second. The board's RxD comes from the main UART's TXD, which the driver routes into
	// this generator directly. See src/osd/libretro_m2/m2vk_baud.h.
	if (!m2vk_baud::install(config, "uart_clock", 16_MHz_XTAL / 2 / 16, m_uart))
#endif
	{
	clock_device &uart_clock(CLOCK(config, "uart_clock", 16_MHz_XTAL / 2 / 16)); // 16 times 31.25kHz (standard Sega/MIDI sound data rate)
	uart_clock.signal_handler().set(m_uart, FUNC(i8251_device::write_txc));
	uart_clock.signal_handler().append(m_uart, FUNC(i8251_device::write_rxc));
	}

	// DAC output clocks measures:
	// BYTECLK = 10/8 (1.25MHz)
	// WORDCLK = 10/8/28 (44.642857kHz)
}

//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

//-------------------------------------------------
//  segam1audio_device - constructor
//-------------------------------------------------

segam1audio_device::segam1audio_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, SEGAM1AUDIO, tag, owner, clock),
	m_audiocpu(*this, "sndcpu"),
	m_multipcm_1(*this, "pcm1"),
	m_multipcm_2(*this, "pcm2"),
	m_ym(*this, "ymsnd"),
	m_uart(*this, "uart"),
	m_multipcm1_region(*this, "pcm1"),
	m_multipcm2_region(*this, "pcm2"),
	m_mpcmbank1(*this, "m1pcm1_bank"),
	m_mpcmbank2(*this, "m1pcm2_bank"),
	m_rxd_handler(*this)
{
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void segam1audio_device::device_start()
{
#ifdef M2VK
	// dynamic_cast, not subdevice<T>(): that is a downcast, so with M2VK_LAZY_BAUD=0 it would hand back
	// the stock clock_device reinterpreted as a generator.
	m_baud = dynamic_cast<m2vk_baud_device *>(subdevice("uart_clock"));
#endif

	m_mpcmbank1->configure_entries(0, 4, m_multipcm1_region->base(), 0x100000);
	m_mpcmbank2->configure_entries(0, 4, m_multipcm2_region->base(), 0x100000);

	// Stage-0 de-risk: optional delay line on the sound->main serial reply (see header).
	m_rxd_delay_timer = timer_alloc(FUNC(segam1audio_device::rxd_delay_tick), this);
	if (char const *const env = std::getenv("M2VK_SOUND_DELAY"))
	{
		long const us = std::atol(env);
		if (us > 0)
			m_rxd_delay = attotime::from_usec(us);
	}
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void segam1audio_device::device_reset()
{
	m_uart->write_cts(0);
}

void segam1audio_device::m1_snd_mpcm_bnk1_w(uint16_t data)
{
	m_mpcmbank1->set_entry(data & 3);
}

void segam1audio_device::m1_snd_mpcm_bnk2_w(uint16_t data)
{
	m_mpcmbank2->set_entry(data & 3);
}

void segam1audio_device::write_txd(int state)
{
#ifdef M2VK
	// M2VK: through the demand-gated baud clock when it is on, so a sleeping receiver is woken. Doing
	// it here rather than at every caller covers model1.cpp and manxttdx too. See m2vk_baud.h.
	if (m_baud)
	{
		m_baud->rxd_w(state);
		return;
	}
#endif
	m_uart->write_rxd(state);
}

void segam1audio_device::output_txd(int state)
{
	if (m_rxd_delay.is_zero())
	{
		m_rxd_handler(state);
		return;
	}

	// Queue the line transition and deliver it m_rxd_delay later, preserving order.
	// Constant delay + monotonic enqueue => FIFO order == delivery order, so a single
	// timer armed on the empty->non-empty edge is sufficient.
	bool const was_empty = m_rxd_delay_fifo.empty();
	m_rxd_delay_fifo.emplace(machine().time() + m_rxd_delay, state);
	if (was_empty)
		m_rxd_delay_timer->adjust(m_rxd_delay);
}

TIMER_CALLBACK_MEMBER(segam1audio_device::rxd_delay_tick)
{
	int const state = m_rxd_delay_fifo.front().second;
	m_rxd_delay_fifo.pop();
	m_rxd_handler(state);

	if (!m_rxd_delay_fifo.empty())
		m_rxd_delay_timer->adjust(m_rxd_delay_fifo.front().first - machine().time());
}
