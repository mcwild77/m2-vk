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


// 🛑 THERE ARE NO PAD DEVICE SUBCLASSES ANY MORE, and their absence is the feature.
//
// RETRO_DEVICE_M2_PAD_MODERN and RETRO_DEVICE_M2_PAD_CLASSIC used to live here — FBNeo's two generic
// layouts — and a third entry, "RetroPad (Cabinet)", carried the per-game row.  All three are gone.
// The reasoning, so it is not rebuilt:
//
//   * A layout a player has to go and select is not a default, and a *cabinet* layout is the only
//     correct default there is.  Every set now resolves to its own row (devnotes/input_layouts.json)
//     on plain RETRO_DEVICE_JOYPAD, so loading a game and playing it produces the arcade mapping and
//     the Controls menu is for changing it, not for finding it.
//   * With rows in place the two generic layouts have nothing left to do.  vf2's row is B/A/Y =
//     Punch/Kick/Guard, which is what Classic already was; the driving rows put button 5 where the
//     cabinet had it, which is what Modern was reaching for.  They were choices between a wrong
//     answer and another wrong answer.
//   * A device type also made the device LIST per-game — two retro_controller_info arrays and a
//     has_cabinet_layout() call to pick between them — so removing it removes that too.
//
// ⚠️ A frontend config remembering one of the retired subclass ids must still play.  update() treats
// every unrecognised device value as "use this machine's row", which is what guarantees it.


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


// One RetroPad.  Axis storage is indexed by RetroPad id; button storage is not — see the slot enum.
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

	// How many MAME buttons the layout table can fill.  Nine, because daytona has exactly nine and
	// is the set the layouts exist for.
	//
	// ⚠ It was six until 2026-07-29, with buttons 7 and 8 welded to the trigger thresholds in
	// configure() and button 9 welded to R3 in FIXED_BUTTONS.  That weld is precisely why daytona's
	// pedals and its VR2/VR3 could not be separated — no layout could move a button that was not a
	// layout entry.  Widening this is the whole of that fix: a trigger threshold is now just another
	// *source* a layout row may name, alongside the RetroPad ids.  devnotes/per-game-input.md §3.1.
	static inline constexpr unsigned NUMBERED_BUTTONS = 9;

	// Button state slots.  The first NUMBERED_BUTTONS of them are MAME button *numbers*, not
	// RetroPad ids, and that indirection is the whole of the pad-layout mechanism: configure()
	// fixes each MAME item's pointer to a slot once and for all, so a layout change can only be a
	// change to which RetroPad id update() reads into the slot.  Rebuilding the assignment vector
	// instead would fight MAME, whose item state pointers are set when the device is created.
	// devnotes/lightgun.md §2.5.2.
	enum : unsigned
	{
		BUTTON_1 = 0,
		BUTTON_2,
		BUTTON_3,
		BUTTON_4,
		BUTTON_5,
		BUTTON_6,
		BUTTON_7,
		BUTTON_8,
		BUTTON_9,
		BUTTON_SELECT,
		BUTTON_START,
		BUTTON_UP,
		BUTTON_DOWN,
		BUTTON_LEFT,
		BUTTON_RIGHT,
		BUTTON_L3,
		BUTTON_R3,
		// L1/R1. Unlike the numbered buttons these are NOT layout sources — no row feeds a numbered
		// button from them by design (the shoulders are where the joystick-shifter racers put shift).
		// They carry an item of their own so configure() can bind IPT_JOYSTICK_DOWN/UP to them on the
		// joy_shifter cabinets; inert everywhere else.
		BUTTON_L,
		BUTTON_R,
		BUTTON_COUNT
	};

	// diagnostic is an m2opt::diagnostic_input; it is an unsigned here so that this header keeps out
	// of the core-options one, which the emulation side has no other reason to see.
	//
	// layout is the loaded set's row of NUMBERED_BUTTONS sources, resolved once in input_init() and
	// shared by every pad: the row is a property of the machine, not of a port.  It is NEVER null —
	// a set with no row of its own gets the generic one — which is what lets update() drop the
	// null test it used to carry for the cabinet case.
	libretro_m2_pad_device(
			std::string &&name,
			std::string &&id,
			input_module &module,
			unsigned port,
			unsigned diagnostic,
			unsigned const *layout,
			bool joy_shifter);

	virtual void reset() override;
	virtual void configure(osd::input_device &device) override;
	virtual void update(retro_input_state_t state_cb, unsigned device) override;

