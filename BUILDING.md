# Building ReShade LUT Baker

## Prerequisites

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.23 or newer
- Git when dependencies are fetched automatically
- Python 3.10 or newer for the CUBE/PNG/TEX validators and independent writer reference tests

The build is pinned to the official ReShade `v6.8.0` headers and the Dear ImGui revision used by that release.

Wilds export also builds the CPU GDeflate codec from Microsoft DirectStorage commit `c53f1499d5f67a61b69a1a348d22dcd2b4cb4ede` and its NVIDIA libdeflate submodule `8ba9502fb30d2bf728592d121f0d402e40c8cb05`. Only the codec subdirectory is built. No vcpkg, graphics demo, game DLL or DirectStorage runtime installation is required. Both libraries are linked statically; their upstream licenses are copied into `build/<configuration>/licenses/`. Keep those files and `THIRD_PARTY_NOTICES.md` in release packages.

PNG encoding uses Windows Imaging Component, the image encoder included with Windows. The Windows SDK supplies `windowscodecs.lib` and `ole32.lib`; no extra image dependency is fetched or bundled.

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

Automatic configuration fetches the pinned ReShade, ImGui and GDeflate revisions from their official GitHub repositories. To build without fetching, provide existing checkouts:

```powershell
cmake -S . -B build -A x64 `
  -DRESHADE_SDK_ROOT="C:\src\reshade" `
  -DIMGUI_ROOT="C:\src\imgui" `
  -DGDEFLATE_ROOT="C:\src\DirectStorage"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`RESHADE_SDK_ROOT` must contain `include/reshade.hpp`. `IMGUI_ROOT` must contain `imgui.h`. Use ReShade 6.8.0/API 20 commit `18deaa52de0c425a78b329e9cb3c497281cd00ec` and ImGui commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` to reproduce the audited build.

`GDEFLATE_ROOT` must contain `GDeflate/GDeflate/GDeflate.h` and the initialized `GDeflate/3rdparty/libdeflate` submodule. In that checkout, run `git submodule update --init GDeflate/3rdparty/libdeflate`. Use the pinned commits above.

## Test only the offline validator

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

No game or GPU is needed for the core and parser tests. The actual offscreen effect execution and GPU readback must be tested inside ReShade.

`wilds_tex_tests` adds fixed-profile, half-precision rounding, row padding, compression round-trip, malformed-offset, finite-range and non-overwriting-write tests. The Python suite independently checks the Wilds parser and binary16 nodes. `writer_fixture_reference` checks every decoded Wilds byte against Python's `struct` half-precision conversion and compares with a 33³ CUBE. It does not need a native game asset or DLL.

CTest runs `cube_core_tests`, `bake_control_tests`, `technique_preparation_tests`, `runtime_settings_tests`, `rise_tex_tests`, `export_formats_tests`, `png_lut_tests`, the Python validator suite and `writer_fixture_reference`. Bake-control tests retain the production retry/deadline checks with a fake clock and reload events. Technique-preparation tests model ReShade's default-resource prerequisite for never-enabled techniques and verify state restoration, bounded waits, failures and cancellation. Runtime-settings tests exercise the real restoration helper with a fake settings API: unsaved values/full order, repeated compilation, unselected effects, late cancellation/timeout recovery, context/layout changes and write vetoes. These policy tests do not execute ReShade or a GPU. Format tests cover 128³, custom boundaries/odd sizes, immutable snapshots and disk budgets. PNG tests cover both layouts and bit depths, low-byte preservation, conversion and the real Windows encoder. The writer reference test checks every TEX byte and 8-bit/16-bit PNG sample with independent Python decoding, plus CUBE identity/float round trips at preset and custom sizes. Its 128³ CUBE check streams rows to keep memory bounded. Do not omit Python for release validation.

## Generate offline test fixtures

With `BUILD_TESTING=ON` (the default), the build also produces `rise_tex_fixtures.exe`. It is validation tooling, not a converter or grading preset:

```powershell
build/Release/rise_tex_fixtures.exe build/fixtures
python -B tools/validate_rise_tex.py inspect build/fixtures/Identity32.tex.28
python -B tools/validate_rise_tex.py identity build/fixtures/Identity32.tex.28
python -B tools/validate_rise_tex.py compare-cube build/fixtures/Asymmetric32.tex.28 build/fixtures/Asymmetric32.cube
python -B tools/validate_png.py identity build/fixtures/Identity64_horizontal.png --layout horizontal
python -B tools/validate_png.py identity build/fixtures/Identity64_square.png --layout square
python -B tools/validate_png.py identity build/fixtures/Identity64_16bit_square.png --layout square
python -B tools/validate_png.py compare-cube build/fixtures/Asymmetric64_square.png build/fixtures/Asymmetric64.cube --layout square
python -B tools/validate_png.py compare-cube build/fixtures/Asymmetric64_16bit_square.png build/fixtures/Asymmetric64.cube --layout square
python -B tools/validate_wilds_tex.py identity build/fixtures/Identity33.tex.241106027 --tolerance 0
python -B tools/validate_wilds_tex.py compare-cube build/fixtures/Asymmetric33.tex.241106027 build/fixtures/Asymmetric33.cube
```

Use a new or empty destination; rerunning against existing names fails without overwriting them. The tool generates identity CUBEs at 2, 16, 32, 33, 64, 65 and 128; the existing Rise identity/asymmetric fixtures; 8-bit and 16-bit PNG identities in every supported size/layout; and an asymmetric 64³ CUBE with matching PNGs. The 16-bit PNG names contain `_16bit`; existing 8-bit names are unchanged. The axes fixture follows the transformation documented in [RISE_TEX28.md](docs/RISE_TEX28.md). These are synthetic writer fixtures, not GPU bakes or proposed presets. Allow at least 150 MiB of free space for the complete set. Generated files and binaries are ignored by Git.

The fixture tool also generates `Identity33.tex.241106027` and an asymmetric Wilds TEX/CUBE pair. These use the real writer and contain no game assets. Identity is exactly representable at steps of 1/32, so its offline maximum, mean and RMS numeric errors are zero. This does not prove visual neutrality in Wilds.

The Windows workflow publishes the add-on, README, project license and dependency license notices, plus separate synthetic fixtures and Wilds validation-tools artifacts. No proprietary shader or native game asset is bundled.

For an independent codec check, explicitly choose a **trusted Microsoft `dstorage.dll` from your own installation**, with its `dstoragecore.dll` beside it. The validator loads that sibling dependency before creating the codec. The following commands only decompress copied byte buffers in the validator process; they do not launch or modify the game:

```powershell
python -B tools/validate_wilds_tex.py identity build/fixtures/Identity33.tex.241106027 --tolerance 0 --codec "C:\Game\dstorage.dll"
python -B tools/validate_wilds_tex.py compare-cube build/fixtures/Asymmetric33.tex.241106027 build/fixtures/Asymmetric33.cube --codec "C:\Game\dstorage.dll"
```

Without `--codec`, the Python tool uses `build/Release/wilds_tex_validate.exe`. Specify `--decoder` when that executable is elsewhere. `wilds_tex_validate.exe <file>` also performs a bounded structural/decompression check directly. Its optional `--payload <new-file>` is for offline validation and never overwrites an existing file; it is not part of normal add-on export.

The TEX identity validator defaults to a half-byte quantization step plus float32 identity error. For an actual RGBA16F fallback identity, allow its separately reported GPU error as well (a conservative bound is `--tolerance 0.002461`). Do not use that relaxed bound to excuse errors in a float32 export.

## Release checklist

Wilds additionally requires a fixed 33³ export with its visible color-domain warning and no Clamp option. Validate a GPU identity and compare a real grading with a Custom 33³ CUBE while keeping parameters unchanged. Switch all four formats and check preferences and compound filename suffixes. Test negative/super-white values and rejection beyond the finite half-precision range.

With a compatible Wilds LUT manager, load the generated identity into one active slot and test reversible replacement before trying a real grading. Check slot blending, transitions, SDR and HDR separately, with the original ReShade grading disabled and other engine settings fixed. Do not mark native loading or visual equivalence as passed from byte/codec tests alone.

1. Build x64 Release against the pinned dependencies.
2. Run CTest with `--output-on-failure`.
3. Confirm the binary exports `NAME` and `DESCRIPTION`.
4. Install in a ReShade 6.8.0 or newer test game.
5. Run a zero-selection 64^3 identity export and validate it with `tools/validate_cube.py identity`.
6. Bake a known grading preset and inspect the CUBE metadata order. Use a compatible LUT reader to compare it against the original chain, with matching input domain and no double grading.
7. Choose Rise output, clear the selection and export a 32³ GPU identity with clipping off. Check the reported GPU identity and 8-bit quantization metrics separately, then run `validate_rise_tex.py identity` on the result.
8. Switch CUBE → Rise → CUBE and verify that the CUBE size preference returns and full filename suffixes are correct. Export a real RGB chain in both formats at 32³; compare nodes with `validate_rise_tex.py compare-cube`. Exercise explicit range rejection/clipping and export after an effect/preset reload.
9. When an authorized native loading path exists, test the identity/asymmetric fixtures and real grading in Rise, in SDR first and HDR separately. The published Rise Rehydrated 1.1.0 manager does not yet load custom LUTs; do not claim this step passed merely by copying files. See README for physical/logical paths and double-grading/LUT-mixing precautions.
10. Confirm `AUTHOR` is exported and **ISpectre23** appears in ReShade's Add-ons metadata. Abort during offscreen compilation/GPU wait and verify no file appears, no retry resumes after a late reload, and a subsequent identity export works once the fence completes. Select an FP-incompatible shader (for example Lilium's SDR TRC fix in its normal SDR configuration) and verify a bounded failure/timeout, no frame-rate compilation loop and no output. This requires real ReShade; the policy tests do not emulate a GPU or guarantee in-game shader loading.
11. Export CUBE identities at 128³ and Custom 33³/65³. Check GPU identity metrics and validate the files with `validate_cube.py identity`. Enter 1, 129 and a negative custom value; export must remain disabled. Switch CUBE, PNG and Rise and verify that preferences return unchanged.
12. Export PNG 64³ identities in Horizontal strip and Square tiles. Validate both with `validate_png.py`, then compare a real grading export against a CUBE of the same size. Configure `LUT.fx` for a 4096×64 strip; use a square-grid-capable reader for 512×512. Check all color axes, clipping and quantization separately. Unsupported square sizes must fail without padding or an automatic size change.
13. Start with a blank preset and no enabled techniques. Select a never-enabled technique such as DPX in the baker and export it. Verify normal resource preparation completes before offscreen compilation, the technique remains disabled between frames, no preset is written and the export finishes without a crash. Repeat after a reload, with an already initialized but disabled technique and with multiple selected techniques. Cancel during normal resource preparation and confirm a late completion cannot restart the bake. FakeHDR can also reproduce the initialization case, but its neighbor-dependent processing is not a valid RGB-only accuracy reference.
14. In normal mode with automatic preset saving off, change DPX parameters and enabled state without saving. Also change an unselected effect and reorder techniques. Export a previously unused CUBE size to force new offscreen compilation. Verify parameters/states/order survive, the preset file's bytes and modification time do not change, and the log reports live settings recovery. Export again with the same settings/size; compare both CUBE files numerically. Repeat with multiple techniques requiring compilation, then cancel during a fresh compilation and check recovery without any output or automatic retry. Changing presets must not apply the old backup to the new one. A genuine manual reload discards pending recovery instead of guessing identities. In Performance mode, grading must fail before queueing; identity must still export. Check failed-shader/restoration errors without claiming the unavailable shader was re-enabled.
15. Confirm PNG defaults to 8-bit. Export a 64³ identity in 16-bit with each layout, then validate it with `validate_png.py identity`. Compare a 16-bit grading PNG against a same-size CUBE while keeping parameters unchanged. Check the result/log reports 16 bits per channel and a matching quantization tolerance. Switch PNG → CUBE → Rise → PNG; the bit-depth preference must return unchanged. Verify the tooltip explains ReShade 6.8.0's 8-bit PNG loading path. Test native 16-bit precision in an external reader that preserves 16-bit samples, not through `LUT.fx`.
