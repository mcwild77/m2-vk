#version 450

// Namco System 22 libretro core — one projected quad vertex (S2: untextured first).
//
// Nothing is transformed here beyond the pixel-to-NDC map. The geometry engine has already projected,
// viewport-transformed and clipped, so in_pos.xy arrives in the 640x480 visible bitmap's pixel
// coordinates (center 320,240) and the only job is to turn those into normalised device coordinates —
// the same thing poly.vert does for Model 2, and scale-invariant for the same reason: the perspective
// divide has already happened, so this produces NDC and half_size is always the VISIBLE half-extent.
//
// in_pos.z is a constant 0.5 and means nothing: the System 22 tree is walked back-to-front, so depth
// is a painter's algorithm in draw order and the pipeline runs with the depth test off.
//
// in_color is the flat Gouraud colour resolved on the host: the polygon's base palette colour scaled
// by the per-vertex hardware brightness. Interpolated across the polygon like the hardware's shade.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform push_block
{
	vec2 half_size;
} pc;

layout(location = 0) out vec4 v_color;

void main()
{
	v_color = in_color;
	gl_Position = vec4((in_pos.xy / pc.half_size) - 1.0, in_pos.z, 1.0);
}
