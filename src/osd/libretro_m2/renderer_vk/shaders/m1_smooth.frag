#version 450

// Sega Model 1 libretro core — the SMOOTH-SHADED quad fragment ("Smooth Shading", Model 1 only).
//
// See m1_smooth.vert. This re-runs Model 1's own per-face lighting maths (model1_v.cpp push_object) at
// per-pixel granularity, using the interpolated per-vertex normal instead of the one flat face normal.
// The tone curve is the game's REAL one: the driver maps (5-bit palette channel, 6-bit luma) through the
// color_xlat RAM LUT, and this shader indexes a snapshot of that same table (set 0, binding 0) so a
// perfectly flat quad reproduces the flat pass bit-for-bit — the built-in correctness check. On top of
// the driver's diffuse term this adds a real Blinn-Phong specular highlight scaled by the face's own
// specular light-parameter (lightparams[].s), which the hardware parsed but never rendered
// (compute_specular is dead in the driver). That highlight is the visible payoff and is pure enhancement.
//
// The colour byte order matches m1.frag / the passthrough: the attachment is B8G8R8A8_UNORM and MAME's
// 0x00RRGGBB is placed by the format, so the shader emits R from bits 16..23.

layout(location = 0) in vec3      v_normal;
layout(location = 1) flat in vec3 v_light;
layout(location = 2) flat in vec4 v_lparams;   // ambient, diffuse, specular, power
layout(location = 3) flat in uint v_col;
layout(location = 4) flat in uint v_albedo;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint stipple_div;
	uint flat_luma;
} pc;

// The color_xlat luma LUT, snapshotted from the driver's RAM (m1_seam color_xlat_snapshot) and packed
// four bytes per uint. Only bits 3..7 of each entry's low byte matter, exactly as in the driver.
layout(std430, set = 0, binding = 0) readonly buffer Xlat { uint data[]; } xlat;

uint lut(uint idx)
{
	return (xlat.data[idx >> 2] >> ((idx & 3u) * 8u)) & 0xffu;
}

// pal5bit: expand a 5-bit channel to 8-bit exactly as MAME's pal5bit (v<<3 | v>>2), then to [0,1].
float pal5(uint v)
{
	return float((v << 3) | (v >> 2)) / 255.0;
}

void main()
{
	// MOIRE translucency — identical stipple to the flat pass; a property of the polygon, not the lighting.
	if ((v_col & 0x01000000u) != 0u)
	{
		uint d = (pc.stipple_div == 0u) ? 1u : pc.stipple_div;
		uint px = uint(gl_FragCoord.x) / d;
		uint py = uint(gl_FragCoord.y) / d;
		if (((px ^ py) & 1u) == 0u)
			discard;
	}

	// The pre-luma 5-bit palette channels, recovered from the 8-bit albedo (pal5bit is exactly invertible
	// by >>3): these are what the driver fed the color_xlat LUT.
	uint r5 = ((v_albedo >> 16) & 0xffu) >> 3;
	uint g5 = ((v_albedo >>  8) & 0xffu) >> 3;
	uint b5 = ( v_albedo        & 0xffu) >> 3;

	// No Lighting (shared with the flat pass): emit the raw albedo, smoothing has no lit gradient to show.
	if (pc.flat_luma != 0u)
	{
		out_color = vec4(pal5(r5), pal5(g5), pal5(b5), 1.0);
		return;
	}

	// A quad with no synthesised normal (the draw_direct path, which carries a precomputed luminosity and no
	// face normal) cannot be smooth-shaded: fall back to the driver's own flat lit colour, so direct
	// geometry looks exactly as it does in the flat pass rather than going black.
	if ((v_col & 0x02000000u) == 0u)
	{
		out_color = vec4(float((v_col >> 16) & 0xffu) / 255.0,
				float((v_col >> 8) & 0xffu) / 255.0,
				float(v_col & 0xffu) / 255.0,
				1.0);
		return;
	}

	vec3 N = normalize(v_normal);
	vec3 L = v_light;                     // normalised on the CPU (m_view->light)
	float amb = v_lparams.x;
	float kd  = v_lparams.y;
	float ks  = v_lparams.z;
	float pw  = v_lparams.w;

	// The driver's diffuse: ln = ambient + diffuse * max(0, N·L). (compute_specular is 0 in the driver.)
	float dif = dot(N, L);
	float ln = amb + kd * max(0.0, dif);

	// The enhancement: a Blinn-Phong specular the hardware parsed (lightparams[].s / .p) but never drew.
	// View direction is a constant toward the camera in this view space (the TGP looks down +z, project
	// divides by z). power 0..7 → a gentle-to-tight highlight.
	vec3 V = vec3(0.0, 0.0, -1.0);
	vec3 H = normalize(L + V);
	float shininess = exp2(2.0 + pw);
	ln += ks * pow(max(0.0, dot(N, H)), shininess);

	// The driver's luma quantise: lumval = (255 * min(1, ln)) >> 2, clamped to 0..0x3f.
	int lum = int(255.0 * min(1.0, ln)) >> 2;
	lum = clamp(lum, 0, 0x3f);
	uint ul = uint(lum);

	// The color_xlat LUT, per channel, in the driver's three banks (R 0x0000, G 0x2000, B 0x4000).
	uint ro = (lut((r5 << 8) | ul | 0x0000u) >> 3) & 0x1fu;
	uint go = (lut((g5 << 8) | ul | 0x2000u) >> 3) & 0x1fu;
	uint bo = (lut((b5 << 8) | ul | 0x4000u) >> 3) & 0x1fu;

	out_color = vec4(pal5(ro), pal5(go), pal5(bo), 1.0);
}
