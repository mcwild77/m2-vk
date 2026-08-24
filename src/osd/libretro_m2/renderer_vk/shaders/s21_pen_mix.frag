#version 450

// Namco System 21 libretro core — the layer-0 C355 z-mix in PEN space (option B; was s21_mix.frag).
//
// Reproduces namcos21_c67_state::mix_layer0_sprites against the real polygon z-buffer, but writes a
// palette pen INDEX rather than a resolved colour, so it feeds the same pen composite the OVER shadow
// then reads. The driver (namcos21_c67.cpp capture_mix_sprites, s21_seam.h capture_mix) has already
// decided, per pixel, whether the layer-0 sprite is untouched (tag 0, discarded), an unconditional
// show (tag 255 — the software `src[x] < 0x1000` branch) or gated on a priority bank (tag 1..16, bank
// = tag-1 — the `pri[bank] <= z[x]` branch); what is left for the GPU is the depth comparison, because
// only the GPU still has the z-buffer once the software rasteriser is off.
//
// The priority table is mix_layer0_sprites' `pri[i] = i==0 ? 0x7fc0 : pri[i-1]/1.24`, recomputed here
// so the two copies of the recurrence sit textually next to each other. gl_FragDepth is the sprite's
// threshold, not a rasterised position; the pipeline tests GREATER_OR_EQUAL against the polygon pass's
// depth (LOAD, not cleared) and writes no depth back, mirroring `pri[bank] <= z[x]` exactly.

layout(location = 0) in vec2 v_uv;

layout(location = 0) out uint o_pen;

layout(push_constant) uniform Push
{
	uint width, height;
} pc;

// Packed by s21_seam.h capture_mix: tag in the top byte, the pen index in the low bits.
layout(std430, binding = 0) readonly buffer MixBuf
{
	uint pixels[];
};

void main()
{
	ivec2 coord = ivec2(v_uv * vec2(pc.width, pc.height));
	coord = clamp(coord, ivec2(0), ivec2(int(pc.width) - 1, int(pc.height) - 1));
	uint index = uint(coord.y) * pc.width + uint(coord.x);
	uint texel = (index < pixels.length()) ? pixels[index] : 0u;

	uint tag = texel >> 24;
	if (tag == 0u)
		discard;

	float z;
	if (tag == 255u)
	{
		z = 1.0;
	}
	else
	{
		uint bank = tag - 1u;
		float pri = 32704.0; // 0x7fc0, i==0
		for (uint i = 0u; i < bank; i++)
			pri /= 1.24;
		z = 1.0 - pri / 32768.0;
	}

	gl_FragDepth = z;
	o_pen = texel & 0x00ffffffu;
}
