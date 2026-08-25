#version 450

// Namco System 22 libretro core — the polygon fragment (S2 shading tail: fog, fade, poly-alpha).
//
// This is namcos22_renderer::renderscanline_poly (plain System 22) and renderscanline_poly_ss22
// (Super System 22) from src/mame/namco/namcos22_v.cpp, the per-pixel shading tail, transliterated.
// Where a line looks gratuitously literal — a mask that provably does nothing, a >>8 where a divide by
// 255 would read cleaner — it is literal because the software rasteriser is the reference and matching
// its integer arithmetic bit-for-bit is the job. rgbaint_t::blend is (a*f + b*(256-f)) >> 8 and
// scale_*_and_clamp is clamp((c*s) >> 8, 0, 255); both are reproduced here in integer.
//
// The two paths differ in ORDER: plain System 22 fogs BEFORE it shades; Super System 22 shades first,
// then fogs (per-z from the czram table, or direct), then poly-fade, then screen-fade, then a per-pixel
// destination alpha blend. The alpha blend against the framebuffer is the one effect not done here: the
// fragment emits its alpha weight and fixed-function SRC_ALPHA/ONE_MINUS_SRC_ALPHA blending does the
// mix (see s22_geom.cpp) — that step is float UNORM, not integer >>8, so an alpha pixel carries a small
// rounding residual, expected for the SS22 tail. Screen fade, poly fade, alpha factor and alpha pen are
// per-frame globals on the push constant; the four z-fog tables are a storage buffer; everything else
// is per quad on the vertex.
//
// The texture system is uploaded as raw storage buffers exactly as it sits in the driver's arrays — no
// decode, no atlas — and every index is masked to its buffer so a coordinate the software renderer
// would read past the end of stays in bounds here instead.

layout(location = 0) noperspective in vec3  v_uvw;   // (u+0.5)*ooz, (v+0.5)*ooz, ooz  (driver param[1,2,0])
layout(location = 1) noperspective in float v_iw;    // (bri+0.5)*ooz                   (driver param[3])
layout(location = 2) flat          in uint  v_attr;  // flags | color<<8 | cmode<<16
layout(location = 3) flat          in uint  v_bn;    // texturebank
layout(location = 4) flat          in uint  v_base;  // untextured fill, 0x00RRGGBB
layout(location = 5) flat          in uint  v_sf0;   // fogfactor | cz_bank<<8 | zfog<<10 | alpha_en<<11 | ss22<<12 | (sdelta+256)<<13
layout(location = 6) flat          in uint  v_sf1;   // fog colour, 0x00RRGGBB

layout(location = 0) out vec4 o_color;

// Shared with s22.vert. half_size belongs to the vertex shader; the rest are the SS22 tail's per-frame
// globals read below.
layout(push_constant) uniform push_block
{
	vec2 half_size;
	uint alpha_pen;
	uint alpha_factor;
	uint fade_factor;
	uint fade_r, fade_g, fade_b;
	uint poly_flags;
	uint poly_r, poly_g, poly_b;
	uint tex_filter;   // 0 = point sample (hardware); 1 = bilinear on the 3D texture tail (enhancement)
	float depth_scale; // s22.vert only (depth-buffer mode); present here so the shared layout matches
	float depth_bias;  // s22.vert only
} pc;

const uint PFLAG_POLY_FADE = 1u;
// Debug/enhancement view toggles, ORed into poly_flags per frame by draw_batches() from the S22 core
// options (see s22_geom.cpp). Global for the frame, unlike the per-poly flags in v_attr.
const uint PFLAG_NO_FOG   = 2u;   // system22_fog=off: skip every fog/z-fog blend (poly and sprite)
const uint PFLAG_NO_TEX   = 4u;   // system22_no_textures: force the surface white, so shade renders it greyscale
const uint PFLAG_NO_LIGHT = 8u;   // model2_flat_luma (No Lighting): skip the per-pixel shade, full brightness

