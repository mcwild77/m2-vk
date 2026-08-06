// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "libretro_m2_input.h"

#include "retro_options.h"

#include "emu.h"
#include "emuopts.h"

#include "util/strformat.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>


namespace {

//============================================================
//  the RetroPad layout
//============================================================

// Names as they appear in MAME's input menus. Indexed by RETRO_DEVICE_ID_JOYPAD_*.
constexpr unsigned RETROPAD_ID_COUNT = RETRO_DEVICE_ID_JOYPAD_R3 + 1;

char const *const RETROPAD_BUTTON_NAMES[RETROPAD_ID_COUNT] = {
	"B", "Y", "Select", "Start", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
	"A", "X", "L", "R", "L2", "R2", "L3", "R3" };

char const *const RETROPAD_AXIS_NAMES[libretro_m2_pad_device::AXIS_COUNT] = {
	"Left X", "Left Y", "Right X", "Right Y", "L2", "R2" };

// The six axes, in exposure order, and the MAME item each maps to. Matches sdl_game_controller_device
// so that anything tuned against an Xbox-style pad under the SDL OSD behaves the same here.
constexpr input_item_id AXIS_ITEMS[libretro_m2_pad_device::AXIS_COUNT] = {
	ITEM_ID_XAXIS, ITEM_ID_YAXIS, ITEM_ID_ZAXIS, ITEM_ID_RZAXIS, ITEM_ID_SLIDER1, ITEM_ID_SLIDER2 };

// A layout entry names a SOURCE, not necessarily a RetroPad button id. Most sources are ids, and
// those are stored as themselves so that the diagnostic combo can compare a layout entry against a
// combo's id directly. The two that are not are the analogue triggers read as switches: they have no
// RETRO_DEVICE_ID_JOYPAD id of their own that means "past the threshold" rather than "held", and the
// threshold is what MAME's button item wants.
//
// The tag bit is above every RetroPad id, so a source can never be mistaken for one — which matters,
// because update_diagnostic() tests exactly that equality to decide what a fired combo consumes.
//
// SOURCE_NONE is the third tagged source and it is what a cabinet row uses for a MAME button the
// cabinet has but the pad has run out of controls for. It reads 0 forever, which is exactly what an
// unpressed button reads, so nothing downstream needs to know it is special — and it is a real entry
// rather than a hole because every row has to have NUMBERED_BUTTONS of them.
constexpr unsigned SOURCE_TAG     = 0x100;
constexpr unsigned SOURCE_L2_AXIS = SOURCE_TAG | 0;
constexpr unsigned SOURCE_R2_AXIS = SOURCE_TAG | 1;
constexpr unsigned SOURCE_NONE    = SOURCE_TAG | 2;


//============================================================
//  the per-game layout table
//============================================================

// One row per port set: which RetroPad control produces each MAME button, and what every control is
// called in the frontend's remap UI. NUMBERED_BUTTONS sources, slot n holding MAME button n+1, plus a
// label per RetroPad control. Everything else about the pad — the sticks, the pedals, coin, start, the
// service switches — is the same under every row, so a row's sources are only ever a statement about
// the nine numbered buttons.
//
// 🚨 THE TABLE IS GENERATED AND MUST NOT BE HAND-EDITED. devnotes/tools/padmap.html authors
// input_layouts.json and devnotes/tools/padmap-gen.py renders the .ipp; `padmap-gen.py --check` fails
// if the two have drifted. The generator exists for one reason worth knowing before working around it:
// the labels are DERIVED from the sources rather than written beside them. Two hand-written copies of
// that relationship is exactly what put daytona's GEAR 4 and VR1 (Red) the wrong way round in the
// remap UI for months (devnotes/input-map.md §5.1), and a derived array cannot drift from what it is
// derived from.
//
// 🚨 A row may name a D-PAD control only if the set declares no IPT_JOYSTICK_*. The d-pad slots keep
// their ITEM_ID_HAT1* items and the IPT_JOYSTICK_* assignments add_directional_assignments() gives
// them, so pointing a numbered button at one makes a single pad control feed two MAME items — free
// on a set with no digital joystick and a genuine double press on one that has. Likewise a row may
// only name SOURCE_L2_AXIS/SOURCE_R2_AXIS on a set with no IPT_PEDAL, or flooring the accelerator
// presses that button too — the daytona collision. **Neither rule is checked here and neither needs
// to be**: the editor knows which types each set declares, because padmap-sweep.sh boots the machine
// and asks, and it refuses a row that breaks either. That is the whole reason the editor exists.
//
// The set name is matched first and the parent's second, so one row covers a set and all its clones
// while a clone can still be given a row of its own later without moving anything.

// Label slots: the sixteen RetroPad ids in their own numeric order, then the four analog axes. Indexed
// by the libretro id directly, so building descriptors needs no mapping table — which is also why the
// generated array's order is not free to change.
constexpr unsigned LABEL_ANALOG_LX = RETROPAD_ID_COUNT;
constexpr unsigned LABEL_ANALOG_LY = RETROPAD_ID_COUNT + 1;
constexpr unsigned LABEL_ANALOG_RX = RETROPAD_ID_COUNT + 2;
constexpr unsigned LABEL_ANALOG_RY = RETROPAD_ID_COUNT + 3;
constexpr unsigned LABEL_COUNT     = RETROPAD_ID_COUNT + 4;

struct game_layout
{
	char const        *id;
	char const *const *sets;        // null-terminated; nullptr on the generic row, which matches nothing

	// Whether the set declares IPT_LIGHTGUN_X/Y. It rides on the row because the answer is needed in
	// retro_load_game(), where there is no machine to ask — and it is needed at all because without it
	// the gun descriptors went out on every set: daytona's remap screen listed a lightgun trigger called
	// "GEAR 1" and vf2's listed one called "Punch". A port can still be SET to a gun on any set, as it
	// always could; this only decides whether the gun's controls are given names.
	bool               lightgun;

