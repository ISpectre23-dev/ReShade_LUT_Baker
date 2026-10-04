# Building ReShade LUT Baker

## Prerequisites

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.23 or newer
- Git when dependencies are fetched automatically
- Python 3.10 or newer for the CUBE/TEX validators and independent writer reference tests

The build is pinned to the official ReShade `v6.8.0` headers and the Dear ImGui revision used by that release.

## Configure and build

From a Visual Studio 2022 developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The add-on is produced at:

```text
build/Release/ReShadeLUTBaker.addon64
```

Copy that file next to the game's ReShade DLL or executable, according to the ReShade installation layout.

## Offline or pre-fetched dependencies

Automatic configuration fetches the pinned ReShade and ImGui revisions from their official GitHub repositories. To build without fetching, provide existing checkouts:

```powershell
cmake -S . -B build -A x64 `
  -DRESHADE_SDK_ROOT="C:\src\reshade" `
  -DIMGUI_ROOT="C:\src\imgui"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`RESHADE_SDK_ROOT` must contain `include/reshade.hpp`. `IMGUI_ROOT` must contain `imgui.h`. Use ReShade 6.8.0/API 20 commit `18deaa52de0c425a78b329e9cb3c497281cd00ec` and ImGui commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` to reproduce the audited build.

## Test only the offline validator

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

No game or GPU is needed for the core and parser tests. The actual offscreen effect execution and GPU readback must be tested inside ReShade.

CTest runs `cube_core_tests`, `rise_tex_tests`, the Python validator suite, and (when Python is available) `writer_fixture_reference`. The last test runs the real C++ writers and checks every TEX byte against an independent Python reference, plus CUBE identity/float round trips at all supported sizes. Do not omit Python for release validation.

## Generate offline test fixtures

With `BUILD_TESTING=ON` (the default), the build also produces `rise_tex_fixtures.exe`. It is validation tooling, not a converter or companion grading shader:

```powershell
build/Release/rise_tex_fixtures.exe build/fixtures
python -B tools/validate_rise_tex.py inspect build/fixtures/Identity32.tex.28
python -B tools/validate_rise_tex.py identity build/fixtures/Identity32.tex.28
python -B tools/validate_rise_tex.py compare-cube build/fixtures/Asymmetric32.tex.28 build/fixtures/Asymmetric32.cube
```

Use a new or empty destination; rerunning against existing fixture names deliberately fails without overwriting them. Files are `Identity16.cube`, `Identity32.cube`, `Identity64.cube`, `Identity32.tex.28`, `Asymmetric32.cube` and `Asymmetric32.tex.28`. The asymmetric fixture uses the transformation documented in [RISE_TEX28.md](docs/RISE_TEX28.md) to expose axis/channel mistakes. These are synthetic writer fixtures, not output from ReShade GPU execution or proposed visual presets. Generated LUTs and binaries are ignored by Git.

The Windows workflow publishes the add-on and companion CUBE preview shader, plus a separate synthetic fixtures artifact. No proprietary shader or native game asset is bundled.

The TEX identity validator defaults to a half-byte quantization step plus float32 identity error. For an actual RGBA16F fallback identity, allow its separately reported GPU error as well (a conservative bound is `--tolerance 0.002461`). Do not use that relaxed bound to excuse errors in a float32 export.

## Release checklist

1. Build x64 Release against the pinned dependencies.
2. Run CTest with `--output-on-failure`.
3. Confirm the binary exports `NAME` and `DESCRIPTION`.
4. Install in a ReShade 6.8.0 or newer test game.
5. Run a zero-selection 64^3 identity export and validate it with `tools/validate_cube.py identity`.
6. Bake a known grading preset, inspect the CUBE metadata order, disable the original chain and compare it visually through `ReShadeLUTPreview.fx`.
7. Choose Rise output, clear the selection and export a 32³ GPU identity with clipping off. Check the reported GPU identity and 8-bit quantization metrics separately, then run `validate_rise_tex.py identity` on the result.
8. Switch CUBE → Rise → CUBE and verify that the CUBE size preference returns and full filename suffixes are correct. Export a real RGB chain in both formats at 32³; compare nodes with `validate_rise_tex.py compare-cube`. Exercise explicit range rejection/clipping and export after an effect/preset reload.
9. When an authorized native loading path exists, test the identity/asymmetric fixtures and real grading in Rise, in SDR first and HDR separately. The published Rise Rehydrated 1.1.0 manager does not yet load custom LUTs; do not claim this step passed merely by copying files. See README for physical/logical paths and double-grading/LUT-mixing precautions.
