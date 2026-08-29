#version 450

// Model 2 libretro core — the raster tail.
//
// This is model2_renderer::draw_scanline_solid() and draw_scanline_tex() from
// src/mame/sega/model2rd.ipp, transliterated. It is transliteration and not design: where a line here
// looks gratuitously literal — an integer division that could be a shift, a mask that provably does
// nothing, a fudge that is not an addressing mode — it is literal because the software renderer is
// the reference and matching it is the whole job. The place to be clever is P5.
//
// THE UNTEXTURED PATH is draw_scanline_solid's colour chain, entire:
//
//     luma  = object.luma >> 2                             // 8 bits of luma down to 6
//     color = m_palram[colorbase + 0x1000]                 // resolved at the seam, arrives as palcolor
//     tr    = gamma[ colorxlat[0x0000 + ((color >> 0) & 0x1f) * 256 + luma] & 0xff ]
//     tg    = gamma[ colorxlat[0x2000 + ((color >> 5) & 0x1f) * 256 + luma] & 0xff ]
//     tb    = gamma[ colorxlat[0x4000 + ((color >>10) & 0x1f) * 256 + luma] & 0xff ]
//
// (MAME writes those three ramp bases as byte offsets 0x0000/0x4000/0x8000 into a u16 array, which is
// the same thing.) The gamma lookup is not here because it is already folded into the table the record
// uploads — every reader of a ramp in model2rd.ipp is `colortable_x[luma] & 0xff` immediately followed
// by `gamma_value[...]`, so baking one into the other is lossless and turns two dependent lookups per
// component into one. m2vk_frame.cpp does the fold.
//
// THE TEXTURED PATH replaces `object.luma >> 2` with the filtered texel put through the luma
// translator — lumaram[lumabase + (t >> 1)] * object.luma / 256, clamped to 0x3f — and the same three
// ramp lookups finish it. Everything above that is getting `t`, and it is the largest thing in P3:
// get_texel, fetch_bilinear_texel, fast_log2, the mip level, the trilinear blend and the
// microtexture blend.
//
// WHY THERE IS NO VkSampler. The obvious move — LINEAR filtering with a real mip chain — is wrong
// three times over, and each one alone is fatal:
//
//   * the filtering happens in INDEX space, before the LUT. MAME bilinearly interpolates 4-bit texel
//     values shifted to 8 bits, and only then does the result index lumaram -> colorxlat. Filtering
//     after the LUT is a different picture, not a rounding difference.
//   * the wrap/clamp behaviour is a fudge, not an addressing mode. With smooth wrap off and the
//     filter straddling the edge, MAME rewrites u0/u1 and FORCES ufrac to 0 or 0x100 depending on
//     which side of the texel centre it fell — a hard snap, not CLAMP_TO_EDGE. Mirroring is
//     `if (mirror && (u & (width << 8))) u = ~u` on the 8.8 fixed-point coordinate.
//   * the translucent filter is not a filter at all: a transparent texel takes the luma of its
//     neighbour so the interpolation cannot drag colour out of the transparent region.
//
// So the texture sheets are a storage buffer of raw u32 words exactly as they sit in RAM — no decode,
// no atlas, no cache, because both sheets together are only 2 MB — and every step of the filter is
// spelled out below.
//
// THE MIP CHAIN IS ALREADY RESIDENT and this is the part that is easy to get wrong. Level n is not
// generated: fetch_bilinear_texel halves texwidth/texheight, shifts texx/texy, and reads
// texsheet[level & 1] — the game put the levels in texture RAM itself, at shifted addresses,
// ALTERNATING BETWEEN THE TWO SHEETS. Levels 0 and 2 come from this polygon's sheet, levels 1 and 3
// from the other one, so both sheets must be addressable from every draw and the sheet index is
// `p.sheet ^ (level & 1)`. Microtexture is level -1: a 128x128 window at utexx,utexy in the OTHER
// sheet, with u and v shifted left by 1 << utexminlod.
//
// u, v and z come in as noperspective varyings, which is not an approximation — it is what MAME does.
// It interpolates ooz, uoz and voz LINEARLY IN SCREEN SPACE along the polygon's edges and then
// computes z = 1/ooz and u = uoz * z * 256. noperspective with gl_Position.w == 1 is exactly that, so
// the first three lines of the textured branch are MAME's inner loop verbatim. Perspective-correct
// interpolation with a real w would compute the same quantity by a different route and would not
// round identically.
//
// Two behaviours that are easy to lose:
//
//   * A translucent *untextured* polygon draws nothing at all — draw_scanline_solid<true> returns
//     before it writes a pixel. It is dropped at upload rather than discarded here, so this shader
//     never sees one. A translucent *textured* one does reach here, and its cutout is a discard.
//   * The checker flag is a 50% screen door, not a blend: MAME steps x by two and starts at the first
//     x where (x ^ scanline) & 1 is 1. gl_FragCoord.xy divided by pc.stipple_div is the same x and
//     scanline whenever the divisor is the picture's, so the test is literal. This is why the vertex
//     shader maps to the visible extent rather than to a 512-wide target — and why the divisor is a
//     per-frame decision rather than the resolution, which is the comment at the discard.
//
// Nothing blends anywhere in this renderer AS THE HARDWARE DREW IT. Model 2's translucency is a cutout
// and checker is a stipple; both are per-pixel discards and every surviving fragment is opaque, and
// that is what pc.blend == 0 — the default, and the only thing the A/B harness ever measures — gets.
// The model2_transparency option turns the screen door into a real 50% blend instead, which is an
// enhancement and not an accuracy fix; the two discards below and the alpha at the bottom are the
// whole of it here, and vk_geom.cpp's deferred second pass is the rest. See that file's header for why
// blending cannot happen in the polygon's own place in the stream.
//
// THIS FILE COMPILES TWICE. -DEARLY_Z=1 produces the specialisation for polygons that cannot discard,
// which is MAME's draw_scanline_tex<false> reached by a preprocessor symbol instead of a template
// parameter. The two discard sites below are the entire difference, and they are gated on exactly the
// two flags vk_geom.cpp tests to choose the pipeline — one predicate, written once, so the shader and
// the renderer cannot drift apart. Both variants must stay in this one file for that reason.
//
// EarlyFragmentTests is an execution mode on the entry point, not a value, so a specialisation
// constant cannot reach it: it has to be a second module. And it may only be declared in a shader
// with no reachable discard, because it moves the depth write to BEFORE the fragment shader runs —
// so a discard under it would leave a pixel claimed in the depth buffer and unwritten in the colour
// buffer. That is precisely the "unclaimed key" rule inverted, which is why the flag predicate is
// load-bearing rather than an optimisation detail.
#ifdef EARLY_Z
layout(early_fragment_tests) in;
#endif