	unsigned           sources[libretro_m2_pad_device::NUMBERED_BUTTONS];
	char const        *labels[LABEL_COUNT];   // nullptr = send no descriptor for that control
};

#include "input_layouts.ipp"

// The row for a set, its parent's row if it has none of its own, or the generic row. Two passes rather
// than one, so that an exact name always beats a parent's row whatever order the table is written in.
//
// ⚠️ It never returns null, which is a change from the cabinet version it replaces: there is no longer
// a "this set has no layout" case to test for at every call site, because the generic row IS that case
// and it is byte-for-byte the layout every set used before there were rows.
game_layout const &layout_for(char const *name, char const *parent)
{
	for (auto const &row : GAME_LAYOUTS)
	{
		for (char const *const *set = row.sets; *set != nullptr; set++)
		{
			if ((name != nullptr) && (std::strcmp(*set, name) == 0))
				return row;
		}
	}
	// "0" and not just the empty string: game_driver::parent is the literal string "0" when a set has no
	// parent, so an unguarded compare would look for a row named "0" on every parent set in the tree.
	if ((parent != nullptr) && (*parent != '\0') && (std::strcmp(parent, "0") != 0))
	{
		for (auto const &row : GAME_LAYOUTS)
		{
			for (char const *const *set = row.sets; *set != nullptr; set++)
			{
				if (std::strcmp(*set, parent) == 0)
					return row;
			}
		}
	}
	return GENERIC_LAYOUT;
}

// The label of whichever control the row points MAME button n at.  The gun's trigger and its two aux
// buttons ARE MAME buttons 1-3, so reading the row is what keeps a gun cabinet's remap labels and a pad
// port's remap labels from being two separate claims about the same machine.
//
// The fallback is used when the row leaves the button unmapped, and callers pass nullptr for it where an
// absent descriptor is the honest answer: vcop declares only button 1, so its Aux A and Aux B are not
// controls that need naming — they are controls that do nothing.
char const *gun_label(game_layout const &row, unsigned button, char const *fallback)
{
	if ((button < 1) || (button > libretro_m2_pad_device::NUMBERED_BUTTONS))
		return fallback;

	const unsigned source = row.sources[button - 1];
	if (source == SOURCE_NONE)
		return fallback;

	const unsigned slot = (source == SOURCE_L2_AXIS) ? unsigned(RETRO_DEVICE_ID_JOYPAD_L2)
			: (source == SOURCE_R2_AXIS) ? unsigned(RETRO_DEVICE_ID_JOYPAD_R2)
			: source;
	char const *const label = (slot < RETROPAD_ID_COUNT) ? row.labels[slot] : nullptr;
	return ((label != nullptr) && (*label != '\0')) ? label : fallback;
}

// Buttons with a fixed MAME item id rather than a number: their slot is not in the layout table
// above, so no device type can move them.
//
// The stick clicks are here rather than among the numbered buttons on purpose: they are the two
// controls the cabinet layouts have no use for, so they carry whatever is left over — the service
// switches, the UI menu, and daytona's ninth button. They still need items of their own — an
// assignment naming an item the device never added is dropped on the floor by add_assignment(),
// which is what used to happen to the UI_MENU binding below.
//
// This used to say "no Model 2 game has nine buttons", and that was wrong: daytona has exactly nine.
// Buttons 1-5 are the gearbox, read back through daytona_gearbox_r, and 6-9 are the VR camera
// buttons. All nine are layout slots now, so none of them is here.
//
// ⚠ R3 is still listed, and it no longer carries IPT_BUTTON9 — the layout table does. It keeps an
// item of its own so that a row is free to leave it alone and so a remap has something to name; the
// item ids of both stick clicks shifted up by one to make room for the ninth numbered button.
struct fixed_button { unsigned slot; unsigned id; input_item_id item; };

constexpr fixed_button FIXED_BUTTONS[] = {
	{ libretro_m2_pad_device::BUTTON_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT, ITEM_ID_SELECT },
	{ libretro_m2_pad_device::BUTTON_START,  RETRO_DEVICE_ID_JOYPAD_START,  ITEM_ID_START },
	{ libretro_m2_pad_device::BUTTON_UP,     RETRO_DEVICE_ID_JOYPAD_UP,     ITEM_ID_HAT1UP },
	{ libretro_m2_pad_device::BUTTON_DOWN,   RETRO_DEVICE_ID_JOYPAD_DOWN,   ITEM_ID_HAT1DOWN },
	{ libretro_m2_pad_device::BUTTON_LEFT,   RETRO_DEVICE_ID_JOYPAD_LEFT,   ITEM_ID_HAT1LEFT },
	{ libretro_m2_pad_device::BUTTON_RIGHT,  RETRO_DEVICE_ID_JOYPAD_RIGHT,  ITEM_ID_HAT1RIGHT },
	{ libretro_m2_pad_device::BUTTON_L3,     RETRO_DEVICE_ID_JOYPAD_L3,     ITEM_ID_BUTTON10 },
	{ libretro_m2_pad_device::BUTTON_R3,     RETRO_DEVICE_ID_JOYPAD_R3,     ITEM_ID_BUTTON11 } };

static_assert(std::size(FIXED_BUTTONS) + libretro_m2_pad_device::NUMBERED_BUTTONS
				== libretro_m2_pad_device::BUTTON_COUNT,
		"every slot is filled by exactly one of the layout table or the fixed list");


//============================================================
//  the diagnostic combo
//============================================================

// What each model2_diagnostic_input value means, in RetroPad ids and one flag. Indexed by
// m2opt::diagnostic_input, which is also the position of the value in the option's own list, so the
// words the player picked and the controls read here are one table apart and cannot drift.
//
// These are RetroPad ids on purpose and not slots: the option says "Start + A + B", meaning the pad's
// A and B, and it has to keep meaning that under either pad layout. What the layout decides is which
// MAME buttons the combo then has to *consume*, which update_diagnostic() works back through it.
//
// ⚠ Model 2 has two switches where FBNeo models one. This drives IPT_SERVICE, the test switch that
// opens the menu; IPT_SERVICE1, the service coin, has no equivalent in FBNeo's vocabulary and stays
// on L3 whenever the option is not None. devnotes/lightgun.md §2.5.3.
constexpr unsigned COMBO_NO_ID = ~0U;

struct diagnostic_combo { unsigned ids[3]; bool hold; };

constexpr diagnostic_combo DIAGNOSTIC_COMBOS[m2opt::DIAG_COUNT] = {
	/* None                */ { { COMBO_NO_ID, COMBO_NO_ID, COMBO_NO_ID }, false },
	/* Hold Start          */ { { RETRO_DEVICE_ID_JOYPAD_START,  COMBO_NO_ID, COMBO_NO_ID }, true },
	/* Start + A + B       */ { { RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B }, false },
	/* Hold Start + A + B  */ { { RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B }, true },
	/* Start + L + R       */ { { RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R }, false },
	/* Hold Start + L + R  */ { { RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R }, true },
	/* Hold Select         */ { { RETRO_DEVICE_ID_JOYPAD_SELECT, COMBO_NO_ID, COMBO_NO_ID }, true },
	/* Select + A + B      */ { { RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B }, false },
	/* Hold Select + A + B */ { { RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B }, true },
	/* Select + L + R      */ { { RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R }, false },
	/* Hold Select + L + R */ { { RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R }, true } };

static_assert(std::size(DIAGNOSTIC_COMBOS) == m2opt::DIAG_COUNT,
		"one combo per declared value of model2_diagnostic_input, in the same order");

// How long a "Hold …" combo wants. About a second at the driver's 57.52 Hz — named once here rather
// than spelled out at the comparison, because a frame count with no name is a frame count nobody
// dares change.
constexpr unsigned COMBO_HOLD_FRAMES = 58;

} // anonymous namespace


