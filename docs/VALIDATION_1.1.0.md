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
