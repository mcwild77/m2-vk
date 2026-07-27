// license:BSD-3-Clause
// copyright-holders:mcwild77
/*********************************************************************************************************************************

    Model 2 libretro OSD — input.

    A real MAME input_module rather than per-game ioport injection. One joystick device is
    registered per RetroPad port, laid out exactly like SDL's game-controller device, so MAME's
    own default assignments and its remapping UI/ctrlr files apply unchanged and every game in the
    library gets working controls with no per-game table. See devnotes/p1-libretro-core.md.

    Two consequences of the layout being fixed and known:

      * the device index is the player number.  MAME's stock defaults for START1/COIN1 already read
        JOYCODE_START_INDEXED(0) / JOYCODE_SELECT_INDEXED(0) (and (1) for player 2), so exposing
        ITEM_ID_START and ITEM_ID_SELECT is all that coin and start need.
      * there is no enumeration, no hotplug and no availability testing — the RetroPad always has
        every control, so configure() is the SDL controller path with the probing removed.

    A second device class sits alongside the pads: one DEVICE_CLASS_LIGHTGUN device per gun-capable
    port, for the six lightgun sets.  Both kinds of device always exist and both are always polled;
    which of them is allowed to *move* is decided per frame by the port's selected libretro device,
    because MAME sums OR'd absolute axes rather than picking one.  See devnotes/lightgun.md §2.

    Polling is inverted relative to a normal OSD.  MAME never asks the frontend for anything: the
    item pointers handed to add_item() address this object's state directly, and retro_run() writes
    that state on the libretro thread while the emulation thread is parked on the frame baton.
    poll() is therefore a no-op, and what MAME reads during a frame is exactly the snapshot the
    frontend published for it — which is what the A/B harness needs.

*********************************************************************************************************************************/

#ifndef MAME_OSD_LIBRETRO_M2_INPUT_H
#define MAME_OSD_LIBRETRO_M2_INPUT_H

#pragma once

#include "libretro.h"
#include "libretro_m2_osd.h"

#include "modules/input/assignmenthelper.h"
#include "modules/input/input_common.h"

#include <cstdint>
#include <string>

// input_module_impl<> dynamic_casts the osd_interface it is handed to its second template
// argument, so libretro_m2_osd_interface has to be complete here, not merely declared.


// What the two device kinds have in common: a port, and a per-frame update that is handed the
// libretro device that port is currently set to.  They differ in what they read from the frontend,
// not in how or when they are read, so one list holds both and one call drives both.
class libretro_m2_device : public device_info
{
public:
	libretro_m2_device(std::string &&name, std::string &&id, input_module &module, unsigned port)
		: device_info(std::move(name), std::move(id), module)
		, m_port(port)
	{
	}

	// state is written by update(), not pulled from a device; nothing to do per poll
	virtual void poll(bool relative_reset) override { }

	// Called on the libretro thread from retro_run(), while the emulation thread is parked.
	// device is the port's current retro_set_controller_port_device value.
	virtual void update(retro_input_state_t state_cb, unsigned device) = 0;

	unsigned port() const { return m_port; }

protected:
	unsigned m_port;
};


// One RetroPad.  Axis and button storage is indexed by RetroPad id so the mapping from a
// retro_input_state_t call to a slot is the identity.
class libretro_m2_pad_device : public libretro_m2_device, protected osd::joystick_assignment_helper
{
public:
	// the six RetroPad axes, in the order they are exposed to MAME
	enum : unsigned
	{
		AXIS_LEFT_X = 0,
		AXIS_LEFT_Y,
		AXIS_RIGHT_X,
		AXIS_RIGHT_Y,
		AXIS_L2,
		AXIS_R2,
		AXIS_COUNT
	};

	static inline constexpr unsigned BUTTON_COUNT = 16; // RETRO_DEVICE_ID_JOYPAD_B .. _R3

	libretro_m2_pad_device(std::string &&name, std::string &&id, input_module &module, unsigned port, bool service_buttons);

	virtual void reset() override;
	virtual void configure(osd::input_device &device) override;
	virtual void update(retro_input_state_t state_cb, unsigned device) override;

private:
	bool      m_service_buttons;
	int32_t   m_axes[AXIS_COUNT];
	int32_t   m_buttons[BUTTON_COUNT];
};


// One emulated lightgun.  Deliberately small: MAME's own defaults for IPT_LIGHTGUN_X/Y and
// IPT_BUTTON1/2 already name GUNCODE_*_INDEXED(n), so the items below are bound the moment they
// exist and configure() has almost nothing to add.  devnotes/lightgun.md §1.1, §2.3.
class libretro_m2_gun_device : public libretro_m2_device
{
public:
	enum : unsigned
	{
		AXIS_X = 0,
		AXIS_Y,
		AXIS_COUNT
	};

	enum : unsigned
	{
		BUTTON_TRIGGER = 0,
		BUTTON_AUX_A,
		BUTTON_AUX_B,
		BUTTON_COUNT
	};

	libretro_m2_gun_device(std::string &&name, std::string &&id, input_module &module, unsigned port);

	virtual void reset() override;
	virtual void configure(osd::input_device &device) override;
	virtual void update(retro_input_state_t state_cb, unsigned device) override;

private:
	int32_t m_axes[AXIS_COUNT];
	int32_t m_buttons[BUTTON_COUNT];
};


class libretro_m2_input : public input_module_impl<libretro_m2_device, libretro_m2_osd_interface>
{
public:
	// Four, because airwlkrs is a genuine four-player cabinet: four PLAYER(n) blocks of three
	// buttons and a four-way stick, plus COIN3/COIN4 and START3/START4. It is the only such set,
	// and it is MACHINE_NOT_WORKING, so the ports are wired and nothing is claimed about playing it.
	// Every other set uses at most two, and pads 3 and 4 simply bind to types those sets do not have.
	static inline constexpr unsigned MAX_PADS = 4;

	// Deliberately NOT MAX_PADS. The gun cabinets are all two-player, so whatever creates lightgun
	// devices sizes off this instead; one constant serving both meanings is how a four-player pad
	// count would silently become four guns.
	static inline constexpr unsigned MAX_GUNS = 2;

	static_assert(MAX_GUNS <= MAX_PADS, "a gun's port index has to be a valid pad port index too");

	// service_buttons is the model2_service_buttons core option, carried down to the pads because
	// it changes what configure() puts in their default assignment vector.
	explicit libretro_m2_input(bool service_buttons);
	virtual ~libretro_m2_input();

	virtual void input_init(running_machine &machine) override;

	// Called on the libretro thread from retro_run(), while the emulation thread is parked.
	// port_device is MAX_PADS entries of retro_set_controller_port_device state, owned by the
	// entry-point file because a frontend may set it before there is a machine to tell.
	void poll_frontend(retro_input_state_t state_cb, unsigned const *port_device);

private:
	bool m_service_buttons;
};

#endif // MAME_OSD_LIBRETRO_M2_INPUT_H
