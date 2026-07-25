#version 450

// Model 2 libretro core — MAME's software frame, one texel per pixel.
//
// The exit criterion for this phase is a bit-exact match against the software renderer, so this
// shader is deliberately the identity. The sampler is NEAREST and the destination is the same size
// as the source, so a fragment at pixel x samples at (x + 0.5) / w, which is texel x exactly — no
// filtering, no rounding, nothing to lose a low bit to.
//
// Alpha is written as 1.0 rather than forwarded. MAME's bitmap_rgb32 pixel is 0xAARRGGBB but the
// high byte is X, not A: nothing in the driver writes it meaningfully, so the sampled value is
// whatever the rasteriser happened to leave there. The frontend is entitled to look at the channel,
// and an image that is transparent in patches according to stale bits would be an unpleasant thing
// to diagnose in P3.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_frame;

void main()
{
	out_colour = vec4(texture(u_frame, v_uv).rgb, 1.0);
}
