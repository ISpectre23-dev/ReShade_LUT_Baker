"""Independent binary/quantization reference and strict parser regressions."""

import hashlib
import math
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from validate_cube import identity_samples, measure
from validate_rise_tex import FILE_SIZE, HEADER, RiseTexError, compare_cube, load_rise_tex


def float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def reference_bytes(samples):
    result = bytearray(HEADER)
    for row in samples:
        result.extend(math.floor(value * 255.0 + 0.5) for value in row)
        result.append(255)
    return bytes(result)


def asymmetric_samples():
    for b in range(32):
        for g in range(32):
            for r in range(32):
                yield tuple(float32(v) for v in (0.1 + 0.8 * (b / 31.0),
                                                 0.1 + 0.8 * (r / 31.0) * (r / 31.0),
                                                 0.1 + 0.8 * (g / 31.0)))


class RiseTexValidationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rise_tex_validation_")
        self.directory = pathlib.Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.path = self.directory / "identity.tex.28"
        self.data = reference_bytes(identity_samples(32))
        self.path.write_bytes(self.data)

    def test_profile_and_identity(self):
        self.assertEqual(hashlib.sha256(HEADER).hexdigest(), "2b77b7325e98114c3fdaf22233d4a7cd1183791e8ec95ca7e7587c0904ad9c04")
        self.assertEqual(len(self.data), 131128)
        self.assertEqual(struct.unpack_from("<I", HEADER, 4), (28,))
        self.assertEqual(struct.unpack_from("<HHH", HEADER, 8), (32, 32, 32))
        self.assertEqual(struct.unpack_from("<I", HEADER, 16), (29,))
        self.assertEqual(struct.unpack_from("<QII", HEADER, 40), (56, 128, 4096))
        metrics = measure(load_rise_tex(self.path).samples, identity_samples(32))
        self.assertLessEqual(metrics.maximum_absolute, 0.5 / 255.0)
        self.assertAlmostEqual(metrics.maximum_absolute, 0.00189753320683117)
        self.assertAlmostEqual(metrics.mean_absolute, 0.000948766603415565)

    def test_all_header_bytes_are_checked(self):
        for offset in range(56):
            corrupted = bytearray(self.data)
            corrupted[offset] ^= 1
            self.path.write_bytes(corrupted)
            with self.assertRaises(RiseTexError):
                load_rise_tex(self.path)

    def test_reject_length_and_alpha(self):
        for invalid in (b"", self.data[:55], self.data[:-1], self.data + b"extra"):
            self.path.write_bytes(invalid)
            with self.assertRaises(RiseTexError):
                load_rise_tex(self.path)
        corrupted = bytearray(self.data)
        corrupted[59] = 0
        self.path.write_bytes(corrupted)
        with self.assertRaises(RiseTexError):
            load_rise_tex(self.path)

    def test_missing_file(self):
        with self.assertRaises(RiseTexError):
            load_rise_tex(self.directory / "missing.tex.28")

    def test_comparison_size_and_range_policy(self):
        cube = self.directory / "compare.cube"
        cube.write_text("LUT_3D_SIZE 2\n" + "0 0 0\n" * 8, encoding="utf-8")
        tex = load_rise_tex(self.path)
        with self.assertRaises(RiseTexError):
            compare_cube(tex, cube)
        cube.write_text("LUT_3D_SIZE 32\n" + "-0.2 0.5 1.2\n" * 32768, encoding="utf-8")
        with self.assertRaises(RiseTexError):
            compare_cube(tex, cube)
        metrics = compare_cube(tex, cube, clamp=True)
        self.assertTrue(math.isfinite(metrics.maximum_absolute))

    def test_asymmetric_reference_preserves_axes(self):
        self.path.write_bytes(reference_bytes(asymmetric_samples()))
        actual = load_rise_tex(self.path)
        self.assertLessEqual(measure(actual.samples, asymmetric_samples()).maximum_absolute, 0.5 / 255.0)
        self.assertNotEqual(actual.samples[31], actual.samples[31 * 32 * 32])
        self.assertEqual(len(actual.payload), FILE_SIZE - 56)


if __name__ == "__main__":
    unittest.main()
