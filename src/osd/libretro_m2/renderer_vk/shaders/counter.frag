#version 450

// Polygon-counter read-out. A fullscreen triangle scissored to a small box in the top-right corner,
// drawing the per-frame primitive count as decimal digits from a 3x5 bitmap font. Opaque (the present
// pipelines have blendEnable=VK_FALSE): a glyph pixel takes the foreground colour, every other pixel in
// the box takes the background, and everything outside the box is scissored away before it reaches here.
//
// The number is pre-formatted on the CPU (draw_counter in vk_present.cpp): `count` is how many digits to
// draw and `digits` packs them one per nibble, nibble 0 = the leftmost (most significant) digit.

layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Push
{
	vec2 origin;    // box top-left, in attachment pixels
	vec2 cell;      // one digit cell (w,h), attachment pixels — the box is cell.x*count by cell.y
	vec2 inset;     // margin (left,top and right,bottom) around the 3x5 glyph inside its cell
	uint count;     // number of digits, 1..8
	uint digits;    // packed BCD, nibble i = the i-th digit from the left
	uint fg;        // 0x00RRGGBB glyph colour
	uint bg;        // 0x00RRGGBB cell background
} pc;

// 3 wide x 5 tall digits, bit = row*3 + col (row 0 at top, col 0 at left). Generated to match the table
// in the build step; see the commit that added this file. Index 10 is a decimal point — a single cell at
// the bottom-centre (row 4, col 1 -> bit 13) — so the frame-rate read-out can draw "57.795".
const uint FONT[11] = uint[11](
	0x7b6fu, 0x749au, 0x73e7u, 0x79e7u, 0x49edu, 0x79cfu, 0x7bcfu, 0x24a7u, 0x7befu, 0x79efu, 0x2000u
);

vec3 unpack(uint c)
{
	return vec3(float((c >> 16) & 0xffu), float((c >> 8) & 0xffu), float(c & 0xffu)) / 255.0;
}

void main()
{
	vec2 d = gl_FragCoord.xy - pc.origin;
	float boxw = pc.cell.x * float(pc.count);
	if (d.x < 0.0 || d.y < 0.0 || d.x >= boxw || d.y >= pc.cell.y)
		discard;   // belt-and-braces; the scissor already excludes these

	uint idx = uint(d.x / pc.cell.x);                       // which digit from the left
	vec2 cin = vec2(d.x - float(idx) * pc.cell.x, d.y);     // coordinate within the cell

	vec2 g = cin - pc.inset;                                // coordinate within the glyph box
	vec2 gsize = pc.cell - 2.0 * pc.inset;                  // the glyph box size
	if (g.x >= 0.0 && g.y >= 0.0 && g.x < gsize.x && g.y < gsize.y && gsize.x > 0.0 && gsize.y > 0.0)
	{
		int gx = int(g.x / (gsize.x / 3.0));
		int gy = int(g.y / (gsize.y / 5.0));
		if (gx >= 0 && gx < 3 && gy >= 0 && gy < 5)
		{
			uint digit = (pc.digits >> (idx * 4u)) & 0xfu;
			uint bit = uint(gy * 3 + gx);
			if (digit < 11u && (FONT[digit] & (1u << bit)) != 0u)
			{
				out_colour = vec4(unpack(pc.fg), 1.0);
				return;
			}
		}
	}

	out_colour = vec4(unpack(pc.bg), 1.0);
}