//============================================================
//  libretro_m2_pad_device
//============================================================

libretro_m2_pad_device::libretro_m2_pad_device(
		std::string &&name,
		std::string &&id,
		input_module &module,
		unsigned port,
		unsigned diagnostic,
		unsigned const *layout)
	: libretro_m2_device(std::move(name), std::move(id), module, port)
	, m_diagnostic((diagnostic < m2opt::DIAG_COUNT) ? diagnostic : unsigned(m2opt::DIAG_NONE))
	, m_layout((layout != nullptr) ? layout : GENERIC_LAYOUT.sources)
{
	reset();
}

void libretro_m2_pad_device::reset()
{
	std::memset(m_axes, 0, sizeof(m_axes));
	std::memset(m_buttons, 0, sizeof(m_buttons));
	m_combo = 0;
	m_combo_frames = 0;
}

// Reads one RetroPad. Runs on the libretro thread from retro_run(), between the emulation thread
// parking on the frame baton and being released for the next frame, so no locking is needed: the
// only reader is asleep.
//
// device is what the frontend last selected for this port. Two things about it matter here:
//
//   * RETRO_DEVICE_NONE reports nothing. This states an intent rather than fixing anything — a
//     frontend with a port set to None already answers 0 to every state_cb for it — which is also
//     why it is safe to leave unexercised by the harness, which cannot select None.
//   * on a gun port the two primary stick axes are silenced, and nothing else is. See the gate at
//     the bottom of this function.
//
// EVERY other value uses this machine's layout row, and that is the whole dispatch now. It used to
// branch on two pad subclasses; those are gone, along with the third entry that carried the per-game
// row (see the header). What survives from that arrangement is the property that made it safe: an
// unrecognised device value — including a config still remembering one of the retired subclass ids —
// must not silently stop the pad working, so it lands on the row rather than on nothing.
//
// m_layout is read here and nowhere else, once per frame, which is what makes the row free: it is not
// baked into any MAME assignment, so nothing about it needs a content reload.
void libretro_m2_pad_device::update(retro_input_state_t state_cb, unsigned device)
{
	const unsigned kind = device & RETRO_DEVICE_MASK;
	if (kind == RETRO_DEVICE_NONE)
	{
		reset();
		return;
	}

	unsigned const *const layout = m_layout;

	const std::pair<unsigned, unsigned> sticks[] = {
		{ RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X },
		{ RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y },
		{ RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X },
		{ RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y } };
	for (unsigned axis = 0; axis < std::size(sticks); axis++)
	{
		const int16_t raw = state_cb(m_port, RETRO_DEVICE_ANALOG, sticks[axis].first, sticks[axis].second);
		m_axes[axis] = normalize_absolute_axis(raw, -32'767, 32'767);
	}

	// Triggers. RETRO_DEVICE_INDEX_ANALOG_BUTTON is optional — a frontend that does not implement
	// it returns 0 for everything — so fall back to the digital button, which every frontend has.
	// MAME wants triggers to rest at zero and run negative, the same convention the SDL OSD uses,
	// because that is what the ITEM_MODIFIER_NEG pedal assignments below expect.
	const std::pair<unsigned, unsigned> triggers[] = {
		{ AXIS_L2, RETRO_DEVICE_ID_JOYPAD_L2 },
		{ AXIS_R2, RETRO_DEVICE_ID_JOYPAD_R2 } };
	for (auto [axis, button] : triggers)
	{
		int16_t raw = state_cb(m_port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, button);
		if ((raw <= 0) && state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, button))
			raw = 32'767;
		m_axes[axis] = -normalize_absolute_axis(raw, -32'767, 32'767);
	}

	// The numbered buttons, through the port's layout: slot n holds MAME button n+1, from whichever
	// source the selected device type names. The full device value is compared, not the masked
	// class, because the layouts are subclasses of the same class.
	//
	// ⚠ This runs AFTER the axes rather than before them, and it has to: a layout row may name a
	// trigger threshold, and the threshold reads m_axes. It used to run first, when the trigger
	// buttons were built in configure() from an item that read the axis directly. Both orders see
	// the same frontend snapshot, so nothing else about moving it matters.
	//
	// generic_button_get_state<> shifts right by 7, hence 0x80 rather than 1.
	for (unsigned n = 0; n < NUMBERED_BUTTONS; n++)
		m_buttons[BUTTON_1 + n] = read_source(state_cb, layout[n]);

	for (auto const &fixed : FIXED_BUTTONS)
		m_buttons[fixed.slot] = state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, fixed.id) ? 0x80 : 0x00;

	// After both button reads, because a fired combo takes its controls back out of them.
	update_diagnostic(state_cb, layout);

	// The gate, and it covers these two axes and nothing else.
	//
	// configure() below gives the primary stick to IPT_LIGHTGUN_X/Y through
	// add_directional_assignments, and MAME ORs that onto the core default GUNCODE_X_INDEXED(n) |
	// MOUSECODE_X_INDEXED(n) rather than replacing it (ioport.cpp, apply_device_defaults). Two OR'd
	// *absolute* axes are then SUMMED and saturated (input.cpp, accumulate_axis_value), so a gun
	// aiming at screen centre plus a stick pushed left aims hard left, and a gun at the left edge
	// plus a nudge left stops moving. Both read as calibration bugs.
	//
	// A contributor sitting at exactly 0 adds nothing, so "exactly one absolute source moves at a
	// time" is the whole fix, and it belongs here rather than in the assignments. Buttons, the
	// d-pad, the triggers and the right stick are untouched on purpose: a RETRO_DEVICE_LIGHTGUN port
	// reports no shoulder buttons and no L3/R3, and this core draws no MAME UI, so gating them would
	// leave the gun games with no route into a test menu at all. Switches OR into switches without
	// summing, so there is nothing to fix there. devnotes/lightgun.md §1.2, §2.2, §2.4.
	if (kind == RETRO_DEVICE_LIGHTGUN)
		m_axes[AXIS_LEFT_X] = m_axes[AXIS_LEFT_Y] = 0;
}

