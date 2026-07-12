#!/usr/bin/env python3
"""Compute colorCorrection.srgbMatrix from a RAW ColorChecker capture.

Input: a Jetson VI RAW10 capture (see raw_stats.py for the layout) of a
standard 24-patch ColorChecker, plus the pixel coordinates of the FOUR
CORNER PATCH CENTERS in chart order:

    --corners x,y x,y x,y x,y
               |    |    |    +-- patch 19 "white" row start side... no:
               |    |    +------- patch 24 (black) corner
               |    +------------ patch 6 (bluish green) corner
               +----------------- patch 1 (dark skin) corner

i.e. top-left = dark skin, top-right = bluish green, bottom-right = black,
bottom-left = white, when the chart is oriented the classic way (6 columns
x 4 rows, neutral row at the bottom).

Method: average a window at each interpolated patch center, subtract the
black level, white-balance on the neutral patches, then least-squares fit a
3x3 matrix mapping measured (linear, WB'd) RGB to the chart's linear sRGB
reference values. Rows are normalized to preserve white (sum 1) unless
--no-normalize is given. Requires numpy only.
"""

import argparse
import sys

import numpy as np

# BabelColor average ColorChecker reference, sRGB 8-bit (D65), patches 1-24
# row-major. Good enough for a DIY CCM; swap in your chart's datasheet
# values if you have them.
REFERENCE_SRGB = [
    (115, 82, 68), (194, 150, 130), (98, 122, 157), (87, 108, 67),
    (133, 128, 177), (103, 189, 170),
    (214, 126, 44), (80, 91, 166), (193, 90, 99), (94, 60, 108),
    (157, 188, 64), (224, 163, 46),
    (56, 61, 150), (70, 148, 73), (175, 54, 60), (231, 199, 31),
    (187, 86, 149), (8, 133, 161),
    (243, 243, 242), (200, 200, 200), (160, 160, 160), (122, 122, 121),
    (85, 85, 85), (52, 52, 52),
]
NEUTRAL_PATCHES = range(18, 24)  # bottom row


def srgb_to_linear(v):
    v = v / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def load_frame(path, width, height):
    data = np.fromfile(path, dtype="<u2")
    n = width * height
    if data.size < n:
        sys.exit(f"{path}: shorter than one {width}x{height} frame")
    frames = data[: (data.size // n) * n].reshape(-1, height, width)
    if frames.max() >= 1 << 10:
        frames = frames >> 6
    return frames.astype(np.float64).mean(axis=0)


def half_res_rgb(frame, pattern, black):
    """Cheap half-resolution demosaic: one RGB pixel per 2x2 Bayer cell."""
    idx = {"rggb": (0, 1, 2, 3), "grbg": (1, 0, 3, 2),
           "gbrg": (2, 3, 0, 1), "bggr": (3, 2, 1, 0)}
    if pattern not in idx:
        sys.exit(f"unknown Bayer pattern '{pattern}'")
    cells = [frame[0::2, 0::2], frame[0::2, 1::2],
             frame[1::2, 0::2], frame[1::2, 1::2]]
    r_i, g1_i, g2_i, b_i = idx[pattern]
    r = cells[r_i] - black
    g = (cells[g1_i] + cells[g2_i]) / 2 - black
    b = cells[b_i] - black
    return np.clip(np.dstack([r, g, b]), 0, None)


def patch_centers(corners):
    """Bilinear-interpolate the 24 patch centers from the 4 corner centers."""
    tl, tr, br, bl = [np.array(c, dtype=float) for c in corners]
    centers = []
    for row in range(4):
        v = row / 3.0
        left = tl + (bl - tl) * v
        right = tr + (br - tr) * v
        for col in range(6):
            u = col / 5.0
            centers.append(left + (right - left) * u)
    return centers


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--width", type=int, default=1440)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--pattern", default="rggb")
    ap.add_argument("--black", type=float, default=64.0)
    ap.add_argument("--corners", nargs=4, required=True, metavar="X,Y",
                    help="pixel coords of the 4 corner patch centers: "
                         "dark-skin bluish-green black white (chart order)")
    ap.add_argument("--window", type=int, default=12,
                    help="half-size of the sampling window per patch (full-res px)")
    ap.add_argument("--no-normalize", action="store_true",
                    help="don't normalize matrix rows to sum 1")
    args = ap.parse_args()

    corners = []
    for c in args.corners:
        x, y = c.split(",")
        corners.append((float(x) / 2, float(y) / 2))  # half-res demosaic

    rgb = half_res_rgb(load_frame(args.file, args.width, args.height),
                       args.pattern, args.black)
    win = max(args.window // 2, 2)

    measured = []
    for cx, cy in patch_centers(corners):
        x, y = int(round(cx)), int(round(cy))
        m = rgb[y - win:y + win, x - win:x + win].reshape(-1, 3).mean(axis=0)
        measured.append(m)
    measured = np.array(measured)

    if measured.max() >= (1023 - args.black) * 0.98:
        print("# WARNING: near-clipping patches, reduce exposure and recapture",
              file=sys.stderr)

    # White balance from the neutral row (skip white & black extremes).
    neutrals = measured[[19, 20, 21, 22]]
    g = neutrals[:, 1].mean()
    wb = np.array([g / neutrals[:, 0].mean(), 1.0, g / neutrals[:, 2].mean()])
    measured_wb = measured * wb

    reference = srgb_to_linear(np.array(REFERENCE_SRGB, dtype=float))
    # Match overall exposure on the neutral patches, then solve M in
    # ref ~= measured_wb @ M.T  (least squares over all 24 patches).
    scale = reference[[19, 20, 21, 22]].mean() / measured_wb[[19, 20, 21, 22]].mean()
    measured_wb *= scale
    M, *_ = np.linalg.lstsq(measured_wb, reference, rcond=None)
    M = M.T
    if not args.no_normalize:
        M /= M.sum(axis=1, keepdims=True)

    fit = measured_wb @ M.T
    err = np.abs(fit - reference).mean()

    print(f"# WB gains used (info only, AWB handles this at runtime): "
          f"R {wb[0]:.4f}  B {wb[2]:.4f}")
    print(f"# mean abs fit error (linear sRGB): {err:.4f}")
    print("# paste into camera_overrides.isp:")
    for i in range(3):
        print(f"colorCorrection.srgbMatrix[{i}]\t = "
              f"{{{M[i][0]:.8f},{M[i][1]:.8f},{M[i][2]:.8f}}};")


if __name__ == "__main__":
    main()