// The tile system, byte- and halfword-packed into uint words: on little-endian hardware byte i of the
// buffer is raw byte i, so the upload is a plain memcpy and the unpack lives here.
layout(std430, set = 0, binding = 0) readonly buffer ttdata_block { uint ttdata[]; };  // 8bpp tile pixels
layout(std430, set = 0, binding = 1) readonly buffer ttattr_block { uint ttattr[]; };  // per-tile orientation
layout(std430, set = 0, binding = 2) readonly buffer ttmap_block  { uint ttmap[];  };  // 16-bit tile numbers
layout(std430, set = 0, binding = 3) readonly buffer ayx_block    { uint ayx[];    };  // attr/y/x -> pixel
layout(std430, set = 0, binding = 4) readonly buffer pal_block    { uint palette[];};  // 0x00RRGGBB pens
layout(std430, set = 0, binding = 5) readonly buffer cz_block     { uint czram[];  };  // 4 z-fog banks, 0x2000 each
layout(std430, set = 0, binding = 6) readonly buffer gamma_block  { uint gamma[];  };  // plain-S22 final gamma: rlut|glut|blut
layout(std430, set = 0, binding = 7) readonly buffer sprite_block { uint sprgfx[]; };  // gfx(2): 8bpp 32x32 sprite tiles

const uint FLAG_TEXTURED = 1u;
const uint FLAG_SHADE    = 2u;
const uint FLAG_SPRITE   = 4u;    // this vertex is a sprite tile — take the sprite fetch below
const uint FLAG_SFLIPX   = 8u;    // sprite: the one-texel x sampling shift
const uint FLAG_SFLIPY   = 16u;   // sprite: the one-texel y sampling shift

const uint SF0_ZFOG     = 1u << 10;
const uint SF0_ALPHA_EN = 1u << 11;
const uint SF0_SS22     = 1u << 12;

// Sprite sf0 (read only under FLAG_SPRITE): fogfactor | fadefactor<<8 | alpha<<16 | flags.
const uint SSF_ALPHA_EN = 1u << 24;

// Sprite gfx layout: 32 bytes per tile row (line_modulo), 0x1000000 bytes total on Super System 22.
const uint SPRITE_ROW  = 32u;
const uint SPRITE_MASK = 0xffffffu;

// Buffer-size masks (entries - 1). Fixed by the hardware; see s22_geom.cpp for the sizes.
const uint TTDATA_MASK = 0xffffffu;   // 0x1000000 bytes
const uint TTATTR_MASK = 0xfffffu;    // 0x100000 bytes
const uint TTMAP_MASK  = 0xfffffu;    // 0x100000 halfwords
const uint AYX_MASK    = 0xfffu;      // 0x1000 bytes
const uint PAL_MASK    = 0x7fffu;     // 0x8000 entries
const uint CZRAM_MASK  = 0x7fffu;     // 0x8000 bytes (4 banks of 0x2000)

uint ttdata_at(uint i) { i &= TTDATA_MASK; return (ttdata[i >> 2u] >> ((i & 3u) << 3u)) & 0xffu; }
uint ttattr_at(uint i) { i &= TTATTR_MASK; return (ttattr[i >> 2u] >> ((i & 3u) << 3u)) & 0xffu; }
uint ayx_at(uint i)    { i &= AYX_MASK;    return (ayx[i >> 2u]    >> ((i & 3u) << 3u)) & 0xffu; }
uint ttmap_at(uint i)  { i &= TTMAP_MASK;  return (ttmap[i >> 1u]  >> ((i & 1u) << 4u)) & 0xffffu; }
uint czram_at(uint i)  { i &= CZRAM_MASK;  return (czram[i >> 2u]  >> ((i & 3u) << 3u)) & 0xffu; }
uint gamma_at(uint i)  { i &= 0x3ffu;      return (gamma[i >> 2u]  >> ((i & 3u) << 3u)) & 0xffu; }
uint sprite_at(uint i) { i &= SPRITE_MASK; return (sprgfx[i >> 2u] >> ((i & 3u) << 3u)) & 0xffu; }

ivec3 unpack_rgb(uint p) { return ivec3(int((p >> 16u) & 0xffu), int((p >> 8u) & 0xffu), int(p & 0xffu)); }

// rgbaint_t::blend(other, factor): (this*factor + other*(256-factor)) >> 8, per channel, no clamp.
ivec3 mame_blend(ivec3 a, ivec3 b, int f) { return (a * f + b * (256 - f)) >> 8; }

