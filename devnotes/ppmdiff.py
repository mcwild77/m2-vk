#!/usr/bin/env python3
"""Compare two renderers' output over the 3D layer only.

P2's exit criterion was `cmp` and it is gone for good: a GPU rasterizer and MAME's scanline loop do
not agree on which pixels a triangle covers, and no amount of care makes them.  What replaces it is
four measurements — one that stays exact, two that stay meaningful after that, and one to look at.

Every one of them is restricted to the **covered region**, and that restriction is the whole design.
A pixel is "covered" by a renderer when it differs from the background reference, the same run with
M2VK_NO_3D=1, which both renderers produce bit-identically because neither of them touches those
pixels.  The 3D layer is 19-99 % of the picture depending on the game; the two 2D tilemap layers
around it are identical by construction, so any score computed over the whole frame is mostly a
measurement of pixels that cannot differ.  See `ssim` below for what that costs.

    ppmdiff.py coverage <background.ppm> <software.ppm> <vulkan.ppm>

        Drawn-or-not agreement, colour ignored.  Separates "the rasterizer disagrees about which
        pixels the triangle covers" from "the shading is wrong"; those two have completely different
        causes.  Expect a thin disagreement along every polygon edge and nothing else, so the report
        says what fraction of the disagreement is on an edge (has a both-covered neighbour) rather
        than only how much there is.  A *filled region* of coverage difference is a real bug.

        The one caveat, stated rather than hidden: a polygon whose colour happens to equal the
        background it covers reads as not covered, and this tool cannot tell "drew black" from "did
        not draw".  Interior disagreements are therefore split by how much the two renderings
        actually differ — one that differs by <= 8 is that artefact and does not fail the run, one
        that differs by more is a polygon somebody is missing.  Treat small coverage totals with
        suspicion, and cross-check with M2VK_FORCE_SOLID=2 (flat shading cannot land on the
        background by accident).

    ppmdiff.py exact <background.ppm> <a.ppm> <b.ppm>

        Exit criterion 1: every pixel that neither renderer's 3D touched must still be bit-exact.
        Masks off the union of the two coverages and compares the remainder.  This is the hard test
        that survives into P3 and later, and it is the one that catches compositing, crop and
        palette regressions.

    ppmdiff.py ssim <background.ppm> <a.ppm> <b.ppm>

        Exit criterion 2's number.  Standard SSIM (11x11 Gaussian, sigma 1.5, C1=(0.01*255)^2,
        C2=(0.03*255)^2), computed per RGB channel and averaged, then **reduced over the covered
        region only**.  Three means are reported and the gap between them is the point:

          whole frame   what a naive SSIM would say.  Dominated by the identical 2D layers.
          covered       mean over the union of the two coverages.  This is the headline number.
          interior      mean over covered pixels whose entire 11x11 window is also covered, i.e.
                        with no identical background leaking into the window.  Always the lowest
                        of the three, and the honest one for a small 3D layer.

        Percentiles of the covered map come with it, because a mean of 0.97 made of a uniformly
        slightly-wrong picture and one made of a perfect picture with a broken object in it are the
        same number and not the same bug.

    ppmdiff.py heatmap <background.ppm> <a.ppm> <b.ppm> <out.png>

        The picture behind the numbers, at native 1x.  The background reference, dimmed, with:

          heat ramp     both covered and differing: blue (1) -> green -> yellow -> red (>=64),
                        on the largest per-channel absolute difference
          unchanged     both covered, identical: the dimmed background shows through
          cyan          covered by A only
          magenta       covered by B only
          white         outside both coverages and differing -- an exit-criterion-1 violation, and
                        it should never appear

    ppmdiff.py report <background.ppm> <a.ppm> <b.ppm> [out.png]

        All of the above in one pass, which is what the A/B script calls.  Exit status is 0 only if
        coverage has no interior disagreement and exact passes.

A is the software renderer and B the Vulkan one, by convention; the measurements are symmetric
except for which of cyan and magenta means what.

Takes the plain binary P6 files that retrohost and M2VK_VK_DUMP write.  Needs numpy; the PNG writer
is stdlib zlib, so there is no image library dependency.
"""

import struct
import sys
import zlib

import numpy as np

