// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "retro_options.h"
#include "retro_options_text.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>


namespace {

//============================================================
//  the option table
//============================================================

// No categories: with this few options a category tree is more navigation than the options are
// worth, and a frontend that supports categories renders an uncategorised set perfectly well.
//
// Non-const because set_native_resolution() patches the Internal Resolution entry in place at startup
// (the default and one "(Native)" label differ per driver family; the object file is shared across
// subtargets, so this can't be a compile-time choice — it is decided at runtime from the loaded
// driver family). Every declare form and default_value() read it after that patch has run.
retro_core_option_v2_definition DEFINITIONS[] = {
	{
		m2opt::KEY_RENDERER,
		m2txt::RENDERER_LABEL,
		nullptr,
		m2txt::RENDERER_INFO,
		nullptr,
		nullptr,
		{
			{ "vulkan",   m2txt::RENDERER_VULKAN },
			{ "software", m2txt::RENDERER_SOFTWARE },
			{ nullptr, nullptr }
		},
		"vulkan"
	},
	{
		m2opt::KEY_INTERNAL_RES,
		m2txt::RES_LABEL,
		nullptr,
		m2txt::RES_INFO,
		nullptr,
		nullptr,
		{
			// The value IS the size, parsed by get_internal_size(). The list is shared between the driver
			// families; only which entry is native — and so the default and the "(Native)" label —
			// differs, and set_native_resolution() patches that in at startup (496x384 for Model 2,
			// 640x480 for System 22, 496x480 for System 21). The default authored here is Model 2's, and
			// each family's native size is authored with a plain label because the Model 2 path never
			// calls set_native_resolution() and must see its own entry already marked. The aspect the
			// frontend is told never changes — these are sample grids for one picture, not different
			// shapes of picture.
			{ "496x384",   m2txt::RES_496x384_NATIVE },
			{ "496x480",   m2txt::RES_496x480 },
			{ "640x480",   m2txt::RES_640x480 },
			{ "1024x768",  "1024x768" },
			{ "1280x960",  "1280x960" },
			{ "1440x1080", "1440x1080" },
			{ "1600x1200", "1600x1200" },
			{ "1920x1440", "1920x1440" },
			{ "2560x1920", "2560x1920" },
			{ "2848x2136", "2848x2136" },
			{ nullptr, nullptr }
		},
		"496x384"
	},
	{
		m2opt::KEY_TRANSPARENCY,
		m2txt::TRANSPARENCY_LABEL,
		nullptr,
		m2txt::TRANSPARENCY_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_S22_TEXTURE_FILTER,
		m2txt::S22_TEXTURE_FILTER_LABEL,
		nullptr,
		m2txt::S22_TEXTURE_FILTER_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_S22_FOG,
		m2txt::S22_FOG_LABEL,
		nullptr,
		m2txt::S22_FOG_INFO,
		nullptr,
		nullptr,
		{
			{ "on",  m2txt::V_ON },
			{ "off", m2txt::V_OFF },
			{ nullptr, nullptr }
		},
		"on"
	},
	// NOTE: the "Depth Buffer (3D)" option (KEY_S22_DEPTH_BUFFER) was removed before release — the
	// per-pixel depth experiment corrupted textures/UVs on the ground and broke the layered UI, so it is
	// not shippable. The renderer code is still present but dormant (s22::depth_enabled() is forced off);
	// see devnotes/zfighting.md. Do not re-add this menu entry without fixing the underlying issues.
	{
		m2opt::KEY_S22_NO_TEXTURES,
		m2txt::S22_NO_TEXTURES_LABEL,
		nullptr,
		m2txt::S22_NO_TEXTURES_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_FLAT_SHADING,
		m2txt::FLAT_SHADING_LABEL,
		nullptr,
		m2txt::FLAT_SHADING_INFO,
		nullptr,
		nullptr,
		{
			{ "off",  m2txt::V_OFF },
			{ "flat", m2txt::FLAT_SHADING_UNTEXTURED },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_FLAT_LUMA,
		m2txt::FLAT_LUMA_LABEL,
		nullptr,
		m2txt::FLAT_LUMA_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_SMOOTH_SHADING,
		m2txt::SMOOTH_SHADING_LABEL,
		nullptr,
		m2txt::SMOOTH_SHADING_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_M2_SMOOTH_SHADING,
		m2txt::M2_SMOOTH_SHADING_LABEL,
		nullptr,
		m2txt::M2_SMOOTH_SHADING_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_POLY_COUNTER,
		m2txt::POLY_COUNTER_LABEL,
		nullptr,
		m2txt::POLY_COUNTER_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_S22_2D_OVERLAY,
		m2txt::S22_2D_OVERLAY_LABEL,
		nullptr,
		m2txt::S22_2D_OVERLAY_INFO,
		nullptr,
		nullptr,
		{
			{ "on",  m2txt::V_ON },
			{ "off", m2txt::V_OFF },
			{ nullptr, nullptr }
		},
		"on"
	},
	{
		m2opt::KEY_STEERING_RESPONSE,
		m2txt::STEERING_RESPONSE_LABEL,
		nullptr,
		m2txt::STEERING_RESPONSE_INFO,
		nullptr,
		nullptr,
		{
			// One entry per m2opt::steering_response, in that order — get_steering_response() returns
			// the position in this list and STEERING_RESPONSE_GAMMA is indexed by it, so a reordering
			// here silently reassigns every curve.
			{ m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_LINEAR],      nullptr },
			{ m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_SLIGHT],      nullptr },
			{ m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_MEDIUM],      nullptr },
			{ m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_STRONG],      nullptr },
			{ m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_VERY_STRONG], nullptr },
			{ nullptr, nullptr }
		},
		// Slight (gamma 1.30), decided by hand-check 2026-08-08. See steering-handcheck.md.
		m2opt::STEERING_RESPONSE_VALUES[m2opt::STEER_SLIGHT]
	},
	{
		m2opt::KEY_STEERING_DEADZONE,
		m2txt::STEERING_DEADZONE_LABEL,
		nullptr,
		m2txt::STEERING_DEADZONE_INFO,
		nullptr,
		nullptr,
		{
			// The value IS the percentage, parsed by get_steering_deadzone().
			{ "0%",  m2txt::PCT_0_OFF },
			{ "2%",  "2%" },
			{ "5%",  "5%" },
			{ "8%",  "8%" },
			{ "10%", "10%" },
			{ "15%", "15%" },
			{ "20%", "20%" },
			{ nullptr, nullptr }
		},
		"5%"
	},
	{
		m2opt::KEY_STEERING_RANGE,
		m2txt::STEERING_RANGE_LABEL,
		nullptr,
		m2txt::STEERING_RANGE_INFO,
		nullptr,
		nullptr,
		{
			{ "100%", m2txt::PCT_100_FULL_LOCK },
			{ "90%",  "90%" },
			{ "80%",  "80%" },
			{ "70%",  "70%" },
			{ "60%",  "60%" },
			{ nullptr, nullptr }
		},
		// 80%, decided by hand-check. See steering-handcheck.md.
		"80%"
	},
	{
		m2opt::KEY_STEERING_DAMP_DRIVE,
		m2txt::STEERING_DAMP_DRIVE_LABEL,
		nullptr,
		m2txt::STEERING_DAMP_DRIVE_INFO,
		nullptr,
		nullptr,
		{
			// The value is a bare frame count, parsed by get_steering_damp_drive(); "0" → instant.
			{ "0",  m2txt::DAMP_0 },
			{ "2",  m2txt::DAMP_2 },
			{ "4",  m2txt::DAMP_4 },
			{ "8",  m2txt::DAMP_8 },
			{ "16", m2txt::DAMP_16 },
			{ nullptr, nullptr }
		},
		// 8, not the 4 the original hand-check picked (steering-handcheck.md) — moved to match Return
		// below and requested directly, 2026-08-27.
		"8"
	},
	{
		m2opt::KEY_STEERING_DAMP_RETURN,
		m2txt::STEERING_DAMP_RETURN_LABEL,
		nullptr,
		m2txt::STEERING_DAMP_RETURN_INFO,
		nullptr,
		nullptr,
		{
			{ "0",  m2txt::DAMP_0 },
			{ "2",  m2txt::DAMP_2 },
			{ "4",  m2txt::DAMP_4 },
			{ "8",  m2txt::DAMP_8 },
			{ "16", m2txt::DAMP_16 },
			{ nullptr, nullptr }
		},
		"8"
	},
	{
		m2opt::KEY_STEERING_DISPLAY,
		m2txt::STEERING_DISPLAY_LABEL,
		nullptr,
		m2txt::STEERING_DISPLAY_INFO,
		nullptr,
		nullptr,
		{
			{ "off", m2txt::V_OFF },
			{ "on",  m2txt::V_ON },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_ANALOG_DEADZONE,
		m2txt::ANALOG_DEADZONE_LABEL,
		nullptr,
		m2txt::ANALOG_DEADZONE_INFO,
		nullptr,
		nullptr,
		{
			// The value IS the percentage, parsed by get_analog_deadzone().
			{ "0%",  m2txt::PCT_0_OFF },
			{ "2%",  "2%" },
			{ "5%",  "5%" },
			{ "10%", "10%" },
			{ "15%", "15%" },
			{ nullptr, nullptr }
		},
		"5%"
	},
	{
		m2opt::KEY_ANALOG_REACH,
		m2txt::ANALOG_REACH_LABEL,
		nullptr,
		m2txt::ANALOG_REACH_INFO,
		nullptr,
		nullptr,
		{
			// The value IS the percentage, parsed by get_analog_reach().
			{ "100%", m2txt::PCT_100_FULL_DEFLECTION },
			{ "95%",  "95%" },
			{ "90%",  "90%" },
			{ "85%",  "85%" },
			{ "80%",  "80%" },
			{ "75%",  "75%" },
			{ nullptr, nullptr }
		},
		"100%"
	},
	{
		m2opt::KEY_DIAGNOSTIC_INPUT,
		m2txt::DIAGNOSTIC_INPUT_LABEL,
		nullptr,
		m2txt::DIAGNOSTIC_INPUT_INFO,
		nullptr,
		nullptr,
		{
			// One entry per m2opt::diagnostic_input, in that order — get_diagnostic() below returns
			// the position in this list, so a reordering here silently renames every combo.
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_NONE],        nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_L3_R3],       nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_SELECT], nullptr },
			{ nullptr, nullptr }
		},
		m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_NONE]
	},
	{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, { { nullptr, nullptr } }, nullptr }
};

