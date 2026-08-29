#version 450

// Namco System 23 libretro core — the polygon fragment (23-3: textured, painter's order).
//
// This is namcos23_renderer::texture_lookup + the per-pixel texel/shade steps of render_scanline
// (namcos23.cpp), transliterated. Where a line looks gratuitously literal — a mask that provably does
// nothing (cmode is only three bits, so cmode&8 is always 0), a >>6 where a divide would read cleaner —
// it is literal because the software rasteriser is the reference and matching its integer arithmetic is
// the job.
//
// System 23 has NO untextured path: every render_scanline pixel is a texel fetch. So unlike System 22
// (whose flat-shaded polys take a base colour) there is no fill branch here — the texel is always
// sampled. The tile system is uploaded as raw storage buffers exactly as it sits in the driver's arrays;
// the tileid->address indirection is already resolved into tmrom_decoded, so the fetch is one indirection
// shorter than S22's ttmap/ttattr/ayx chain.
//
// The shading tail render_flush's 6-bit render_hash selects (stencil / poly-fade / colour-fade / blend
// / poly-alpha) is 23-4, added a flag at a time. All six are applied now: SHADE, stencil (bit5), poly-fade
// (bit3), colour-fade (bit2), and — the two that read the framebuffer — blend (bit1) and poly-alpha
// (bit0). blend/poly-alpha are NOT done with a dst read in the shader: this fragment emits a per-pixel
// weight a in out_color.a and the fixed-function blend unit does src*a + dst*(1-a) over the painter's
// pass (already back-to-front). See s23_geom.cpp's blend_attachment and plan_system23.md for why that
// beats an input-attachment/deferred rework on the tile GPUs this core targets.
//
// Colour order matches s22.frag / the passthrough: the attachment is B8G8R8A8_UNORM and the palette pens
// are 0x00RRGGBB, so unpack_rgb -> (R,G,B) and the shader writes vec3(R,G,B)/255 straight out.

layout(location = 0) noperspective in vec4 v_param;   // ooz, u*ooz, v*ooz, shade*ooz  (driver param[0..3])
layout(location = 1) flat          in uint v_flags;    // bit0 shade, bit1 stencil, bit2 poly-fade, bit3 colour-fade
layout(location = 2) flat          in uint v_tbase;    // texture base, added to the recovered v
layout(location = 3) flat          in uint v_peninfo;  // pens_base | (cmode << 20)
layout(location = 4) flat          in uint v_pfade;    // poly-fade:   polycolor_r | g<<8 | b<<16
layout(location = 5) flat          in uint v_cfade;    // colour-fade: fadefactor | fadecolor_r<<8 | g<<16 | b<<24
layout(location = 6) flat          in uint v_ablend;   // poly-alpha:  alpha | alpha_pen<<8 | alpha_enabled<<16

layout(location = 0) out vec4 out_color;

// Shared with s23.vert. half_size belongs to the vertex shader; the four masks are the fragment's.
layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint tileid_mask;    // the y-mask in tileid = (u>>4)&0xff | (v<<4)&tileid_mask
	uint decoded_mask;   // decoded_count - 1 (both decoded arrays)
	uint texrom_mask;    // texrom_bytes - 1
	uint pal_mask;       // palette_count - 1 (0x7fff)
} pc;

// tmrom_decoded is one u32 per tileid (the texrom base, already <<8); the other three are byte- or
// byte-in-word packed, unpacked here the same way s22.frag unpacks its arrays.
layout(std430, set = 0, binding = 0) readonly buffer tmrom_block   { uint tmrom[];   };  // tileid -> base
layout(std430, set = 0, binding = 1) readonly buffer texattr_block { uint texattr[]; };  // tileid -> attr
layout(std430, set = 0, binding = 2) readonly buffer texrom_block  { uint texrom[];  };  // 8bpp texels
layout(std430, set = 0, binding = 3) readonly buffer pal_block     { uint palette[]; };  // 0x00RRGGBB pens
layout(std430, set = 0, binding = 4) readonly buffer texram_block  { uint texram[];  };  // C412 sram, u16 packed

const uint FLAG_SHADE     = 1u;
const uint FLAG_STENCIL   = 2u;
const uint FLAG_PFADE     = 4u;
const uint FLAG_COLORFADE = 8u;
const uint FLAG_BLEND     = 16u;
const uint FLAG_POLYALPHA = 32u;

uint texattr_at(uint i) { i &= pc.decoded_mask; return (texattr[i >> 2u] >> ((i & 3u) << 3u)) & 0xffu; }
uint texrom_at(uint i)  { i &= pc.texrom_mask;  return (texrom[i >> 2u]  >> ((i & 3u) << 3u)) & 0xffu; }

// namcos23_renderer::stencil_lookup(x, y): read one bit out of the C412 sram (u16). Returns true when the
// bit is CLEAR — render_scanline skips (does not draw) a stencil_enabled pixel when this is true. x,y are
// the pre-tbase truncated texel coords. texram is bound as u32; index the u16 out of the word.
bool stencil_cut(uint x, uint y)
{
	const uint bit  = (x & 15u) ^ 15u;
	const uint offs = ((y << 6u) | (x >> 4u)) & 0x1ffffu;
	const uint word = texram[offs >> 1u];
	const uint val16 = (word >> ((offs & 1u) << 4u)) & 0xffffu;
	return ((val16 >> bit) & 1u) == 0u;
}

