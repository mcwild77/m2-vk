#version 450

// Namco System 21 libretro core — the layer-0 C355 z-mix (T2b).
//
// Reproduces namcos21_c67_state::mix_layer0_sprites on the GPU, against the REAL depth buffer T2a
// gives S21: the driver already decided, per pixel, whether it is untouched (tag 0, discarded here),
// an unconditional show (tag 255 — the software `src[x] < 0x1000` branch), or gated on a priority bank
// (tag 1..16, bank = tag-1 — the software `pri[bank] <= z[x]` branch). What is left for the GPU is the
// depth comparison itself, because only the GPU still has the z-buffer once T2a turns the software
// rasteriser off (s21_seam.h capture_mix, namcos21_c67.cpp capture_mix_sprites).
//
// The priority table is namcos21_c67_state::mix_layer0_sprites' `pri[i] = i==0 ? 0x7fc0 : pri[i-1]/1.24`,
// recomputed here rather than passed in: 16 divides is cheaper than a buffer/push-constant array and
// keeps the two copies of the recurrence textually next to each other for whoever next has to check
// they still agree.
//
// gl_FragDepth is the sprite's threshold, not a rasterised position — this pass has no geometry, only a
// fullscreen triangle (fullscreen.vert) and a per-pixel decision. Writing it disables early depth
// testing for this pipeline, which is fine: there is exactly one draw. The pipeline this runs on tests
// GREATER_OR_EQUAL against the polygon pass's depth attachment (already written, LOAD not CLEARed) and
// writes nothing back, so it neither disturbs the 3D's own depth nor is order-sensitive against it.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Push
{
	uint width, height;
} pc;

// Packed exactly as s21_seam.h's capture_mix leaves it: tag in the top byte, 0x00RRGGBB below.
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
	out_colour = vec4(float((texel >> 16) & 0xffu) / 255.0,
			float((texel >> 8) & 0xffu) / 255.0,
			float(texel & 0xffu) / 255.0, 1.0);
}
