# ReShade LUT Baker

ReShade LUT Baker is a generic ReShade add-on that exports the combined RGB transformation of user-selected techniques as a floating-point 3D `.cube` LUT. It reproduces the useful part of the traditional neutral-colormap screenshot workflow without a screenshot, PNG, DDS, or 8/10-bit intermediate.

The add-on is not tied to ReGrade or any other shader package. A selected technique may be enabled or disabled in the current preset. The baker calls it directly and never changes the preset or the user's enabled technique states.

## What the bake does

For the default 64-point LUT, the pipeline is:

```text
64 x 64 x 64 identity RGB lattice, red axis fastest
    -> flattened to a 512 x 512 RGBA32F render target
    -> selected techniques, in ReShade's real relative order
    -> RGBA32F GPU readback (RGBA16F only as an explicit fallback)
    -> strict 64^3 floating-point CUBE file
```

ReShade's ordered technique list is used directly. The add-on does not sort by name or by effect file. Each selected technique is rendered on the same offscreen target, so the output of one becomes `COLOR` for the next. The `reshade_render_technique` event is checked after every call. A file is not written unless every requested technique reports execution in the exact expected order.

GPU work is submitted with a completion fence and polled on later presentations. The add-on does not perform a blocking GPU wait from inside ReShade's present callback. Once the readback fence is complete, samples are copied to CPU memory and CUBE serialization runs on a background worker. An in-progress worker is safely drained if its effect runtime is destroyed.

Backend detail: ReShade 6.8.0 implements the OpenGL fence signal with `glFinish`, so OpenGL can still incur a one-time synchronous hitch during export. Vulkan requires timeline-semaphore support for the completion fence. If the active backend cannot create or signal the required fence, the baker fails explicitly and does not reuse an unsynchronized target.

The 64^3 lattice contains 262,144 RGB samples. CUBE values are written with `std::numeric_limits<float>::max_digits10`, which is sufficient for binary32 round trips. Shader outputs are not clamped before export, so finite values below 0 or above 1 are retained.

## Important accuracy boundary

A custom floating-point render target causes ReShade 6.8.0 to compile a custom effect permutation for the bake target. Its built-ins describe the bake resource, not the gameplay back buffer:

- `BUFFER_WIDTH` and `BUFFER_HEIGHT` are the flattened lattice dimensions.
- `BUFFER_COLOR_FORMAT` and `BUFFER_COLOR_BIT_DEPTH` describe RGBA32F or the RGBA16F fallback.
- `BUFFER_COLOR_SPACE` is `unknown` (`0`).
- A floating-point render target has no separate sRGB SRV or RTV, so `SRGBTexture` and `SRGBWriteEnabled` reads/writes behave linearly and cannot reproduce an sRGB back buffer view exactly.

The public add-on API has no supported way to use an FP32 offscreen target while retaining the gameplay permutation's dimensions, format and color-space built-ins. ReShade LUT Baker therefore shows this warning in the UI and records both gameplay and bake buffer information in every CUBE file. This matters most for HDR and for any shader that conditionally compiles from `BUFFER_*` values.

Executing a selected subset also calls ReShade's begin/finish effect events once around each direct technique call. Normal full-chain rendering emits those events once around the whole chain. Another installed add-on that reacts to those events can therefore alter the bake. ReShade exposes no public subset call with a single shared event wrapper, so ordered direct calls remain the least invasive supported implementation and this difference is recorded as a warning.

ReShade's public technique API exposes an effect filename, not its full search-path location. If multiple search paths contain the same `.fx` basename and the same technique name, the UI disambiguates them as ordered instances. Recheck those selections after a reload or manual reorder because the API provides no stable path-level identity for otherwise identical entries.

The LUT represents the shader transformation evaluated on normalized RGB inputs from 0 to 1. Its precision does not depend on Windows output being SDR 8-bit, SDR 10-bit or HDR10, but permutation-dependent shader behavior can still differ for the reason above. Do not treat an export as equivalent until it has been visually checked in the target preset and display mode.

## What a 3D LUT cannot represent

A 3D LUT can only encode a deterministic mapping from one RGB triplet to another. Do not select techniques that depend on:

- screen position, resolution or aspect ratio
- neighboring pixels, sharpening, blur or bloom
- depth, motion vectors or scene geometry
- previous frames, time, animation or temporal history
- random noise, film grain, dithering or stochastic state
- vignette, chromatic aberration, lens effects or local masks

The add-on deliberately does not attempt to classify shaders. The user owns the selection. A grading technique that depends on an unselected technique's private resources or side effects may also be unsuitable even if its final pass looks RGB-only.

## Requirements

- Windows x64
- ReShade 6.8.0 or newer with full add-on support
- A renderer supported by ReShade whose graphics queue can render/copy RGBA32F or RGBA16F textures and create a completion fence

The implementation targets ReShade add-on API 20 and was audited and compiled against the official `v6.8.0` source. Older ReShade versions are not supported.

## Installation

1. Build or download `ReShadeLUTBaker.addon64`.
2. Copy it next to the game's ReShade DLL, normally the same directory as the game executable.
3. Start the game and open ReShade's **Add-ons** tab.
4. Confirm that **ReShade LUT Baker** is present.

ReShade must be installed in a configuration that permits third-party add-ons. If the panel is absent, check `ReShade.log` and the ReShade installation variant before troubleshooting the baker.