constexpr unsigned OPTION_COUNT = (sizeof(DEFINITIONS) / sizeof(DEFINITIONS[0])) - 1;

// Options hidden from the declared set by hide_option() (a family keeping its options off the other
// family's menu). The get()/default_value() readers ignore this — hiding affects only what the frontend
// is told about, not what a value resolves to — so a hidden option still reads its declared default.
bool s_hidden[OPTION_COUNT] = {};

// Set by redeclare() so the three declare_*() forms rebuild their cached definition table instead of
// keeping the first-call copy. The merged core re-gates the menu at load time (clear_hidden() +
// hide_option() + set_native_resolution()), and the frontend needs the updated set.
bool s_defs_dirty = false;


//============================================================
//  the three declaration forms
//============================================================

bool declare_v2(retro_environment_t environ_cb)
{
	// A filtered copy so hidden options never reach the frontend. Static storage, so it stays valid after
	// return; still non-const because a frontend is permitted to rewrite it in place when it localises the
	// option set. Built once — set_native_resolution() and hide_option() have both run by declare() time.
	static std::vector<retro_core_option_v2_definition> defs;
	if (defs.empty() || s_defs_dirty)
	{
		defs.clear();
		defs.reserve(OPTION_COUNT + 1);
		for (unsigned i = 0; i < OPTION_COUNT; i++)
			if (!s_hidden[i])
				defs.push_back(DEFINITIONS[i]);
		defs.push_back(retro_core_option_v2_definition{});   // all-null terminator
	}
	retro_core_options_v2 options{ nullptr, defs.data() };
	return environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}

