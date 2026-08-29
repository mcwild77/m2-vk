#version 450

// Sega Model 1 libretro core — the SMOOTH-SHADED quad vertex ("Smooth Shading", Model 1 only).
//
// This is the enhancement companion to m1.vert's flat pass. Model 1 is flat-shaded per FACE in
// hardware (model1_v.cpp push_object resolves one normal → one lumval → one colour per quad); this pass
// instead carries a per-vertex normal that the renderer synthesised by averaging the authored face
// normals of every face meeting at that vertex (m1_geom.cpp weld pass), plus the raw lighting inputs, and
// re-runs the driver's own lighting maths PER PIXEL in m1_smooth.frag. It is NOT accurate — Model 1 never
// smooth-shaded — so it is an opt-in toggle, default off, gated on the Model 1 family.
//
// The normal is the only smoothly-interpolated varying; the light vector and the per-face light
// parameters are flat (constant across the quad), and colour/albedo ride the vertex exactly as in the
// flat pass. The pixel-to-NDC map is identical to m1.vert (the projection already happened in the TGP),
// so this pass supersamples at a raised internal resolution for the same reason.

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

layout(location = 0) out vec3      v_normal;    // the one smoothly-interpolated varying
layout(location = 1) flat out vec3 v_light;
layout(location = 2) flat out vec4 v_lparams;
layout(location = 3) flat out uint v_col;
layout(location = 4) flat out uint v_albedo;

void main()
{
	v_normal = in_normal;
	v_light = in_light;
	v_lparams = in_lparams;
	v_col = in_col;
	v_albedo = in_albedo;
	gl_Position = vec4((in_pos / pc.half_size) - 1.0, 0.5, 1.0);
}
