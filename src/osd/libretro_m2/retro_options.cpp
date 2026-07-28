// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "retro_options.h"

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

// The pre-options form: one string per option, "description; first|second", where the first value
// listed is the default. Our defaults are the first value in every option above, which this relies
// on — if that ever stops being true this has to reorder rather than copy.
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

unsigned m2opt::get_transparency(retro_environment_t environ_cb)
{
	// Tested against the enhancement rather than against the default, so that anything unrecognised —
	// a frontend's invention, a hand-written .opt file — lands on the accurate screen door.
	return (get(environ_cb, KEY_TRANSPARENCY) == "blended") ? 1 : 0;
}

bool m2opt::updated(retro_environment_t environ_cb)
{
	bool flag = false;
	return (environ_cb != nullptr) && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &flag) && flag;
}