// v1 is v2 without the categorised strings, so it is a field-dropping copy rather than a second
// table to keep in step.
bool declare_v1(retro_environment_t environ_cb)
{
	static std::vector<retro_core_option_definition> defs;
	if (defs.empty() || s_defs_dirty)
	{
		defs.clear();
		defs.resize(OPTION_COUNT + 1);
		unsigned j = 0;
		for (unsigned i = 0; i < OPTION_COUNT; i++)
		{
			if (s_hidden[i])
				continue;
			defs[j].key = DEFINITIONS[i].key;
			defs[j].desc = DEFINITIONS[i].desc;
			defs[j].info = DEFINITIONS[i].info;
			std::memcpy(defs[j].values, DEFINITIONS[i].values, sizeof(defs[j].values));
			defs[j].default_value = DEFINITIONS[i].default_value;
			j++;
		}
		defs.resize(j + 1);   // trim to visible + the value-initialised (all-null) terminator
	}
	return environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, defs.data());
}

// Pre-options form: "description; first|second", first = default.
// ⚠️ Emits the default first then skips it in the loop — not a straight copy. Load-bearing since
// model2_steering_deadzone whose default (5%) is not its first value.
bool declare_variables(retro_environment_t environ_cb)
{
	static std::vector<std::string> text;
	static std::vector<retro_variable> vars;
	if (vars.empty() || s_defs_dirty)
	{
		text.clear();
		vars.clear();
		text.reserve(OPTION_COUNT);   // reserved so a push_back never reallocs a live c_str() below
		vars.resize(OPTION_COUNT + 1);
		unsigned j = 0;
		for (unsigned i = 0; i < OPTION_COUNT; i++)
		{
			if (s_hidden[i])
				continue;
			std::string line(DEFINITIONS[i].desc);
			line += "; ";
			line += DEFINITIONS[i].default_value;
			for (unsigned v = 0; DEFINITIONS[i].values[v].value != nullptr; v++)
			{
				if (std::strcmp(DEFINITIONS[i].values[v].value, DEFINITIONS[i].default_value) != 0)
				{
					line += '|';
					line += DEFINITIONS[i].values[v].value;
				}
			}
			text.push_back(std::move(line));
			vars[j].key = DEFINITIONS[i].key;
			vars[j].value = text.back().c_str();
			j++;
		}
		vars.resize(j + 1);   // trim to visible + the value-initialised (all-null) terminator
	}
	return environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars.data());
}


