#version 450

// Sega Model 1 libretro core — one flat-shaded quad vertex (M1-2: untextured, painter's order).
//
// Model 1's rasteriser (model1_v.cpp draw_quads) is untextured: every face is one flat 32-bit colour,
// already lit and palette-translated at geometry time. The seam (m1_seam.h) hands the corners in the
// software framebuffer's integer pixel coordinates, so the only transform here is the pixel-to-NDC map —
// exactly what s21.vert / s22.vert do, and scale-invariant for the same reason (the projection already
// happened upstream in the TGP; half_size is always the VISIBLE half-extent).
//
// Depth is DRAW ORDER, not z (like Model 2, unlike S21). sort_quads has already sorted the stream
// back-to-front and the seam records in that order, so this is a plain painter's pass: draw in record
// order, last writer wins, depth test disabled. There is no depth attachment use and no per-vertex z.
//
// in_col is the resolved value fill_quad writes to the bitmap: bits 0..23 = 0x00RRGGBB, bit 24 = the
// MOIRE translucency flag, which the fragment shader turns into a stipple. It is flat across the quad.

layout(location = 0) in vec2 in_pos;    // sub-pixel screen-space (the float projected pixel, M1-3)
layout(location = 1) in uint in_col;    // 0x00RRGGBB | (MOIRE << 24), the lit colour
layout(location = 2) in uint in_albedo; // 0x00RRGGBB, the pre-luma albedo ("No Lighting")

layout(push_constant) uniform push_block
{
	vec2 half_size;      // the visible picture's half-extent in pixels
	uint stipple_div;    // attachment pixels per moiré checker square (1 at native)
	uint flat_luma;      // No Lighting: non-zero → the fragment emits albedo
} pc;

layout(location = 0) flat out uint v_col;
layout(location = 1) flat out uint v_albedo;

void main()
{
	v_col = in_col;
	v_albedo = in_albedo;
	gl_Position = vec4((in_pos / pc.half_size) - 1.0, 0.5, 1.0);
}
