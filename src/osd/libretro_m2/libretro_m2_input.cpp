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

// Which RetroPad control produces each MAME button, indexed by button number minus one. Read once
// per frame in update() and nowhere else, which is what makes a mid-run layout change free.
//
// These are FBNeo's, resolved from its RETRO_DEVICE_ID_FIREnn macros (src/burner/libretro/
// retro_input.cpp) — matching it was the point, because a player coming from any other libretro
// arcade core arrives with those fingers already trained. Buttons 1-4 are the face diamond in both
// — RetroPad's face is SNES-style, so the diamond maps to SDL's A/B/X/Y as B=bottom, A=right,
// Y=left, X=top — and only the shoulder pair moves. L2/R2 are absent from Classic for the reason
// they always were: they are exposed as the analogue trigger axes below and take their MAME button
// item (IPT_BUTTON7/8) from a threshold on the axis.
//
// ⚠ Modern puts button 5 on R2, which is also the accelerator pedal. Both fire, and that is
// accepted rather than gated: the sets with a button 5 are the driving sets, so a player who chose
// Modern on one of those chose it knowing what is under that finger. devnotes/lightgun.md §2.5.1.
enum : unsigned
{
	LAYOUT_CLASSIC = 0,
	LAYOUT_MODERN,
	LAYOUT_COUNT
};

constexpr unsigned BUTTON_LAYOUTS[LAYOUT_COUNT][libretro_m2_pad_device::NUMBERED_BUTTONS] = {
	{
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_Y,
		RETRO_DEVICE_ID_JOYPAD_X,
		RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_L
	},
	{
		RETRO_DEVICE_ID_JOYPAD_B,
		RETRO_DEVICE_ID_JOYPAD_A,
		RETRO_DEVICE_ID_JOYPAD_Y,
		RETRO_DEVICE_ID_JOYPAD_X,
		RETRO_DEVICE_ID_JOYPAD_R2,
		RETRO_DEVICE_ID_JOYPAD_R
	} };

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
// buttons. IPT_BUTTON10 is still unassigned because nothing needs it.
struct fixed_button { unsigned slot; unsigned id; input_item_id item; };

constexpr fixed_button FIXED_BUTTONS[] = {
	{ libretro_m2_pad_device::BUTTON_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT, ITEM_ID_SELECT },
	{ libretro_m2_pad_device::BUTTON_START,  RETRO_DEVICE_ID_JOYPAD_START,  ITEM_ID_START },
	{ libretro_m2_pad_device::BUTTON_UP,     RETRO_DEVICE_ID_JOYPAD_UP,     ITEM_ID_HAT1UP },
	{ libretro_m2_pad_device::BUTTON_DOWN,   RETRO_DEVICE_ID_JOYPAD_DOWN,   ITEM_ID_HAT1DOWN },
	{ libretro_m2_pad_device::BUTTON_LEFT,   RETRO_DEVICE_ID_JOYPAD_LEFT,   ITEM_ID_HAT1LEFT },
	{ libretro_m2_pad_device::BUTTON_RIGHT,  RETRO_DEVICE_ID_JOYPAD_RIGHT,  ITEM_ID_HAT1RIGHT },
	{ libretro_m2_pad_device::BUTTON_L3,     RETRO_DEVICE_ID_JOYPAD_L3,     ITEM_ID_BUTTON9 },
	{ libretro_m2_pad_device::BUTTON_R3,     RETRO_DEVICE_ID_JOYPAD_R3,     ITEM_ID_BUTTON10 } };

static_assert(std::size(FIXED_BUTTONS) + libretro_m2_pad_device::NUMBERED_BUTTONS
				== libretro_m2_pad_device::BUTTON_COUNT,
		"every slot is filled by exactly one of the layout table or the fixed list");

