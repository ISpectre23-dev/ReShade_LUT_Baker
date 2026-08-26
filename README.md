# ReShade LUT Baker

ReShade LUT Baker is a ReShade add-on that exports the combined RGB transformation of selected techniques as a floating-point 3D `.cube` LUT.

It evaluates a neutral RGB lattice directly through ReShade, preserving the real technique execution order and avoiding screenshot, PNG, DDS, or other image intermediates. The default export is a 64³ LUT containing 262,144 RGB samples.

Selected techniques can be baked whether they are currently enabled or disabled. The baker executes them directly and does not change the preset or the user's enabled technique states.

An optional companion shader, [`ReShadeLUTPreview.fx`](shaders/ReShadeLUTPreview.fx), is included for applying and comparing exported LUTs inside ReShade.

## Features

- Exports standard floating-point `.cube` 3D LUT files.
- 64³ output by default, with 16³ and 32³ also available.
- Uses ReShade's actual relative technique order.
- Supports baking selected techniques without changing their enabled state.
- Uses an RGBA32F bake target when available, with RGBA16F as an explicit fallback.
- Preserves finite shader output below 0 and above 1 instead of clamping it before export.
- Verifies the expected ReShade technique execution sequence before writing a LUT.
- Uses GPU completion fences before readback.
- Writes LUT files atomically and never overwrites an existing named export.
- Includes identity/error validation tools and an in-game LUT preview shader.

## What can be baked

A 3D LUT can represent a deterministic mapping from one RGB triplet to another.

The best candidates are color-grading techniques whose output depends only on the input pixel color. Multiple compatible techniques can be selected and baked together, and their transformations are applied in ReShade's real relative execution order.

## What cannot be baked

Do not select techniques that depend on information a 3D LUT cannot represent, including:

- screen position, resolution or aspect ratio
- neighboring pixels, sharpening, blur or bloom
- depth, motion vectors or scene geometry
- previous frames, time, animation or temporal history
- random noise, film grain, dithering or stochastic state
- vignette, chromatic aberration, lens effects or local masks

A grading technique that depends on private resources or side effects from another technique may also be unsuitable even if its final pass appears to be RGB-only.

The baker deliberately does not attempt to classify shaders automatically. The selected techniques should be visually checked against the original chain after export.

## Requirements

- Windows x64
- ReShade 6.8.0 with full add-on support or newer*
- A renderer supported by ReShade whose graphics queue can render/copy RGBA32F or RGBA16F textures and create a completion fence

*The implementation targets ReShade add-on API 20 and is audited and compiled against the official ReShade 6.8.0 source. Newer ReShade versions are expected to work while they remain API-compatible, but future compatibility is not guaranteed. Older versions are not supported.

## Installation

1. Build or download `ReShadeLUTBaker.addon64`.
2. Copy it next to the game's ReShade DLL, normally in the same directory as the game executable.
3. Start the game and open ReShade's **Add-ons** tab.
4. Confirm that **ReShade LUT Baker** is present.

ReShade must be installed in a configuration that permits third-party add-ons. If the panel is absent, check `ReShade.log` and the ReShade installation variant before troubleshooting the baker.

The optional preview shader can be copied into any configured ReShade Effect Search Path.

## Usage

1. Configure the grading techniques and their uniforms as desired.
2. Open **Add-ons > ReShade LUT Baker**.
3. Leave **LUT Size** at 64 unless a smaller LUT is intentional.
4. Select exactly the techniques to bake. They are shown in ReShade execution order.
5. Optionally enter an output filename. Leaving the field empty creates `ReShade_LUT_YYYYMMDD_HHMMSS.cube`.
6. Press **Export LUT**.

**Select currently enabled** replaces the current selection with exactly the techniques that are enabled at that moment.

After effects are reloaded, valid selections are preserved and techniques that no longer exist are removed automatically. An active bake keeps its own immutable selection snapshot, so a later catalog refresh cannot silently change the requested export.

