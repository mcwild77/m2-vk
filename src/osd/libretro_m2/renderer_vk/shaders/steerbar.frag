#version 450

// Steering read-out bar. Fullscreen triangle, scissored to the bar's box.
// ⚠️ part_at() below duplicates m2vk::steerbar_part_at() in m2vk_steerbar.h — change both.
// Opaque, not blended, so the two paths produce the same pixels.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Push
{
	vec2  origin;       // the bar's top-left, in attachment pixels
	vec2  size;         // its width and height, same
	float value;        // the port value, -1..+1 — what the game receives
	float raw;          // the stick, -1..+1, before shaping
	float border_u;
	float border_v;
	float tick_half;
	float centre_half;
	uint  c_border;
	uint  c_empty;
	uint  c_fill;
	uint  c_centre;
	uint  c_tick;
} pc;

const uint PART_NONE   = 0u;
const uint PART_BORDER = 1u;
const uint PART_EMPTY  = 2u;
const uint PART_FILL   = 3u;
const uint PART_CENTRE = 4u;
const uint PART_TICK   = 5u;

uint part_at(float u, float v)
{
	float au = abs(u);
	if ((au > 1.0) || (v < 0.0) || (v > 1.0))
		return PART_NONE;
	if ((au > (1.0 - pc.border_u)) || (v < pc.border_v) || (v > (1.0 - pc.border_v)))
		return PART_BORDER;

	float iu = u / (1.0 - pc.border_u);

	if (abs(iu - pc.raw) <= pc.tick_half)
		return PART_TICK;

	bool same_side = ((iu < 0.0) == (pc.value < 0.0));
	if (same_side && (abs(iu) <= abs(pc.value)))
		return PART_FILL;

	if (abs(iu) <= pc.centre_half)
		return PART_CENTRE;

	return PART_EMPTY;
}

vec3 unpack(uint rgb)
{
	return vec3(float((rgb >> 16) & 0xffu), float((rgb >> 8) & 0xffu), float(rgb & 0xffu)) / 255.0;
}

void main()
{
	vec2 d = gl_FragCoord.xy - pc.origin;
	float u = ((d.x / pc.size.x) - 0.5) * 2.0;
	float v = d.y / pc.size.y;

	uint part = part_at(u, v);

	if (part == PART_BORDER)      out_colour = vec4(unpack(pc.c_border), 1.0);
	else if (part == PART_EMPTY)  out_colour = vec4(unpack(pc.c_empty), 1.0);
	else if (part == PART_FILL)   out_colour = vec4(unpack(pc.c_fill), 1.0);
	else if (part == PART_CENTRE) out_colour = vec4(unpack(pc.c_centre), 1.0);
	else if (part == PART_TICK)   out_colour = vec4(unpack(pc.c_tick), 1.0);
	else                          discard;
}
