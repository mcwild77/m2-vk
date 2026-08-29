#version 450

// Model 2 libretro core — one polygon vertex, already projected.
//
// Nothing is transformed here. The geometry engine has already projected, viewport-transformed and
// clipped, so in_pos.xy arrives in m_destmap's pixel coordinates and the only job is to turn those
// into normalised device coordinates. Doing it with the *visible* size rather than a hardcoded 512
// is what makes gl_FragCoord.xy in the fragment shader equal the software renderer's x/scanline
// exactly, which the checker stipple depends on: it draws where (x ^ y) & 1 is 1, and a half-pixel
// or origin disagreement inverts the whole pattern.
//
// This is also scale-invariant, which is what P5's internal-res scaling needs: rendering at S x is
// a viewport of S*width by S*height and NOTHING HERE CHANGES — pc.half_size stays the *visible*
// half-extent at every scale — because the perspective divide has already happened, this produces
// NDC, and NDC is the resolution-independent quantity. Scaling half_size along with the viewport is
// the obvious wrong move and it puts the whole frame in a 1/S corner of the attachment.
//
// P4 step 2 measured this rather than leaving it as the argument it was until 2026-07-27: M2VK_SS=n
// renders into an n x attachment and resolves back down, and no polygon that won a pixel at 1x loses
// it at 2x, 3x or 4x on any fixture. See devnotes/p4-depth-and-decals.md §3 step 2.
//
// in_pos.z is NOT depth in any geometric sense. It is the draw-order key — 1 - n/65536 for the nth
// polygon in draw order — and with a GREATER test and depth writes on it reproduces the software
// renderer's m_fillmap exactly: first writer wins the pixel. Real interpolated z is P4's, together
// with the decal problem it creates.
//
// in_param carries what the raster tail needs and this shader does not touch:
//   .x  rz — 1/z, always, both texture classes normalised to the textured convention at upload
//   .yz uz, vz — u/z and v/z, MAME's uoz/voz
// They are declared noperspective because MAME interpolates them linearly in *screen* space and
// then divides. With w == 1 that is what smooth would do anyway; saying noperspective is what stops
// step 4 from quietly acquiring perspective-correct interpolation, which computes the same thing by
// a different route and does not round identically.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_param;
layout(location = 2) in uint in_poly;
// model2_smooth_shading: the per-vertex luma the upload welded from adjacent faces. When the option is
// off the upload writes the flat poly luma into all of a polygon's vertices, so this interpolates to a
// constant and the fragment shader reproduces the flat path exactly. gl_Position.w is 1 everywhere, so
// smooth (perspective-correct) interpolation equals screen-linear here — no qualifier needed.
layout(location = 3) in float in_smooth_luma;

// The visible picture's half-extent in pixels — the PICTURE's, never the attachment's, which is what
// makes this shader indifferent to the internal resolution. A push constant rather than a constant so
// that the crop and the resolution are the renderer's business and not the shader's.
//
// The block is declared identically in poly.frag, because the range covers both stages — this shader
// reads neither `stipple_div` nor `blend` and must still declare both, or the two stages disagree
// about where half_size ends and the fragment shader reads the wrong four bytes.
layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint stipple_div;
	uint blend;
} pc;

layout(location = 0) noperspective out vec3 v_param;
layout(location = 1) flat out uint v_poly;
layout(location = 2) out float v_smooth_luma;

void main()
{
	v_param = in_param;
	v_poly = in_poly;
	v_smooth_luma = in_smooth_luma;
	gl_Position = vec4((in_pos.xy / pc.half_size) - 1.0, in_pos.z, 1.0);
}
