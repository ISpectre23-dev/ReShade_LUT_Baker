"""CTest integration: independently verify files from the real C++ writers."""

import pathlib
import math
import subprocess
import struct
import sys
import tempfile

from test_validate_rise_tex import asymmetric_samples, float32, reference_bytes
from validate_cube import identity_samples, load_cube, measure
from validate_rise_tex import compare_cube, load_rise_tex
from validate_png import load_png


def check_large_identity(path, size):
    """Check every 128-cubed row without building a multi-million-tuple Python buffer."""
    axis = tuple(float32(i / (size - 1)) for i in range(size))
    maximum = absolute_sum = squared_sum = 0.0
    count = 0
    saw_size = False
    with path.open(encoding="utf-8") as source:
        for raw in source:
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(("TITLE ", "DOMAIN_MIN ", "DOMAIN_MAX ")):
                continue
            if line.startswith("LUT_3D_SIZE "):
                assert int(line.split()[1]) == size and not saw_size
                saw_size = True
                continue
            assert saw_size and count < size**3
            actual = tuple(float(value) for value in line.split())
            expected = (axis[count % size], axis[(count // size) % size], axis[count // (size * size)])
            assert tuple(float32(value) for value in actual) == expected, "128 float round trip or ordering failed"
            ideal = ((count % size) / (size - 1), ((count // size) % size) / (size - 1), (count // (size * size)) / (size - 1))
            for a, b in zip(actual, ideal):
                difference = abs(a - b)
                maximum = max(maximum, difference)
                absolute_sum += difference
                squared_sum += difference * difference
            count += 1
    # Binary32 division has at most half an ULP near 1, and nine significant
    # decimal digits add at most 0.5e-9 when parsed as binary64. Float32 round
    # trips were checked exactly above; this bound is only for ideal-vs-text metrics.
    assert count == size**3 and maximum <= 2**-25 + 0.5e-9
    print(f"Identity{size}: {count:,} rows verified; max/mean/RMS {maximum:.12g} / "
          f"{absolute_sum / (count * 3):.12g} / {math.sqrt(squared_sum / (count * 3)):.12g}")


def main():
    with tempfile.TemporaryDirectory(prefix="lut_baker_writer_fixtures_") as temporary:
        directory = pathlib.Path(temporary)
        subprocess.run([sys.argv[1], str(directory)], check=True)
        identity_path = directory / "Identity32.tex.28"
        expected_identity = reference_bytes(tuple(float32(c) for c in row) for row in identity_samples(32))
        assert identity_path.read_bytes() == expected_identity, "C++ identity differs from independent reference"
        expected_grade = reference_bytes(asymmetric_samples())
        assert (directory / "Asymmetric32.tex.28").read_bytes() == expected_grade, "C++ asymmetric fixture differs from independent reference"
        for size in (2, 16, 32, 33, 64, 65):
            path = directory / f"Identity{size}.cube"
            cube = load_cube(path)
            assert cube.size == size
            assert measure(cube.samples, identity_samples(size)).maximum_absolute <= 3e-8
            # All serialized RGB values must restore the actual float32 inputs.
            for actual, ideal in zip(cube.samples, identity_samples(size)):
                assert tuple(float32(v) for v in actual) == tuple(float32(v) for v in ideal)
        check_large_identity(directory / "Identity128.cube", 128)
        grade = load_cube(directory / "Asymmetric32.cube")
        for actual, expected in zip(grade.samples, asymmetric_samples()):
            assert tuple(float32(v) for v in actual) == expected
        for basename in ("Identity32", "Asymmetric32"):
            tex = load_rise_tex(directory / f"{basename}.tex.28")
            quant = compare_cube(tex, directory / f"{basename}.cube")
            assert quant.maximum_absolute <= 0.5 / 255.0 + 1e-8
            print(f"{basename}: C++ writer matches all reference bytes; TEX vs float CUBE max/mean/RMS "
                  f"{quant.maximum_absolute:.12g} / {quant.mean_absolute:.12g} / {quant.rms:.12g}")
        for size in (16, 32, 64, 128):
            for layout in ("horizontal", "square"):
                if layout == "square" and size not in (16, 64):
                    continue
                for depth in (8, 16):
                    depth_suffix = "_16bit" if depth == 16 else ""
                    png = load_png(directory / f"Identity{size}{depth_suffix}_{layout}.png", layout)
                    assert png.size == size and png.bit_depth == depth and png.software.startswith("ReShade LUT Baker ")
                    maximum = (1 << depth) - 1
                    for actual, ideal in zip(png.samples(), identity_samples(size)):
                        expected = tuple(math.floor(float32(v) * maximum + 0.5) / maximum for v in ideal)
                        assert actual == expected, "PNG samples differ from independent identity reference"
                    quant = measure(png.samples(), identity_samples(size))
                    print(f"Identity{size} PNG {layout} {depth}-bit: all pixels verified; max/mean/RMS "
                          f"{quant.maximum_absolute:.12g} / {quant.mean_absolute:.12g} / {quant.rms:.12g}")
                    assert quant.maximum_absolute <= 0.5 / maximum + 3e-8
        grade64 = load_cube(directory / "Asymmetric64.cube")
        for layout in ("horizontal", "square"):
            for depth in (8, 16):
                depth_suffix = "_16bit" if depth == 16 else ""
                png = load_png(directory / f"Asymmetric64{depth_suffix}_{layout}.png", layout)
                maximum = (1 << depth) - 1
                assert png.bit_depth == depth
                for actual, sample in zip(png.samples(), grade64.samples):
                    expected = tuple(math.floor(float32(v) * maximum + 0.5) / maximum for v in sample)
                    assert actual == expected, "PNG axis/channel/codec reference failed"
                quant = measure(png.samples(), grade64.samples)
                assert quant.maximum_absolute <= 0.5 / maximum + 1e-8
                if depth == 16:
                    assert any(value % 257 for value in struct.unpack_from(">3H", png.pixels, 0)), "16-bit output must retain non-repeated low bytes"
        before = identity_path.read_bytes()
        retry = subprocess.run([sys.argv[1], str(directory)], capture_output=True, text=True)
        assert retry.returncode != 0 and identity_path.read_bytes() == before, "fixture tool must not overwrite exports"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
