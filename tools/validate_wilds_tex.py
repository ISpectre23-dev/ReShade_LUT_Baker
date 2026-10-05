#!/usr/bin/env python3
"""Read-only Wilds TEX validator. File/node tests do not prove in-game equivalence."""

from __future__ import annotations

import argparse
import ctypes
import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import uuid
from dataclasses import dataclass

from validate_cube import CubeError, Metrics, identity_samples, load_cube, measure

SIZE = 33
PITCH = 512
PAYLOAD_SIZE = PITCH * SIZE * SIZE
FILE_BUDGET = PAYLOAD_SIZE + 65536 + 64
# Independent field declaration, not imported from the writer.
HEADER = struct.pack("<IIHHHBBIiI4B8sQII", 0x00584554, 241106027,
                     SIZE, SIZE, SIZE, 1, 16, 10, -1, 0, 0, 7, 0, 0,
                     bytes(8), 56, PITCH, PITCH * SIZE)


class WildsTexError(ValueError):
    pass


@dataclass(frozen=True)
class WildsTex:
    payload: bytes

    @property
    def samples(self):
        return tuple(struct.unpack_from("<3e", self.payload,
                     (index // SIZE) * PITCH + (index % SIZE) * 8)
                     for index in range(SIZE**3))


def validate_container(data: bytes) -> bytes:
    """Bound metadata and tile ranges before either native decoder sees the data."""
    if not 108 < len(data) <= FILE_BUDGET or data[:56] != HEADER:
        raise WildsTexError("file does not match the supported 33-cubed RGBA16F Wilds TEX profile")
    image_size, image_offset = struct.unpack_from("<II", data, 56)
    if image_size != len(data) - 64 or image_offset != 0:
        raise WildsTexError("compressed image length or offset does not match the file")
    compressed = data[64:]
    codec, complement, tiles, flags = struct.unpack_from("<BBHI", compressed)
    if (codec, complement, tiles, flags) != (4, 0xFB, 9, 1 | ((PAYLOAD_SIZE % 65536) << 2)):
        raise WildsTexError("unexpected GDeflate codec, tile count or decoded size")
    table = struct.unpack_from("<9I", compressed, 8)
    data_size = len(compressed) - 44
    offsets = (0, *table[1:], data_size)
    if any(not 0 <= start < end <= data_size or end % 4 for start, end in zip(offsets, offsets[1:])):
        raise WildsTexError("invalid or out-of-file GDeflate tile offset")
    if table[0] != data_size - offsets[-2]:
        raise WildsTexError("invalid final GDeflate tile size")
    return compressed


def validate_payload(payload: bytes) -> WildsTex:
    if len(payload) != PAYLOAD_SIZE:
        raise WildsTexError(f"expected {PAYLOAD_SIZE} decoded bytes, found {len(payload)}")
    for index in range(SIZE**3):
        offset = (index // SIZE) * PITCH + (index % SIZE) * 8
        rgba = struct.unpack_from("<4e", payload, offset)
        if not all(math.isfinite(v) for v in rgba):
            raise WildsTexError(f"non-finite RGBA value at sample {index}")
        if rgba[3] != 1:
            raise WildsTexError(f"alpha is not opaque at sample {index}")
    return WildsTex(payload)


def decode_texture(data: bytes, decompressor) -> WildsTex:
    compressed = validate_container(data)
    return validate_payload(decompressor(compressed, PAYLOAD_SIZE))


class DirectStorageDecoder:
    """Optional independent Microsoft codec, from a path explicitly supplied by the caller."""

    def __init__(self, dll: pathlib.Path):
        if sys.platform != "win32":
            raise WildsTexError("the DirectStorage decoder requires Windows")
        self.codec = ctypes.c_void_p()
        try:
            dll = dll.resolve(strict=True)
            # dstorage.dll loads its core lazily by basename. Load only the
            # explicit sibling first, rather than depending on the process PATH.
            # This optional validator path is never used by the add-on.
            if dll.name.lower() == "dstorage.dll":
                self.core_library = ctypes.WinDLL(str(dll.with_name("dstoragecore.dll").resolve(strict=True)), winmode=0x1100)
            self.library = ctypes.WinDLL(str(dll), winmode=0x1100)
            create = self.library.DStorageCreateCompressionCodec
            create.argtypes = [ctypes.c_uint8, ctypes.c_uint32, ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_void_p)]
            create.restype = ctypes.c_int32
            # IDStorageCompressionCodec from Microsoft's dstorage.h.
            iid = (ctypes.c_ubyte * 16).from_buffer_copy(
                uuid.UUID("84ef5121-9b43-4d03-b5c1-cc34606b262d").bytes_le)
            status = create(1, 1, ctypes.byref(iid), ctypes.byref(self.codec))
            if status < 0 or not self.codec.value:
                raise WildsTexError(f"DirectStorage codec creation failed: {status & 0xffffffff:#x}")
            vtable = ctypes.cast(self.codec, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            self.decompress = ctypes.WINFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))(vtable[4])
            self.release = ctypes.WINFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)(vtable[2])
        except (OSError, AttributeError) as exc:
            raise WildsTexError(f"cannot use the specified DirectStorage codec: {exc}. Specify a trusted dstorage.dll with its dstoragecore.dll beside it") from exc

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        if self.codec.value:
            self.release(self.codec)
            self.codec = ctypes.c_void_p()

    def __call__(self, compressed: bytes, expected: int) -> bytes:
        source = ctypes.create_string_buffer(compressed)
        destination = ctypes.create_string_buffer(expected)
        written = ctypes.c_size_t()
        status = self.decompress(self.codec, source, len(compressed), destination, expected, ctypes.byref(written))
        if status < 0 or written.value != expected:
            raise WildsTexError(f"DirectStorage decompression failed: {status & 0xffffffff:#x}, {written.value} bytes")
        return destination.raw


