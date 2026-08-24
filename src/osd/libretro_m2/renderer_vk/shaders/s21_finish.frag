#version 450

// Namco System 21 libretro core — the finish pass: OVER-sprite composite + CLUT resolve (option B).
//
// The last slice of the pen-space composite, and the reason for the whole rework. The pen pass has
// left the composited 2D-under + 3D + layer-0 mix in the R16_UINT pen attachment (sampled here, 1:1 —
// the attachment is the same size as this pass's target). This pass draws the OVER band on top and
// resolves everything to RGB in one go:
//
//   * The C355 OVER band (high-priority sprites, and the flat layer-0 for pri1 in {0,2}) was captured
//     by namcos21_c67.cpp capture_over_sprites into a native-resolution buffer, tagged per pixel:
//       tag 0  transparent — keep the pen beneath.
//       tag 1  palette-shadow, bank 1 — dest = 0x4000 | (pen & 0x1fff).
//       tag 2  palette-shadow, bank 2 — dest = 0x6000 | (pen & 0x1fff).
//       tag 3  opaque — take the carried pen (a normal sprite, or a shadow that already resolved over
//              another OVER-band pixel on the CPU, which no longer depends on what is beneath).
//     This is exactly namcos21_c67_state::sprite_mix_callback's `dest = 0x4000|(dest&0x1fff)` etc.,
//     but with `dest` being the REAL scene pen — the whole point of compositing in pen space, since an
//     RGB composite has thrown that pen away by the time the shadow is applied. Banks 1/2 are the
//     independent "polygon palette for sprite blending" gradients, so nothing but a pen index resolves
//     them correctly.
//
//   * The final pen indexes the palette CLUT (m_palette->entry_list_adjusted(), re-uploaded each frame)
//     to the 0x00RRGGBB the frontend expects.

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push
{
	uint width, height;   // the OVER buffer's native dimensions
	// Winning Run only: its GPU bitmap draws pen 0x00/0x01 OPAQUE where the pen beneath is the backdrop
	// sentinel, and a palette shadow everywhere else (namcos21.cpp bitmap_draw). shadow_enable is 0 for the
	// C67 games, whose OVER band shadows unconditionally, so their resolve below is unchanged.
	uint shadow_enable;
	uint sentinel;        // (gpu_color<<8 & 0xf00) | 0xff — the untouched-backdrop pen
	uint opaque_base;     // (gpu_color<<8 & 0xf00) — ORed with 0/1 for the opaque substitute
} pc;

layout(set = 0, binding = 0) uniform usampler2D pen_tex;

layout(std430, set = 0, binding = 1) readonly buffer OverBuf
{
	uint over[];
};

layout(std430, set = 0, binding = 2) readonly buffer Palette
{
	uint pens[];
};

void main()
{
	// The composited pen beneath, 1:1 with this pixel (pen attachment == target extent).
	uint pen = texelFetch(pen_tex, ivec2(gl_FragCoord.xy), 0).r;

	// The OVER band, point-sampled from its native-resolution capture.
	ivec2 c = ivec2(v_uv * vec2(pc.width, pc.height));
	c = clamp(c, ivec2(0), ivec2(int(pc.width) - 1, int(pc.height) - 1));
	uint oidx = uint(c.y) * pc.width + uint(c.x);
	uint otexel = (oidx < over.length()) ? over[oidx] : 0u;
	uint tag = otexel >> 24;

	bool over_backdrop = (pc.shadow_enable != 0u) && (pen == pc.sentinel);
	uint final_pen = pen;
	if (tag == 1u)
		final_pen = over_backdrop ? (pc.opaque_base | 0x00u) : (0x4000u | (pen & 0x1fffu));
	else if (tag == 2u)
		final_pen = over_backdrop ? (pc.opaque_base | 0x01u) : (0x6000u | (pen & 0x1fffu));
	else if (tag == 3u)
		final_pen = otexel & 0x00ffffffu;

	uint rgb = (final_pen < pens.length()) ? pens[final_pen] : 0u;
	out_color = vec4(float((rgb >> 16) & 0xffu) / 255.0,
			float((rgb >> 8) & 0xffu) / 255.0,
			float(rgb & 0xffu) / 255.0, 1.0);
}