// "<n>" or "<n>%" as a fraction. Anything else → fallback.
float percent_option(retro_environment_t environ_cb, char const *key, float fallback)
{
	const std::string value = m2opt::get(environ_cb, key);
	unsigned n = 0;
	char tail = 0;
	const int matched = std::sscanf(value.c_str(), "%u%c", &n, &tail);
	if ((matched < 1) || (n > 100) || ((matched == 2) && (tail != '%')))
		return fallback;
	return float(n) / 100.0f;
}


char const *default_value(char const *key)
{
	for (unsigned i = 0; i < OPTION_COUNT; i++)
	{
		if (std::strcmp(DEFINITIONS[i].key, key) == 0)
			return DEFINITIONS[i].default_value;
	}
	return "";
}

} // anonymous namespace


void m2opt::hide_option(char const *key)
{
	if (key == nullptr)
		return;
	for (unsigned i = 0; i < OPTION_COUNT; i++)
	{
		if (std::strcmp(DEFINITIONS[i].key, key) == 0)
		{
			s_hidden[i] = true;
			return;
		}
	}
}


void m2opt::set_native_resolution(char const *native, char const *native_label_496)
{
	if (native == nullptr)
		return;
	char const *const label_496_native = native_label_496 ? native_label_496 : m2txt::RES_496x384_NATIVE;

	for (unsigned i = 0; i < OPTION_COUNT; i++)
	{
		if (std::strcmp(DEFINITIONS[i].key, KEY_INTERNAL_RES) != 0)
			continue;

		retro_core_option_v2_definition &def = DEFINITIONS[i];

		// Only accept a size the option actually lists; a stray value would set a default the menu can
		// never match, which reads as the menu being out of step with the core.
		bool listed = false;
		for (unsigned v = 0; def.values[v].value != nullptr; v++)
		{
			if (std::strcmp(def.values[v].value, native) == 0) { listed = true; break; }
		}
		if (!listed)
			return;

		def.default_value = def.values[0].value;  // reset then re-point, so repeated calls are idempotent
		for (unsigned v = 0; def.values[v].value != nullptr; v++)
		{
			char const *const val = def.values[v].value;
			const bool is_native = (std::strcmp(val, native) == 0);
			// Only 496x384 (Model 2), 496x480 (System 21) and 640x480 (System 22) are ever native; give
			// the native one the label, the others their plain size. Every larger entry keeps its plain
			// label untouched.
			if (std::strcmp(val, "496x384") == 0)
				def.values[v].label = is_native ? label_496_native : m2txt::RES_496x384;
			else if (std::strcmp(val, "496x480") == 0)
				def.values[v].label = is_native ? m2txt::RES_496x480_NATIVE : m2txt::RES_496x480;
			else if (std::strcmp(val, "640x480") == 0)
				def.values[v].label = is_native ? m2txt::RES_640x480_NATIVE : m2txt::RES_640x480;
			if (is_native)
				def.default_value = val;
		}
		return;
	}
}