layout(location = 0) noperspective in vec3 v_param;
layout(location = 1) flat in uint v_poly;
// model2_smooth_shading: the interpolated per-vertex luma. Off → a constant equal to the poly luma, so
// the two luma sites below reproduce the flat path bit-for-bit; on → a smooth gradient across the face.
layout(location = 2) in float v_smooth_luma;

// Declared identically in poly.vert, because the push constant range covers both stages. This shader
// reads only `stipple_div` and `blend`, the vertex shader only `half_size`; both must declare all
// three, or the offsets disagree.
layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint stipple_div;
	uint blend;
} pc;

layout(location = 0) out vec4 out_colour;

// Per polygon, in draw order, written by the renderer from the frame record. Sixteen words, all
// scalar, so the std430 array stride is a plain 64 bytes.
struct poly_params
{
	uint palcolor;      // the raw m_palram word; only bits 0..14 are ever read
	uint luma;          // poly.luma, 8 bits
	uint flags;         // see below
	int  texlod;        // the polygon's LOD bias, already summed with the log RAM entry

	uint texwidth;      // 32 << n
	uint texheight;     // 32 << n
	uint texx;          // 32 * n, before the -2048 the mip arithmetic applies
	uint texy;          // 32 * n, before the -1024

	uint lumabase;      // (texheader[1] & 0xff) << 7
	uint sheet;         // which sheet texheader[2] bit 12 names: texsheet[i] is sheet ^ i
	uint utexx;
	uint utexy;

	uint utexminlod;
	int  max_level;     // 30 - clz(min(texwidth, texheight)) — resolved on the CPU
	uint reserved0;
	uint reserved1;
};

const uint FLAG_TRANSLUCENT = 1u;
const uint FLAG_TEXTURED    = 2u;
const uint FLAG_CHECKER     = 4u;
const uint FLAG_WRAPX       = 8u;
const uint FLAG_WRAPY       = 16u;
const uint FLAG_MIRRORX     = 32u;
const uint FLAG_MIRRORY     = 64u;
const uint FLAG_UTEX        = 128u;

