# Building ReShade LUT Baker

## Prerequisites

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.23 or newer
- Git, to fetch the dependencies
- Python 3.10 or newer, for the validation tools and part of the tests

## Build

From a Visual Studio developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The add-on is written to `build/Release/ReShadeLUTBaker.addon64`. Include the repository's `LICENSE` file in release packages; it also contains the dependency notices.

A Windows GitHub Actions workflow runs the same build and tests.

## Dependencies

CMake fetches two pinned dependencies from their official repositories:

| Dependency | Version | Used for |
| --- | --- | --- |
| ReShade | 6.8.0, commit `18deaa52de0c425a78b329e9cb3c497281cd00ec` | Add-on API headers |
| Dear ImGui | commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` | Panel UI, the revision ReShade 6.8.0 uses |

PNG encoding uses the encoder included with Windows, so it needs no extra library.

To build without fetching, point CMake at existing checkouts:

```powershell
cmake -S . -B build -A x64 `
  -DRESHADE_SDK_ROOT="C:\src\reshade" `
  -DIMGUI_ROOT="C:\src\imgui"
```

## Tests

CTest runs the C++ tests, the Python tests and a reference check that compares files from the real writers against an independent Python decoder. None of them needs a game or a GPU, so the actual bake still has to be tested inside ReShade.

To run only the Python tests:

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

## Validation tools

The tools in `tools/` check exported files offline. They compare values at the LUT points only.

| Tool | Commands |
| --- | --- |
| `validate_cube.py` | `inspect`, `identity`, `compare` |
| `validate_png.py` | `inspect`, `identity`, `compare-cube`; needs `--layout horizontal` or `--layout square` |
| `validate_rise_tex.py` | `inspect`, `identity`, `compare-cube` |

`identity` measures an identity export against the ideal values. `compare` and `compare-cube` compare an export against a CUBE of the same size.

```powershell
python tools/validate_cube.py identity Identity.cube --tolerance 1e-6
python tools/validate_cube.py compare reference.cube candidate.cube --tolerance 1e-6
python tools/validate_png.py compare-cube MyGrade.png MyGrade.cube --layout horizontal
python tools/validate_rise_tex.py compare-cube MyGrade.tex.28 MyGrade.cube
```

For PNG and Rise, add `--clamp` to `compare-cube` only when the file was exported with **Clamp to 0-1**.

### Test fixtures

The build also produces `rise_tex_fixtures.exe`, which writes synthetic identity and test LUTs in every format:

```powershell
build/Release/rise_tex_fixtures.exe build/fixtures
```

Use an empty folder with about 150 MiB free. Existing files are never overwritten.

## Release checklist

1. Build x64 Release against the pinned dependencies and run CTest.
2. Check that the add-on appears in ReShade's **Add-ons** tab with its name, author and description.
3. Export an identity LUT in every format and validate each file with its tool.
4. Bake a known grading in every format and compare it against a CUBE of the same size.
5. Switch between all formats and check that each one keeps its settings and file extension.
6. Check that out-of-range values and invalid custom sizes are rejected, and that **Clamp to 0-1** works.
7. Bake a technique that was never enabled, with unsaved parameter changes. Parameters, states and order must be restored, and the preset file must not change.
8. Cancel during shader compilation. No file may be written and the bake must not restart on its own.
9. In Performance mode, a technique bake must be rejected and an identity export must still work.
10. Load the exports where they will be used (`CubeLUT3D.fx` for CUBE, `LUT.fx` for PNG, in-game for Rise) and compare them with the original grading, in SDR and in HDR.
