// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    M2VK_LAZY_BAUD — a demand-gated replacement for the 500 kHz CLOCK that drives the i8251's TxC/RxC.

    Why (measured, 2026-09-01, devnotes/plan_model2_quantum.md): every Model 2 machine carries one or
    two `clock_device`s at 16 MHz/2/16 = 500 kHz wired to i8251_device::write_txc + write_rxc. A
    clock_device fires on both edges, so each one is 1,000,000 timer callbacks per emulated second —
    99.87 % of every timer callback in the machine, and ~1 M forced scheduler breaks/s at ~85 ns each.
    Silencing them (which breaks the sound link) cut desktop core time by 34–57 %. The callbacks are
    cheap; the *break points* are the cost.

    Fifteen of every sixteen of those edges do nothing but increment a divider counter. This device
    delivers the same edges to the same i8251 at the same emulated instants, but only schedules a timer
    for an edge the UART can actually act on:

      * TX — transmit_clock() produces output only when m_txc_count reaches m_br_factor. The skipped
        edges are a pure counter bump, so we add them in bulk and schedule one timer at the bit
        boundary. Bit-exact by construction; 16x fewer break points (500 k/s -> 31.25 k/s).
      * RX — while the receiver is mid-character, m_rxc_count just counts down to the next sample
        point, so we subtract in bulk and schedule the sample edge. While it is waiting for a start bit
        with RxD steady, every edge re-samples the same level and changes nothing at all, so we sleep
        entirely and wake on write_rxd() — which is why RxD is routed through rxd_w() below. That is
        the big one: the link is idle the overwhelming majority of the time.

    Naively *batching* RX edges would not be safe — start-bit detection runs at the full 16x rate, and
    quantising it to a whole bit can sample the neighbouring bit. Nothing here batches: every edge that
    can change the receiver is still delivered at its own exact instant, on the same grid a free-running
    clock_device would have produced (edge k at k / (2 * clock), anchored at machine time 0).

    Gate: M2VK_LAZY_BAUD (env, default ON). Set it to 0 to get the stock CLOCK back for an A/B.

*********************************************************************************************************************************/
#ifndef MAME_OSD_LIBRETRO_M2_M2VK_BAUD_H
#define MAME_OSD_LIBRETRO_M2_M2VK_BAUD_H

#pragma once

#include "machine/i8251.h"

#include <utility>


//**************************************************************************
//  DEVICE
//**************************************************************************

class m2vk_baud_device : public device_t
{
public:
	m2vk_baud_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

	// the i8251 this generator clocks; same tag the CLOCK's signal_handler would have named
	template <typename T> void set_uart(T &&tag) { m_uart.set_tag(std::forward<T>(tag)); }

	// -- wake points ------------------------------------------------------------------------------
	// The generator sleeps whenever no clock edge can change the UART, so anything that CAN change it
	// has to come through here: the register window (a mode byte re-phases the divider, a command byte
	// can enable the receiver) and the RxD line (the only thing that can start a character).

	u8   uart_r(offs_t offset);
	void uart_w(offs_t offset, u8 data);
	void rxd_w(int state);

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

private:
	TIMER_CALLBACK_MEMBER(tx_tick);
	TIMER_CALLBACK_MEMBER(rx_tick);

	attotime edge_time(u64 edge) const { return attotime::from_ticks(edge, m_grid_hz); }
	bool edges_before(const attotime &now, u64 &limit) const;
	void arm(emu_timer *timer, u64 edge, const attotime &now);

	void sync_tx(const attotime &now);
	void sync_rx(const attotime &now);
	void arm_tx(const attotime &now);
	void arm_rx(const attotime &now);

	required_device<i8251_device> m_uart;

	emu_timer *m_tx_timer = nullptr;
	emu_timer *m_rx_timer = nullptr;

	u32 m_grid_hz = 0;              // one grid edge per half clock period, i.e. 2 * clock()
	u64 m_attos_per_tick = 0;       // exact only when m_grid_hz divides a second; see device_start()

	u64 m_tx_next = 2;              // next TxC-acting grid edge not yet accounted for (even, >= 2)
	u64 m_rx_next = 1;              // next RxC-acting grid edge not yet accounted for (odd,  >= 1)

	// True when the receive shift register's top bit already holds the current RxD level, i.e. every
	// further sample of a steady line is a no-op and the receiver can sleep. Cleared by rxd_w().
	bool m_rx_primed = false;

	// One-shot latch for the transmitter-stall guard in arm_tx(); not saved, purely diagnostic.
	bool m_warned_txc = false;
};

DECLARE_DEVICE_TYPE(M2VK_BAUD, m2vk_baud_device)


//**************************************************************************
//  MACHINE-CONFIG HOOKS
//**************************************************************************

namespace m2vk_baud {

// M2VK_LAZY_BAUD: 0 = off (stock CLOCK), 1 = demand-gated, 2 = eager (this device, a timer for every
// edge — the diagnostic control), 3 = TX lazy only, 4 = RX lazy only. The core option seeds the on/off
// choice; the environment variable overrides it.
bool enabled();
bool tx_eager();
bool rx_eager();

// Force the resolved value, from the core option, before the machine starts. The environment variable
// still wins when set (harness override), same composition as m2vk_snd::set_option_enabled().
void set_option_enabled(bool on);

// Create the generator at `tag` and bind it to `uart`, or return nullptr when the lazy path is off —
// in which case the caller creates the stock CLOCK exactly as upstream does.
template <typename T>
m2vk_baud_device *install(machine_config &config, const char *tag, u32 clock_hz, T &&uart)
{
	if (!enabled())
		return nullptr;

	m2vk_baud_device &baud(M2VK_BAUD(config, tag, clock_hz));
	baud.set_uart(std::forward<T>(uart));
	return &baud;
}

template <typename T>
m2vk_baud_device *install(machine_config &config, const char *tag, const XTAL &clock_hz, T &&uart)
{
	return install(config, tag, clock_hz.value(), std::forward<T>(uart));
}

// The generator a previous install() put at `tag` (may be a path, e.g. "m1audio:uart_clock"), or
// nullptr when the lazy path is off. For config functions that extend a config built elsewhere.
m2vk_baud_device *find(machine_config &config, const char *tag);

// Map the i8251's register window through the generator when it is on, straight at the device when it
// is not. Returns the entry so the caller can chain .umask*() as before.
template <typename T>
address_map_entry &map_uart(address_map &map, offs_t start, offs_t end, T &&uart, const char *tag = "uart_clock")
{
	if (enabled())
		return map(start, end).rw(tag, FUNC(m2vk_baud_device::uart_r), FUNC(m2vk_baud_device::uart_w));

	return map(start, end).rw(std::forward<T>(uart), FUNC(i8251_device::read), FUNC(i8251_device::write));
}

} // namespace m2vk_baud

#endif // MAME_OSD_LIBRETRO_M2_M2VK_BAUD_H