// namcos23_renderer::texture_lookup, returning the RAW pen byte (the cmode resolve is applied by the
// caller). u,v are the truncated perspective-correct texel coordinates (v already has tbase added).
uint texel_pen(uint u, uint v)
{
	const uint tileid = ((u >> 4u) & 0xffu) | ((v << 4u) & pc.tileid_mask);
	const uint tile = tmrom[tileid & pc.decoded_mask];
	const uint attr = texattr_at(tileid);
	if ((attr & 1u) != 0u) v = ~v;
	if ((attr & 2u) != 0u) u = ~u;
	if ((attr & 4u) != 0u) { const uint t = u; u = v; v = t; }
	return texrom_at(tile | ((v << 4u) & 0xf0u) | (u & 0x0fu));
}

void main()
{
	const float ooz  = v_param.x;                 // driver param[0]
	const float rooz = 1.0 / ooz;                 // render_scanline's ooz = 1/z

	// u32(u*ooz) / u32(v*ooz) — truncate toward zero (positive on-screen). tbase is added to v AFTER the
	// stencil test, which render_scanline runs on the pre-tbase coords (tx, ty).
	const uint tx = uint(int(v_param.y * rooz));
	const uint ty = uint(int(v_param.z * rooz));

	// The stencil cutout (render_hash bit5): a stencil_enabled poly skips any pixel the C412 sram masks off.
	if ((v_flags & FLAG_STENCIL) != 0u && stencil_cut(tx, ty))
		discard;

	const uint pen = texel_pen(tx, ty + v_tbase);

	// The cmode pen resolve (render_scanline, before the scanline loop): shift/mask the pen and offset the
	// palette base. cmode is three bits, so cmode&8 is always 0 — kept for fidelity with the driver.
	const uint cmode     = v_peninfo >> 20u;
	uint       base      = v_peninfo & 0xfffffu;
	uint       penmask   = 0xffu;
	uint       penshift  = 0u;
	if ((cmode & 4u) != 0u)
	{
		base += 0xecu + ((cmode & 8u) << 1u);
		penmask = 0x03u;
		penshift = 2u * ((~cmode) & 3u);
	}
	else if ((cmode & 2u) != 0u)
	{
		base += 0xe0u + ((cmode & 8u) << 1u);
		penmask = 0x0fu;
		penshift = 4u * ((~cmode) & 1u);
	}

	const uint rgb = palette[(base + ((pen >> penshift) & penmask)) & pc.pal_mask];
	int r = int((rgb >> 16u) & 0xffu);
	int g = int((rgb >> 8u) & 0xffu);
	int b = int(rgb & 0xffu);

	// The per-pixel SHADE step: shade = clamp(param3/param0, 0, 63); c = (c * shade) >> 6. A poly with
	// shade disabled draws the texel unshaded (full brightness), matching render_scanline's !Shade path.
	if ((v_flags & FLAG_SHADE) != 0u)
	{
		const int shade = clamp(int(v_param.w * rooz), 0, 63);
		r = (r * shade) >> 6;
		g = (g * shade) >> 6;
		b = (b * shade) >> 6;
	}

	// Poly-fade (render_hash bit3): a per-poly colour multiply. c = (c * polycolor) >> 8.
	if ((v_flags & FLAG_PFADE) != 0u)
	{
		r = (r * int(v_pfade & 0xffu)) >> 8;
		g = (g * int((v_pfade >> 8u) & 0xffu)) >> 8;
		b = (b * int((v_pfade >> 16u) & 0xffu)) >> 8;
	}

	// Colour-fade (render_hash bit2): a lerp toward fadecolor by fadefactor. The driver keeps
	// fadefactor_inv = 0x100 - fadefactor; recover it here. c = (c*fadefactor + fadecolor*inv) >> 8.
	if ((v_flags & FLAG_COLORFADE) != 0u)
	{
		const int ff  = int(v_cfade & 0xffu);
		const int inv = 0x100 - ff;
		r = ((r * ff) + (int((v_cfade >> 8u)  & 0xffu) * inv)) >> 8;
		g = ((g * ff) + (int((v_cfade >> 16u) & 0xffu) * inv)) >> 8;
		b = ((b * ff) + (int((v_cfade >> 24u) & 0xffu) * inv)) >> 8;
	}

	// Blend (bit1) and poly-alpha (bit0): emit the per-pixel SRC weight a for the fixed-function blend unit
	// (src*a + dst*(1-a)); the pass is back-to-front, so this reproduces render_scanline's dst read. Match
	// its priority exactly — poly-alpha wins over blend where its per-texel gate passes, else blend's 50%:
	//   if (PolyAlpha && (alpha_enabled || pen == alpha_pen))  a = alpha/256   (alpha = 0xff - poly_alpha)
	//   else if (Blend)                                        a = 0x80/256    (fixed 50%)
	//   else                                                   a = 1.0         (opaque, exact under a=1)
	// pen here is the RAW texel byte (texel_pen's return), the same value render_scanline gates on. a=alpha/256
	// keeps software's >>8 (÷256) basis on BOTH terms; only unorm rounding differs (<1 LSB). alpha is never
	// 0xff when FLAG_POLYALPHA is set (poly_alpha = alpha != 0xff), so a=1.0 stays reserved for true opacity.
	float a = 1.0;
	const bool polyalpha_here = ((v_flags & FLAG_POLYALPHA) != 0u)
			&& (((v_ablend >> 16u) & 1u) != 0u || pen == ((v_ablend >> 8u) & 0xffu));
	if (polyalpha_here)
		a = float(v_ablend & 0xffu) / 256.0;
	else if ((v_flags & FLAG_BLEND) != 0u)
		a = 128.0 / 256.0;

	out_color = vec4(vec3(r, g, b) / 255.0, a);
}
