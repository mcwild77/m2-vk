// license:BSD-3-Clause
// copyright-holders:mcwild77

#include "retro_options.h"

#include <cstring>
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
		m2opt::KEY_SERVICE_BUTTONS,
		"Test/Service on Stick Clicks",
		nullptr,
		"Puts the cabinet's service coin on L3 and its test switch on R3, for player 1. This core "
		"draws none of MAME's menus, so with this off there is no way to reach a game's test mode "
		"or change its settings. Off by default: an accidental stick click should not drop a "
		"service coin mid-game. Applied when content is loaded.",
		nullptr,
		nullptr,
		{
			{ "disabled", nullptr },
			{ "enabled",  nullptr },
			{ nullptr, nullptr }
		},
		"disabled"
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

bool m2opt::get_bool(retro_environment_t environ_cb, char const *key)
{
	return get(environ_cb, key) == "enabled";
}

bool m2opt::updated(retro_environment_t environ_cb)
{
	bool flag = false;
	return (environ_cb != nullptr) && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &flag) && flag;
}
