# ReShade LUT Baker

ReShade LUT Baker is a ReShade add-on that exports the combined RGB transformation of selected techniques as a floating-point 3D `.cube` LUT.

Version **1.1.0** also exports native **Monster Hunter Rise `.tex.28`** LUTs directly from the same floating-point bake. CUBE remains the default; Rise uses a fixed 32³ lattice and quantizes only the final CPU output to 8-bit RGB. Native loading and visual SDR/HDR equivalence of these new exports still require in-game testing.

It evaluates a neutral RGB lattice directly through ReShade, preserving the real technique execution order and avoiding screenshot, PNG, DDS, or other image intermediates. The default export is a 64³ LUT containing 262,144 RGB samples.

Selected techniques can be baked whether they are currently enabled or disabled. The baker executes them directly and does not change the preset or the user's enabled technique states.

An optional companion shader, [`ReShadeLUTPreview.fx`](shaders/ReShadeLUTPreview.fx), is included for applying and comparing exported LUTs inside ReShade.

## Features

- Exports standard floating-point `.cube` 3D LUT files.
- Exports the verified Monster Hunter Rise 32³ RGBA8 `.tex.28` profile directly, without a converter or image intermediary.
- 64³ output by default, with 16³ and 32³ also available.
- Uses ReShade's actual relative technique order.
- Supports baking selected techniques without changing their enabled state.
- Uses an RGBA32F bake target when available, with RGBA16F as an explicit fallback.
- Preserves finite shader output below 0 and above 1 in CUBE; Rise rejects it unless explicit clipping is enabled.
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
3. Under **Techniques**, tick exactly the techniques to bake (click the checkbox or anywhere on the row). They are listed in ReShade execution order; the **Live** dot shows whether each one is currently enabled in ReShade.
4. Under **Output**, pick the **Format** from the dropdown. CUBE defaults to 64x64x64 with 16x16x16/32x32x32 available. Monster Hunter Rise always uses 32x32x32; switching back preserves the CUBE size preference.
5. Optionally enter a **File name** (basename or complete matching suffix). The line below the field shows the exact name that will be written. Leaving it empty creates `ReShade_LUT_YYYYMMDD_HHMMSS.cube` or `.tex.28`, depending on the format.
6. Press the bake button. Its label states what will be written, for example **Bake 3 techniques - CUBE 64x64x64**. With nothing selected it reads **Export identity LUT (GPU validation)**.

**Use enabled** replaces the current selection with exactly the techniques that are enabled at that moment, **Clear** deselects everything and **Refresh** re-reads the technique list. The filter field narrows the list by effect or technique name without changing the selection.

While a bake runs, the settings are locked and a progress bar shows the current step. The **Result** section then reports the written file, format, technique count and duration, any warnings and validation metrics, with **Open output folder** and **Copy file name** (handy for `ReShadeLUTPreview.fx`). The accuracy limitations are listed under **How it works and limitations**.

After effects are reloaded, valid selections are preserved and techniques that no longer exist are removed automatically. An active bake keeps its own immutable selection snapshot, so a later catalog refresh cannot silently change the requested export.

During the first attempt, ReShade may need to compile a custom offscreen effect permutation. The baker retries for up to 60 seconds. It fails without writing a file if a requested technique disappears, compilation never completes, the execution sequence differs from the expected order, allocation/readback fails, a non-finite result is found, or the file cannot be committed.

## Output

Exports are written to:

```text
<ReShade base directory>/LUT_Bakes/
```

Existing exports are not overwritten. If a requested filename already exists, a numeric suffix such as `_001` is added automatically.

For Rise the full suffix is retained: `MyGrade.tex.28`, then `MyGrade_001.tex.28`. A collision that occurs during the final atomic commit causes an explicit failure, never an overwrite. Format, effective size, name, output directory and range policy are snapshotted when the request is queued and cannot change during its bake/readback/worker.

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

### Monster Hunter Rise output

Choose **Monster Hunter Rise (.tex.28)** for a direct 32 × 32 × 32 bake. The resulting file is exactly **131,128 bytes**: the verified 56-byte TEX28 header followed by 32,768 RGBA samples. Red changes fastest, then green, then blue; alpha is always 255. This is only the verified Rise profile, not a general RE Engine texture exporter. See [the binary profile](docs/RISE_TEX28.md).

Rise stores normalized RGB only. NaN/infinity always fails. Finite values outside 0–1 fail by default with the original range in the error. The optional **Clamp to 0-1** checkbox explicitly clips those values; it does not normalize the lattice or modify the float samples. The result/UI/log reports source range, clipped RGB component/sample counts and maximum/mean/RMS quantization error. These quantization metrics are against the policy-adjusted float input, separate from GPU identity validation; they do not include the loss caused by clipping. Quantization uses `floor(double(value) * 255 + 0.5)` without an added gamma/sRGB transform, even though the verified header declares DXGI 29 (`R8G8B8A8_UNORM_SRGB`).

Only the requested TEX is written, with no sidecar or automatically managed alternate filename. Its verified technique order and range/quantization metrics are logged to `ReShade.log`; CUBE retains its embedded metadata. `ReShadeLUTPreview.fx` loads CUBE, **not TEX**. You can additionally export the same selection as a 32³ CUBE for numerical node comparison, keeping uniforms unchanged between the two exports.

For the future Rise Rehydrated custom-LUT manager, the intended paths are:

```text
Physical: natives/STM/rise_rehydrated/custom_lut/MyGrade.tex.28
Logical:  rise_rehydrated/custom_lut/MyGrade.tex
```

External loading requires REFramework's **Enable Loose File Loader**. The published Rise Rehydrated 1.1.0 manager does **not** discover custom LUTs yet: copying a file there is not a loading test. Custom discovery, selection/presets, refresh and fallback belong to that separate project. The baker neither searches for nor installs into a game.

