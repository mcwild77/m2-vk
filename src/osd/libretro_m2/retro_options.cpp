// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "retro_options.h"

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
const retro_core_option_v2_definition DEFINITIONS[] = {
	{
		m2opt::KEY_RENDERER,
		"3D Renderer",
		nullptr,
		"How Model 2's 3D is drawn. Vulkan is why this core exists. Software is MAME's own "
		"rasteriser — slower, but it is the reference the Vulkan output is checked against, and it "
		"is the fallback on machines with no usable Vulkan driver. Applied when content is loaded.",
		nullptr,
		nullptr,
		{
			{ "vulkan",   "Vulkan (hardware)" },
			{ "software", "Software (MAME)" },
			{ nullptr, nullptr }
		},
		"vulkan"
	},
	{
		m2opt::KEY_DIAGNOSTIC_INPUT,
		"Diagnostic Input (Test Menu)",
		nullptr,
		"Button combination that flips the cabinet's test switch, for player 1. This core draws none "
		"of MAME's menus, so with this set to None there is no way to reach a game's test mode or "
		"change its settings. The buttons a combination names are consumed by it, so Start is not "
		"also pressed; the Hold variants want about a second. Setting anything other than None also "
		"puts the service coin (a free credit) on L3. Applied when content is loaded.",
		nullptr,
		nullptr,
		{
			// One entry per m2opt::diagnostic_input, in that order — get_diagnostic() below returns
			// the position in this list, so a reordering here silently renames every combo.
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_NONE],             nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_START],       nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_START_AB],         nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_START_AB],    nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_START_LR],         nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_START_LR],    nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_SELECT],      nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_SELECT_AB],        nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_SELECT_AB],   nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_SELECT_LR],        nullptr },
			{ m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_HOLD_SELECT_LR],   nullptr },
			{ nullptr, nullptr }
		},
		m2opt::DIAGNOSTIC_VALUES[m2opt::DIAG_NONE]
	},
	{
		m2opt::KEY_INTERNAL_RES,
		"Internal Resolution",
		nullptr,
		"The framebuffer the game is drawn into. The hardware's own is 496x384; anything above it "
		"draws the same scene with more pixels, so polygon edges stop stair-stepping. Textures do not "
		"get sharper — the mip level comes from the game, not from the resolution. Costs memory and "
		"fill rate with the pixel count, so 2848x2136 is 32 times the work of native. Vulkan only; "
		"the software renderer always draws at 496x384. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			// The value IS the size, parsed by get_internal_size(). Every entry above native is 4:3;
			// native is 1.2917, and the aspect the frontend is told never changes — these are sample
			// grids for one picture, not different shapes of picture.
			{ "496x384",   "496x384 (Native)" },
			{ "640x480",   "640x480" },
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
		m2opt::KEY_FLAT_SHADING,
		"Flat Shading",
		nullptr,
		"Draws every polygon in its base colour with no texture, which is roughly what the geometry "
		"looked like on the workstations these games were modelled on. Acts on both renderers, so the "
		"software and Vulkan pictures stay comparable. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			{ "off",  "Off" },
			{ "flat", "Untextured" },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_FLAT_LUMA,
		"No Lighting",
		nullptr,
		"Draws every surface at full brightness, so you get the texture and the polygon's own colour "
		"with nothing shaded onto them. Model 2 lights each face by how it is angled to the light, "
		"which is what makes cars darken as they turn and rooms fall into shadow; switching it off "
		"gives a flat, evenly lit picture that shows the artwork as it was drawn. Acts on both "
		"renderers, so the software and Vulkan pictures stay comparable. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			{ "off", "Off" },
			{ "on",  "On" },
			{ nullptr, nullptr }
		},
		"off"
	},
	{
		m2opt::KEY_TRANSPARENCY,
		"Transparency",
		nullptr,
		"How see-through surfaces are drawn. Model 2 has no alpha blender, so the hardware fakes them "
		"with a 50% screen door — a checkerboard of holes, which is what Screen Door reproduces. "
		"Blended draws them as real half-transparency instead: smoke, glass, shadows and headlight "
		"cones stop shimmering, at the cost of no longer matching the arcade. Vulkan only; the "
		"software renderer always uses the screen door. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			{ "stipple", "Screen Door (accurate)" },
			{ "blended", "Blended" },
			{ nullptr, nullptr }
		},
		"stipple"
	},
	{
		m2opt::KEY_STEERING_RESPONSE,
		"Steering Response",
		nullptr,
		"How stick movement maps to the wheel on the driving games. A real cabinet's wheel is most of "
		"a turn lock to lock and a thumbstick is about a centimetre, so mapping the two straight onto "
		"each other makes the car dart at the slightest touch — that is what Linear does, and it is "
		"what this core did before. The other settings keep full lock at full deflection but make the "
		"movement around centre finer, which is what lets you hold a line. Stronger is finer. Only "
		"affects games with a wheel; the fighting and gun games are untouched. Takes effect immediately.",
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
		"Steering Deadzone",
		nullptr,
		"How far the stick must move before the wheel does. Raise it if the car wanders on a straight "
		"with your thumb off the stick; a worn stick needs more than a new one. The travel it costs is "
		"given back to the rest of the sweep rather than thrown away, so a larger deadzone does not "
		"cost you lock. Only affects games with a wheel. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			// The value IS the percentage, parsed by get_steering_deadzone().
			{ "0%",  "0% (off)" },
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
		"Steering Range",
		nullptr,
		"How much of the wheel full stick deflection reaches. At 100% the stick's edge is full lock. "
		"Lowering it trades top-end lock for finer control everywhere, which suits the tracks that "
		"never ask for a hairpin — but if you cannot get round one, it is set too low. Only affects "
		"games with a wheel. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			{ "100%", "100% (full lock)" },
			{ "90%",  "90%" },
			{ "80%",  "80%" },
			{ "70%",  "70%" },
			{ "60%",  "60%" },
			{ nullptr, nullptr }
		},
		"100%"
	},
	{
		m2opt::KEY_STEERING_DISPLAY,
		"Steering Display",
		nullptr,
		"Draws a bar across the top of the screen showing how much steering the game is actually "
		"receiving: red is the wheel you are not using, green is the wheel you are. A white notch "
		"shows where the stick itself is, so the gap between the notch and the end of the green is "
		"what the three settings above are doing. Only appears on games with a wheel. Meant for "
		"setting the steering up rather than for playing with it on. Takes effect immediately.",
		nullptr,
		nullptr,
		{
			{ "off", "Off" },
			{ "on",  "On" },
			{ nullptr, nullptr }
		},
		"off"
	},
	{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, { { nullptr, nullptr } }, nullptr }
};

