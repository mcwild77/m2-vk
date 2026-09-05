#version 450

// Sega Model 1 libretro core — the SMOOTH-SHADED quad vertex ("Smooth Shading", Model 1 only).
//
// GOURAUD variant. This is the enhancement companion to m1.vert's flat pass. Model 1 is flat-shaded per
// FACE in hardware (model1_v.cpp push_object resolves one normal -> one lumval -> one colour per quad);
// this pass instead carries a per-vertex normal that the renderer synthesised by averaging the authored
// face normals of every face meeting at that vertex (m1_geom.cpp weld pass), and re-runs the driver's own
// lighting maths PER VERTEX here. The single resulting luminance is the only smoothly-interpolated varying;
// m1_smooth.frag then quantises that interpolated luma and runs it through the color_xlat LUT per pixel.
// (An earlier form interpolated the normal and lit per pixel — Phong; this interpolates the shaded result
// instead, exactly as Model 2's model2_smooth_shading does.) It is NOT accurate — Model 1 never
// smooth-shaded — so it is an opt-in toggle, default off, gated on the Model 1 family.
//
// A flat quad (identical vertex normals) yields a constant luminance at every vertex, so the interpolation
// is constant and the fragment reproduces the flat pass bit-for-bit — the built-in correctness check.
// colour/albedo ride the vertex exactly as in the flat pass. The pixel-to-NDC map is identical to m1.vert
// (the projection already happened in the TGP), so this pass supersamples at a raised internal resolution
// for the same reason.

layout(location = 0) in vec2 in_pos;      // sub-pixel screen-space (the float projected pixel, M1-3)
layout(location = 1) in vec3 in_normal;   // per-vertex view-space normal (welded from face normals)
layout(location = 2) in vec3 in_light;    // view-space light direction, already normalised (flat)
layout(location = 3) in vec4 in_lparams;  // this face's light params: ambient, diffuse, specular, power
layout(location = 4) in uint in_col;      // 0x00RRGGBB | (MOIRE<<24) | (HAS_NORMAL<<25), the lit colour
layout(location = 5) in uint in_albedo;   // 0x00RRGGBB, the pre-luma albedo ("No Lighting")

layout(push_constant) uniform push_block
{
	vec2 half_size;      // the visible picture's half-extent in pixels
	uint stipple_div;    // attachment pixels per moiré checker square (1 at native)
	uint flat_luma;      // No Lighting: non-zero → the fragment emits albedo
} pc;

layout(location = 0) out float     v_lum;     // the one smoothly-interpolated varying: shaded luminance
layout(location = 1) flat out uint v_col;
layout(location = 2) flat out uint v_albedo;

void main()
{
	// The driver's per-face lighting, evaluated here per vertex with the welded normal.
	// ln = ambient + diffuse * max(0, N·L). (compute_specular is 0 in the driver.)
	vec3 N = normalize(in_normal);
	vec3 L = in_light;                    // normalised on the CPU (m_view->light)
	float amb = in_lparams.x;
	float kd  = in_lparams.y;
	float ks  = in_lparams.z;
	float pw  = in_lparams.w;

	float ln = amb + kd * max(0.0, dot(N, L));

	// The enhancement: a Blinn-Phong specular the hardware parsed (lightparams[].s / .p) but never drew.
	// View direction is a constant toward the camera in this view space (the TGP looks down +z, project
	// divides by z). power 0..7 → a gentle-to-tight highlight. Evaluated per vertex and interpolated —
	// the classic Gouraud specular.
	vec3 V = vec3(0.0, 0.0, -1.0);
	vec3 H = normalize(L + V);
	float shininess = exp2(2.0 + pw);
	ln += ks * pow(max(0.0, dot(N, H)), shininess);

	v_lum = ln;
	v_col = in_col;
	v_albedo = in_albedo;
	gl_Position = vec4((in_pos / pc.half_size) - 1.0, 0.5, 1.0);
}
