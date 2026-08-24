#version 450

// Namco System 21 libretro core — flat untextured fragment (T2).
//
// The whole fragment is a CLUT lookup: the flat per-quad pen indexes the palette the driver handed the
// seam (m_palette->pens(), 0x00RRGGBB), and that colour is emitted opaque. There is no texture, no
// per-pixel shade, no fog — S21 is a flat-shaded rasteriser and the per-quad depth cue is already baked
// into the pen index on the CPU. The palette is re-uploaded each frame because the game writes palette
// RAM live.

layout(location = 0) flat in uint v_pen;

layout(location = 0) out vec4 out_color;

// The palette CLUT: one 0x00RRGGBB word per pen. std430 so the array length is exact.
layout(std430, binding = 0) readonly buffer Palette
{
	uint pens[];
};

void main()
{
	uint pen = v_pen;
	uint rgb = (pen < pens.length()) ? pens[pen] : 0u;

	float r = float((rgb >> 16) & 0xffu) / 255.0;
	float g = float((rgb >>  8) & 0xffu) / 255.0;
	float b = float( rgb        & 0xffu) / 255.0;
	out_color = vec4(r, g, b, 1.0);
}
