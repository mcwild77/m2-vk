#version 450

// Namco System 21 libretro core — the 2D-UNDER slice of the pen-space composite (option B).
//
// Lays down the 2D background as palette pen INDICES: the C355 low-priority sprite band and the
// backdrop, exactly the bitmap MAME's screen_update has drawn before the 3D, captured by the driver
// (s21_seam.h capture_under) as raw pens. A fullscreen triangle (fullscreen.vert) point-samples that
// native-resolution pen buffer into the pen attachment, which may be larger under M2VK_SS or a raised
// internal resolution — the 3D pass then draws over it, depth-tested, and s21_finish.frag resolves the
// lot to RGB in one pass. Writes no depth: the attachment is cleared to 0 so any polygon (z > 0) wins.

layout(location = 0) in vec2 v_uv;

layout(location = 0) out uint o_pen;

layout(push_constant) uniform Push
{
	uint width, height;   // the pen buffer's native dimensions
} pc;

layout(std430, binding = 0) readonly buffer UnderBuf
{
	uint pixels[];        // one pen index per native pixel
};

void main()
{
	ivec2 c = ivec2(v_uv * vec2(pc.width, pc.height));
	c = clamp(c, ivec2(0), ivec2(int(pc.width) - 1, int(pc.height) - 1));
	uint idx = uint(c.y) * pc.width + uint(c.x);
	o_pen = (idx < pixels.length()) ? pixels[idx] : 0u;
}
