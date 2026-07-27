#version 450

// Model 2 libretro core — the lightgun reticle, drawn over the finished picture.
//
// The same fullscreen triangle as every other 2D draw in this renderer, scissored down to the
// cross's bounding box by the caller so that the fragments actually shaded are the ~400 in that box
// rather than the whole picture. It samples nothing: the cross is generated from the four numbers in
// the push block, which are m2vk::RETICLE_SHAPE and are the only copy of them — there is no bitmap
// and no texture, which is the point (devnotes/lightgun.md §1.7).
//
// ⚠️ covers() below is m2vk::reticle_covers() in src/osd/libretro_m2/m2vk_reticle.h, written a second
// time because GLSL cannot include it. The CONSTANTS are shared — they arrive in the push block — but
// this expression is duplicated, so a change to one has to be made to the other. The failure it
// produces is the software and Vulkan paths disagreeing about a shape that is on screen the whole
// time, which is the cheapest kind to notice and the reason the duplication is tolerable.
//
// gl_FragCoord is in ATTACHMENT pixels, which under M2VK_SS is scale times the picture. Dividing by
// the scale puts the whole test back into picture pixels, so one set of geometry constants serves
// every internal resolution and the reticle grows with the picture instead of shrinking into it.
//
// No blending: the cross replaces what is under it and everything else discards, which is exactly
// what the software path's blit does. A translucent reticle would look better and would have to be
// matched by an alpha blend in the CPU blitter against pixels it cannot see (it writes into the
// foreground layer, not the composite), so the two paths would stop producing the same pixels.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Push
{
	vec2  centre;       // in picture pixels
	float scale;        // M2VK_SS
	float half_thick;
	float gap;
	float arm;
	float outline;
	uint  colour;       // 0x00RRGGBB
	uint  outline_colour;
} pc;

bool covers(vec2 d, float grow)
{
	float t = pc.half_thick + grow;
	float g = pc.gap - grow;
	float a = pc.arm + grow;
	return ((d.y <= t) && (d.x >= g) && (d.x <= a))
			|| ((d.x <= t) && (d.y >= g) && (d.y <= a));
}

vec3 unpack(uint rgb)
{
	return vec3(float((rgb >> 16) & 0xffu), float((rgb >> 8) & 0xffu), float(rgb & 0xffu)) / 255.0;
}

void main()
{
	vec2 d = abs((gl_FragCoord.xy / pc.scale) - pc.centre);

	if (covers(d, 0.0))
		out_colour = vec4(unpack(pc.colour), 1.0);
	else if (covers(d, pc.outline))
		out_colour = vec4(unpack(pc.outline_colour), 1.0);
	else
		discard;
}
