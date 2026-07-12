#!/usr/bin/env python3
"""Statistics from Jetson VI RAW captures (v4l2-ctl --stream-to files).

Assumes the Jetson VI RAW10 memory layout: one pixel per 16-bit
little-endian word, 10 valid bits. A capture file may hold several frames;
all are averaged (use --stream-skip when capturing so AE/sensor settling
doesn't pollute the data).

Modes:
  dark  <file>   per-Bayer-channel black level -> opticalBlack.manualBias*
  flat  <file>   center vs corner/edge means per channel -> lens falloff
  gray  <file>   gray-world white balance gains (R/G, B/G)
"""

import argparse
import sys

import numpy as np


def load_frames(path, width, height):
    data = np.fromfile(path, dtype="<u2")
    pixels_per_frame = width * height
    if data.size < pixels_per_frame:
        sys.exit(f"{path}: {data.size} px words < one {width}x{height} frame")
    frames = data.size // pixels_per_frame
    data = data[: frames * pixels_per_frame].reshape(frames, height, width)
    # 10 valid bits; VI may left-justify to 16 bits depending on mode.
    # Detect: if the maximum needs more than 10 bits, assume left-justified.
    if data.max() >= 1 << 10:
        data = data >> 6
    return data.astype(np.float64).mean(axis=0)  # average the frames


def bayer_planes(frame, pattern):
    """Split into the four Bayer planes, keyed by color: R, G(r-row), G(b-row), B."""
    p = pattern.lower()
    offsets = {
        "rggb": (("R", 0, 0), ("Gr", 0, 1), ("Gb", 1, 0), ("B", 1, 1)),
        "grbg": (("Gr", 0, 0), ("R", 0, 1), ("B", 1, 0), ("Gb", 1, 1)),
        "gbrg": (("Gb", 0, 0), ("B", 0, 1), ("R", 1, 0), ("Gr", 1, 1)),
        "bggr": (("B", 0, 0), ("Gb", 0, 1), ("Gr", 1, 0), ("R", 1, 1)),
    }
    if p not in offsets:
        sys.exit(f"unknown Bayer pattern '{pattern}'")
    return {name: frame[y::2, x::2] for name, y, x in offsets[p]}


def mode_dark(frame, pattern):
    print("# per-channel black level (10-bit units), paste into the overrides:")
    planes = bayer_planes(frame, pattern)
    key = {"R": "R", "Gr": "GR", "Gb": "GB", "B": "B"}
    for name in ("R", "Gr", "Gb", "B"):
        mean = planes[name].mean()
        print(f"opticalBlack.manualBias{key[name]:<3}\t = {mean:.0f};"
              f"   # std {planes[name].std():.1f}")
        print(f"opticalBlack.float.manualBias{key[name]:<3}\t = {mean / 1023.0:.8f};")


def mode_flat(frame, pattern):
    print("# relative illumination per channel (center = 1.0):")
    planes = bayer_planes(frame, pattern)
    for name, plane in planes.items():
        h, w = plane.shape
        box = min(h, w) // 8

        def region(cy, cx):
            return plane[max(cy - box, 0):cy + box, max(cx - box, 0):cx + box].mean()

        center = region(h // 2, w // 2)
        corners = [region(box, box), region(box, w - box),
                   region(h - box, box), region(h - box, w - box)]
        edges = [region(box, w // 2), region(h - box, w // 2),
                 region(h // 2, box), region(h // 2, w - box)]
        print(f"{name:>2}: corners {min(corners)/center:.3f}..{max(corners)/center:.3f}"
              f"  edges {min(edges)/center:.3f}..{max(edges)/center:.3f}")
    print("# < ~0.85 in the corners: lens shading correction is worth the")
    print("# effort; otherwise set ap15Function.lensShading = FALSE and move on.")


def mode_gray(frame, pattern, black):
    planes = bayer_planes(frame, pattern)
    r = planes["R"].mean() - black
    g = (planes["Gr"].mean() + planes["Gb"].mean()) / 2 - black
    b = planes["B"].mean() - black
    print(f"# gray-world means (black-subtracted): R {r:.1f}  G {g:.1f}  B {b:.1f}")
    print(f"# WB gains to neutralize this scene: R/G {g / r:.4f}  B/G {g / b:.4f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("dark", "flat", "gray"))
    ap.add_argument("file")
    ap.add_argument("--width", type=int, default=1440)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--pattern", default="rggb",
                    help="Bayer order as reported by v4l2-ctl --list-formats-ext "
                         "(default rggb)")
    ap.add_argument("--black", type=float, default=64.0,
                    help="black level for gray mode (default 64)")
    args = ap.parse_args()

    frame = load_frames(args.file, args.width, args.height)
    if args.mode == "dark":
        mode_dark(frame, args.pattern)
    elif args.mode == "flat":
        mode_flat(frame, args.pattern)
    else:
        mode_gray(frame, args.pattern, args.black)


if __name__ == "__main__":
    main()
