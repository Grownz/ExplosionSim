#!/usr/bin/env python3
"""Compare two 8-bit RGB/RGBA PNG screenshots without third-party packages."""

import argparse
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG file")
    pos = 8
    width = height = bit_depth = color_type = 0
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(
                ">IIBB", payload[:10]
            )
        elif kind == b"IDAT":
            idat.extend(payload)
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(f"{path}: expected 8-bit RGB or RGBA")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    pixels = bytearray(width * height * 3)
    previous = bytearray(stride)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset : offset + stride])
        offset += stride
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if filter_type == 1:
                line[x] = (line[x] + left) & 0xFF
            elif filter_type == 2:
                line[x] = (line[x] + up) & 0xFF
            elif filter_type == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                p = left + up - upper_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else (
                    up if pb <= pc else upper_left
                )
                line[x] = (line[x] + predictor) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"{path}: unsupported PNG filter {filter_type}")
        out = (y * width) * 3
        for x in range(width):
            src = x * channels
            pixels[out + x * 3 + 0] = line[src + 0]
            pixels[out + x * 3 + 1] = line[src + 1]
            pixels[out + x * 3 + 2] = line[src + 2]
        previous = line
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=20.0,
        help="maximum allowed mean absolute RGB difference (0..255)",
    )
    parser.add_argument(
        "--max-fraction",
        type=float,
        default=0.02,
        help="maximum allowed fraction of pixels above 32/255 difference",
    )
    args = parser.parse_args()

    aw, ah, a = read_png(args.reference)
    bw, bh, b = read_png(args.candidate)
    if (aw, ah) != (bw, bh):
        print(f"size mismatch: {aw}x{ah} vs {bw}x{bh}", file=sys.stderr)
        return 2

    total = 0
    maximum = 0
    large = 0
    pixels = aw * ah
    for i in range(pixels):
        pixel_max = 0
        for channel in range(3):
            diff = abs(a[i * 3 + channel] - b[i * 3 + channel])
            total += diff
            maximum = max(maximum, diff)
            pixel_max = max(pixel_max, diff)
        if pixel_max > 32:
            large += 1
    mean = total / float(pixels * 3)
    fraction = large / float(pixels)
    print(
        f"{aw}x{ah}: mean_abs={mean:.2f} max={maximum} "
        f"pixels_above_32={fraction * 100.0:.2f}%"
    )
    if mean > args.tolerance or fraction > args.max_fraction:
        print("comparison exceeds tolerance", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
