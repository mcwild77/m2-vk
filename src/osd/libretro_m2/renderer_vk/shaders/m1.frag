#version 450

// Sega Model 1 libretro core — the flat quad fragment (M1-2: untextured, painter's order).
//
// v_col is the resolved value fill_quad writes straight into bitmap_rgb32: bits 0..23 = 0x00RRGGBB,
// bit 24 = MOIRE. The software path draws a MOIRE quad through draw_hline_moired, which writes a pixel
// only where (x ^ y) & 1 is odd — a one-pixel checker stipple standing in for translucency (the same
// screen-door idea as Model 2's `checker`). Reproduced here as a discard on the even squares. At a
// raised internal resolution one bitmap pixel spans stipple_div attachment pixels, so gl_FragCoord is
// quantised by it first, keeping the checker one BITMAP pixel wide the way the software render is.
//
// Colour order matches s21_finish.frag / the passthrough: the attachment is B8G8R8A8_UNORM and MAME's
// 0x00RRGGBB is memcpy'd into it, so the shader emits R from bits 16..23 and the format places the bytes.

layout(location = 0) flat in uint v_col;
layout(location = 1) flat in uint v_albedo;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint stipple_div;
	uint flat_luma;      // No Lighting: non-zero → emit the pre-luma albedo instead of the lit colour
} pc;

void main()
{
	// The MOIRE translucency flag lives on the lit colour (bit 24); it is a property of the polygon, not of
	// the lighting, so the stipple stands whether or not No Lighting is on.
	if ((v_col & 0x01000000u) != 0u)
	{
		uint d = (pc.stipple_div == 0u) ? 1u : pc.stipple_div;
		uint px = uint(gl_FragCoord.x) / d;
		uint py = uint(gl_FragCoord.y) / d;
		if (((px ^ py) & 1u) == 0u)
			discard;
	}

	// Default: the driver's exact lit colour (color_xlat LUT output). No Lighting: the raw albedo.
	uint rgb = (pc.flat_luma != 0u) ? v_albedo : v_col;

	out_color = vec4(float((rgb >> 16) & 0xffu) / 255.0,
			float((rgb >> 8) & 0xffu) / 255.0,
			float(rgb & 0xffu) / 255.0,
			1.0);
}