// rgbaint_t::scale_imm_and_clamp / scale_and_clamp: clamp((c * scale) >> 8, 0, 255).
ivec3 mame_scale_imm(ivec3 c, int s)  { return clamp((c * s) >> 8, 0, 255); }
ivec3 mame_scale(ivec3 c, ivec3 s)    { return clamp((c * s) >> 8, 0, 255); }

// One texel of renderscanline_poly's fetch, factored so the point tap and the four bilinear taps share
// one body. tx wraps to 12 bits; ty takes its low 12 bits and then the texturebank (v_bn), exactly as
// the inline fetch did. Returns the RAW ttdata pen byte — what the alpha test compares against.
uint texel_pen(int tx, int ty)
{
	tx &= 0xfff;
	ty = (ty & 0xfff) | int(v_bn << 12u);
	const int to = ((ty << 4) & 0xfff00) | (tx >> 4);
	const uint tile  = ttmap_at(uint(to));
	const uint attrb = ttattr_at(uint(to));
	const uint inner = ayx_at((attrb << 8u) | uint(((ty << 4) & 0xf0) | (tx & 0xf)));
	return ttdata_at((tile << 8u) | inner);
}

// The cmode pen resolve: raw pen -> palette RGB. pens_base/mask/shift are resolved once per poly.
ivec3 pen_to_rgb(uint pen, uint pens_base, uint penmask, uint penshift)
{
	return unpack_rgb(palette[(pens_base + ((pen >> penshift) & penmask)) & PAL_MASK]);
}

