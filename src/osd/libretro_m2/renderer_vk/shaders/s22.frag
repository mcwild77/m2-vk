#version 450

// Namco System 22 libretro core — the untextured polygon fragment (S2: untextured first).
//
// The flat Gouraud colour, straight through. No texel fetch, no fog, no alpha: those are later S2
// steps. The alpha channel is 1.0 so the polygon is opaque against the 2D composite, exactly as an
// untextured System 22 polygon is.

layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(v_color.rgb, 1.0);
}
