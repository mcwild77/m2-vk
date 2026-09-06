// license:BSD-3-Clause
// copyright-holders:mcwild77
//
// M2VK_LAZY_BAUD — demand-gated i8251 baud clock. Rationale and the exactness argument for each of
// the three skip cases are in m2vk_baud.h; this file is the mechanism.

#include "emu.h"
#include "m2vk_baud.h"

#include <algorithm>
#include <cstdlib>


DEFINE_DEVICE_TYPE(M2VK_BAUD, m2vk_baud_device, "m2vk_baud", "Demand-gated baud clock")


namespace m2vk_baud {

static int mode()
{
	// 0 = off (stock CLOCK), 1 = demand-gated (default), 2 = eager: the same device, the same edges,
	// but a timer for every one of them. Mode 2 skips nothing, so it reproduces clock_device's break
	// points exactly and is the control that separates "the mechanism is wrong" from "the machine
	// noticed the missing scheduler breaks".
	// 🚨 DEFAULT OFF. Two hand-checks in a row found audible faults on daytona that every digest,
	// savestate and byte-stream measurement had passed (devnotes/reference/lazy-baud.md). It stays opt-in
	// (M2VK_LAZY_BAUD=1) until a listening check signs it off.
	static int cached = -1;
	if (cached < 0)
	{
		char const *const env = std::getenv("M2VK_LAZY_BAUD");
		cached = env ? std::atoi(env) : 0;
		if (cached < 0 || cached > 4)
			cached = 0;
	}
	return cached;
}

bool enabled()
{
	return mode() != 0;
}

// 2 = both eager; 3 = TX demand-gated, RX eager; 4 = the other way round. Diagnostic only, to
// localise a divergence between "the mechanism is wrong" and "the machine noticed the missing breaks".
bool tx_eager()
{
	return (mode() == 2) || (mode() == 4);
}

bool rx_eager()
{
	return (mode() == 2) || (mode() == 3);
}

m2vk_baud_device *find(machine_config &config, const char *tag)
{
	if (!enabled())
		return nullptr;

	return dynamic_cast<m2vk_baud_device *>(config.root_device().subdevice(tag));
}

} // namespace m2vk_baud


//**************************************************************************
//  CONSTRUCTION
//**************************************************************************

m2vk_baud_device::m2vk_baud_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	device_t(mconfig, M2VK_BAUD, tag, owner, clock),
	m_uart(*this, finder_base::DUMMY_TAG)
{
}

void m2vk_baud_device::device_start()
{
	// The grid a free-running clock_device would have produced: it toggles every half period, so edge
	// k sits at k / (2 * clock), anchored at machine time 0. RxC acts on the rising edges (odd k), TxC
	// on the falling ones (even k >= 2); k = 0 is the start-of-machine output and acts on neither.
	m_grid_hz = clock() * 2;
	m_attos_per_tick = m_grid_hz ? (ATTOSECONDS_PER_SECOND / m_grid_hz) : 0;

	// Every Sega baud clock here is 16 MHz/2/16 = 500 kHz, so the grid divides a second exactly and
	// the edge times are exact integers. Anything else would need rounding, which would put edges off
	// the grid the untouched path uses — refuse rather than be subtly wrong.
	if (!m_grid_hz || (u64(ATTOSECONDS_PER_SECOND) % m_grid_hz) != 0)
		fatalerror("m2vk_baud: clock %u does not divide a second evenly\n", clock());

	m_tx_timer = timer_alloc(FUNC(m2vk_baud_device::tx_tick), this);
	m_rx_timer = timer_alloc(FUNC(m2vk_baud_device::rx_tick), this);

	m_tx_next = 2;
	m_rx_next = 1;
	m_rx_primed = false;

	save_item(NAME(m_tx_next));
	save_item(NAME(m_rx_next));
	save_item(NAME(m_rx_primed));
}

void m2vk_baud_device::device_reset()
{
	// The UART's own reset zeroes its dividers, and device reset order is not ours to rely on, so
	// re-derive the schedule from a zero-delay callback that lands after every device has reset. The
	// grid itself is anchored at machine time 0 and does not restart — neither does clock_device's.
	m_tx_timer->adjust(attotime::zero);
	m_rx_timer->adjust(attotime::zero);
}


//**************************************************************************
//  GRID
//**************************************************************************

// Index of the last grid edge strictly before `now`. False when there is none. An edge landing exactly
// on `now` is left to the timer, so a wake and the edge it coincides with are not double-counted.
bool m2vk_baud_device::edges_before(const attotime &now, u64 &limit) const
{
	u64 k = u64(now.seconds()) * m_grid_hz + u64(now.attoseconds()) / m_attos_per_tick;
	if ((u64(now.attoseconds()) % m_attos_per_tick) == 0)
	{
		if (k == 0)
			return false;
		--k;
	}
	if (k == 0)
		return false;

	limit = k;
	return true;
}