//============================================================
//  m2opt
//============================================================

void m2opt::declare(retro_environment_t environ_cb)
{
	if (environ_cb == nullptr)
		return;

	unsigned version = 0;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
		version = 0;

	// Each step falls through on refusal as well as on an older reported version: a frontend that
	// answers the version query but rejects the call is better served by the older form than by
	// no options at all.
	if ((version >= 2) && declare_v2(environ_cb))
		return;
	if ((version >= 1) && declare_v1(environ_cb))
		return;
	declare_variables(environ_cb);
}

void m2opt::clear_hidden()
{
	for (unsigned i = 0; i < OPTION_COUNT; i++)
		s_hidden[i] = false;
}

void m2opt::redeclare(retro_environment_t environ_cb)
{
	if (environ_cb == nullptr)
		return;
	s_defs_dirty = true;    // force the declare_*() forms to rebuild their cached table
	declare(environ_cb);
	s_defs_dirty = false;
}

void m2opt::set_option_display(retro_environment_t environ_cb, char const *key, bool visible)
{
	if ((environ_cb == nullptr) || (key == nullptr))
		return;
	retro_core_option_display disp{};
	disp.key = key;
	disp.visible = visible;
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &disp);
}

std::string m2opt::get(retro_environment_t environ_cb, char const *key)
{
	retro_variable var{ key, nullptr };
	if ((environ_cb != nullptr) && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && (var.value != nullptr))
		return std::string(var.value);

	// No frontend value: a frontend with no option support at all, or one that has not yet written
	// its config. Our own default is the right answer in both cases.
	return std::string(default_value(key));
}

unsigned m2opt::get_diagnostic(retro_environment_t environ_cb)
{
	const std::string value = get(environ_cb, KEY_DIAGNOSTIC_INPUT);
	for (unsigned i = 0; i < DIAG_COUNT; i++)
	{
		if (value == DIAGNOSTIC_VALUES[i])
			return i;
	}
	return DIAG_NONE;
}

void m2opt::get_internal_size(retro_environment_t environ_cb, unsigned &width, unsigned &height)
{
	width = 0;
	height = 0;

	// "<w>x<h>", and nothing else accepted — not a bare number, not a trailing suffix. The renderer
	// reads 0 as "native", so a frontend holding "2x" from the option set this replaced, or a value it
	// invented, lands on the hardware's own resolution rather than on a guess.
	const std::string value = get(environ_cb, KEY_INTERNAL_RES);
	const std::size_t split = value.find('x');
	if ((split == std::string::npos) || (split == 0) || (split + 1 >= value.size()))
		return;

	unsigned long w = 0, h = 0;
	try
	{
		std::size_t used = 0;
		w = std::stoul(value.substr(0, split), &used);
		if (used != split)
			return;
		h = std::stoul(value.substr(split + 1), &used);
		if (used != (value.size() - split - 1))
			return;
	}
	catch (std::exception const &)
	{
		return;
	}

	// The renderer clamps against the device's own limits; this only rejects the absurd, so that a
	// typo in a hand-written .opt file cannot ask for a 4 GB attachment before anything sane has run.
	if ((w == 0) || (h == 0) || (w > 16384) || (h > 16384))
		return;

	width = unsigned(w);
	height = unsigned(h);
}

unsigned m2opt::get_flat_shading(retro_environment_t environ_cb)
{
	// 2, not 1: mode 1 leaves translucent polygons undrawn. See the header.
	return (get(environ_cb, KEY_FLAT_SHADING) == "flat") ? 2 : 0;
}

bool m2opt::get_flat_luma(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_FLAT_LUMA) == "on";
}

bool m2opt::get_smooth_shading(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_SMOOTH_SHADING) == "on";
}