layout(std430, set = 0, binding = 0) readonly buffer poly_block { poly_params polys[]; };

// m_colorxlat with the gamma table folded in, one byte per entry, packed four to a word. Bytes rather
// than a uint per entry because texture RAM below has to be packed anyway, and one accessor idiom for
// both is worth more than the two instructions.
layout(std430, set = 0, binding = 1) readonly buffer xlat_block { uint colorxlat[]; };

// m_lumaram, 0x8000 bytes, four to a word.
layout(std430, set = 0, binding = 2) readonly buffer luma_block { uint lumaram[]; };

// Both texture sheets, raw, sheet 0 first: word i of sheet s is texram[(s << 18) + i]. 0x40000 u32
// words each is 1 MB, which is the whole of one sheet as get_texel can address it.
layout(std430, set = 0, binding = 3) readonly buffer texram_block { uint texram[]; };

const uint TEXRAM_SHEET_SHIFT = 18u;
const uint TEXRAM_WORD_MASK   = 0x3ffffu;

uint fetch_xlat(uint index)
{
	return (colorxlat[index >> 2u] >> ((index & 3u) << 3u)) & 0xffu;
}

uint fetch_luma(uint index)
{
	return (lumaram[index >> 2u] >> ((index & 3u) << 3u)) & 0xffu;
}


//============================================================
//  get_texel
//============================================================

// model2_renderer::get_texel, literally. The sheets are mapped as 2048x1024 but stored in RAM as
// 1024x2048, which is what the x2 >= 1024 fold is; two texels share a byte and two bytes share a
// 16-bit half of the fetched word, which is what the three conditional shifts are.
//
// The index is masked to the sheet rather than trusted. MAME does not mask, and a texheight of 4096
// with a large texy would let it read past the end of the 1 MB share — so this differs from the
// software renderer only where the software renderer is reading someone else's memory.
uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet)
{
	int x2 = int(base_x) + x;
	int y2 = int(base_y) + y;

	if (x2 >= 1024)
	{
		x2 -= 1024;
		y2 ^= 1024;
	}

	uint offset = uint(((y2 / 2) * 512) + (x2 / 2));
	uint texel = texram[(sheet << TEXRAM_SHEET_SHIFT) + ((offset >> 1u) & TEXRAM_WORD_MASK)];

	if ((offset & 1u) != 0u)
		texel >>= 16;

	if ((y & 1) == 0)
		texel >>= 8;

	if ((x & 1) == 0)
		texel >>= 4;

	return texel & 0x0fu;
}


//============================================================
//  fetch_bilinear_texel
//============================================================

// Two 8-bit lanes packed into 0x00ff00ff: the texel in bits 0..7 and the alpha flag at bit 23.
// Interpolating both in one expression is what makes the translucent path's neighbour rule
// expressible at all, so the packing is here even though the opaque path only uses one lane.
uint LERP(uint x, uint y, uint a)
{
	return (x + (((y - x) * a) >> 8u)) & 0x00ff00ffu;
}

