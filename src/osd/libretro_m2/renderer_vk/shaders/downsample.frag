#version 450

// Model 2 libretro core — the supersample resolve, and it exists for a measurement rather than for
// the picture. M2VK_SS=<n> renders the whole frame into an n x oversized attachment and this brings
// it back to the visible extent, so that P4 step 2 can ask whether the depth path is
// resolution-invariant with the answer expressed in the same 496x384 PPMs everything else measures.
// P5's internal-res scaling is where this becomes a feature; it is not one yet.
//
// Two modes, and the second is the reason this is a shader rather than a linear vkCmdBlitImage:
//
//   box     the mean of the n x n subpixels. A linear blit is exactly this at n == 2 and is not at
//           n == 4 (it takes four taps however far apart they are), so the blit was never going to
//           answer the question for more than one scale.
//
//   point   the centre subpixel, discarding the rest. For ODD n the centre subpixel's centre lies
//           at exactly the 1x pixel's centre — 1x pixel x has its centre at x + 0.5, subpixel
//           n*x + (n-1)/2 at (n*x + (n-1)/2 + 0.5)/n = x + 0.5 — so the fragment shader runs at the
//           same screen positions as the 1x render and the result is comparable pixel for pixel
//           rather than only statistically. That is what turns "coverage agrees" into "the picture
//           is the same picture", which is a far stronger statement about ordering. For even n
//           there is no such subpixel and the mode is meaningless; the renderer refuses it.
//
// The 2D layers survive a box resolve exactly, which is what makes the background reference a valid
// check on this file: they are uploaded at 1x and magnified by a NEAREST sampler, so all n x n
// subpixels of a pixel hold the identical texel and their mean is that texel. A UNORM store rounds
// f * 255, and the accumulated float error over 16 taps is ~1e-7 against a quantisation step of
// 1/255, so the byte comes back unchanged.

layout(location = 0) out vec4 out_colour;

layout(set = 0, binding = 0) uniform sampler2D u_source;

layout(push_constant) uniform push_block
{
	uint scale;
	uint point_sample;
} pc;

void main()
{
	const int n = int(pc.scale);
	const ivec2 base = ivec2(gl_FragCoord.xy) * n;

	vec3 sum;

	if (pc.point_sample != 0u)
	{
		sum = texelFetch(u_source, base + ivec2((n - 1) / 2), 0).rgb;
	}
	else
	{
		sum = vec3(0.0);
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				sum += texelFetch(u_source, base + ivec2(x, y), 0).rgb;
		sum /= float(n * n);
	}

	// Alpha 1.0, as in passthrough.frag and overlay.frag: MAME's high byte is X rather than A and the
	// frontend is entitled to look at the channel.
	out_colour = vec4(sum, 1.0);
}