// Arm `timer` for grid edge `edge`. The edge can already be in the past — a wake stops short of an
// undelivered boundary and leaves it here — so clamp rather than hand emu_timer a negative duration.
void m2vk_baud_device::arm(emu_timer *timer, u64 edge, const attotime &now)
{
	const attotime t = edge_time(edge);
	timer->adjust((t > now) ? (t - now) : attotime::zero);
}


//**************************************************************************
//  TRANSMITTER
//**************************************************************************

// Account for the TxC edges strictly before `now` without delivering them. Each one is a pure
// m_txc_count++ (transmit_clock() returns early on it) — but ONLY up to the next bit boundary.
//
// 🚨 A wake CAN land past a boundary the timer has not run yet: a CPU overshoots a pending timer by
// up to one instruction inside a timeslice, so a register access can arrive tens of ns after a
// boundary whose callback is still queued. Bulk-adding across it lands m_txc_count exactly ON
// m_br_factor without transmit_clock() ever having run, and since the counter is only reset when it
// *equals* the factor, the next tick makes it factor+1 and the transmitter never fires again.
// Measured on daytona: TX died permanently 56 s in, mid-race. Stop at the boundary instead and leave
// it for the timer, which arms at zero delay and delivers it just after the register access — the
// same order stock MAME produces when the CPU overshoots.
void m2vk_baud_device::sync_tx(const attotime &now)
{
	u64 limit;
	if (!edges_before(now, limit) || (limit < m_tx_next))
		return;

	const u64 n = (limit - m_tx_next) / 2 + 1;
	const int br = m_uart->m2vk_br_factor();
	const int c = m_uart->m2vk_txc_count();
	const u64 to_boundary = (br > c) ? u64(br - c) : 1;   // edges until transmit_clock() acts

	const u64 take = std::min(n, to_boundary - 1);
	if (!take)
		return;

	m_uart->m2vk_set_txc_count(c + int(take));
	m_tx_next += 2 * take;
}

void m2vk_baud_device::arm_tx(const attotime &now)
{
	// transmit_clock() acts when the counter reaches m_br_factor; everything before that is skippable.
	// A counter already at or past the factor mirrors an upstream UART that would never reach it — arm
	// the next edge and let it behave identically.
	const int br = m_uart->m2vk_br_factor();
	const int c = m_uart->m2vk_txc_count();

	// The divider must never overtake the factor. transmit_clock() only resets the counter when it
	// *equals* m_br_factor, so a counter that has passed it never fires again and the transmitter is
	// dead for the rest of the run — silently, with the picture unaffected. That is exactly the bug
	// that skipping an already-elapsed boundary in sync_tx() caused (daytona, 56 s in, mid-race), so
	// keep the guard: it is one comparison per bit and it names the failure.
	if (c > br)
	{
		if (!m_warned_txc)
		{
			m_warned_txc = true;
			osd_printf_error("[m2vk_baud] %s: TxC divider %d overtook factor %d — transmitter stalled\n",
				tag(), c, br);
		}
	}

	const u64 remaining = (m2vk_baud::tx_eager() || !(br > c)) ? 1 : u64(br - c);

	arm(m_tx_timer, m_tx_next + 2 * (remaining - 1), now);
}

TIMER_CALLBACK_MEMBER(m2vk_baud_device::tx_tick)
{
	const attotime now = machine().time();

	sync_tx(now);

	if (edge_time(m_tx_next) <= now)
	{
		// One falling TxC edge. write_txc() acts on the transition, and m_txc is left low, so the
		// rise-then-fall pair always produces exactly one transmit_clock() whatever the pin held.
		m_uart->write_txc(1);
		m_uart->write_txc(0);
		m_tx_next += 2;
	}

	arm_tx(now);
}


//**************************************************************************
//  RECEIVER
//**************************************************************************

