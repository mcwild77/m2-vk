#version 450

// Namco System 22 libretro core — one projected quad vertex (S2: textured).
//
// Nothing is transformed here beyond the pixel-to-NDC map. The geometry engine has already projected,
// viewport-transformed and clipped, so in_xy arrives in the 640x480 visible bitmap's pixel coordinates
// (centre 320,240) and the only job is to turn those into normalised device coordinates — the same
// thing poly.vert does for Model 2, and scale-invariant for the same reason: the perspective divide has
// already happened, so this produces NDC and half_size is always the VISIBLE half-extent.
//
// The depth is a constant 0.5 and means nothing: the System 22 tree is walked back-to-front, so depth
// is a painter's algorithm in draw order and the pipeline runs with the depth test off.
//
// in_uvw and in_iw carry the driver's clipv params interpolated by the fragment shader NOPERSPECTIVE,
// which is not an approximation — it is exactly what render_triangle_fan does. The driver interpolates
// (u+0.5)*ooz, (v+0.5)*ooz, ooz and (bri+0.5)*ooz linearly in screen space along the edges, then
// recovers u = uoz/ooz and shade = ioz/ooz per pixel. noperspective reproduces the screen-linear
// interpolation; the fragment shader does the divide.
//
// in_attr / in_bn / in_base are per-quad and flat: the quad's flags, texture bank and untextured fill
// colour, replicated into every vertex so the flat value is the same whichever the provoking vertex is.

layout(location = 0) in vec2  in_xy;
layout(location = 1) in vec3  in_uvw;   // (u+0.5)*ooz, (v+0.5)*ooz, ooz
layout(location = 2) in float in_iw;    // (bri+0.5)*ooz
layout(location = 3) in uint  in_attr;
layout(location = 4) in uint  in_bn;
layout(location = 5) in uint  in_base;
layout(location = 6) in uint  in_sf0;   // fogfactor | cz_bank<<8 | zfog<<10 | alpha_en<<11 | ss22<<12 | (sdelta+256)<<13
layout(location = 7) in uint  in_sf1;   // fog colour, 0x00RRGGBB

// Shared with s22.frag: half_size is the vertex shader's only field; the rest are the SS22 shading
// tail's per-frame globals, read in the fragment shader.
layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint alpha_pen;
	uint alpha_factor;
	uint fade_factor;
	uint fade_r, fade_g, fade_b;
	uint poly_flags;
	uint poly_r, poly_g, poly_b;
} pc;

layout(location = 0) noperspective out vec3  v_uvw;
layout(location = 1) noperspective out float v_iw;
layout(location = 2) flat          out uint  v_attr;
layout(location = 3) flat          out uint  v_bn;
layout(location = 4) flat          out uint  v_base;
layout(location = 5) flat          out uint  v_sf0;
layout(location = 6) flat          out uint  v_sf1;

void main()
{
	v_uvw  = in_uvw;
	v_iw   = in_iw;
	v_attr = in_attr;
	v_bn   = in_bn;
	v_base = in_base;
	v_sf0  = in_sf0;
	v_sf1  = in_sf1;
	gl_Position = vec4((in_xy / pc.half_size) - 1.0, 0.5, 1.0);
}