// model2_renderer::fetch_bilinear_texel. MAME's Translucent is a template parameter; here it is an
// ordinary bool, uniform across the polygon, and the driver hoists the branch or it does not — the
// alternative is two copies of a hundred lines that must stay identical, which is the more expensive
// mistake. Texel index 15 (0xf0 once shifted) is the transparent one.
uint fetch_bilinear_texel(poly_params p, bool translucent, int miplevel, int u, int v)
{
	uint tex_width, tex_height, tex_x, tex_y, sheet;

	if (miplevel == -1)
	{
		// microtexture: a fixed 128x128 window in the OTHER sheet
		tex_width = 128u;
		tex_height = 128u;
		tex_x = p.utexx;
		tex_y = p.utexy;
		sheet = p.sheet ^ 1u;
		u <<= 1 << int(p.utexminlod);
		v <<= 1 << int(p.utexminlod);
	}
	else
	{
		// regular texture. The level is already resident at a shifted address in one of the two
		// sheets, alternating — see the note at the top of the file.
		tex_width = p.texwidth >> miplevel;
		tex_height = p.texheight >> miplevel;
		tex_x = ((p.texx - 2048u) >> miplevel) & 2047u;
		tex_y = ((p.texy - 1024u) >> miplevel) & 1023u;
		sheet = p.sheet ^ uint(miplevel & 1);
		u >>= miplevel;
		v >>= miplevel;
	}

	if (((p.flags & FLAG_MIRRORX) != 0u) && ((uint(u) & (tex_width << 8u)) != 0u))
		u = ~u;

	if (((p.flags & FLAG_MIRRORY) != 0u) && ((uint(v) & (tex_height << 8u)) != 0u))
		v = ~v;

	// subtract 1/2 texel
	u -= 0x80;
	v -= 0x80;

	// extract the fractions to use as blending factors
	uint ufrac = uint(u) & 0xffu;
	uint vfrac = uint(v) & 0xffu;

	// get the four texel locations and confine to texture dimensions
	uint u0 = uint(u >> 8) & (tex_width - 1u);
	uint u1 = (u0 + 1u) & (tex_width - 1u);
	uint v0 = uint(v >> 8) & (tex_height - 1u);
	uint v1 = (v0 + 1u) & (tex_height - 1u);

	// Clamp if smooth wrapping is not enabled — and this is the fudge no sampler can do: the pair is
	// pushed off the edge and the fraction is FORCED to one end or the other, so the filter collapses
	// to a nearest fetch of whichever texel the sample fell inside.
	if (((p.flags & FLAG_WRAPX) == 0u) && (u1 == 0u))
	{
		if (ufrac >= 0x80u)
		{
			u0 = u1; u1++; ufrac = 0u;          // left edge of texture
		}
		else
		{
			u1 = u0; u0--; ufrac = 0x100u;      // right edge of texture
		}
	}

	if (((p.flags & FLAG_WRAPY) == 0u) && (v1 == 0u))
	{
		if (vfrac >= 0x80u)
		{
			v0 = 0u; v1++; vfrac = 0u;          // top edge of texture
		}
		else
		{
			v1 = v0; v0--; vfrac = 0x100u;      // bottom edge of texture
		}
	}

	// read the four texels from the texture sheet
	uint tex00 = get_texel(tex_x, tex_y, int(u0), int(v0), sheet) << 4u;
	uint tex01 = get_texel(tex_x, tex_y, int(u1), int(v0), sheet) << 4u;
	uint tex10 = get_texel(tex_x, tex_y, int(u0), int(v1), sheet) << 4u;
	uint tex11 = get_texel(tex_x, tex_y, int(u1), int(v1), sheet) << 4u;

	if (translucent)
	{
		// pack the alpha components into the upper 16 bits
		if (tex00 != 0xf0u) tex00 |= 0x00800000u;
		if (tex01 != 0xf0u) tex01 |= 0x00800000u;
		if (tex10 != 0xf0u) tex10 |= 0x00800000u;
		if (tex11 != 0xf0u) tex11 |= 0x00800000u;

		// If a texel is transparent, it takes the luma value of the neighbouring texel — so the two
		// LERPs below interpolate the alpha lane down towards zero without dragging colour out of the
		// transparent region with it. This is the whole reason the packing exists.
		//
		// The four tests are sequential and each reads the value the one before it may have written,
		// exactly as in model2rd.ipp. They agree with a parallel reading in every case (two adjacent
		// transparent texels both end up 0x000000f0 either way), but the software renderer is the
		// reference and the order it does things in is not ours to tidy.
		if (tex00 == 0x000000f0u) tex00 = tex01 & 0xffu;
		if (tex01 == 0x000000f0u) tex01 = tex00 & 0xffu;
		if (tex10 == 0x000000f0u) tex10 = tex11 & 0xffu;
		if (tex11 == 0x000000f0u) tex11 = tex10 & 0xffu;
	}

	// linearly interpolate between left and right texels, then between the two rows
	uint tex0x = LERP(tex00, tex01, ufrac);
	uint tex1x = LERP(tex10, tex11, ufrac);

	// the same rule again between the rows: a fully transparent row takes the other row's luma
	if (translucent)
	{
		if (tex0x == 0x000000f0u) tex0x = tex1x & 0xffu;
		if (tex1x == 0x000000f0u) tex1x = tex0x & 0xffu;
	}

	return LERP(tex0x, tex1x, vfrac);
}


//============================================================
//  fast_log2
//============================================================

