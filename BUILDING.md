# Building ReShade LUT Baker

## Prerequisites

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.23 or newer
- Git when dependencies are fetched automatically
- Python 3.10 or newer for the optional CUBE validator tests

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

## Release checklist

1. Build x64 Release against the pinned dependencies.
2. Run CTest with `--output-on-failure`.
3. Confirm the binary exports `NAME` and `DESCRIPTION`.
4. Install in a ReShade 6.8.0 or newer test game.
5. Run a zero-selection 64^3 identity export and validate it with `tools/validate_cube.py identity`.
6. Bake a known grading preset, inspect the CUBE metadata order, disable the original chain and compare it visually through `ReShadeLUTPreview.fx`.