During the first attempt, ReShade may need to compile a custom offscreen effect permutation. The baker retries for up to 60 seconds. It fails without writing a file if a requested technique disappears, compilation never completes, the execution sequence differs from the expected order, allocation/readback fails, a non-finite result is found, or the file cannot be committed.

## Output

Exports are written to:

```text
<ReShade base directory>/LUT_Bakes/
```

Existing exports are not overwritten. If a requested filename already exists, a numeric suffix such as `_001` is added automatically.

Each CUBE file contains:

- `LUT_3D_SIZE`
- `DOMAIN_MIN 0.0 0.0 0.0`
- `DOMAIN_MAX 1.0 1.0 1.0`
- the verified selected technique order
- exporter and minimum ReShade/API version information
- graphics API information
- gameplay and bake buffer descriptions
- relevant accuracy warnings

Rows use standard CUBE order with red changing fastest, then green, then blue.

CUBE values are serialized with `std::numeric_limits<float>::max_digits10`, which is sufficient for binary32 round trips. Finite values outside the 0 to 1 output range are retained.

The default 0 to 1 input domain is also the domain handled correctly by ReShade 6.8.0's native CUBE texture loader.

## Previewing a LUT

[`shaders/ReShadeLUTPreview.fx`](shaders/ReShadeLUTPreview.fx) loads an exported CUBE as a native RGBA32F 3D texture.

It provides:

- tetrahedral or trilinear interpolation
- **Apply LUT** view
- **Split: Input | LUT** view with an aligned one-pixel divider
- **Absolute difference** view with adjustable gain

To use it:

1. Copy `ReShadeLUTPreview.fx` into a ReShade Effect Search Path.
2. Add the directory containing the exported CUBE files, normally `LUT_Bakes`, to ReShade's Texture Search Paths.
3. In ReShade's preprocessor definitions, set:

   ```text
   LUT_BAKER_CUBE_FILENAME="MyPreset.cube"
   ```

   Replace `MyPreset.cube` with the basename of the exported LUT you want to preview.

4. If the exported LUT is not 64³, also set `LUT_BAKER_CUBE_SIZE` to the matching size.
5. Reload ReShade effects.
6. Disable the original grading techniques and enable **ReShade LUT Preview** to inspect the LUT on normal game content.

The shader contains `LUT_Name.cube` as a fallback placeholder filename, but normal configuration should be done through ReShade's preprocessor definitions.

ReShade does not hot-reload changed 3D texture files, so reload effects after changing the selected CUBE or replacing its contents.

The absolute-difference mode shows the magnitude of the LUT's change relative to its own input. It is not a simultaneous pixel-perfect comparison against a separate live grading chain.

## Accuracy and technical limitations

### Bake target and effect permutations

For the default 64³ LUT, the identity lattice is flattened into a 512 × 512 floating-point render target:

```text
64 × 64 × 64 identity RGB lattice, red axis fastest
    -> flattened 512 × 512 RGBA32F render target
    -> selected techniques in ReShade execution order
    -> RGBA32F GPU readback, or RGBA16F fallback
    -> floating-point CUBE export
```

A custom floating-point render target causes ReShade 6.8.0 to compile a custom effect permutation for the bake target. Its built-ins describe the bake resource rather than the gameplay back buffer:

- `BUFFER_WIDTH` and `BUFFER_HEIGHT` describe the flattened LUT texture.
- `BUFFER_COLOR_FORMAT` and `BUFFER_COLOR_BIT_DEPTH` describe RGBA32F or RGBA16F.
- `BUFFER_COLOR_SPACE` is `unknown` (`0`).
- A floating-point render target has no separate sRGB SRV or RTV, so `SRGBTexture` and `SRGBWriteEnabled` reads/writes behave linearly and cannot reproduce an sRGB back-buffer view exactly.

The public ReShade add-on API has no supported way to use an FP32 offscreen target while retaining the gameplay permutation's dimensions, format and color-space built-ins. This matters most for shaders that conditionally compile from `BUFFER_*` values and for some HDR-aware effects.

The baker records gameplay and bake buffer information in every exported CUBE so this difference is visible when troubleshooting accuracy.

