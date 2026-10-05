import pathlib
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from validate_wilds_tex import (FILE_BUDGET, HEADER, PAYLOAD_SIZE, WildsTexError,
                               decode_texture, validate_container, validate_payload)


def reference_payload(samples):
    payload = bytearray(PAYLOAD_SIZE)
    for index, sample in enumerate(samples):
        struct.pack_into("<4e", payload, (index // 33) * 512 + (index % 33) * 8, *sample, 1.0)
    return bytes(payload)


def asymmetric_samples():
    return [tuple(struct.unpack("<f", struct.pack("<f", value))[0] for value in
                  (-0.125 + 1.5 * b / 32, 0.1 + 0.8 * (r / 32)**2, 0.025 + 0.9 * g / 32))
            for b in range(33) for g in range(33) for r in range(33)]


def fake_container():
    # Parser fixture only. A fake decoder below supplies the expected payload;
    # CTest separately exercises real GDeflate streams from the C++ writer.
    stream = struct.pack("<BBHI9I", 4, 0xFB, 9, 1 | ((PAYLOAD_SIZE % 65536) << 2),
                         4, *range(4, 36, 4)) + bytes(36)
    return HEADER + struct.pack("<II", len(stream), 0) + stream


class WildsValidatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.identity = [(r / 32, g / 32, b / 32) for b in range(33) for g in range(33) for r in range(33)]
        cls.payload = reference_payload(cls.identity)

    def test_identity_padding_order_and_channels(self):
        tex = decode_texture(fake_container(), lambda compressed, expected: self.payload)
        self.assertEqual(tex.samples, tuple(self.identity))
        self.assertEqual(len(tex.payload), 557568)
        for row in range(1089):
            self.assertEqual(tex.payload[row * 512 + 264:(row + 1) * 512], bytes(248))

    def test_float_values_are_not_clamped_or_gamma_converted(self):
        samples = asymmetric_samples()
        tex = validate_payload(reference_payload(samples))
        self.assertEqual(tex.samples[0][0], -0.125)
        self.assertEqual(tex.samples[-1][0], 1.375)
        self.assertEqual(tex.samples[1234], tuple(struct.unpack("<e", struct.pack("<e", v))[0] for v in samples[1234]))

    def test_all_header_fields_and_truncations_before_decoder(self):
        data = fake_container()
        def forbidden(*unused):
            self.fail("malformed header must not reach native decoder")
        for length in (0, 4, 55, 56, 63, 64, 71, 107, 108):
            with self.assertRaises(WildsTexError):
                decode_texture(data[:length], forbidden)
        for offset in range(72):
            invalid = bytearray(data)
            invalid[offset] ^= 0x80
            with self.assertRaises(WildsTexError):
                decode_texture(bytes(invalid), forbidden)

    def test_offsets_sizes_and_huge_input_rejected(self):
        for tile in range(9):
            data = bytearray(fake_container())
            struct.pack_into("<I", data, 72 + tile * 4, 0xffffffff)
            with self.assertRaises(WildsTexError):
                validate_container(bytes(data))
        with self.assertRaises(WildsTexError):
            validate_container(fake_container() + b"extra")
        with self.assertRaises(WildsTexError):
            validate_container(bytes(FILE_BUDGET + 1))

    def test_wrong_decoder_size_nonfinite_and_alpha_rejected(self):
        for payload in (self.payload[:-1], self.payload + b"extra"):
            with self.assertRaises(WildsTexError):
                validate_payload(payload)
        for half in (0x7c00, 0xfc00, 0x7e00):
            payload = bytearray(self.payload)
            struct.pack_into("<H", payload, 0, half)
            with self.assertRaises(WildsTexError):
                validate_payload(bytes(payload))
        payload = bytearray(self.payload)
        struct.pack_into("<e", payload, 6, 0.5)
        with self.assertRaises(WildsTexError):
            validate_payload(bytes(payload))

    def test_compared_cube_requires_exact_half_samples(self):
        from validate_wilds_tex import compare_cube
        with tempfile.TemporaryDirectory(prefix="lut_baker_wilds_python_") as directory:
            cube_path = pathlib.Path(directory) / "identity.cube"
            cube_path.write_text("LUT_3D_SIZE 33\n" + "".join(f"{r} {g} {b}\n" for r, g, b in self.identity), encoding="utf-8")
            self.assertEqual(compare_cube(validate_payload(self.payload), cube_path).maximum_absolute, 0)
            exchanged = reference_payload((b, g, r) for r, g, b in self.identity)
            with self.assertRaises(WildsTexError):
                compare_cube(validate_payload(exchanged), cube_path)
            cube_path.write_text("LUT_3D_SIZE 2\n" + "0 0 0\n" * 8, encoding="utf-8")
            with self.assertRaises(WildsTexError):
                compare_cube(validate_payload(self.payload), cube_path)

    def test_optional_codec_preloads_only_the_explicit_sibling(self):
        from validate_wilds_tex import DirectStorageDecoder
        with tempfile.TemporaryDirectory(prefix="lut_baker_wilds_codec_mock_") as directory:
            root = pathlib.Path(directory)
            for name in ("dstorage.dll", "dstoragecore.dll"):
                (root / name).write_bytes(b"mock, not a DLL")
            loaded = []
            def fake_create(*unused):
                return -2147467263  # E_NOTIMPL; no COM object to release.
            def fake_load(path, **options):
                loaded.append(pathlib.Path(path))
                self.assertEqual(options["winmode"], 0x1100)
                return SimpleNamespace(DStorageCreateCompressionCodec=fake_create)
            with patch("validate_wilds_tex.sys.platform", "win32"), patch("validate_wilds_tex.ctypes.WinDLL", fake_load, create=True):
                with self.assertRaisesRegex(WildsTexError, "creation failed"):
                    DirectStorageDecoder(root / "dstorage.dll")
            self.assertEqual(loaded, [(root / "dstoragecore.dll").resolve(), (root / "dstorage.dll").resolve()])


if __name__ == "__main__":
    unittest.main()