// Account for the RxC edges strictly before `now`. Three cases:
//   * mid-character: receive_clock() only does --m_rxc_count until the sample point, so the elapsed
//     edges subtract in bulk — but never onto or past the sample edge. Same hazard as sync_tx(): a
//     wake can land after a sample the timer has not run yet, and stepping over it loses the bit;
//   * idle: the line has been steady across the whole slept span and the shift register's top bit
//     already matches it (m_rx_primed), so each edge only scrolls the 16-bit register. Sixteen
//     replays leave it bit-identical however long the sleep was. When NOT primed we are not asleep,
//     so every elapsed edge is delivered for real — each one can still detect a start bit;
//   * synchronous mode: sync1_rxc()/sync2_rxc() act on EVERY edge, so nothing is skippable there —
//     arm_rx() never sleeps in that mode and this loop sees n == 0 in practice.
void m2vk_baud_device::sync_rx(const attotime &now)
{
	u64 limit;
	if (!edges_before(now, limit) || (limit < m_rx_next))
		return;

	const u64 n = (limit - m_rx_next) / 2 + 1;
	const bool rx_on = m_uart->m2vk_rx_enabled();

	if (rx_on && m_uart->m2vk_sync_mode())
	{
		for (u64 i = 0; i < n; i++)
		{
			m_uart->write_rxc(0);
			m_uart->write_rxc(1);
		}
	}
	else if (rx_on && m_uart->m2vk_rx_synced())
	{
		// Same hazard as sync_tx: never step onto or past the sample edge, or the sample is lost.
		const int k = m_uart->m2vk_rxc_count();
		const u64 take = (k > 1) ? std::min(n, u64(k - 1)) : 0;
		if (!take)
			return;
		m_uart->m2vk_set_rxc_count(k - int(take));
		m_rx_next += 2 * take;
		return;
	}
	else
	{
		// Capping at sixteen is only sound when the span is provably a no-op: the receiver asleep on
		// a steady line, or disabled (every path in receive_clock() returns at once). Otherwise these
		// are live edges and all of them must be delivered.
		const u64 replay = (m_rx_primed || !rx_on) ? std::min<u64>(n, 16) : n;
		for (u64 i = 0; i < replay; i++)
		{
			m_uart->write_rxc(0);
			m_uart->write_rxc(1);
		}
	}

	m_rx_next += 2 * n;
}

void m2vk_baud_device::arm_rx(const attotime &now)
{
	if (m2vk_baud::rx_eager())
	{
		arm(m_rx_timer, m_rx_next, now);
		return;
	}

	if (!m_uart->m2vk_rx_enabled())
	{
		// receive_clock() and the two sync paths all return immediately; only a command byte can
		// change that, and that arrives through uart_w().
		m_rx_timer->adjust(attotime::never);
		return;
	}

	if (m_uart->m2vk_sync_mode())
	{
		arm(m_rx_timer, m_rx_next, now);
		return;
	}

	if (m_uart->m2vk_rx_synced())
	{
		// Mid-character: the next sample is m_rxc_count edges away.
		const int k = m_uart->m2vk_rxc_count();
		const u64 steps = (k > 1) ? u64(k - 1) : 0;
		arm(m_rx_timer, m_rx_next + 2 * steps, now);
		return;
	}

	if (m_rx_primed)
	{
		// Waiting for a start bit with the shift register's top bit already equal to RxD: every
		// further sample sees no transition and does nothing. Sleep until rxd_w() moves the line.
		m_rx_timer->adjust(attotime::never);
		return;
	}

	arm(m_rx_timer, m_rx_next, now);
}

TIMER_CALLBACK_MEMBER(m2vk_baud_device::rx_tick)
{
	const attotime now = machine().time();

	sync_rx(now);

	if (edge_time(m_rx_next) <= now)
	{
		// One rising RxC edge; m_rxc is left high, so the fall-then-rise pair always produces exactly
		// one receive_clock() whatever the pin held.
		m_uart->write_rxc(0);
		m_uart->write_rxc(1);
		m_rx_next += 2;

		// Whatever that sample did, the shift register's top bit now holds the level just sampled.
		m_rx_primed = m_uart->m2vk_rx_enabled();
	}

	arm_rx(now);
}


//**************************************************************************
//  WAKE POINTS
//**************************************************************************

u8 m2vk_baud_device::uart_r(offs_t offset)
{
	// Reads (data_r / status_r) touch neither divider nor the receive register, so there is nothing to
	// catch up on and nothing to re-arm — and the CPU polls the status register hard enough that it is
	// worth not paying for it. Straight through.
	return m_uart->read(offset);
}

void m2vk_baud_device::uart_w(offs_t offset, u8 data)
{
	// A mode byte rewrites m_br_factor and zeroes both dividers; a command byte can enable the
	// receiver. Settle the elapsed edges against the OLD state first, then re-derive from the new.
	const attotime now = machine().time();

	sync_tx(now);
	sync_rx(now);

	m_uart->write(offset, data);

	arm_tx(now);
	arm_rx(now);
}

void m2vk_baud_device::rxd_w(int state)
{
	// The only thing that can start a character. Sample the edges that elapsed while the line held its
	// previous level, then re-arm against the new one.
	const attotime now = machine().time();

	sync_rx(now);
	m_rx_primed = false;
	m_uart->write_rxd(state);
	arm_rx(now);
}
