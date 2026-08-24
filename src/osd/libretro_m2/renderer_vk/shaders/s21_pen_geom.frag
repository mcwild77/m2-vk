#version 450

// Namco System 21 libretro core — the polygon pass in PEN space (option B composite).
//
// The whole S21 frame is composited as palette pen INDICES, not RGB, so the C355 palette-shadow
// sprites in the OVER band can index the polygon-blend banks (1/2) by the pen beneath them — the one
// thing an RGB composite throws away. This is the 3D slice of that composite: each flat-shaded quad
// writes its resolved pen straight into the R16_UINT pen attachment, depth-tested against the real
// per-quad z-buffer (s21.vert maps zsort to z). No CLUT here — s21_finish.frag resolves once, at the
// end, after the OVER shadow has had its say.

layout(location = 0) flat in uint v_pen;

layout(location = 0) out uint o_pen;

void main()
{
	o_pen = v_pen;
}