// A trigger axis read as a switch. The axis rests at 0 and runs to ABSOLUTE_MIN when pulled
// (see update()), so the threshold matches the one sdl_game_controller_device uses.
int trigger_button_get_state(void *device_internal, void *item_internal)
{
	return (*reinterpret_cast<int32_t const *>(item_internal) <= -16'384) ? 1 : 0;
}


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

libretro_m2_pad_device::libretro_m2_pad_device(std::string &&name, std::string &&id, input_module &module, unsigned port, unsigned diagnostic)
	: libretro_m2_device(std::move(name), std::move(id), module, port)
	, m_diagnostic((diagnostic < m2opt::DIAG_COUNT) ? diagnostic : unsigned(m2opt::DIAG_NONE))
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
// device is what the frontend last selected for this port. Three things about it matter here, and
// the second and third are the entire reason this parameter exists:
//
//   * RETRO_DEVICE_NONE reports nothing. This states an intent rather than fixing anything — a
//     frontend with a port set to None already answers 0 to every state_cb for it — which is also
//     why it is safe to leave unexercised by the harness, which cannot select None.
//   * RETRO_DEVICE_M2_PAD_MODERN picks the second button layout. Nothing else changes with it: the
//     d-pad, the sticks, coin and start are the same controls under every layout.
//   * on a gun port the two primary stick axes are silenced, and nothing else is. See the gate at
//     the bottom of this function.
//
// Any other value is Classic, deliberately: an unrecognised device type must not silently stop the
// pad working.
void libretro_m2_pad_device::update(retro_input_state_t state_cb, unsigned device)
{
	const unsigned kind = device & RETRO_DEVICE_MASK;
	if (kind == RETRO_DEVICE_NONE)
	{
		reset();
		return;
	}

	// The numbered buttons, through the port's layout: slot n holds MAME button n+1, whichever
	// RetroPad control the selected device type says produces it. The full device value is compared,
	// not the masked class, because the layouts are subclasses of the same class.
	// generic_button_get_state<> shifts right by 7, hence 0x80 rather than 1
	unsigned const *const layout = BUTTON_LAYOUTS[(device == RETRO_DEVICE_M2_PAD_MODERN) ? LAYOUT_MODERN : LAYOUT_CLASSIC];
	for (unsigned n = 0; n < NUMBERED_BUTTONS; n++)
		m_buttons[BUTTON_1 + n] = state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, layout[n]) ? 0x80 : 0x00;

	for (auto const &fixed : FIXED_BUTTONS)
		m_buttons[fixed.slot] = state_cb(m_port, RETRO_DEVICE_JOYPAD, 0, fixed.id) ? 0x80 : 0x00;

	// After both button reads, because a fired combo takes its controls back out of them.
	update_diagnostic(state_cb, layout);

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
	// The trigger pair follows the six switch buttons, taking its state from a threshold on the
	// trigger axis so it works whether or not the frontend reports analogue triggers.
	input_item_id buttonitems[BUTTON_COUNT];
	std::fill(std::begin(buttonitems), std::end(buttonitems), ITEM_ID_INVALID);

	input_item_id numbereditems[NUMBERED_BUTTONS + 2];
	input_item_id button_item = ITEM_ID_BUTTON1;
	unsigned buttoncount = 0;

	for (unsigned n = 0; n < NUMBERED_BUTTONS; n++)
	{
		numbereditems[buttoncount] = buttonitems[BUTTON_1 + n] = device.add_item(
				util::string_format("Button %u", n + 1),
				std::string_view(),
				button_item++,
				generic_button_get_state<int32_t>,
				&m_buttons[BUTTON_1 + n]);
		add_button_assignment(assignments, ioport_type(IPT_BUTTON1 + buttoncount), { buttonitems[BUTTON_1 + n] });
		buttoncount++;
	}

	input_item_id triggeritems[2];
	for (unsigned trigger = 0; trigger < 2; trigger++)
	{
		const unsigned axis = AXIS_L2 + trigger;
		numbereditems[buttoncount] = triggeritems[trigger] = device.add_item(
				RETROPAD_BUTTON_NAMES[(trigger == 0) ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_R2],
				std::string_view(),
				button_item++,
				trigger_button_get_state,
				&m_axes[axis]);
		add_button_assignment(assignments, ioport_type(IPT_BUTTON1 + buttoncount), { triggeritems[trigger] });
		buttoncount++;
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
	add_button_assignment(assignments, IPT_UI_SELECT, { numbereditems[0] });
	add_button_assignment(assignments, IPT_UI_BACK,   { numbereditems[1] });
	add_button_assignment(assignments, IPT_UI_CLEAR,  { numbereditems[2] });
	add_button_assignment(assignments, IPT_UI_HELP,   { numbereditems[3] });
	add_button_pair_assignment(assignments, IPT_UI_PAGE_UP, IPT_UI_PAGE_DOWN, triggeritems[0], triggeritems[1]);

	// The cabinet's test switch, on the synthetic combo item rather than on a pad control. The item
	// is added whichever way the option is set: it is 0 forever when the combo is None, and keeping
	// the item list independent of an option means a ctrlr file or a saved remap cannot change
	// meaning underneath the player when they change it.
	//
	// ITEM_ID_BUTTON11 is free: L3 and R3 take 9 and 10, the six numbered buttons and the two
	// triggers take 1..8, and IPT_BUTTON11's own default in inpttype.ipp is KEYCODE_M — a keyboard
	// code, and this OSD registers no keyboard, so nothing else can arrive at this item.
	const input_item_id comboitem = device.add_item(
			"Diagnostic Combo",
			std::string_view(),
			ITEM_ID_BUTTON11,
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

	// daytona's VR4 (Green), and it is outside the branch above deliberately: the test switch used to
	// take R3 and no longer takes any pad control, so this binding no longer depends on an option
	// being off. The other three VR buttons are 6, 7 and 8, and 7/8 land on the trigger thresholds
	// alongside the pedals; see the note above IPT_PEDAL below.
	add_button_assignment(assignments, IPT_BUTTON9, { buttonitems[BUTTON_R3] });

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

void libretro_m2_input::input_init(running_machine &machine)
{
	input_module_impl<libretro_m2_device, libretro_m2_osd_interface>::input_init(machine);

	// Fixed set, no enumeration: the frontend always has as many RetroPads as we ask about, and
	// the device index has to equal the player number for the START1/COIN1 defaults to line up.
	for (unsigned port = 0; port < MAX_PADS; port++)
	{
		create_device<libretro_m2_pad_device>(
				DEVICE_CLASS_JOYSTICK,
				util::string_format("RetroPad %u", port + 1),
				util::string_format("RETROPAD_%u", port + 1),
				port,
				m_diagnostic);
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