# Wang et al. 2004, the parameters every other implementation uses, so the numbers here are
# comparable to a number from anywhere else.
SSIM_WINDOW = 11
SSIM_SIGMA = 1.5
SSIM_C1 = (0.01 * 255.0) ** 2
SSIM_C2 = (0.03 * 255.0) ** 2

HEAT_FULL_SCALE = 64.0  # a per-channel difference of 64/255 saturates the ramp

# An interior coverage disagreement whose two renderings differ by no more than this is the
# "drew black" artefact rather than a missing polygon — one renderer's colour simply landed on the
# background exactly.  dynamcop's are 2 and 6; a genuinely omitted polygon is nowhere near this
# small.  Raising it would start hiding real bugs, so it stays at the measured scale of the artefact.
BACKGROUND_COLLISION = 8


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()

    # P6\n<w> <h>\n<max>\n, as written by retrohost and vk_present's write_ppm — no comments, so the
    # header is three whitespace-separated tokens after the magic.
    if not data.startswith(b'P6'):
        raise SystemExit("%s: not a binary PPM" % path)

    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1

    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit("%s: maxval %d, expected 255" % (path, maxval))

    pixels = data[pos:pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise SystemExit("%s: short by %d bytes" % (path, width * height * 3 - len(pixels)))

    return np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 3)


def load_three(paths):
    images = [read_ppm(p) for p in paths]
    if len({im.shape for im in images}) != 1:
        raise SystemExit("the three images are not the same size")
    return images


def write_png(path, rgb):
    """Minimal RGB8 PNG.  Twenty lines of zlib beats an image-library dependency in a harness."""
    height, width, _ = rgb.shape
    raw = np.zeros((height, width * 3 + 1), dtype=np.uint8)
    raw[:, 1:] = rgb.reshape(height, width * 3)  # filter type 0 on every scanline

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload
                + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', header))
        f.write(chunk(b'IDAT', zlib.compress(raw.tobytes(), 9)))
        f.write(chunk(b'IEND', b''))


def coverage_of(image, background):
    return np.any(image != background, axis=2)


def dilate3(mask):
    """3x3 max.  Used to ask whether a disagreeing pixel touches one both renderers agree on."""
    out = np.zeros_like(mask)
    height, width = mask.shape
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            ys, yd = (slice(max(0, -dy), height - max(0, dy)), slice(max(0, dy), height - max(0, -dy)))
            xs, xd = (slice(max(0, -dx), width - max(0, dx)), slice(max(0, dx), width - max(0, -dx)))
            out[yd, xd] |= mask[ys, xs]
    return out


def erode(mask, radius):
    """Shrink a mask by `radius`, so what is left has its whole window inside the original."""
    out = mask.copy()
    height, width = mask.shape
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            shifted = np.zeros_like(mask)
            ys, yd = (slice(max(0, -dy), height - max(0, dy)), slice(max(0, dy), height - max(0, -dy)))
            xs, xd = (slice(max(0, -dx), width - max(0, dx)), slice(max(0, dx), width - max(0, -dx)))
            shifted[yd, xd] = mask[ys, xs]
            out &= shifted
    return out