// The one in model2rd.ipp, which is voodoo_render.cpp's: exponent plus a 7-bit mantissa lookup, in
// 8.8 fixed point. It is not an approximation of log2 that happens to be good enough — the mip
// selection below is defined in terms of *this* function's output, table included, so the table is
// copied rather than replaced with log2().
const uint LOG2_TABLE[128] = uint[128](
	  0u,   2u,   5u,   8u,  11u,  14u,  16u,  19u,  22u,  25u,  27u,  30u,  33u,  35u,  38u,  40u,
	 43u,  46u,  48u,  51u,  53u,  56u,  58u,  61u,  63u,  65u,  68u,  70u,  73u,  75u,  77u,  80u,
	 82u,  84u,  87u,  89u,  91u,  93u,  96u,  98u, 100u, 102u, 104u, 106u, 109u, 111u, 113u, 115u,
	117u, 119u, 121u, 123u, 125u, 127u, 129u, 132u, 134u, 136u, 138u, 140u, 141u, 143u, 145u, 147u,
	149u, 151u, 153u, 155u, 157u, 159u, 161u, 162u, 164u, 166u, 168u, 170u, 172u, 173u, 175u, 177u,
	179u, 181u, 182u, 184u, 186u, 188u, 189u, 191u, 193u, 194u, 196u, 198u, 200u, 201u, 203u, 205u,
	206u, 208u, 209u, 211u, 213u, 214u, 216u, 218u, 219u, 221u, 222u, 224u, 225u, 227u, 229u, 230u,
	232u, 233u, 235u, 236u, 238u, 239u, 241u, 242u, 244u, 245u, 247u, 248u, 250u, 251u, 253u, 254u
);

int fast_log2(float value)
{
	// return 0 for negative values; should never happen
	if (value < 0.0)
		return 0;

	// we only need the exponent and highest 7 bits of mantissa
	uint ival = floatBitsToUint(value) >> 16u;

	int expo = int(ival >> 7u) - 127;

	// Done in uint because shifting a negative int left is undefined in GLSL and the exponent is
	// routinely negative. C++ gets the two's-complement answer by custom; this gets it by definition.
	return int((uint(expo) << 8u) | LOG2_TABLE[ival & 127u]);
}


//============================================================
//  the fragment
//============================================================

