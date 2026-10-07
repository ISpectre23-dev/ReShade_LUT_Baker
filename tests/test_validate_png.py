"""Independent PNG parser/layout regressions; no auxiliary grading shader."""

import contextlib
import io
import math
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from validate_png import PNG_SIGNATURE, PngError, compare_cube, decode_rgb_png, load_png, main, _paeth
from validate_cube import identity_samples, measure


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_bytes(width, height, rgb, filter_type=0, color=2, bit_depth=8):
    channels = 3 if color == 2 else 4
    pixel_bytes = channels * (bit_depth // 8)
    stride = width * pixel_bytes
    filtered = bytearray()
    previous = bytes(stride)
    for y in range(height):
        row = rgb[y * stride:(y + 1) * stride]
        filtered.append(filter_type)
        for x, value in enumerate(row):
            a = row[x - pixel_bytes] if x >= pixel_bytes else 0
            b = previous[x]
            c = previous[x - pixel_bytes] if x >= pixel_bytes else 0
            # Independent Paeth predictor for the fixture generator.
            distances = (abs(b - c), abs(a - c), abs(a + b - 2 * c))
            paeth = (a, b, c)[distances.index(min(distances))]
            predictors = (0, a, b, (a + b) // 2, paeth)
            filtered.append((value - predictors[filter_type]) & 255)
        previous = row
    header = struct.pack(">IIBBBBB", width, height, bit_depth, color, 0, 0, 0)
    return PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(filtered)) + chunk(b"IEND", b"")


def identity_rgb(size, square=False, bit_depth=8):
    columns = math.isqrt(size) if square else size
    width, height = size * columns, size * (size // columns)
    component_bytes = bit_depth // 8
    maximum = (1 << bit_depth) - 1
    data = bytearray(width * height * 3 * component_bytes)
    for y in range(height):
        for x in range(width):
            rgb = (x % size, y % size, (y // size) * columns + x // size)
            offset = (y * width + x) * 3 * component_bytes
            samples = tuple(math.floor(c / (size - 1) * maximum + 0.5) for c in rgb)
            data[offset:offset + 3 * component_bytes] = bytes(samples) if bit_depth == 8 else struct.pack(">3H", *samples)
    return width, height, bytes(data)


class PngTests(unittest.TestCase):
    def test_all_row_filters(self):
        for depth in (8, 16):
            rgb = bytes((i * 71 + 13) % 256 for i in range(5 * 3 * 3 * (depth // 8)))
            for filter_type in range(5):
                width, height, actual, _, actual_depth = decode_rgb_png(png_bytes(5, 3, rgb, filter_type, bit_depth=depth))
                self.assertEqual((width, height, actual, actual_depth), (5, 3, rgb, depth))
        self.assertEqual(_paeth(10, 20, 30), 10)

    def test_layout_axes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "identity.png"
            for layout in ("horizontal", "square"):
                for depth in (8, 16):
                    width, height, data = identity_rgb(16, layout == "square", depth)
                    path.write_bytes(png_bytes(width, height, data, bit_depth=depth))
                    lut = load_png(path, layout)
                    self.assertEqual((lut.size, lut.bit_depth), (16, depth))
                    self.assertEqual(measure(lut.samples(), identity_samples(16)).maximum_absolute, 0.0)
                    with self.assertRaises(PngError):
                        load_png(path, "square" if layout == "horizontal" else "horizontal")

    def test_opaque_rgba(self):
        rgb = b"\x01\x7f\xfe\x03\x04\x05"
        rgba = rgb[:3] + b"\xff" + rgb[3:] + b"\xff"
        self.assertEqual(decode_rgb_png(png_bytes(2, 1, rgba, color=6))[2], rgb)
        with self.assertRaises(PngError):
            decode_rgb_png(png_bytes(2, 1, rgba[:-1] + b"\x7f", color=6))

    def test_16bit_low_bytes_and_alpha(self):
        rgb = struct.pack(">6H", 0x1234, 0xabcd, 1, 0xfffe, 0x8081, 0x12ff)
        rgba = rgb[:6] + b"\xff\xff" + rgb[6:] + b"\xff\xff"
        for color, data in ((2, rgb), (6, rgba)):
            for filter_type in range(5):
                decoded = decode_rgb_png(png_bytes(2, 1, data, filter_type, color, 16))
                self.assertEqual((decoded[2], decoded[4]), (rgb, 16))
        for alpha in (b"\xff\x7f", b"\x7f\xff"):
            with self.assertRaises(PngError):
                decode_rgb_png(png_bytes(2, 1, rgba[:-2] + alpha, color=6, bit_depth=16))

    def test_16bit_cli_tolerance(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "identity16.png"
            width, height, data = identity_rgb(2, bit_depth=16)
            # An error that fits the old 8-bit tolerance must fail at 16 bits.
            path.write_bytes(png_bytes(width, height, b"\x00\x14" + data[2:], bit_depth=16))
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(["identity", str(path), "--layout", "horizontal"]), 1)
                self.assertEqual(main(["identity", str(path), "--layout", "horizontal", "--tolerance", "0.0005"]), 0)
                self.assertEqual(main(["identity", str(path), "--layout", "horizontal", "--tolerance", "nan"]), 2)

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
        good16 = png_bytes(4, 2, bytes(48), bit_depth=16)
        for invalid in (good16[:-1], good16[:30] + bytes([good16[30] ^ 1]) + good16[31:],
                        png_bytes(4, 2, bytes(48), bit_depth=12)):
            with self.assertRaises(PngError):
                decode_rgb_png(invalid)
        self.assertEqual(decode_rgb_png(good16)[2], bytes(48))

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

    def test_16bit_cube_comparison(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "lut16.png"
            cube = pathlib.Path(temporary) / "lut.cube"
            width, height, rgb = identity_rgb(2, bit_depth=16)
            path.write_bytes(png_bytes(width, height, rgb, bit_depth=16))
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