def report_coverage(background, a, b):
    height, width, _ = background.shape
    cov_a = coverage_of(a, background)
    cov_b = coverage_of(b, background)

    both = cov_a & cov_b
    a_only = cov_a & ~cov_b
    b_only = cov_b & ~cov_a
    disagree = a_only | b_only

    n_both = int(both.sum())
    n_a_only = int(a_only.sum())
    n_b_only = int(b_only.sum())
    n_disagree = n_a_only + n_b_only
    same_colour = int((both & np.all(a == b, axis=2)).sum())

    total = width * height
    print("picture         %dx%d, %d pixels" % (width, height, total))
    print("covered by A    %7d  (%.3f %% of the picture)" % (n_a_only + n_both, 100.0 * (n_a_only + n_both) / total))
    print("covered by B    %7d  (%.3f %%)" % (n_b_only + n_both, 100.0 * (n_b_only + n_both) / total))
    print("both            %7d" % n_both)
    print("  same colour   %7d  (%.3f %% of the overlap)"
          % (same_colour, 100.0 * same_colour / n_both if n_both else 0.0))
    print("A only          %7d" % n_a_only)
    print("B only          %7d" % n_b_only)

    union = n_both + n_disagree
    print("coverage agreement %.4f  (both / union)" % (n_both / float(union) if union else 1.0))

    # A disagreement pixel that touches a both-covered pixel is a rasterization fill-rule difference
    # on a polygon edge, which is expected and unfixable.  One that does not is a polygon the two
    # renderers disagree about the existence of, which is a bug.
    on_edge = int((disagree & dilate3(both)).sum())
    interior_mask = disagree & ~dilate3(both)
    interior = int(interior_mask.sum())

    # ...except when it is not.  This tool cannot tell "drew black" from "did not draw", so a pixel
    # whose two renderings differ by a hair reads as a coverage disagreement purely because one of
    # them landed exactly on the background colour.  A polygon that one renderer genuinely omits
    # differs by much more than this.  Both counts are printed; only the second decides the verdict.
    magnitude = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    indistinguishable = int((interior_mask & (magnitude <= BACKGROUND_COLLISION)).sum())
    real = interior - indistinguishable

    if n_disagree:
        print("disagreements on an edge %d of %d  (%.2f %%)"
              % (on_edge, n_disagree, 100.0 * on_edge / n_disagree))
        print("  interior disagreements %d" % interior)
        if indistinguishable:
            print("    of which %d differ by <= %d and are the drew-black artefact, not coverage"
                  % (indistinguishable, BACKGROUND_COLLISION))
        print("    real interior disagreements %d  <- any of these is a bug, not a fill rule" % real)
    else:
        print("no coverage disagreement at all")

    return 0 if real == 0 else 1


def report_exact(background, a, b):
    height, width, _ = background.shape
    outside_mask = ~(coverage_of(a, background) | coverage_of(b, background))
    differing_mask = outside_mask & np.any(a != b, axis=2)

    outside = int(outside_mask.sum())
    differing = int(differing_mask.sum())

    print("outside both coverages  %d pixels (%.2f %% of the picture)"
          % (outside, 100.0 * outside / (width * height)))
    if differing:
        ys, xs = np.nonzero(differing_mask)
        first = int(np.argmin(ys.astype(np.int64) * width + xs))  # raster order
        print("DIFFERING               %d, first at (%d, %d)" % (differing, xs[first], ys[first]))
        return 1

    print("identical               exit criterion 1 holds")
    return 0


def gaussian_blur(plane, kernel):
    """Separable Gaussian with reflect padding, so every pixel has a full window and the covered
    region can run to the frame edge."""
    radius = len(kernel) // 2
    padded = np.pad(plane, radius, mode='reflect')
    tmp = np.zeros((padded.shape[0], plane.shape[1]), dtype=np.float64)
    for i, w in enumerate(kernel):
        tmp += w * padded[:, i:i + plane.shape[1]]
    out = np.zeros(plane.shape, dtype=np.float64)
    for i, w in enumerate(kernel):
        out += w * tmp[i:i + plane.shape[0], :]
    return out


def ssim_channels(a, b):
    """SSIM computed separately on R, G and B.  Chroma-only differences are exactly what the
    colourxlat tail produces, and a luma-only SSIM would score them as perfect."""
    radius = SSIM_WINDOW // 2
    offsets = np.arange(-radius, radius + 1, dtype=np.float64)
    kernel = np.exp(-(offsets ** 2) / (2.0 * SSIM_SIGMA ** 2))
    kernel /= kernel.sum()

    maps = []
    for c in range(3):
        x = a[:, :, c].astype(np.float64)
        y = b[:, :, c].astype(np.float64)

        mu_x = gaussian_blur(x, kernel)
        mu_y = gaussian_blur(y, kernel)
        sigma_xx = gaussian_blur(x * x, kernel) - mu_x * mu_x
        sigma_yy = gaussian_blur(y * y, kernel) - mu_y * mu_y
        sigma_xy = gaussian_blur(x * y, kernel) - mu_x * mu_y

        maps.append(((2 * mu_x * mu_y + SSIM_C1) * (2 * sigma_xy + SSIM_C2))
                    / ((mu_x ** 2 + mu_y ** 2 + SSIM_C1) * (sigma_xx + sigma_yy + SSIM_C2)))

    return maps


def ssim_map(a, b):
    return sum(ssim_channels(a, b)) / 3.0


