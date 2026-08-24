#version 450

// Namco System 21 libretro core — one flat-shaded, pre-projected quad vertex (T2: untextured).
//
// Nothing is transformed here beyond the pixel-to-NDC map. The S21 polygonizer (namcos21_3d_device)
// hands the seam vertices already in the poly framebuffer's pixel coordinates (frame centre width/2,
// height/2), so the only job is to turn those into normalised device coordinates — the same thing
// poly.vert / s22.vert do, and scale-invariant for the same reason (the projection already happened
// upstream in the C67 DSP; half_size is always the VISIBLE half-extent).
//
// Depth is a REAL per-quad z-buffer, unlike S22's painter's pass. renderscanline_flat tests and writes
// a single per-quad zsort (the clamped mean zcode), so in_xyz.z arrives already mapped on the CPU to
//   z = 1 - zsort/32768
// which turns the software test `zsort < zbuf` (nearer = smaller zsort, zbuf cleared to 0x8000) into
// the pipeline's clear-0.0 / COMPARE_GREATER / write test: nearer = larger z wins, and a coplanar tie
// (equal z) falls to the FIRST writer exactly as the software's strict `<` does. The value is constant
// across the four corners, so there is nothing to interpolate.
//
// in_pen is the final resolved framebuffer pen (palette base + the per-quad depth cue already folded
// in); it is flat, and the fragment shader looks it up in the CLUT.

layout(location = 0) in vec3 in_xyz;   // x, y in framebuffer pixels; z already in [0,1] NDC depth
layout(location = 1) in uint in_pen;

layout(push_constant) uniform push_block
{
	vec2 half_size;   // the visible picture's half-extent in pixels
} pc;

layout(location = 0) flat out uint v_pen;

void main()
{
	v_pen = in_pen;
	gl_Position = vec4((in_xyz.xy / pc.half_size) - 1.0, in_xyz.z, 1.0);
}