### Technique execution

ReShade's ordered technique list is used directly. The baker does not sort techniques by name or effect file.

Each selected technique is rendered onto the same offscreen target, so the output of one becomes `COLOR` for the next. The `reshade_render_technique` event sequence is checked after every call, and a CUBE file is not written unless every requested technique reports execution in exactly the expected order.

Executing a selected subset also causes ReShade's begin/finish effect events to occur around each direct technique call. Normal full-chain rendering emits those events once around the whole chain. Another installed add-on that reacts to these events can therefore influence a bake.

ReShade's public technique API exposes an effect filename rather than its full search-path location. If multiple search paths contain the same `.fx` basename and technique name, the UI disambiguates them as ordered instances. Recheck those selections after manually reordering duplicate effects.

### GPU synchronization

GPU work is submitted with a completion fence and polled on later presentations. The add-on does not perform a blocking GPU wait from inside ReShade's present callback. Once the fence completes, samples are copied to CPU memory and CUBE serialization runs on a background worker.

ReShade 6.8.0 implements the OpenGL fence signal with `glFinish`, so OpenGL can still incur a one-time synchronous hitch during export. Vulkan requires timeline-semaphore support for the completion fence. If the active backend cannot create or signal the required fence, the baker fails explicitly rather than reusing an unsynchronized target.

### Display mode

The LUT represents the shader transformation evaluated on normalized RGB inputs from 0 to 1. Its numerical precision does not depend on Windows output being SDR 8-bit, SDR 10-bit or HDR10, but shader behavior that depends on the custom bake permutation can still differ from gameplay rendering.

An export should therefore be visually compared against the original grading chain in the target game and display mode before being treated as equivalent.

## Validation tools

The repository includes both runtime and offline validation paths:

- Export with no selected techniques to run a complete GPU identity bake. The UI reports maximum absolute RGB error, mean absolute RGB error and RMS RGB error.
- Identity exports are withheld if maximum error exceeds `1e-6` on RGBA32F or `5e-4` on the RGBA16F fallback.
- The exporter verifies the exact `reshade_render_technique` sequence before writing the CUBE and records that sequence in the file.
- `tools/validate_cube.py inspect` checks the declaration, input domain, finite values and exact `N³` row count.
- `tools/validate_cube.py identity` compares an export against the ideal red-fastest identity lattice.
- `tools/validate_cube.py compare` compares every lattice node of two generated LUTs and reports maximum, mean and RMS RGB error.
- `ReShadeLUTPreview.fx` provides a practical visual apply/split/difference comparison on game content.

Examples:

```powershell
python tools/validate_cube.py inspect "C:\Game\LUT_Bakes\MyPreset.cube"
python tools/validate_cube.py identity "C:\Game\LUT_Bakes\Identity.cube" --tolerance 1e-6
python tools/validate_cube.py compare reference.cube candidate.cube --tolerance 1e-6
```

Offline metrics measure values at lattice nodes. Differences between lattice nodes also include 3D-LUT interpolation and approximation error, which are separate from exporter readback error.

## Building and tests

See [BUILDING.md](BUILDING.md) for reproducible Visual Studio 2022/CMake build commands and offline dependency options.

A Windows GitHub Actions workflow builds the `.addon64` and runs the C++ and Python tests.

The C++ tests cover technique-selection reconciliation, duplicate identity changes, select-currently-enabled semantics, lattice dimensions/order, identity metrics, FP16 conversion, filename safety and atomic non-overwriting CUBE output.

Python tests cover strict parsing, ordering, identity metrics, comparison metrics and malformed-file rejection.

## Technical references

The implementation follows the official ReShade 6.8.0 source and headers:

- [ReShade add-on API header](https://github.com/crosire/reshade/blob/v6.8.0/include/reshade.hpp)
- [`effect_runtime::render_technique` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp#L1245-L1314)
- [Effect technique execution and implicit COLOR copy](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L4066-L4325)
- [Native CUBE loader](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L2931-L3114)

## License

ReShade LUT Baker is available under the [MIT License](LICENSE).