The profile is backed by inspected native neutral/Color Boost textures and offline numerical evidence, but no file from this new exporter has yet been tested in Rise. Start with an identity and an asymmetric axes fixture using an authorized loading path when available, then compare a real RGB grading. Disable the original ReShade grading while applying the native TEX, keep other overrides fixed and neutralize participating original engine LUTs to avoid double grading/mixing. Check primaries, ramps, shadows, skin and highlights in SDR first, then HDR independently. The engine applies LUTs at a different render stage and may blend scene LUTs; valid bytes do not prove the same look. HDR is not disabled or presumed equivalent.

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

Rise instead starts with a 32³ lattice (flattened to 256 × 128), follows the **same floating-point GPU path**, then quantizes and writes TEX on the CPU worker. There is no 8/10-bit bake intermediate or resampling from 64³.

A custom floating-point render target causes ReShade 6.8.0 to compile a custom effect permutation for the bake target. Its built-ins describe the bake resource rather than the gameplay back buffer:

- `BUFFER_WIDTH` and `BUFFER_HEIGHT` describe the flattened LUT texture.
- `BUFFER_COLOR_FORMAT` and `BUFFER_COLOR_BIT_DEPTH` describe RGBA32F or RGBA16F.
- `BUFFER_COLOR_SPACE` is `unknown` (`0`).
- A floating-point render target has no separate sRGB SRV or RTV, so `SRGBTexture` and `SRGBWriteEnabled` reads/writes behave linearly and cannot reproduce an sRGB back-buffer view exactly.

The public ReShade add-on API has no supported way to use an FP32 offscreen target while retaining the gameplay permutation's dimensions, format and color-space built-ins. This matters most for shaders that conditionally compile from `BUFFER_*` values and for some HDR-aware effects.

The baker records gameplay and bake buffer information in every exported CUBE so this difference is visible when troubleshooting accuracy.

### Technique execution

ReShade's ordered technique list is used directly. The baker does not sort techniques by name or effect file.

Each selected technique is rendered onto the same offscreen target, so the output of one becomes `COLOR` for the next. The `reshade_render_technique` event sequence is checked after every call, and no LUT is written unless every requested technique reports execution in exactly the expected order.

Executing a selected subset also causes ReShade's begin/finish effect events to occur around each direct technique call. Normal full-chain rendering emits those events once around the whole chain. Another installed add-on that reacts to these events can therefore influence a bake.

ReShade's public technique API exposes an effect filename rather than its full search-path location. If multiple search paths contain the same `.fx` basename and technique name, the UI disambiguates them as ordered instances. Recheck those selections after manually reordering duplicate effects.

### GPU synchronization

GPU work is submitted with a completion fence and polled on later presentations. The add-on does not perform a blocking GPU wait from inside ReShade's present callback. Once the fence completes, samples are copied to CPU memory and CUBE serialization or TEX quantization/writing runs on a background worker that owns its CPU data and makes no ReShade/runtime calls. Runtime reset cancels queued GPU work and releases resources only after ReShade's queue-idle teardown; a started CPU writer can finish its already validated snapshot independently.

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
- `tools/validate_rise_tex.py inspect` checks the exact Rise profile, byte count and alpha. `identity` measures against ideal nodes; `compare-cube` compares TEX nodes against a matching 32³ float CUBE, optionally with explicit `--clamp`.
- `ReShadeLUTPreview.fx` provides a practical visual apply/split/difference comparison on game content.

Examples:

```powershell
python tools/validate_cube.py inspect "C:\Game\LUT_Bakes\MyPreset.cube"
python tools/validate_cube.py identity "C:\Game\LUT_Bakes\Identity.cube" --tolerance 1e-6
python tools/validate_cube.py compare reference.cube candidate.cube --tolerance 1e-6
python tools/validate_rise_tex.py inspect MyGrade.tex.28
python tools/validate_rise_tex.py identity Identity32.tex.28
python tools/validate_rise_tex.py compare-cube MyGrade.tex.28 MyGrade.cube
```

Offline metrics measure values at lattice nodes. Differences between lattice nodes also include 3D-LUT interpolation and approximation error, which are separate from exporter readback error.

## Building and tests

See [BUILDING.md](BUILDING.md) for reproducible Visual Studio 2022/CMake build commands and offline dependency options.

A Windows GitHub Actions workflow builds the `.addon64` and runs the C++ and Python tests.

The C++ tests cover technique-selection reconciliation, duplicate identity changes, select-currently-enabled semantics, lattice dimensions/order, identity metrics, FP16 conversion, filename safety and atomic non-overwriting output. Rise tests additionally cover exact header/payload, channel axes, rounding, non-finite/range rejection, explicit clipping, unchanged CUBE samples, format snapshots, final-commit collisions and cleanup after write/serializer failures.

Python tests cover strict parsing, ordering, identity metrics, comparison metrics and malformed-file rejection. CTest also generates files using the real C++ writers and verifies every TEX byte against an independent Python float32/quantization reference, plus CUBE 16³/32³/64³ identity and float round trips. The optional `rise_tex_fixtures` executable generates identity and asymmetric axes fixtures for validation; no auxiliary grading shaders or game assets are included. See [BUILDING.md](BUILDING.md) for commands.

## Technical references

The implementation follows the official ReShade 6.8.0 source and headers:

- [ReShade add-on API header](https://github.com/crosire/reshade/blob/v6.8.0/include/reshade.hpp)
- [`effect_runtime::render_technique` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp#L1245-L1314)
- [Effect technique execution and implicit COLOR copy](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L4066-L4325)
- [Native CUBE loader](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L2931-L3114)

## License

ReShade LUT Baker is available under the [MIT License](LICENSE).