void main()
{
	poly_params p = polys[v_poly];

	// The polygon luma the two shading sites below consume. model2_smooth_shading routes it through the
	// interpolated per-vertex value instead of the flat p.luma; with the option off that varying is a
	// constant equal to p.luma, so this is p.luma exactly and both paths stay bit-identical.
	uint poly_luma = uint(clamp(v_smooth_luma + 0.5, 0.0, 255.0));

#ifndef EARLY_Z
	// pc.blend is the model2_transparency option, and a checkered polygon under it is not stippled at
	// all: it is deferred to the renderer's second pass and blended there, so the screen door has to be
	// switched off rather than blended on top of. Every other polygon reads this as false, because the
	// flag test comes first — the two never both apply to one fragment.
	if (((p.flags & FLAG_CHECKER) != 0u) && (pc.blend == 0u))
	{
		// One square of the screen door spans pc.stipple_div attachment pixels, and the renderer sets
		// that per FRAME rather than deriving it from the resolution, because the two things it can
		// mean are genuinely different pictures:
		//
		//   M2VK_SS=n   -> n. The frame is about to be averaged back down to the picture, so the door
		//                  has to be one PICTURE pixel per square: every one of a picture pixel's n*n
		//                  subpixels then lands on the same parity, and the resolve reproduces the
		//                  half-covered pixel the software rasteriser draws. Without it an even scale
		//                  box-resolves a fine checkerboard into a uniform 50% BLEND — P4 step 2
		//                  measured one vcop2 quad drawing 78968 px at 1x and 157945 at 2x, the whole
		//                  hull, with 0.000% of the overlap the same colour. That is a shading change
		//                  dressed up as a resolution option.
		//
		//   internal    -> 1. Nothing is averaged; the frame is presented exactly as drawn. One square
		//   resolution     per OUTPUT pixel is the finest dither the picture can carry, and reads as
		//                  smooth translucency instead of a magnified screen door.
		//
		// The integer divide is exact and by 1 at native, so this is bit-exact the original there.
		ivec2 c = ivec2(gl_FragCoord.xy) / int(pc.stipple_div);
		if (((c.x ^ c.y) & 1) == 0)
			discard;
	}
#endif

	uint luma;

	if ((p.flags & FLAG_TEXTURED) == 0u)
	{
		luma = poly_luma >> 2u;
	}
	else
	{
		// draw_scanline_tex's inner loop, first three lines. ooz/uoz/voz are the screen-linear
		// varyings; everything after this is exact integer arithmetic.
		float ooz = v_param.x;
		float uoz = v_param.y;
		float voz = v_param.z;

		float z = 1.0 / ooz;

		int mml = -p.texlod + fast_log2(z);      // equivalent to log2(z^2)
		int level = clamp(mml >> 7, 0, p.max_level);

		// we give texture coordinates 8 fractional bits
		int u = int(uoz * z * 256.0);
		int v = int(voz * z * 256.0);

		// Constant in the early-Z variant, which is what dead-codes the packed alpha lane, the four
		// neighbour tests and the cutout below. The pipeline predicate guarantees it, so reading the flag
		// here would only give the compiler something it cannot prove.
#ifdef EARLY_Z
		const bool translucent = false;
#else
		bool translucent = (p.flags & FLAG_TRANSLUCENT) != 0u;
#endif

		uint t = fetch_bilinear_texel(p, translucent, level, u, v);

		if ((mml > 0) && (level < p.max_level))
		{
			uint t2 = fetch_bilinear_texel(p, translucent, level + 1, u, v);
			t = LERP(t, t2, uint((mml & 127) << 1));
		}
		else if (((p.flags & FLAG_UTEX) != 0u) && (mml < 0))
		{
			// microtexture; blend up to almost 50%
			uint t2 = fetch_bilinear_texel(p, translucent, -1, u, v);
			t = LERP(t, t2, uint(min((-mml) >> int(p.utexminlod), 127)));
		}

		// Textually removed rather than left for the optimiser to fold away on the constant above: a
		// discard that is merely unreachable is still a discard in the module, and EarlyFragmentTests may
		// only be declared where there is none.
#ifndef EARLY_Z
		if (translucent)
		{
			// The cutout, and it is the only place Model 2's "translucency" means anything: the alpha
			// lane sits in bits 16..23, so 0x00400000 is 50%. Below that the pixel is not drawn — and
			// because this variant has no EarlyFragmentTests execution mode, a discarded fragment does
			// not write depth either, which is what keeps the draw-order key equal to m_fillmap.
			if (t < 0x00400000u)
				discard;

			// remove the alpha value; no longer needed
			t &= 0xffu;
		}
#endif

		// The filtered texel has 8 bits of precision and the translator map has 128 entries, hence the
		// shift — it is not an off-by-one waiting to be fixed. lumabase + (t >> 1) is exactly 15 bits
		// with nothing to spare: lumabase maxes at 0x7f80, t >> 1 at 0x7f, and lumaram is 0x8000.
		luma = (fetch_luma(p.lumabase + (t >> 1u)) * poly_luma) / 256u;

		// Virtua Striker sets up a luma of 0x40 for national flags on bleachers. Load-bearing: without
		// it the index runs past the end of the 64-entry ramp.
		luma = min(luma, 0x3fu);
	}

	// six bits of luma against five bits per colour component, into the master lookup
	uint colour = p.palcolor;

	uint r = fetch_xlat(0x0000u + (((colour >>  0u) & 0x1fu) << 8u) + luma);
	uint g = fetch_xlat(0x2000u + (((colour >>  5u) & 0x1fu) << 8u) + luma);
	uint b = fetch_xlat(0x4000u + (((colour >> 10u) & 0x1fu) << 8u) + luma);

	// The blend pipeline's source factor is SRC_ALPHA, so this is the whole of what "half transparent"
	// means under the option — one half, matching the screen door's one-in-two coverage exactly, so the
	// two modes average to the same picture over any region larger than a texel. Alpha is 1.0 for every
	// other polygon, which is what the other two pipelines need: they have blending disabled and write
	// this straight into the attachment, where the 2D composite expects an opaque frame.
	const float alpha = (((p.flags & FLAG_CHECKER) != 0u) && (pc.blend != 0u)) ? 0.5 : 1.0;

	// Exact: a UNORM attachment stores round(f * 255), so n/255 comes back as n for every n.
	out_colour = vec4(vec3(float(r), float(g), float(b)) / 255.0, alpha);
}
