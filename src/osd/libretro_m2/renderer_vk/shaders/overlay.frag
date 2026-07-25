#version 450

// Model 2 libretro core — the foreground tilemap layer, drawn over what is already there.
//
// This is copybitmap_trans(bitmap, m_sys24_bitmap, ..., 0) from model2_state::screen_update(): copy
// the pixel unless it is exactly zero. MAME compares the whole 32-bit value against the transparent
// pen, so this does too — not just the alpha byte. Every pen the tilemap draws carries alpha 0xff, so
// testing alpha alone would give the same answer today and would be a silent trap the first time
// something writes a pen that does not.
//
// The comparison is exact and is meant to be. A UNORM texel whose bytes are all zero samples as
// exactly 0.0 in every component, so == is the right operator here and a tolerance would be wrong:
// pixel value 1 is opaque and must be drawn.
//
// Alpha out is 1.0, as in passthrough.frag and for the same reason: MAME's high byte is X, not A, and
// the frontend is entitled to look at the channel.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_layer;

void main()
{
	vec4 t = texture(u_layer, v_uv);
	if (all(equal(t, vec4(0.0))))
		discard;
	out_colour = vec4(t.rgb, 1.0);
}
