# 1.1.0 development validation

Offline validation performed on 2026-10-04 from the 1.0.2 base commit `585409514bfc22f048288ad951f0ba4883412dc4`, on local branch `dev`.

## Build and automated checks

- Clean x64 Release build: CMake 3.31.6, NMake, Visual Studio 2026/MSVC 19.51; C++17 with `/W4` and `/permissive-`. No compiler/linker warnings. The documented Visual Studio 2022 CI build has not been run locally.
- ReShade 6.8.0/API 20 and ImGui dependency revisions remain unchanged.
- PE verified as x64 DLL with `NAME` and `DESCRIPTION` exports.
- CTest: **4/4 passed** (`cube_core_tests`, `rise_tex_tests`, `python_validator_tests`, `writer_fixture_reference`). Python discovery: **12/12 passed**.
- C++ writers' identity/asymmetric TEX files match **every byte** of the independent Python binary32 reference; header SHA-256 matches the supplied profile. Both native files are exactly 131128 bytes.
- CUBE identity, standard RGB order and binary32 round trips checked at 16³, 32³ and 64³. Existing selection/duplicate identity, FP16, metadata and out-of-range float tests pass.
- Range rejection, explicit clipping, component/sample counts, fixed alpha, malformed buffers, invalid names, compound suffix collisions, immutable settings snapshots, concurrent destination creation, failed streams, exceptions and stale temporary ownership tested.
- `technique_catalog.hpp` is unchanged. No auxiliary grading shaders are included.

## Measured offline errors

These metrics concern generated CPU fixtures, **not GPU execution** or visual equivalence:

| Fixture and comparison | Max absolute RGB | Mean absolute RGB | RMS RGB |
| --- | --- | --- | --- |
| Identity32 TEX vs ideal identity | 0.00189753320683 | 0.000948766603415 | 0.00111365065539 |
| Identity32 quantization vs float32 samples (C++ metrics) | 0.00189755243414 | 0.000948766903842 | 0.00111365002072 |
| Asymmetric32 quantization vs float32 samples (C++ metrics) | 0.00196078282361 | 0.000966410854454 | 0.00112084923460 |

The asymmetric fixture is `F(r,g,b)=(0.1+0.8*b, 0.1+0.8*r*r, 0.1+0.8*g)`, computed from ideal coordinates then rounded to float32 before writing. Comparison with the supplied double-precision research TEX gives an identical header/length, with at most one byte of difference in 3072 RGB components at rounding boundaries. Against the independent **float32** reference the output matches exactly. No gamma transform is applied.

Reproduce using the commands in [BUILDING.md](../BUILDING.md); persistent fixtures can be generated into `build/fixtures`. The read-only native validator supports `inspect`, `identity` and `compare-cube`.

## Lifecycle/code audit

The queued request owns format, effective size, basename/directory and range policy. GPU allocation and readback consume only this snapshot. The CPU job owns a copy of it, samples and metadata; it does not access ReShade, ImGui or a runtime pointer. Existing selected/requested technique logic, actual execution checks, resource transitions, fence submission/polling and teardown remain intact. No GPU waits or additional readbacks were introduced. A started writer can finish independently during runtime teardown; this is not an in-game device-reset test.

## Still required before release

1. In ReShade, export an unselected Rise identity with clipping off. Verify GPU identity tolerance separately from TEX quantization, then validate the file offline. Check format switching and a real RGB chain exported in both formats at 32³, with uniforms unchanged.
2. Exercise preset/effect reload, resize/reset and shutdown in the actual game. Code audit and CPU tests do not substitute for these runtime checks.
3. Using an authorized native loading path when available, compare identity/asymmetric fixtures and real grading in Rise. The published Rise Rehydrated 1.1.0 manager does not yet discover custom LUTs. Keep other overrides fixed and avoid double grading/engine LUT mixing.
4. Calibrate in SDR first, then HDR independently. Engine application stage, scene LUT mixing and the existing offscreen `BUFFER_*`/sRGB-view caveats can change the result. Native loading and visual equivalence have **not** been validated in this pass.

No game installation, mod/ZIP or Rise Rehydrated source was modified.

## Wilds export addition, 2026-10-05

This entry records the later Wilds work on `dev`, not a retest of the historical
in-game steps above. The release version remains 1.1.0.

- Clean x64 Release build with MSVC 19.51 and the pinned dependencies. No compiler or linker warnings. CTest: **10/10 groups passed**. Python discovery: **28/28 unit tests passed**.
- Added one fixed export profile: TEX 241106027, 33³, RGBA16F, one mip, 512-byte rows and a 557,568-byte decoded payload. No game asset or DLL is distributed.
- The bounded native decoder accepted all eleven RGBA16F map/event textures from the supplied local research. Their first 56 bytes match the writer profile. The separate 32³ RGBA8 neutral is intentionally not this export target.
- The actual writer's identity and asymmetric files were decoded independently with the trusted Microsoft DirectStorage codec from the user's installation. Its DLL signature was valid. Every asymmetric RGB node matched an independently rounded 33³ CUBE. Tests do not depend on that installation; their standard backend is the built CPU decoder.
- Numeric identity maximum/mean/RMS: **0 / 0 / 0**. Steps of 1/32 are exactly representable in binary16.
- Asymmetric fixture rounding maximum/mean/RMS against the actual binary32 inputs: **0.000195324420929 / 0.0000515386198807 / 0.0000813754079875**. The fixture maps `(r,g,b)` to `(-0.125+1.5*b, 0.1+0.8*r*r, 0.025+0.9*g)`; it is writer validation data, not a grading shader.
- Regression coverage includes all header fields, tile offsets/sizes, truncated files, opaque alpha, row padding, exact half inputs, ties/subnormals, non-finite/overflow rejection, negative/super-white preservation, low-compressibility data, atomic non-overwriting output, compound suffix collisions and immutable format snapshots.
- Existing GPU allocation/rendering, technique ordering/selection, synchronization, readback and live-settings recovery paths are unchanged. CUBE, PNG and Rise serialization sources are unchanged.

Native game loading, GPU execution at the new size, color-domain equivalence and
SDR/HDR visual comparisons have **not** been tested in this pass. The UI and
result warn that the numeric export does not convert the ReShade grading into
the logarithmic domain seen in the examined Wilds shader references. Use the
manual checks in [BUILDING.md](../BUILDING.md) before claiming native visual
support. No game installation was modified and no game process was launched.
