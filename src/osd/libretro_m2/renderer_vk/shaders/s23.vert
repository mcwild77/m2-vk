#version 450

// Namco System 23 libretro core — one projected polygon vertex (23-3: textured, painter's order).
//
// Like s22.vert, nothing is transformed here beyond the pixel-to-NDC map: render_project already did the
// perspective divide and the viewport transform, so in_xy arrives in the 640x480 visible bitmap's pixel
// coordinates (the geometry pass has already baked the viewport clip_left/clip_top origin into it, the
// S23 analogue of S22's per-quad clip). Scale-invariant for the same reason — half_size is always the
// VISIBLE half-extent.
//
// Depth is DRAW ORDER, not z, as it is for System 22 and Model 2. render_flush qsorts the frame by zkey
// and the seam records in that (back-to-front) order, so this is a plain painter's pass: draw in record
// order, last writer wins, depth test disabled. z is a constant 0.5.
//
// in_param carries the driver's four screen-linear params interpolated NOPERSPECTIVE, which is not an
// approximation — it is exactly what render_triangle_fan does. The driver interpolates ooz, u*ooz, v*ooz
// and shade*ooz linearly in screen space, then recovers each quantity per pixel by dividing by ooz.
// noperspective reproduces that screen-linear interpolation; the fragment shader does the divides.

layout(location = 0) in vec2 in_xy;      // screen space (viewport offset already baked in)
layout(location = 1) in vec3 in_param;   // ooz, u*ooz, v*ooz  — driver param[0], param[1], param[2]
layout(location = 2) in float in_ish;    // shade*ooz          — driver param[3]
layout(location = 3) in uint in_flags;   // bit0 shade, bit1 stencil, bit2 poly-fade, bit3 colour-fade, bit4 blend, bit5 poly-alpha
layout(location = 4) in uint in_tbase;   // texture base added to the recovered v before the fetch
layout(location = 5) in uint in_peninfo; // pens_base | (cmode << 20)
layout(location = 6) in uint in_pfade;   // poly-fade:   polycolor_r | g<<8 | b<<16
layout(location = 7) in uint in_cfade;   // colour-fade: fadefactor | fadecolor_r<<8 | g<<16 | b<<24
layout(location = 8) in uint in_ablend;  // poly-alpha:  alpha | alpha_pen<<8 | alpha_enabled<<16

layout(push_constant) uniform push_block
{
	vec2 half_size;   // the visible picture's half-extent in pixels
} pc;

layout(location = 0) noperspective out vec4 v_param;   // ooz, u*ooz, v*ooz, shade*ooz
layout(location = 1) flat          out uint v_flags;
layout(location = 2) flat          out uint v_tbase;
layout(location = 3) flat          out uint v_peninfo;
layout(location = 4) flat          out uint v_pfade;
layout(location = 5) flat          out uint v_cfade;
layout(location = 6) flat          out uint v_ablend;

void main()
{
	v_param   = vec4(in_param, in_ish);
	v_flags   = in_flags;
	v_tbase   = in_tbase;
	v_peninfo = in_peninfo;
	v_pfade   = in_pfade;
	v_cfade   = in_cfade;
	v_ablend  = in_ablend;
	gl_Position = vec4((in_xy / pc.half_size) - 1.0, 0.5, 1.0);
}
