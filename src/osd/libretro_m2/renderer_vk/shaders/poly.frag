#version 450

// Model 2 libretro core — the raster tail, untextured half.
//
// This is model2_renderer::draw_scanline_solid() from src/mame/sega/model2rd.ipp, transliterated.
// The whole of its colour chain is:
//
//     luma  = object.luma >> 2                             // 8 bits of luma down to 6
//     color = m_palram[colorbase + 0x1000]                 // resolved at the seam, arrives as palcolor
//     tr    = gamma[ colorxlat[0x0000 + ((color >> 0) & 0x1f) * 256 + luma] & 0xff ]
//     tg    = gamma[ colorxlat[0x2000 + ((color >> 5) & 0x1f) * 256 + luma] & 0xff ]
//     tb    = gamma[ colorxlat[0x4000 + ((color >>10) & 0x1f) * 256 + luma] & 0xff ]
//
// (MAME writes those three ramp bases as byte offsets 0x0000/0x4000/0x8000 into a u16 array, which
// is the same thing.) The gamma lookup is not here because it is already folded into the table the
// record uploads — every reader of a ramp in model2rd.ipp is `colortable_x[luma] & 0xff` immediately
// followed by `gamma_value[...]`, so baking one into the other is lossless and turns two dependent
// lookups per component into one. m2vk_frame.cpp does the fold.
//
// Two behaviours that are easy to lose:
//
//   * A translucent *untextured* polygon draws nothing at all — draw_scanline_solid<true> returns
//     before it writes a pixel. It is dropped at upload rather than discarded here, so this shader
//     never sees one.
//   * The checker flag is a 50% screen door, not a blend: MAME steps x by two and starts at the
//     first x where (x ^ scanline) & 1 is 1. gl_FragCoord.xy is the same x and scanline, so the
//     test is literal. This is why the vertex shader maps to the visible extent rather than to a
//     512-wide target.
//
// Nothing blends anywhere in this renderer. Model 2's translucency is a cutout and checker is a
// stipple; both are per-pixel discards and every surviving fragment is opaque.

layout(location = 0) noperspective in vec3 v_param;
layout(location = 1) flat in uint v_poly;

layout(location = 0) out vec4 out_colour;

// Per polygon, in draw order, written by the renderer from the frame record. Kept at four words so
// that the textured steps can grow it without moving what is already here.
struct poly_params
{
	uint palcolor;      // the raw m_palram word; only bits 0..14 are ever read
	uint luma;          // poly.luma, 8 bits, shifted down to 6 below
	uint flags;         // 1 = translucent, 2 = textured, 4 = checker
	uint reserved;
};

layout(std430, set = 0, binding = 0) readonly buffer poly_block { poly_params polys[]; };

// m_colorxlat with the gamma table folded in, one byte per entry, packed four to a word. Bytes
// rather than a uint per entry because the same shape is what texture RAM has to use in step 4 —
// 2 MB of it — and having one accessor idiom for both is worth more than the two instructions.
layout(std430, set = 0, binding = 1) readonly buffer xlat_block { uint colorxlat[]; };

// m_lumaram. Unused by the solid path; bound now because the record already carries it and an
// unbound descriptor is a worse thing to leave lying around than an unread one.
layout(std430, set = 0, binding = 2) readonly buffer luma_block { uint lumaram[]; };

uint fetch_xlat(uint index)
{
	return (colorxlat[index >> 2u] >> ((index & 3u) << 3u)) & 0xffu;
}

void main()
{
	poly_params p = polys[v_poly];

	if ((p.flags & 4u) != 0u)
	{
		ivec2 c = ivec2(gl_FragCoord.xy);
		if (((c.x ^ c.y) & 1) == 0)
			discard;
	}

	uint luma = p.luma >> 2u;
	uint colour = p.palcolor;

	uint r = fetch_xlat(0x0000u + (((colour >>  0u) & 0x1fu) << 8u) + luma);
	uint g = fetch_xlat(0x2000u + (((colour >>  5u) & 0x1fu) << 8u) + luma);
	uint b = fetch_xlat(0x4000u + (((colour >> 10u) & 0x1fu) << 8u) + luma);

	// Exact: a UNORM attachment stores round(f * 255), so n/255 comes back as n for every n.
	out_colour = vec4(vec3(float(r), float(g), float(b)) / 255.0, 1.0);
}
