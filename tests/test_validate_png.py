"""Independent PNG parser/layout regressions; no auxiliary grading shader."""

import math
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from validate_png import PNG_SIGNATURE, PngError, compare_cube, decode_rgb_png, load_png, _paeth
from validate_cube import identity_samples, measure


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_bytes(width, height, rgb, filter_type=0, color=2):
    channels = 3 if color == 2 else 4
    stride = width * channels
    filtered = bytearray()
    previous = bytes(stride)
    for y in range(height):
        row = rgb[y * stride:(y + 1) * stride]
        filtered.append(filter_type)
        for x, value in enumerate(row):
            a = row[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            # Independent Paeth predictor for the fixture generator.
            distances = (abs(b - c), abs(a - c), abs(a + b - 2 * c))
            paeth = (a, b, c)[distances.index(min(distances))]
            predictors = (0, a, b, (a + b) // 2, paeth)
            filtered.append((value - predictors[filter_type]) & 255)
        previous = row
    header = struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)
    return PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(filtered)) + chunk(b"IEND", b"")


def identity_rgb(size, square=False):
    columns = math.isqrt(size) if square else size
    width, height = size * columns, size * (size // columns)
    data = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            rgb = (x % size, y % size, (y // size) * columns + x // size)
            offset = (y * width + x) * 3
            data[offset:offset + 3] = bytes(math.floor(c / (size - 1) * 255 + 0.5) for c in rgb)
    return width, height, bytes(data)


class PngTests(unittest.TestCase):
    def test_all_row_filters(self):
        rgb = bytes((i * 71 + 13) % 256 for i in range(5 * 3 * 3))
        for filter_type in range(5):
            width, height, actual, _ = decode_rgb_png(png_bytes(5, 3, rgb, filter_type))
            self.assertEqual((width, height, actual), (5, 3, rgb))
        self.assertEqual(_paeth(10, 20, 30), 10)

    def test_layout_axes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "identity.png"
            for layout in ("horizontal", "square"):
                width, height, data = identity_rgb(16, layout == "square")
                path.write_bytes(png_bytes(width, height, data))
                lut = load_png(path, layout)
                self.assertEqual(lut.size, 16)
                self.assertEqual(measure(lut.samples(), identity_samples(16)).maximum_absolute, 0.0)
                with self.assertRaises(PngError):
                    load_png(path, "square" if layout == "horizontal" else "horizontal")

    def test_opaque_rgba(self):
        rgb = b"\x01\x7f\xfe\x03\x04\x05"
        rgba = rgb[:3] + b"\xff" + rgb[3:] + b"\xff"
        self.assertEqual(decode_rgb_png(png_bytes(2, 1, rgba, color=6))[2], rgb)
        with self.assertRaises(PngError):
            decode_rgb_png(png_bytes(2, 1, rgba[:-1] + b"\x7f", color=6))

    def test_corruption_and_limits(self):
        good = png_bytes(4, 2, bytes(4 * 2 * 3))
        for invalid in (b"not png", good[:-1], good + b"extra", good[:30] + bytes([good[30] ^ 1]) + good[31:]):
            with self.assertRaises(PngError):
                decode_rgb_png(invalid)
        oversized = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 16385, 16385, 8, 2, 0, 0, 0))
        with self.assertRaises(PngError):
            decode_rgb_png(oversized)
        bad_length = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 2, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(10000))) + chunk(b"IEND", b"")
        with self.assertRaises(PngError):
            decode_rgb_png(bad_length)
        self.assertEqual(decode_rgb_png(good)[2], bytes(24))

    def test_gamma_metadata_rejected(self):
        good = png_bytes(4, 2, bytes(24))
        tagged = good[:33] + chunk(b"gAMA", struct.pack(">I", 45455)) + good[33:]
        with self.assertRaises(PngError):
            decode_rgb_png(tagged)

    def test_cube_comparison_and_explicit_clamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "lut.png"
            cube = pathlib.Path(temporary) / "lut.cube"
            width, height, rgb = identity_rgb(2)
            path.write_bytes(png_bytes(width, height, rgb))
            rows = [" ".join(str(c) for c in row) for row in identity_samples(2)]
            cube.write_text("LUT_3D_SIZE 2\n" + "\n".join(rows), encoding="utf-8")
            lut = load_png(path, "horizontal")
            self.assertEqual(compare_cube(lut, cube).maximum_absolute, 0.0)
            rows[0] = "-1 0 0"
            cube.write_text("LUT_3D_SIZE 2\n" + "\n".join(rows), encoding="utf-8")
            with self.assertRaises(PngError):
                compare_cube(lut, cube)
            self.assertEqual(compare_cube(lut, cube, clamp=True).maximum_absolute, 0.0)


if __name__ == "__main__":
    unittest.main()