// One layout entry to a button state. Everything without the tag bit is an ordinary RetroPad button
// id and is read as one; two of the tagged sources are the analogue triggers, compared against the
// same threshold sdl_game_controller_device uses. The axes rest at 0 and run to ABSOLUTE_MIN when
// pulled, hence the sign. The third tagged source is SOURCE_NONE, a MAME button with no pad control
// behind it, which is answered before the trigger test rather than after it: its low bits are not a
// trigger index and would read L2.
//
// This threshold used to live in an item state getter of its own, reading the axis whenever MAME
// asked. Doing it here instead is what lets a trigger be a layout source, and it costs nothing: the
// axis is written once per frame from the same snapshot the buttons come from, so a value computed
// here and a value computed on demand are always the same value.
int32_t libretro_m2_pad_device::read_source(retro_input_state_t state_cb, unsigned source) const
{
	if (source == SOURCE_NONE)
		return 0x00;

	if (source & SOURCE_TAG)
		return (m_axes[AXIS_L2 + (source & 1)] <= -16'384) ? 0x80 : 0x00;

	return state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, source) ? 0x80 : 0x00;
}

// The cabinet's test switch, as a button that does not exist on the pad. m_combo is an ordinary
// button item to MAME (configure() below assigns IPT_SERVICE to it), so nothing about how the switch
// reaches the machine is new — only how its state is arrived at.
//
// Reading the combo's controls straight from the frontend rather than from m_buttons is what keeps
// the option's words true under either layout: the slots hold MAME button numbers, and "A" is a pad
// control. layout is passed in so the consumption below can go the other way, from a pad id back to
// whichever slot that layout fills from it.
//
// 🚨 Consumption is the part that is not optional. Without it "Start + A + B" also presses Start —
// the machine would take a credit on the way into its own test menu — so every control the fired
// combo names is cleared for the frame. That is also why this runs after both button reads rather
// than instead of them: a control that is part of the combo is still an ordinary button until the
// combo fires.
//
// A "Hold …" combo fires only after its controls have been down together for COMBO_HOLD_FRAMES, and
// then goes on firing for as long as they stay down — a held switch is a held switch. The frames
// before it fires are deliberately *not* consumed, so a tap of Start on "Hold Start" still starts a
// game. devnotes/lightgun.md §2.5.3.
void libretro_m2_pad_device::update_diagnostic(retro_input_state_t state_cb, unsigned const *layout)
{
	m_combo = 0x00;
	if (m_diagnostic == m2opt::DIAG_NONE)
		return;

	diagnostic_combo const &combo = DIAGNOSTIC_COMBOS[m_diagnostic];

	bool down = true;
	for (unsigned id : combo.ids)
		down = down && ((id == COMBO_NO_ID) || (state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, id) != 0));

	if (!down)
	{
		m_combo_frames = 0;
		return;
	}

	// saturating, so a combo held for minutes cannot wrap back below the threshold
	if (m_combo_frames < COMBO_HOLD_FRAMES)
		m_combo_frames++;
	if (combo.hold && (m_combo_frames < COMBO_HOLD_FRAMES))
		return;

	// generic_button_get_state<> shifts right by 7, as with every other button here
	m_combo = 0x80;

	auto const in_combo = [&combo] (unsigned id)
	{
		for (unsigned which : combo.ids)
		{
			if ((which != COMBO_NO_ID) && (which == id))
				return true;
		}
		return false;
	};

	for (unsigned n = 0; n < NUMBERED_BUTTONS; n++)
	{
		if (in_combo(layout[n]))
			m_buttons[BUTTON_1 + n] = 0x00;
	}
	for (auto const &fixed : FIXED_BUTTONS)
	{
		if (in_combo(fixed.id))
			m_buttons[fixed.slot] = 0x00;
	}
}