bool m2opt::get_m2_smooth_shading(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_M2_SMOOTH_SHADING) == "on";
}

bool m2opt::get_steering_display(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_STEERING_DISPLAY) == "on";
}

bool m2opt::get_poly_counter(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_POLY_COUNTER) == "on";
}

unsigned m2opt::get_transparency(retro_environment_t environ_cb)
{
	// "on" tested rather than not "off", so anything unrecognised — a frontend's invention, a
	// hand-written .opt file — lands on the accurate screen door.
	return (get(environ_cb, KEY_TRANSPARENCY) == "on") ? 1 : 0;
}

bool m2opt::get_s22_depth_buffer(retro_environment_t environ_cb)
{
	// "on" tested rather than not "off", so an unrecognised value lands on the accurate painter's
	// picture — the same rule get_s22_texture_filter() uses for the same reason.
	return get(environ_cb, KEY_S22_DEPTH_BUFFER) == "on";
}

bool m2opt::get_s22_texture_filter(retro_environment_t environ_cb)
{
	// "on" tested rather than not "off", so an unrecognised value lands on the accurate point-sampled
	// picture — the same rule get_transparency() uses for the same reason.
	return get(environ_cb, KEY_S22_TEXTURE_FILTER) == "on";
}

bool m2opt::get_s22_fog(retro_environment_t environ_cb)
{
	// Fog is the accurate default, so this one tests "off": an unreadable value leaves fog ON, the
	// opposite of the enhancement options above.
	return get(environ_cb, KEY_S22_FOG) != "off";
}

bool m2opt::get_s22_no_textures(retro_environment_t environ_cb)
{
	// "on" tested rather than not "off", so an unrecognised value lands on the accurate (textured) picture.
	return get(environ_cb, KEY_S22_NO_TEXTURES) == "on";
}

bool m2opt::get_s22_2d_overlay(retro_environment_t environ_cb)
{
	// Drawing the HUD is the accurate default, so this tests "off": an unreadable value keeps the overlay.
	return get(environ_cb, KEY_S22_2D_OVERLAY) != "off";
}

unsigned m2opt::get_steering_response(retro_environment_t environ_cb)
{
	const std::string value = get(environ_cb, KEY_STEERING_RESPONSE);
	for (unsigned i = 0; i < STEER_RESPONSE_COUNT; i++)
	{
		if (value == STEERING_RESPONSE_VALUES[i])
			return i;
	}
	return STEER_SLIGHT;
}

float m2opt::get_steering_deadzone(retro_environment_t environ_cb)
{
	return percent_option(environ_cb, KEY_STEERING_DEADZONE, 0.05f);
}

float m2opt::get_steering_range(retro_environment_t environ_cb)
{
	return percent_option(environ_cb, KEY_STEERING_RANGE, 0.8f);
}

namespace {

// "Off" or garbage → 0 (the instant sentinel); a bare "<n>" with no trailing junk → n. The 0-and-huge
// guards keep a hand-written .opt file from asking for a nonsense rate; the setter clamps nothing.
unsigned frames_option(retro_environment_t environ_cb, char const *key)
{
	const std::string value = m2opt::get(environ_cb, key);
	unsigned n = 0;
	char tail = 0;
	const int matched = std::sscanf(value.c_str(), "%u%c", &n, &tail);
	if ((matched != 1) || (n == 0) || (n > 240))
		return 0;
	return n;
}

} // anonymous namespace

unsigned m2opt::get_steering_damp_drive(retro_environment_t environ_cb)
{
	return frames_option(environ_cb, KEY_STEERING_DAMP_DRIVE);
}

unsigned m2opt::get_steering_damp_return(retro_environment_t environ_cb)
{
	return frames_option(environ_cb, KEY_STEERING_DAMP_RETURN);
}

float m2opt::get_analog_deadzone(retro_environment_t environ_cb)
{
	return percent_option(environ_cb, KEY_ANALOG_DEADZONE, 0.05f);
}

float m2opt::get_analog_reach(retro_environment_t environ_cb)
{
	return percent_option(environ_cb, KEY_ANALOG_REACH, 1.0f);
}

bool m2opt::updated(retro_environment_t environ_cb)
{
	bool flag = false;
	return (environ_cb != nullptr) && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &flag) && flag;
}