void main()
{
	const uint attr = v_attr;

	// The frame-global debug toggles. no_fog is scene-wide (it also silences sprite fog below); no_tex
	// and no_light are polygon-shading concepts and do not touch the sprite path.
	const bool no_fog   = (pc.poly_flags & PFLAG_NO_FOG)   != 0u;
	const bool no_tex   = (pc.poly_flags & PFLAG_NO_TEX)   != 0u;
	const bool no_light = (pc.poly_flags & PFLAG_NO_LIGHT) != 0u;

	// Sprite tiles (Super System 22): renderscanline_sprite's fetch — a screen-aligned affine textured
	// quad, no perspective and no shade. u/v ride v_uvw.xy interpolated linearly (ooz = 1); the pens base
	// is (color & 0x7f) << 8 like the poly path; the tile byte offset is v_bn; fog/fade/alpha ride sf0
	// (own layout) and the fade colour rides v_base. Handled here and returned; the poly tail is untouched.
	if ((attr & FLAG_SPRITE) != 0u)
	{
		const uint scolor = (attr >> 8u) & 0x7fu;
		const int  flipx = ((attr & FLAG_SFLIPX) != 0u) ? 1 : 0;
		const int  flipy = ((attr & FLAG_SFLIPY) != 0u) ? 1 : 0;

		const int lu = int(v_uvw.x) - flipx;   // (int)x_index - flipx
		const int lv = int(v_uvw.y) - flipy;   // (int)y_index - flipy
		const uint pen = sprite_at(v_bn + uint(lv) * SPRITE_ROW + uint(lu));
		if (pen == 0xffu)                       // 0xff is the sprite transparent pen
			discard;

		ivec3 srgb = unpack_rgb(palette[((scolor << 8u) + pen) & PAL_MASK]);

		const uint  s0 = v_sf0;
		const int   sfog  = int(s0 & 0xffu);
		const int   sfade = int((s0 >> 8u) & 0xffu);
		const int   salpha = int((s0 >> 16u) & 0xffu);
		const bool  salpha_en = (s0 & SSF_ALPHA_EN) != 0u;
		const ivec3 sfogcolor  = unpack_rgb(v_sf1);
		const ivec3 sfadecolor = unpack_rgb(v_base);

		// fog, then fade, then a per-pixel destination alpha blend — the order renderscanline_sprite uses.
		const int fog = 255 - sfog;
		if (fog != 255 && !no_fog)
			srgb = mame_blend(srgb, sfogcolor, fog);
		const int fadef = 255 - sfade;
		if (fadef != 255)
			srgb = mame_blend(srgb, sfadecolor, fadef);

		float sa = 1.0;
		if (salpha != 255 && (salpha_en || pen == pc.alpha_pen))
			sa = float(salpha) / 255.0;   // blend weight = extra.alpha (see s22_geom.cpp)

		srgb = clamp(srgb, 0, 255);
		// SS22 final gamma (^3), the last op the mixer applies to every output pixel.
		srgb = ivec3(int(gamma_at(         (uint(srgb.r) ^ 3u))),
		             int(gamma_at(0x100u + (uint(srgb.g) ^ 3u))),
		             int(gamma_at(0x200u + (uint(srgb.b) ^ 3u))));
		o_color = vec4(vec3(srgb) / 255.0, sa);
		return;
	}

	const bool textured = (attr & FLAG_TEXTURED) != 0u;
	const bool shade_enabled = (attr & FLAG_SHADE) != 0u;
	const uint color = (attr >> 8u) & 0x7fu;
	const uint cmode = (attr >> 16u) & 0xffu;

	const uint  sf0 = v_sf0;
	const int   fogfactor = int(sf0 & 0xffu);
	const uint  cz_bank   = (sf0 >> 8u) & 3u;
	const bool  zfog      = (sf0 & SF0_ZFOG) != 0u;
	const bool  alpha_en  = (sf0 & SF0_ALPHA_EN) != 0u;
	const bool  ss22      = (sf0 & SF0_SS22) != 0u;
	const int   cz_sdelta = int((sf0 >> 13u) & 0x1ffu) - 256;
	const ivec3 fogcolor  = unpack_rgb(v_sf1);

	// The driver names the interpolated param[0] "z" and its reciprocal "ooz"; ooz is the true view
	// depth that recovers a perspective-correct u,v,shade from the screen-linear varyings.
	const float z = v_uvw.z;
	const float ooz = 1.0 / z;

	// The texel fetch (renderscanline_poly's), and the raw pen the alpha test compares against.
	uint pen = 0u;
	ivec3 rgb;
	if (!textured)
	{
		rgb = unpack_rgb(v_base);
	}
	else
	{
		const float fu = v_uvw.x * ooz;
		const float fv = v_uvw.y * ooz;

		// pens base / mask / shift from cmode, exactly as the scanline renderer resolves them once per poly.
		uint pens_base = color << 8u;
		uint penmask = 0xffu;
		uint penshift = 0u;
		if ((cmode & 4u) != 0u)
		{
			pens_base += 0xecu + ((cmode & 8u) << 1u);
			penmask = 0x03u;
			penshift = 2u * ((~cmode) & 3u);
		}
		else if ((cmode & 2u) != 0u)
		{
			pens_base += 0xe0u + ((cmode & 8u) << 1u);
			penmask = 0x0fu;
			penshift = 4u * ((~cmode) & 1u);
		}

		// The alpha test (below) is always on the point-sampled centre pen, so the cutout/alpha-pen shape
		// is identical whether or not filtering is on. Only the COLOUR is filtered.
		pen = texel_pen(int(fu), int(fv));

		if (pc.tex_filter == 0u)
		{
			// Hardware: point sample. Bit-identical to the pre-filter path.
			rgb = pen_to_rgb(pen, pens_base, penmask, penshift);
		}
		else
		{
			// Enhancement (System 22 had no texture filter): a 4-tap bilinear blend in RGB space, AFTER
			// the palette lookup — the pens are indices, so the four neighbours must each resolve to a
			// colour before they can be averaged. Texel centres sit at integer coords (the point tap rounds
			// to nearest), so the sample point is fu-0.5 and its two bracketing texels are floor()/+1.
			const float pu = fu - 0.5;
			const float pv = fv - 0.5;
			const int   u0 = int(floor(pu));
			const int   v0 = int(floor(pv));
			const float wx = pu - float(u0);
			const float wy = pv - float(v0);

			const vec3 c00 = vec3(pen_to_rgb(texel_pen(u0,     v0    ), pens_base, penmask, penshift));
			const vec3 c10 = vec3(pen_to_rgb(texel_pen(u0 + 1, v0    ), pens_base, penmask, penshift));
			const vec3 c01 = vec3(pen_to_rgb(texel_pen(u0,     v0 + 1), pens_base, penmask, penshift));
			const vec3 c11 = vec3(pen_to_rgb(texel_pen(u0 + 1, v0 + 1), pens_base, penmask, penshift));

			rgb = ivec3(round(mix(mix(c00, c10, wx), mix(c01, c11, wx), wy)));
		}
	}

	// system22_no_textures: replace the sampled/base colour with white so the shade below renders the
	// surface as a pure greyscale lit view. The texel fetch above still ran, so the alpha-pen cutout and
	// translucency test are unchanged — only the colour is whitewashed. On S22 shading is luma-only (no
	// coloured lights), so white*shade is genuinely greyscale.
	if (no_tex)
		rgb = ivec3(255);

	const int shade = int(v_iw * ooz) << 2;

	float out_alpha = 1.0;

	if (!ss22)
	{
		// plain System 22 (renderscanline_poly): fog BEFORE shade.
		const int fog = 255 - fogfactor;
		if (fog != 255 && !no_fog)
			rgb = mame_blend(rgb, fogcolor, fog);
		if (shade_enabled && !no_light)
			rgb = mame_scale_imm(rgb, shade);
	}
	else
	{
		// Super System 22 (renderscanline_poly_ss22): shade, fog, poly-fade, screen-fade, alpha.
		if (shade_enabled && !no_light)
			rgb = mame_scale_imm(rgb, shade);

		if (no_fog)
		{
			// system22_fog=off: skip both the per-z and direct fog blends.
		}
		else if (zfog)
		{
			// per-z fog: discard the low byte, clamp to 0..0x1fff, look up the czram table for this bank.
			int cz = int(ooz) >> 8;
			if (cz > 0x1fff) cz = 0x1fff;
			int ff = int(czram_at(cz_bank * 0x2000u + uint(cz))) + cz_sdelta;
			if (ff > 0)
			{
				if (ff > 0xff) ff = 0xff;
				rgb = mame_blend(rgb, fogcolor, 255 - ff);
			}
		}
		else
		{
			const int fog = 255 - fogfactor;   // direct
			if (fog != 255)
				rgb = mame_blend(rgb, fogcolor, fog);
		}

		// poly fade (scale by the per-frame poly colour), then screen fade (blend toward it).
		if ((pc.poly_flags & PFLAG_POLY_FADE) != 0u)
			rgb = mame_scale(rgb, ivec3(int(pc.poly_r), int(pc.poly_g), int(pc.poly_b)));

		const int fadef = 255 - int(pc.fade_factor);
		if (fadef != 255)
			rgb = mame_blend(rgb, ivec3(int(pc.fade_r), int(pc.fade_g), int(pc.fade_b)), fadef);

		// alpha: a per-pixel destination blend when this colour alpha-blends or the pen is the alpha pen.
		const int alocal = 255 - int(pc.alpha_factor);
		if (alocal != 255 && (alpha_en || pen == pc.alpha_pen))
			out_alpha = float(alocal) / 255.0;

		// blend() does not clamp; the UNORM attachment would, so clamp here to keep the stored value exact.
		rgb = clamp(rgb, 0, 255);
	}

	// Final gamma LUT — the last op on every output pixel in both mixers (namcos22_mix_text_layer for
	// plain S22, screen_update_namcos22s's post-pass for SS22). This is why the GPU 3D read ~half as
	// bright before: the software path brightens through this LUT and the GPU did not. Plain S22's LUT
	// is a static PROM indexed directly; SS22's lives in mixer RAM as u32 words, so the byte index is
	// swapped (^3 on little-endian), matching the driver's NATIVE_ENDIAN_VALUE_LE_BE(3,0). For an alpha
	// pixel the fixed-function blend then happens in gamma space rather than before gamma — a small
	// residual, accepted for the SS22 tail.
	rgb = clamp(rgb, 0, 255);
	uint gx = ss22 ? 3u : 0u;
	rgb = ivec3(int(gamma_at(         (uint(rgb.r) ^ gx))),
	            int(gamma_at(0x100u + (uint(rgb.g) ^ gx))),
	            int(gamma_at(0x200u + (uint(rgb.b) ^ gx))));

	o_color = vec4(vec3(rgb) / 255.0, out_alpha);
}
