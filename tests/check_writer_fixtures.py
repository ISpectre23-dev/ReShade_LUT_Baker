"""CTest integration: independently verify files from the real C++ writers."""

import pathlib
import subprocess
import sys
import tempfile

from test_validate_rise_tex import asymmetric_samples, float32, reference_bytes
from validate_cube import identity_samples, load_cube, measure
from validate_rise_tex import compare_cube, load_rise_tex


def main():
    with tempfile.TemporaryDirectory(prefix="lut_baker_writer_fixtures_") as temporary:
        directory = pathlib.Path(temporary)
        subprocess.run([sys.argv[1], str(directory)], check=True)
        identity_path = directory / "Identity32.tex.28"
        expected_identity = reference_bytes(tuple(float32(c) for c in row) for row in identity_samples(32))
        assert identity_path.read_bytes() == expected_identity, "C++ identity differs from independent reference"
        expected_grade = reference_bytes(asymmetric_samples())
        assert (directory / "Asymmetric32.tex.28").read_bytes() == expected_grade, "C++ asymmetric fixture differs from independent reference"
        for size in (16, 32, 64):
            path = directory / f"Identity{size}.cube"
            cube = load_cube(path)
            assert cube.size == size
            assert measure(cube.samples, identity_samples(size)).maximum_absolute <= 3e-8
            # All serialized RGB values must restore the actual float32 inputs.
            for actual, ideal in zip(cube.samples, identity_samples(size)):
                assert tuple(float32(v) for v in actual) == tuple(float32(v) for v in ideal)
        grade = load_cube(directory / "Asymmetric32.cube")
        for actual, expected in zip(grade.samples, asymmetric_samples()):
            assert tuple(float32(v) for v in actual) == expected
        for basename in ("Identity32", "Asymmetric32"):
            tex = load_rise_tex(directory / f"{basename}.tex.28")
            quant = compare_cube(tex, directory / f"{basename}.cube")
            assert quant.maximum_absolute <= 0.5 / 255.0 + 1e-8
            print(f"{basename}: C++ writer matches all reference bytes; TEX vs float CUBE max/mean/RMS "
                  f"{quant.maximum_absolute:.12g} / {quant.mean_absolute:.12g} / {quant.rms:.12g}")
        before = identity_path.read_bytes()
        retry = subprocess.run([sys.argv[1], str(directory)], capture_output=True, text=True)
        assert retry.returncode != 0 and identity_path.read_bytes() == before, "fixture tool must not overwrite exports"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