def report_ssim(background, a, b):
    covered = coverage_of(a, background) | coverage_of(b, background)
    channels = ssim_channels(a, b)
    smap = sum(channels) / 3.0

    n_covered = int(covered.sum())
    interior = erode(covered, SSIM_WINDOW // 2)
    n_interior = int(interior.sum())

    print("ssim whole frame  %.6f   <- what a naive SSIM says; mostly the identical 2D layers"
          % smap.mean())
    if not n_covered:
        print("nothing covered   no 3D in this frame, so there is no SSIM to report")
        return 0

    values = smap[covered]
    print("ssim covered      %.6f   over %d pixels (%.2f %% of the picture)"
          % (values.mean(), n_covered, 100.0 * n_covered / covered.size))
    if n_interior:
        print("ssim interior     %.6f   over %d pixels, whole %dx%d window covered"
              % (smap[interior].mean(), n_interior, SSIM_WINDOW, SSIM_WINDOW))
    else:
        print("ssim interior     n/a       no covered pixel has a fully covered window")

    p1, p5, p50 = np.percentile(values, [1, 5, 50])
    print("  covered p1 %.4f  p5 %.4f  median %.4f  min %.4f"
          % (p1, p5, p50, values.min()))

    for name, chan in zip('RGB', channels):
        print("  %s covered      %.6f" % (name, chan[covered].mean()))

    return 0


def heat_ramp(magnitude):
    """blue -> cyan -> green -> yellow -> red over [1, HEAT_FULL_SCALE].  Built by hand rather than
    imported so the tool keeps working with nothing but numpy installed."""
    t = np.clip(magnitude / HEAT_FULL_SCALE, 0.0, 1.0)
    stops = np.array([[0, 0, 255], [0, 255, 255], [0, 255, 0], [255, 255, 0], [255, 0, 0]],
                     dtype=np.float64)
    pos = t * (len(stops) - 1)
    lo = np.clip(np.floor(pos).astype(np.int32), 0, len(stops) - 2)
    frac = (pos - lo)[..., None]
    return (stops[lo] * (1.0 - frac) + stops[lo + 1] * frac).astype(np.uint8)


def report_heatmap(background, a, b, out_path):
    cov_a = coverage_of(a, background)
    cov_b = coverage_of(b, background)
    both = cov_a & cov_b
    outside_bad = ~(cov_a | cov_b) & np.any(a != b, axis=2)

    magnitude = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)

    image = (background.astype(np.float64) * 0.28).astype(np.uint8)  # the scene, dimmed, for bearing
    image[both & (magnitude > 0)] = heat_ramp(magnitude)[both & (magnitude > 0)]
    image[cov_a & ~cov_b] = (0, 255, 255)
    image[cov_b & ~cov_a] = (255, 0, 255)
    image[outside_bad] = (255, 255, 255)

    write_png(out_path, image)

    differing = int((both & (magnitude > 0)).sum())
    print("heatmap         %s" % out_path)
    print("  ramp          %d both-covered pixels differ; max %d, mean %.2f, 0 -> %.0f full scale"
          % (differing, int(magnitude[both].max()) if both.any() else 0,
             float(magnitude[both & (magnitude > 0)].mean()) if differing else 0.0, HEAT_FULL_SCALE))
    print("  cyan          %d  A only" % int((cov_a & ~cov_b).sum()))
    print("  magenta       %d  B only" % int((cov_b & ~cov_a).sum()))
    print("  white         %d  outside both and differing (must be 0)" % int(outside_bad.sum()))
    return 0


def main(argv):
    # argc after the three image paths: heatmap requires an output, report accepts one, the rest
    # take none.
    allowed = {'coverage': (5,), 'exact': (5,), 'ssim': (5,), 'heatmap': (6,), 'report': (5, 6)}
    if len(argv) < 2 or argv[1] not in allowed or len(argv) not in allowed[argv[1]]:
        print(__doc__)
        return 2

    background, a, b = load_three(argv[2:5])

    if argv[1] == 'coverage':
        return report_coverage(background, a, b)
    if argv[1] == 'exact':
        return report_exact(background, a, b)
    if argv[1] == 'ssim':
        return report_ssim(background, a, b)
    if argv[1] == 'heatmap':
        return report_heatmap(background, a, b, argv[5])

    status = report_coverage(background, a, b)
    print()
    status |= report_exact(background, a, b)
    print()
    report_ssim(background, a, b)
    if len(argv) == 6:
        print()
        report_heatmap(background, a, b, argv[5])
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv))