## Usage

1. Configure the grading shaders and their uniforms as desired.
2. Open **Add-ons > ReShade LUT Baker**.
3. Leave **LUT Size** at 64 unless a smaller test LUT is intentional.
4. Select exactly the techniques to bake. The list is displayed in ReShade execution order.
5. Optionally enter a filename. An empty field creates `ReShade_LUT_YYYYMMDD_HHMMSS.cube`.
6. Select **Update preview alias** only if overwriting `ReShade_LUT_Latest.cube` is desired.
7. Press **Export LUT**.

Disabled techniques can be selected and baked without enabling them in the preset. **Select currently enabled** is only a convenience for populating the selection. The original enabled/disabled state is never modified.

During the first attempt ReShade may need to compile the custom offscreen permutation. The baker retries for up to 60 seconds. It fails without writing a file if a selected technique disappears, compilation never completes, the execution event sequence differs, allocation/readback fails, a non-finite result is found, or the file cannot be committed.

## Output

Files are written under:

```text
<ReShade base directory>/LUT_Bakes/
```

Existing named exports are not overwritten. A numeric suffix is added instead. The optional `ReShade_LUT_Latest.cube` preview alias is the only intentional overwrite and is disabled by default.

Each export contains:

- `LUT_3D_SIZE`
- `DOMAIN_MIN 0.0 0.0 0.0`
- `DOMAIN_MAX 1.0 1.0 1.0`
- the verified selected technique order
- exporter and minimum ReShade/API version
- graphics API, gameplay buffer and bake buffer descriptions
- accuracy warnings

Rows use standard CUBE order with red changing fastest, then green, then blue. The default 0 to 1 domain is also the only domain handled correctly by ReShade 6.8.0's native CUBE texture loader.

## Companion preview shader

[`shaders/ReShadeLUTPreview.fx`](shaders/ReShadeLUTPreview.fx) loads the optional `ReShade_LUT_Latest.cube` alias as a native RGBA32F 3D texture. It offers tetrahedral and trilinear interpolation plus apply, split and absolute input-to-LUT difference views.

To use it:

1. Copy the shader into an Effect Search Path.
2. Add `LUT_Bakes` to ReShade's Texture Search Paths.
3. Enable **Update preview alias** for the export.
4. Reload effects after exporting because ReShade does not hot-reload changed 3D texture files.
5. Disable the original grading techniques and enable `ReShadeLUTPreview` to inspect the generated LUT on normal game content.

The shader defaults to a 64^3 alias. If a 16^3 or 32^3 LUT is exported, change `LUT_BAKER_CUBE_SIZE` through ReShade's preprocessor definitions or in the shader and reload effects. `LUT_BAKER_CUBE_FILENAME` can similarly select another lowercase `.cube` filename.

The absolute-difference mode shows the magnitude of the LUT's change relative to its own input. It is not a simultaneous pixel-perfect comparison against a separate live grading chain.

## Validation

The repository intentionally contains no synthetic grading effects. Validation stays on the real exporter path:

- Export with no selected techniques to run the complete GPU identity bake. The UI reports maximum absolute RGB error, mean absolute RGB error and RMS RGB error. The file is withheld if maximum error exceeds `1e-6` on RGBA32F or `5e-4` on the RGBA16F fallback.
- The exporter verifies the exact `reshade_render_technique` sequence before any CUBE is written and records that sequence in the file.
- `tools/validate_cube.py inspect` strictly checks the declaration, default domain, finite values and exact `N^3` row count.
- `tools/validate_cube.py identity` compares an export with the ideal red-fastest identity lattice.
- `tools/validate_cube.py compare` compares every lattice node of two generated LUTs and reports maximum, mean and RMS RGB error.
- The companion shader provides the practical visual apply/split/difference path on game content.

Examples:

```powershell
python tools/validate_cube.py inspect "C:\Game\LUT_Bakes\ReShade_LUT_20260825_120000.cube"
python tools/validate_cube.py identity "C:\Game\LUT_Bakes\Identity.cube" --tolerance 1e-6
python tools/validate_cube.py compare reference.cube candidate.cube --tolerance 1e-6
```

The offline metrics measure values at lattice nodes. Differences at colors between nodes additionally include 3D-LUT interpolation error and approximation error, which are not exporter readback errors.

## Building and tests

See [BUILDING.md](BUILDING.md) for the reproducible Visual Studio 2022/CMake commands and offline dependency options. A Windows GitHub Actions workflow builds the `.addon64` and runs the C++ and Python tests.

The C++ tests cover lattice dimensions/order, identity metrics, FP16 fallback conversion, filename safety and atomic non-overwriting CUBE output. Python tests cover strict parsing, ordering, identity metrics, comparison metrics and malformed-file rejection.

## Technical references

The implementation follows the official ReShade 6.8.0 source and headers:

- [ReShade add-on API header](https://github.com/crosire/reshade/blob/v6.8.0/include/reshade.hpp)
- [`effect_runtime::render_technique` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp#L1245-L1314)
- [Effect technique execution and implicit COLOR copy](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L4066-L4325)
- [Native CUBE loader](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L2931-L3114)

No proprietary shader source is copied, redistributed or required.

## License

No license has been selected by the repository owner yet. See [LICENSE](LICENSE). MIT is a practical recommendation for this small add-on, but the owner should make that choice before public redistribution.
