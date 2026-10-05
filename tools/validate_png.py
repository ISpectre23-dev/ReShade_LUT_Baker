#!/usr/bin/env python3
"""Inspect and validate baked RGB PNG LUTs without third-party image libraries."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
import pathlib
import struct
import sys
import zlib

from validate_cube import Metrics, identity_samples, load_cube, measure, print_metrics

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_SAMPLES = 128**3


class PngError(ValueError):
    pass


@dataclass(frozen=True)
class PngLut:
    size: int
    width: int
    height: int
    layout: str
    pixels: bytes
    software: str

    def samples(self):
        """Yield samples in CUBE's red-fastest order, not image row order."""
        columns = self.size if self.layout == "horizontal" else math.isqrt(self.size)
        for b in range(self.size):
            for g in range(self.size):
                y = g + (b // columns) * self.size
                for r in range(self.size):
                    x = r + (b % columns) * self.size
                    index = (y * self.width + x) * 3
                    yield tuple(self.pixels[index + c] / 255.0 for c in range(3))


def _paeth(a: int, b: int, c: int) -> int:
    prediction = a + b - c
    da, db, dc = abs(prediction - a), abs(prediction - b), abs(prediction - c)
    return a if da <= db and da <= dc else b if db <= dc else c


def decode_rgb_png(data: bytes) -> tuple[int, int, bytes, str]:
    """Strict bounded decoder for non-interlaced 8-bit RGB/RGBA PNG data."""
    if len(data) > MAX_FILE_BYTES or not data.startswith(PNG_SIGNATURE):
        raise PngError("not a PNG file, or larger than the 16 MiB validation limit")
    offset = 8
    width = height = channels = 0
    compressed = bytearray()
    software = ""
    saw_header = saw_data = saw_end = data_finished = False
    while offset < len(data):
        if offset + 12 > len(data):
            raise PngError("truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise PngError("PNG chunk exceeds the file length")
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(chunk_type + payload) & 0xFFFFFFFF != expected_crc:
            raise PngError("PNG chunk checksum mismatch")
        if not saw_header and chunk_type != b"IHDR":
            raise PngError("IHDR must be the first PNG chunk")
        if chunk_type == b"IHDR":
            if saw_header or length != 13:
                raise PngError("invalid or duplicate PNG header")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if width == 0 or height == 0 or width > 16384 or height > 16384 or width * height > MAX_SAMPLES:
                raise PngError("PNG dimensions exceed the baker's sample budget")
            if depth != 8 or color not in (2, 6) or compression != 0 or filtering != 0 or interlace != 0:
                raise PngError("validation requires non-interlaced 8-bit RGB or RGBA PNG")
            channels = 3 if color == 2 else 4
            saw_header = True
        elif chunk_type == b"IDAT":
            if data_finished:
                raise PngError("PNG image-data chunks must be consecutive")
            compressed.extend(payload)
            saw_data = True
        elif chunk_type == b"IEND":
            if length != 0 or not saw_data or end != len(data):
                raise PngError("invalid PNG end chunk or trailing data")
            saw_end = True
        else:
            if saw_data:
                data_finished = True
            if chunk_type in (b"gAMA", b"sRGB", b"iCCP", b"cHRM"):
                raise PngError("PNG contains color-space metadata; this validator expects the baker's untagged RGB data")
            if chunk_type == b"tEXt" and payload.startswith(b"Software\0"):
                software = payload[len(b"Software\0"):].decode("latin-1")
            elif not chunk_type[0] & 0x20:
                raise PngError("unsupported critical PNG chunk")
        offset = end
    if not (saw_header and saw_data and saw_end):
        raise PngError("PNG is missing its header, image data or end chunk")
    stride = width * channels
    expected = height * (stride + 1)
    decoder = zlib.decompressobj()
    try:
        raw = decoder.decompress(compressed, expected + 1)
    except zlib.error as exc:
        raise PngError("invalid PNG compressed image data") from exc
    if len(raw) != expected or not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:
        raise PngError("PNG decoded length does not match its dimensions")
    pixels = bytearray(width * height * 3)
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        if filter_type > 4:
            raise PngError("unknown PNG row filter")
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            predictor = (0, left, above, (left + above) // 2, _paeth(left, above, upper_left))[filter_type]
            row[x] = (row[x] + predictor) & 255
        if channels == 3:
            pixels[y * width * 3:(y + 1) * width * 3] = row
        else:
            for x in range(width):
                if row[x * 4 + 3] != 255:
                    raise PngError("PNG contains non-opaque alpha")
                index = (y * width + x) * 3
                pixels[index:index + 3] = row[x * 4:x * 4 + 3]
        previous = row
    return width, height, bytes(pixels), software


def load_png(path: pathlib.Path, layout: str) -> PngLut:
    try:
        if path.stat().st_size > MAX_FILE_BYTES:
            raise PngError("PNG exceeds the 16 MiB validation limit")
        width, height, pixels, software = decode_rgb_png(path.read_bytes())
    except OSError as exc:
        raise PngError(f"cannot read PNG: {exc}") from exc
    if layout == "horizontal":
        size = height
        if not 2 <= size <= 128 or width != size * size:
            raise PngError("Horizontal strip dimensions must be size*size by size, with size from 2 to 128")
    elif layout == "square":
        size = next((n for n in range(2, 129) if n**3 == width * height), 0)
        if width != height or size == 0 or math.isqrt(size)**2 != size:
            raise PngError("Square tiles need an exact square grid without padding")
    else:
        raise PngError("choose an explicit layout: horizontal or square")
    return PngLut(size, width, height, layout, pixels, software)


def compare_cube(png: PngLut, cube_path: pathlib.Path, clamp: bool = False) -> Metrics:
    cube = load_cube(cube_path)
    if cube.size != png.size:
        raise PngError("PNG and CUBE must have the same lattice size")
    if not clamp and any(c < 0.0 or c > 1.0 for sample in cube.samples for c in sample):
        raise PngError("CUBE has values outside 0-1; use --clamp only if the PNG was explicitly clamped")
    expected = (tuple(min(1.0, max(0.0, c)) for c in sample) for sample in cube.samples) if clamp else cube.samples
    return measure(png.samples(), expected)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Inspect or validate PNG lookup tables exported by ReShade LUT Baker.")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "identity", "compare-cube"):
        command = commands.add_parser(name)
        command.add_argument("png", type=pathlib.Path)
        command.add_argument("--layout", choices=("horizontal", "square"), required=True)
        if name == "compare-cube":
            command.add_argument("cube", type=pathlib.Path)
            command.add_argument("--clamp", action="store_true")
        if name != "inspect":
            command.add_argument("--tolerance", type=float, default=0.5 / 255.0 + 3e-8)
    args = parser.parse_args(argv)
    try:
        png = load_png(args.png, args.layout)
        print(f"PNG: {png.width}x{png.height}, {png.size}^3, {png.layout}, 8 bits per channel")
        print(f"Exporter: {png.software or '(not recorded)'}")
        if args.command == "inspect":
            return 0
        if not math.isfinite(args.tolerance) or args.tolerance < 0:
            raise PngError("tolerance must be finite and non-negative")
        metrics = measure(png.samples(), identity_samples(png.size)) if args.command == "identity" else compare_cube(png, args.cube, args.clamp)
        print_metrics(metrics)
        if metrics.maximum_absolute > args.tolerance:
            print(f"FAIL: maximum error exceeds {args.tolerance:g}", file=sys.stderr)
            return 1
        print("PASS: maximum error is within tolerance")
        return 0
    except (ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