// The SDL game-controller configure() with the availability probing taken out: every control
// listed here always exists on a RetroPad.
void libretro_m2_pad_device::configure(osd::input_device &device)
{
	osd::input_device::assignment_vector assignments;

	// --- axes ---
	input_item_id axisitems[AXIS_COUNT];
	for (unsigned axis = 0; axis < AXIS_COUNT; axis++)
	{
		axisitems[axis] = device.add_item(
				RETROPAD_AXIS_NAMES[axis],
				std::string_view(),
				AXIS_ITEMS[axis],
				generic_axis_get_state<int32_t>,
				&m_axes[axis]);
	}

	// --- automatically numbered buttons ---
	// Named for the MAME button number rather than for a pad control, because which control fills
	// the slot is a per-port choice the player can change while the machine runs and these names
	// are fixed here for its lifetime.
	//
	// ⚠ All nine are alike now. Buttons 7 and 8 used to be built separately here, from an item that
	// read a trigger axis through a threshold getter, and that separateness was the bug: an item
	// built in configure() is fixed for the device's lifetime, so no layout could move them off the
	// pedals. The threshold moved into read_source(); these are nine identical slots.
	input_item_id buttonitems[BUTTON_COUNT];
	std::fill(std::begin(buttonitems), std::end(buttonitems), ITEM_ID_INVALID);

	input_item_id button_item = ITEM_ID_BUTTON1;

	for (unsigned n = 0; n < NUMBERED_BUTTONS; n++)
	{
		buttonitems[BUTTON_1 + n] = device.add_item(
				util::string_format("Button %u", n + 1),
				std::string_view(),
				button_item++,
				generic_button_get_state<int32_t>,
				&m_buttons[BUTTON_1 + n]);
		add_button_assignment(assignments, ioport_type(IPT_BUTTON1 + n), { buttonitems[BUTTON_1 + n] });
	}

	// --- buttons with fixed item ids ---
	for (auto [slot, which, item] : FIXED_BUTTONS)
	{
		buttonitems[slot] = device.add_item(
				RETROPAD_BUTTON_NAMES[which],
				std::string_view(),
				item,
				generic_button_get_state<int32_t>,
				&m_buttons[slot]);
	}

	// --- movement ---
	input_item_id diraxis[2][2];
	choose_primary_stick(
			diraxis,
			axisitems[AXIS_LEFT_X],
			axisitems[AXIS_LEFT_Y],
			axisitems[AXIS_RIGHT_X],
			axisitems[AXIS_RIGHT_Y]);

	// digital joystick, plus the analogue types Model 2 actually uses — this call is where
	// IPT_PADDLE (the steering games), IPT_AD_STICK_X/Y (the twin-stick and flight sets) and
	// IPT_LIGHTGUN_X/Y (the gun games) all get their defaults
	add_directional_assignments(
			assignments,
			diraxis[0][0],
			diraxis[0][1],
			buttonitems[BUTTON_LEFT],
			buttonitems[BUTTON_RIGHT],
			buttonitems[BUTTON_UP],
			buttonitems[BUTTON_DOWN]);

	// the secondary stick drives the third analogue axis; failing that, combine the triggers
	if (!add_assignment(assignments, IPT_AD_STICK_Z, SEQ_TYPE_STANDARD, ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_NONE, { diraxis[1][1], diraxis[1][0] }))
	{
		assignments.emplace_back(
				IPT_AD_STICK_Z,
				SEQ_TYPE_STANDARD,
				input_seq(
						make_code(ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_NONE, axisitems[AXIS_L2]),
						make_code(ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_REVERSE, axisitems[AXIS_R2])));
	}

	// pedals on the triggers — accelerator right, brake left, as every driving game expects.
	//
	// Known limitation, measured rather than suspected: on daytona these share the triggers with
	// IPT_BUTTON7/BUTTON8 (VR2/VR3), which take their state from a threshold on the same two axes,
	// so flooring the accelerator also presses VR3. It is not fixable by moving something — daytona
	// wants nine buttons, steering and two pedals, and a RetroPad has nothing free once coin, start
	// and the d-pad are spoken for. The fix is a per-set layout table, which is a later phase.
	//
	// IPT_PEDAL3 (srallyc's hand brake, the one set that has it) rides on the button-5 slot rather
	// than on a named pad control, and that is deliberate: srallyc PORT_INCLUDEs gears, whose GEAR 4
	// is IPT_BUTTON5, so the two already shared a control before there were layouts. Tying the hand
	// brake to the slot keeps them sharing one under every layout instead of having them collide on
	// some and not others.
	add_assignment(assignments, IPT_PEDAL,  SEQ_TYPE_STANDARD, ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_NEG, { axisitems[AXIS_R2] });
	add_assignment(assignments, IPT_PEDAL2, SEQ_TYPE_STANDARD, ITEM_CLASS_ABSOLUTE, ITEM_MODIFIER_NEG, { axisitems[AXIS_L2] });
	add_assignment(assignments, IPT_PEDAL3, SEQ_TYPE_INCREMENT, ITEM_CLASS_SWITCH, ITEM_MODIFIER_NONE, { buttonitems[BUTTON_5] });

	// twin sticks (Virtual On) off the two analogue sticks, with the D-pad and face diamond as
	// the digital fallback. The diamond is named by slot, i.e. by MAME button number — buttons 3, 2,
	// 4 and 1 are Y, A, X and B in every layout offered, because only the shoulder pair moves.
	add_twin_stick_assignments(
			assignments,
			axisitems[AXIS_LEFT_X],
			axisitems[AXIS_LEFT_Y],
			axisitems[AXIS_RIGHT_X],
			axisitems[AXIS_RIGHT_Y],
			buttonitems[BUTTON_LEFT],
			buttonitems[BUTTON_RIGHT],
			buttonitems[BUTTON_UP],
			buttonitems[BUTTON_DOWN],
			buttonitems[BUTTON_3],
			buttonitems[BUTTON_2],
			buttonitems[BUTTON_4],
			buttonitems[BUTTON_1]);

	// --- fixed functions ---
	// IPT_SELECT/IPT_START are the per-player types; COIN1..n and START1..n already default to
	// JOYCODE_SELECT_INDEXED(n) / JOYCODE_START_INDEXED(n) in inpttype.ipp, so the arcade coin and
	// start controls come from the ITEM_ID_SELECT/ITEM_ID_START items added above.
	add_button_assignment(assignments, IPT_SELECT, { buttonitems[BUTTON_SELECT] });
	add_button_assignment(assignments, IPT_START,  { buttonitems[BUTTON_START] });

	// MAME's own UI is not drawn in a libretro core, but the assignments cost nothing and keep
	// the input-remapping menus navigable if it ever is.
	//
	// ⚠ These name button SLOTS, so which pad control performs a UI action now depends on the
	// layout — page up/down were the trigger items themselves before, and are slots 7 and 8 here,
	// which is where every layout offered so far puts the triggers. Inert either way while no menu
	// is drawn, and not worth a second mechanism to keep stable.
	add_button_assignment(assignments, IPT_UI_SELECT, { buttonitems[BUTTON_1] });
	add_button_assignment(assignments, IPT_UI_BACK,   { buttonitems[BUTTON_2] });
	add_button_assignment(assignments, IPT_UI_CLEAR,  { buttonitems[BUTTON_3] });
	add_button_assignment(assignments, IPT_UI_HELP,   { buttonitems[BUTTON_4] });
	add_button_pair_assignment(assignments, IPT_UI_PAGE_UP, IPT_UI_PAGE_DOWN, buttonitems[BUTTON_7], buttonitems[BUTTON_8]);

	// The cabinet's test switch, on the synthetic combo item rather than on a pad control. The item
	// is added whichever way the option is set: it is 0 forever when the combo is None, and keeping
	// the item list independent of an option means a ctrlr file or a saved remap cannot change
	// meaning underneath the player when they change it.
	//
	// ITEM_ID_BUTTON12 is free: the nine numbered buttons take 1..9 and L3/R3 take 10 and 11. ✅ The
	// safety argument survives the shift up from BUTTON11 unchanged — IPT_BUTTON12's own default in
	// inpttype.ipp is KEYCODE_COMMA (line 45), a keyboard code, and this OSD registers no keyboard,
	// so nothing else can arrive at this item. Player 2's default is an empty sequence, as before.
	const input_item_id comboitem = device.add_item(
			"Diagnostic Combo",
			std::string_view(),
			ITEM_ID_BUTTON12,
			generic_button_get_state<int32_t>,
			&m_combo);

	// Since this core draws no MAME menu, the combo is the only way into a game's test mode — and it
	// is None by default, because a combination nobody asked for is worse than a menu they have to
	// enable. IPT_SERVICE1, the service coin, has no combination of its own and rides on L3 whenever
	// the option is set to anything: it is a free credit, so it should not be one held button away,
	// but losing it entirely is not acceptable either. Both types are player 0 in inpttype.ipp, so
	// apply_device_defaults() lands them on pad 1 alone and pad 2's copy is skipped.
	if (m_diagnostic != m2opt::DIAG_NONE)
	{
		add_button_assignment(assignments, IPT_SERVICE,  { comboitem });
		add_button_assignment(assignments, IPT_SERVICE1, { buttonitems[BUTTON_L3] });
	}
	else
	{
		add_button_assignment(assignments, IPT_UI_MENU, { buttonitems[BUTTON_L3] });
	}

	// daytona's VR4 (Green) used to be assigned here, to the R3 item, because button 9 was not a
	// layout slot. It is one now — both layouts name R3 for it, so it lands exactly where it did —
	// and assigning IPT_BUTTON9 twice would bind it to two items at once. devnotes/per-game-input.md
	// §3.1.

	device.set_default_assignments(std::move(assignments));
}


