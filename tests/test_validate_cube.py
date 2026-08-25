from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest


TOOL_PATH = pathlib.Path(__file__).parents[1] / "tools" / "validate_cube.py"
SPEC = importlib.util.spec_from_file_location("validate_cube", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
validate_cube = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validate_cube
SPEC.loader.exec_module(validate_cube)


def write_identity(path: pathlib.Path, size: int, perturbation: float = 0.0) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(f"LUT_3D_SIZE {size}\n")
        stream.write("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n")
        for index, sample in enumerate(validate_cube.identity_samples(size)):
            red, green, blue = sample
            if index == 0:
                red += perturbation
            stream.write(f"{red:.9g} {green:.9g} {blue:.9g}\n")


class ValidateCubeTests(unittest.TestCase):
    def test_hard_coded_red_fastest_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cube_path = pathlib.Path(directory) / "fixture.cube"
            cube_path.write_text(
                "LUT_3D_SIZE 2\n"
                "DOMAIN_MIN 0 0 0\n"
                "DOMAIN_MAX 1 1 1\n"
                "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
                "0 0 1\n1 0 1\n0 1 1\n1 1 1\n",
                encoding="utf-8",
            )
            cube = validate_cube.load_cube(cube_path)
            self.assertEqual(cube.samples[1], (1.0, 0.0, 0.0))
            self.assertEqual(cube.samples[2], (0.0, 1.0, 0.0))
            self.assertEqual(cube.samples[4], (0.0, 0.0, 1.0))
            metrics = validate_cube.measure(cube.samples, validate_cube.identity_samples(2))
            self.assertEqual(metrics.maximum_absolute, 0.0)

    def test_identity_and_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cube_path = pathlib.Path(directory) / "identity.cube"
            write_identity(cube_path, 4)
            cube = validate_cube.load_cube(cube_path)
            self.assertEqual(cube.size, 4)
            self.assertEqual(len(cube.samples), 64)
            self.assertEqual(cube.samples[0], (0.0, 0.0, 0.0))
            self.assertAlmostEqual(cube.samples[1][0], 1.0 / 3.0, places=8)
            self.assertEqual(cube.samples[4][1], cube.samples[1][0])
            metrics = validate_cube.measure(cube.samples, validate_cube.identity_samples(4))
            self.assertLess(metrics.maximum_absolute, 1.0e-8)

    def test_comparison_detects_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            reference_path = root / "reference.cube"
            changed_path = root / "changed.cube"
            write_identity(reference_path, 4)
            write_identity(changed_path, 4, perturbation=0.125)
            reference = validate_cube.load_cube(reference_path)
            changed = validate_cube.load_cube(changed_path)
            metrics = validate_cube.measure(changed.samples, reference.samples)
            self.assertAlmostEqual(metrics.maximum_absolute, 0.125)
            self.assertGreater(metrics.rms, 0.0)

    def test_rejects_incomplete_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cube_path = pathlib.Path(directory) / "bad.cube"
            cube_path.write_text("LUT_3D_SIZE 4\n0 0 0\n", encoding="utf-8")
            with self.assertRaises(validate_cube.CubeError):
                validate_cube.load_cube(cube_path)

    def test_rejects_non_default_domain(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cube_path = pathlib.Path(directory) / "domain.cube"
            write_identity(cube_path, 2)
            contents = cube_path.read_text(encoding="utf-8").replace("DOMAIN_MAX 1 1 1", "DOMAIN_MAX 2 2 2")
            cube_path.write_text(contents, encoding="utf-8")
            with self.assertRaises(validate_cube.CubeError):
                validate_cube.load_cube(cube_path)

    def test_rejects_duplicate_domain(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cube_path = pathlib.Path(directory) / "duplicate-domain.cube"
            write_identity(cube_path, 2)
            contents = cube_path.read_text(encoding="utf-8").replace(
                "DOMAIN_MIN 0 0 0", "DOMAIN_MIN 0 0 0\nDOMAIN_MIN 0 0 0"
            )
            cube_path.write_text(contents, encoding="utf-8")
            with self.assertRaises(validate_cube.CubeError):
                validate_cube.load_cube(cube_path)


if __name__ == "__main__":
    unittest.main()