def load_wilds_tex(path: pathlib.Path, decoder: pathlib.Path | None = None,
                   codec: pathlib.Path | None = None) -> WildsTex:
    try:
        with path.open("rb") as source:
            data = source.read(FILE_BUDGET + 1)
        validate_container(data)
        if decoder is not None and codec is not None:
            raise WildsTexError("choose --decoder or --codec, not both")
        if codec is not None:
            with DirectStorageDecoder(codec) as decompress:
                return decode_texture(data, decompress)
        executable = decoder or pathlib.Path(__file__).resolve().parents[1] / "build/Release/wilds_tex_validate.exe"
        executable = executable.resolve()
        if not executable.is_file():
            raise WildsTexError("build wilds_tex_validate.exe or specify --decoder or --codec")
        # The native helper writes only into this owned validation directory.
        with tempfile.TemporaryDirectory(prefix="lut_baker_wilds_decode_") as directory:
            payload_path = pathlib.Path(directory) / "payload.bin"
            result = subprocess.run([str(executable), str(path.resolve()), "--payload", str(payload_path)],
                                    capture_output=True, text=True, timeout=20)
            if result.returncode != 0:
                raise WildsTexError(result.stderr.strip() or "native Wilds decoder failed")
            with payload_path.open("rb") as source:
                return validate_payload(source.read(PAYLOAD_SIZE + 1))
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise WildsTexError(f"cannot validate {path}: {exc}") from exc


def compare_cube(tex: WildsTex, cube_path: pathlib.Path) -> Metrics:
    cube = load_cube(cube_path)
    if cube.size != SIZE:
        raise WildsTexError("comparison CUBE must be 33-cubed; this validator does not resample")
    if any(abs(value) > 65504 for sample in cube.samples for value in sample):
        raise WildsTexError("comparison CUBE has values outside the supported 16-bit float range")
    # Check every node against independently rounded binary32 -> binary16.
    # This catches axis errors and precision loss, not just a loose tolerance.
    for actual, source in zip(tex.samples, cube.samples):
        expected = tuple(struct.unpack("<e", struct.pack("<e", struct.unpack("<f", struct.pack("<f", value))[0]))[0]
                         for value in source)
        if actual != expected:
            raise WildsTexError("a TEX node differs from the independently rounded CUBE; check ordering and export settings")
    return measure(tex.samples, cube.samples)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("inspect", "identity", "compare-cube"):
        sub = commands.add_parser(command)
        sub.add_argument("tex", type=pathlib.Path)
        backend = sub.add_mutually_exclusive_group()
        backend.add_argument("--decoder", type=pathlib.Path, help="path to the built wilds_tex_validate.exe")
        backend.add_argument("--codec", type=pathlib.Path, help="explicit path to a trusted Microsoft dstorage.dll, with dstoragecore.dll beside it")
        if command == "identity":
            sub.add_argument("--tolerance", type=float, default=2**-12 + 1e-6)
        if command == "compare-cube":
            sub.add_argument("cube", type=pathlib.Path)
    args = parser.parse_args()
    try:
        tex = load_wilds_tex(args.tex, args.decoder, args.codec)
        print("Wilds TEX v241106027: 33 x 33 x 33, RGBA16F, GDeflate, red-fastest, alpha 1")
        if args.command == "inspect":
            return 0
        if args.command == "identity":
            if not math.isfinite(args.tolerance) or args.tolerance < 0:
                raise WildsTexError("tolerance must be finite and non-negative")
            metrics = measure(tex.samples, identity_samples(SIZE))
        else:
            metrics = compare_cube(tex, args.cube)
            print("Every node matches the CUBE after independent binary32-to-binary16 rounding.")
        print(f"Maximum absolute RGB error: {metrics.maximum_absolute:.12g}")
        print(f"Mean absolute RGB error: {metrics.mean_absolute:.12g}")
        print(f"RMS RGB error: {metrics.rms:.12g}")
        print("Numeric tests only. In-game loading, interpolation and SDR/HDR color-domain equivalence remain separate tests.")
        return 0 if args.command == "compare-cube" or metrics.maximum_absolute <= args.tolerance else 1
    except (WildsTexError, CubeError, struct.error, OverflowError) as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