//============================================================
//  libretro_m2_gun_device
//============================================================

libretro_m2_gun_device::libretro_m2_gun_device(std::string &&name, std::string &&id, input_module &module, unsigned port)
	: libretro_m2_device(std::move(name), std::move(id), module, port)
{
	std::memset(m_axes, 0, sizeof(m_axes));
	std::memset(m_buttons, 0, sizeof(m_buttons));
}

void libretro_m2_gun_device::reset()
{
	std::memset(m_axes, 0, sizeof(m_axes));
	std::memset(m_buttons, 0, sizeof(m_buttons));
}

// Reads one lightgun, on the libretro thread, under the same parked-emulation-thread rule as the
// pad above. A port that is not set to a gun reports nothing — which is not merely tidy, it is half
// of the gate documented in libretro_m2_pad_device::update(): a gun resting at 0 adds 0 to the
// port it shares with the pad's stick, so the two never sum.
void libretro_m2_gun_device::update(retro_input_state_t state_cb, unsigned device)
{
	if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_LIGHTGUN)
	{
		reset();
		return;
	}

	// SCREEN_X/Y are the frontend's absolute pointer, -0x8000..0x7fff across the viewport. They are
	// normalised the same symmetric way the pad's sticks are, so that dead centre comes out as
	// exactly 0 rather than one part in 65536 off it — which the gate above needs to be exact, and
	// which also puts a centred gun exactly on the port's default value.
	//
	// There is no scale factor and no windowing here, and there must never be one: PORT_MINMAX is
	// the cabinet's own calibration, MAME maps full-scale input straight onto it, and correcting an
	// aim offset on this side is the specific mistake devnotes/lightgun.md §5 warns about.
	const std::pair<unsigned, unsigned> axes[] = {
		{ AXIS_X, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X },
		{ AXIS_Y, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y } };
	for (auto [axis, id] : axes)
	{
		const int16_t raw = state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, id);
		m_axes[axis] = normalize_absolute_axis(raw, -32'767, 32'767);
	}

	const std::pair<unsigned, unsigned> buttons[] = {
		{ BUTTON_TRIGGER, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER },
		{ BUTTON_AUX_A,   RETRO_DEVICE_ID_LIGHTGUN_AUX_A },
		{ BUTTON_AUX_B,   RETRO_DEVICE_ID_LIGHTGUN_AUX_B } };
	for (auto [button, id] : buttons)
		m_buttons[button] = state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, id) ? 0x80 : 0x00;

	// Offscreen and reload, and the whole of it is these four lines because the driver already does
	// the work. model2_state::lightgun_offscreen_r (model2.cpp:1136) reports a player as offscreen
	// when either axis lands within 5 % of the ends of that port's own PORT_MINMAX — so offscreen is
	// not "a value outside the range" but "a value pinned at the edge of the range", which is exactly
	// what driving the axis to ABSOLUTE_MIN produces. Nothing has to survive a clamp; the clamp is
	// the mechanism. devnotes/lightgun.md §1.4.
	//
	// ABSOLUTE_MIN rather than a coordinate near the edge, because it is the one value MAME's scaling
	// maps exactly onto minval: the border is 5 % wide and a scale factor of ours picking a point
	// inside it would be the fudge §5 warns against, aimed at a target that moves per set.
	//
	// Both axes go, not just X, and that is deliberate even though either alone would set the bit:
	// the gun is meant to be pointing away from the screen, and a set whose Y port is the one the
	// game reads would otherwise see a shot at a real place on the playfield.
	//
	// RELOAD is IS_OFFSCREEN plus a trigger the frontend never sent — a real cabinet reloads by
	// firing off the screen, so the button has to produce both halves or the magazine never refills.
	// It ORs into the trigger rather than replacing it, so a player holding both gets one press.
	const bool reload = state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0;
	if (reload || state_cb(m_port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
		m_axes[AXIS_X] = m_axes[AXIS_Y] = osd::input_device::ABSOLUTE_MIN;
	if (reload)
		m_buttons[BUTTON_TRIGGER] = 0x80;
}

// Four items and one assignment, and the shortness is the point: MAME's core input types already
// name this device.
//
//   IPT_LIGHTGUN_X/Y default to GUNCODE_X_INDEXED(n) | MOUSECODE_X_INDEXED(n)  (inpttype.ipp)
//   IPT_BUTTON1      defaults to  ... | GUNCODE_BUTTON1_INDEXED(n)
//   IPT_BUTTON2      defaults to  ... | GUNCODE_BUTTON2_INDEXED(n)
//
// and GUNCODE_*_INDEXED(n) is exactly "lightgun device n, this item". So the axes, the trigger and
// AUX_A are bound the moment the items exist, and adding assignments for them would only put a
// second copy of a binding that already works into this file. IPT_BUTTON3 has no such default,
// which is why it is the one that is spelled out.
//
// The MOUSECODE half of those defaults is inert here: -nomouse is in the argument vector and a
// disabled device class returns 0 before reading any item (input.cpp, code_value).
void libretro_m2_gun_device::configure(osd::input_device &device)
{
	device.add_item("X Axis", std::string_view(), ITEM_ID_XAXIS, generic_axis_get_state<int32_t>, &m_axes[AXIS_X]);
	device.add_item("Y Axis", std::string_view(), ITEM_ID_YAXIS, generic_axis_get_state<int32_t>, &m_axes[AXIS_Y]);

	device.add_item("Trigger", std::string_view(), ITEM_ID_BUTTON1, generic_button_get_state<int32_t>, &m_buttons[BUTTON_TRIGGER]);
	device.add_item("Aux A",   std::string_view(), ITEM_ID_BUTTON2, generic_button_get_state<int32_t>, &m_buttons[BUTTON_AUX_A]);
	const input_item_id auxb = device.add_item(
			"Aux B", std::string_view(), ITEM_ID_BUTTON3, generic_button_get_state<int32_t>, &m_buttons[BUTTON_AUX_B]);

	// Built by hand rather than through joystick_assignment_helper, whose make_code() hardcodes
	// DEVICE_CLASS_JOYSTICK. The index has to be 0: apply_device_defaults() asserts on it and then
	// rewrites it to the real device index, which is how one assignment serves both players.
	osd::input_device::assignment_vector assignments;
	assignments.emplace_back(
			IPT_BUTTON3,
			SEQ_TYPE_STANDARD,
			input_seq(input_code(DEVICE_CLASS_LIGHTGUN, 0, ITEM_CLASS_SWITCH, ITEM_MODIFIER_NONE, auxb)));
	device.set_default_assignments(std::move(assignments));
}


//============================================================
//  libretro_m2_input
//============================================================

libretro_m2_input::libretro_m2_input(unsigned diagnostic)
	: input_module_impl<libretro_m2_device, libretro_m2_osd_interface>(OSD_JOYSTICKINPUT_PROVIDER, "libretro")
	, m_diagnostic(diagnostic)
{
}

// Out of line so libretro_m2_pad_device is complete where the device list is destroyed.
libretro_m2_input::~libretro_m2_input()
{
}

bool libretro_m2_input::has_layout(char const *name, char const *parent)
{
	return &layout_for(name, parent) != &GENERIC_LAYOUT;
}

// The frontend's remap labels, built from the same row the pad reads.
//
// One array covers every port, because a row is a property of the machine and not of a port. Ports 2
// and 3 get the same labels on a two-player set, where they bind to types the set does not declare —
// which is what they did before this existed and costs nothing.
//
// A null or empty label emits NO entry, and that is deliberate rather than lazy: RetroArch shows a
// descriptor as the control's name in its Controls list, so an entry with a placeholder string is a
// control claiming to do something. vf2 has three buttons; X, L, R and the triggers genuinely do
// nothing on it and should read as nothing.
struct retro_input_descriptor const *libretro_m2_input::descriptors(
		char const *name, char const *parent, bool service_coin)
{
	// Static because the frontend keeps the pointer: RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS is
	// documented as taking an array the core owns, and RetroArch reads it after the call returns.
	static std::vector<struct retro_input_descriptor> descs;

	game_layout const &row = layout_for(name, parent);
	descs.clear();

	const auto push = [] (unsigned port, unsigned device, unsigned index, unsigned id, char const *desc)
	{
		if ((desc == nullptr) || (*desc == '\0'))
			return;
		descs.push_back({ port, device, index, id, desc });
	};

	for (unsigned port = 0; port < MAX_PADS; port++)
	{
		for (unsigned id = 0; id < RETROPAD_ID_COUNT; id++)
		{
			// L3 carries IPT_SERVICE1 only while a diagnostic combo is selected; with the option at None
			// it is IPT_UI_MENU, and this core draws no MAME UI, so it does nothing at all. Labelling it
			// then would be the one thing worse than input-map.md §4's complaint that it has no label.
			if ((id == RETRO_DEVICE_ID_JOYPAD_L3) && !service_coin)
				continue;
			push(port, RETRO_DEVICE_JOYPAD, 0, id, row.labels[id]);
		}

		push(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, row.labels[LABEL_ANALOG_LX]);
		push(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, row.labels[LABEL_ANALOG_LY]);
		push(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, row.labels[LABEL_ANALOG_RX]);
		push(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, row.labels[LABEL_ANALOG_RY]);
	}

	// The gun controls, on the ports that can carry a gun — and only on the sets that have one. The
	// trigger and the two aux buttons are MAME buttons 1-3, so they take their names from the row's own
	// slots rather than from a second table: vcop's button 1 is PORT_NAMEd "P1 Trigger" in the driver,
	// and that is what the player sees.
	//
	// ⚠️ RELOAD is listed, and it was missing before: it is the control that makes vcop playable past
	// the first magazine (input-map.md §4's second gap). It has no MAME button of its own — the gun
	// device synthesises an offscreen shot from it — so its label is ours to write.
	for (unsigned port = 0; row.lightgun && (port < MAX_GUNS); port++)
	{
		push(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, gun_label(row, 1, "Trigger"));
		push(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A,   gun_label(row, 2, nullptr));
		push(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B,   gun_label(row, 3, nullptr));
		push(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD,  "Reload");
	}

	descs.push_back({ 0, 0, 0, 0, nullptr });
	return descs.data();
}

void libretro_m2_input::input_init(running_machine &machine)
{
	input_module_impl<libretro_m2_device, libretro_m2_osd_interface>::input_init(machine);

	// ⚠️ The layout editor's data source (M2VK_INPUT_DUMP) is NOT taken here, and it was tried here
	// first: osd().init() runs at machine.cpp:156 and m_ioport.initialize() at 169, so the port list is
	// still empty at this point and the dump comes out with no fields at all. It is taken from
	// libretro_m2_osd_interface::update() instead, behind safe_to_read(). See m2vk_inputdump.h.

	// Resolved once, here, and shared by every pad: the row belongs to the machine and a port has no say
	// in it any more. It is a pointer into a constexpr table with static storage duration, so it outlives
	// the devices without anything owning it.
	game_layout const &row = layout_for(machine.system().name, machine.system().parent);
	unsigned const *const layout = row.sources;
	osd_printf_verbose("libretro_m2: input layout '%s'\n", row.id);

	// Fixed set, no enumeration: the frontend always has as many RetroPads as we ask about, and
	// the device index has to equal the player number for the START1/COIN1 defaults to line up.
	for (unsigned port = 0; port < MAX_PADS; port++)
	{
		create_device<libretro_m2_pad_device>(
				DEVICE_CLASS_JOYSTICK,
				util::string_format("RetroPad %u", port + 1),
				util::string_format("RETROPAD_%u", port + 1),
				port,
				m_diagnostic,
				layout);
	}

	// The guns, on the same terms and for the same reason: created unconditionally, in port order so
	// that the class-local device index is the player number GUNCODE_X_INDEXED(n) names. A set with
	// no IPT_LIGHTGUN_X port simply has nothing for them to bind to, and a gun reporting 0
	// contributes 0, so the 77 non-gun sets pay two idle devices and nothing else.
	//
	// They are also created whether or not any port is currently set to a gun, which is what makes a
	// mid-run device change free: MAME fixes its options when the machine is built, but
	// retro_set_controller_port_device can arrive at any time, and a frontend's input menu will do
	// exactly that. devnotes/lightgun.md §2.1.
	for (unsigned port = 0; port < MAX_GUNS; port++)
	{
		create_device<libretro_m2_gun_device>(
				DEVICE_CLASS_LIGHTGUN,
				util::string_format("Light Gun %u", port + 1),
				util::string_format("LIGHTGUN_%u", port + 1),
				port);
	}
}

void libretro_m2_input::poll_frontend(retro_input_state_t state_cb, unsigned const *port_device)
{
	if ((state_cb == nullptr) || (port_device == nullptr))
		return;

	devicelist().for_each_device(
			[state_cb, port_device] (libretro_m2_device &device)
			{ device.update(state_cb, port_device[device.port()]); });
}