constexpr unsigned OPTION_COUNT = (sizeof(DEFINITIONS) / sizeof(DEFINITIONS[0])) - 1;


//============================================================
//  the three declaration forms
//============================================================

bool declare_v2(retro_environment_t environ_cb)
{
	// The struct is non-const because a frontend is permitted to rewrite it in place when it
	// localises the option set; ours is static storage, so handing it over is safe either way.
	retro_core_options_v2 options{
			nullptr,
			const_cast<retro_core_option_v2_definition *>(DEFINITIONS) };
	return environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}

// v1 is v2 without the categorised strings, so it is a field-dropping copy rather than a second
// table to keep in step.
bool declare_v1(retro_environment_t environ_cb)
{
	static std::vector<retro_core_option_definition> defs;
	if (defs.empty())
	{
		defs.resize(OPTION_COUNT + 1);
		for (unsigned i = 0; i < OPTION_COUNT; i++)
		{
			defs[i].key = DEFINITIONS[i].key;
			defs[i].desc = DEFINITIONS[i].desc;
			defs[i].info = DEFINITIONS[i].info;
			std::memcpy(defs[i].values, DEFINITIONS[i].values, sizeof(defs[i].values));
			defs[i].default_value = DEFINITIONS[i].default_value;
		}
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
	if (vars.empty())
	{
		text.reserve(OPTION_COUNT);
		vars.resize(OPTION_COUNT + 1);
		for (unsigned i = 0; i < OPTION_COUNT; i++)
		{
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
			vars[i].key = DEFINITIONS[i].key;
			vars[i].value = text.back().c_str();
		}
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

bool m2opt::get_steering_display(retro_environment_t environ_cb)
{
	return get(environ_cb, KEY_STEERING_DISPLAY) == "on";
}

unsigned m2opt::get_transparency(retro_environment_t environ_cb)
{
	// Tested against the enhancement rather than against the default, so that anything unrecognised —
	// a frontend's invention, a hand-written .opt file — lands on the accurate screen door.
	return (get(environ_cb, KEY_TRANSPARENCY) == "blended") ? 1 : 0;
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
	return percent_option(environ_cb, KEY_STEERING_RANGE, 1.0f);
}

bool m2opt::updated(retro_environment_t environ_cb)
{
	bool flag = false;
	return (environ_cb != nullptr) && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &flag) && flag;
}
