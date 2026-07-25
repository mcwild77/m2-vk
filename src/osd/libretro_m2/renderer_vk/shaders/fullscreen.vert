#version 450

// Model 2 libretro core — the fullscreen triangle.
//
// No vertex buffer, no index buffer, no vertex input state: the three positions are computed from
// gl_VertexIndex, and vkCmdDraw(cmd, 3, 1, 0, 0) is the whole draw. A quad would need two triangles
// and its diagonal is a real source of interpolation seams; there is nothing to gain from it.
//
// gl_VertexIndex 0,1,2 gives p = (0,0), (2,0), (0,2), so gl_Position spans (-1,-1) to (3,3) — an
// oversized triangle that covers every pixel of NDC and is clipped to it. v_uv is p, which
// interpolates to exactly 0..1 across the visible region.
//
// Vulkan's clip space has +y downwards and framebuffer row 0 is the top row, so v = 0 lands on
// texel row 0, which is MAME's top row. No flip anywhere in the chain.

layout(location = 0) out vec2 v_uv;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	v_uv = p;
	gl_Position = vec4((p * 2.0) - 1.0, 0.0, 1.0);
}