private:
	// One layout entry, resolved to a button state. Reads m_axes for the trigger-threshold sources,
	// so it is only correct once update() has filled them for the frame.
	int32_t read_source(retro_input_state_t state_cb, unsigned source) const;

	void update_diagnostic(retro_input_state_t state_cb, unsigned const *layout);

	// Runs the steering chain on the primary stick X and publishes port 0's before-and-after into
	// m2vk::steer() for the read-out. Not const: the shaped value is written back into m_axes, which
	// is the whole of how devnotes/steering-curve.md reaches MAME. A no-op unless the machine steers
	// and a shape has been named — see m2vk_steer.h.
	void shape_and_publish_steer();

	unsigned         m_diagnostic;
	unsigned const  *m_layout;      // never null; see the constructor
	bool             m_joy_shifter; // gear shift is on the joystick — bind it to L1/R1 in configure()
	int32_t   m_axes[AXIS_COUNT];
	int32_t   m_buttons[BUTTON_COUNT];

	// This seat's steering-damp carry: last frame's shaped-and-limited axis value, in
	// ±osd::input_device::ABSOLUTE_MAX units, fed back into m2vk::steer_damp() each frame. Per device
	// because a two-seat cabinet's wheels are independent. Rest is 0, restored by reset(), which is
	// what keeps a never-touched axis (every ab.sh fixture) byte-exact.
	int32_t   m_steer_damp;

	// The diagnostic combo as a button, and how long its controls have been down. Not a slot in
	// m_buttons because no RetroPad control feeds it — it is computed from several of them, which is
	// exactly the invariant the static_assert over the slot enum protects.
	int32_t   m_combo;
	unsigned  m_combo_frames;
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

	// diagnostic is the model2_diagnostic_input core option, carried down to the pads because it
	// changes both what configure() puts in their default assignment vector and what update()
	// watches for. An m2opt::diagnostic_input, as unsigned — see the pad's constructor.
	explicit libretro_m2_input(unsigned diagnostic);
	virtual ~libretro_m2_input();

	// The frontend's remap labels for the named set — a null-terminated retro_input_descriptor array,
	// owned by the input module and valid until the next call.  Built from the same layout row the pad
	// reads, so what the Controls menu says a control does and what it actually does are one fact.
	//
	// 🚨 That single-source property is the point of routing this through here rather than keeping a
	// table in retro_entry.cpp.  There WAS such a table, and it disagreed with the layout for months:
	// it called L "Button 5" and R "Button 6" while the layout had them the other way round, so
	// daytona's remap screen named GEAR 4 and VR1 (Red) reversed (devnotes/input-map.md §5.1).  A
	// derived array cannot drift from what it is derived from.
	//
	// Static, and answering from the set NAME rather than from a machine, because the caller is
	// retro_load_game(): descriptors go out before there is a running_machine to ask.  Name is matched
	// first and then parent, so one row covers a set and all its clones; either may be null.
	//
	// service_coin says whether model2_diagnostic_input is set to anything, because that is what
	// decides whether L3 is IPT_SERVICE1 or an inert IPT_UI_MENU.  A label on a control that does
	// nothing is worse than no label, so the string is suppressed rather than shown conditionally true.
	static struct retro_input_descriptor const *descriptors(
			char const *name, char const *parent, bool service_coin);

	// Whether the named set has a row of its own, for the log line only. Nothing branches on it: a set
	// without one gets the generic row, which is what every set played as before there were rows.
	static bool has_layout(char const *name, char const *parent);

	virtual void input_init(running_machine &machine) override;

	// Called on the libretro thread from retro_run(), while the emulation thread is parked.
	// port_device is MAX_PADS entries of retro_set_controller_port_device state, owned by the
	// entry-point file because a frontend may set it before there is a machine to tell.
	void poll_frontend(retro_input_state_t state_cb, unsigned const *port_device);

private:
	unsigned m_diagnostic;
};

#endif // MAME_OSD_LIBRETRO_M2_INPUT_H
