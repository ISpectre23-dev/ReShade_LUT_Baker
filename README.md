# ReShade LUT Baker

ReShade LUT Baker is a ReShade add-on that exports the combined color grading of selected techniques as a 3D lookup table (LUT).

Version **1.1.0** exports floating-point **CUBE**, compatibility **PNG**, and native **Monster Hunter Rise `.tex.28`** files from the same floating-point bake. CUBE 64³ remains the default. PNG converts only the final CPU result to 8 bits per color channel by default, or optionally 16 bits. Rise remains 8-bit. Native Rise loading and visual SDR/HDR equivalence still require in-game testing.

It evaluates a neutral RGB lattice directly through ReShade, preserving the real technique execution order and avoiding screenshot, PNG, DDS, or other image intermediates. The default export is a 64³ LUT containing 262,144 RGB samples.

Selected techniques can be baked whether they are currently enabled or disabled. Before rendering, the baker briefly enables each disabled technique through ReShade's API to request its normal resources, then immediately restores its disabled state. It does not save or edit the preset. Already enabled techniques are left alone.

Unsaved shader parameters, technique states and order are backed up in memory before requesting shader initialization or an offscreen shader version. After compilation, the baker restores and verifies those settings before continuing. This protects all loaded effects, including ones that are not selected for the bake.

## Features

- Exports standard floating-point `.cube` 3D LUT files.
- Exports 8-bit or 16-bit PNG lookup tables as Horizontal strip or Square tiles. PNG defaults to 8-bit.
- Exports the verified Monster Hunter Rise 32³ RGBA8 `.tex.28` profile directly, without a converter or image intermediary.
- CUBE 64³ by default, with 16³, 32³, 128³ and Custom sizes from 2 to 128.
- Uses ReShade's actual relative technique order.
- Supports baking disabled techniques, restoring their state before rendering.
- Preserves unsaved parameters, technique states and execution order across bake-triggered compilation.
- Uses an RGBA32F bake target when available, with RGBA16F as an explicit fallback.
- Preserves finite shader output below 0 and above 1 in CUBE; PNG and Rise reject it unless explicit clipping is enabled.
- Shows a conservative file budget and checks free space before baking and writing.
- Verifies the expected ReShade technique execution sequence before writing a LUT.
- Uses GPU completion fences before readback.
- Writes LUT files atomically and never overwrites an existing named export.
- Includes offline identity/error validation tools.

## What can be baked

A 3D LUT can represent a deterministic mapping from one red, green and blue (RGB) triplet to another.

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
- Normal mode for grading exports. Performance mode must be off; identity exports remain available in either mode.
- A renderer supported by ReShade whose graphics queue can render/copy RGBA32F or RGBA16F textures and create a completion fence

*The implementation targets ReShade add-on API 20 and is audited and compiled against the official ReShade 6.8.0 source. Newer ReShade versions are expected to work while they remain API-compatible, but future compatibility is not guaranteed. Older versions are not supported.

## Installation

1. Build or download `ReShadeLUTBaker.addon64`.
2. Copy it next to the game's ReShade DLL, normally in the same directory as the game executable.
3. Start the game and open ReShade's **Add-ons** tab.
4. Confirm that **ReShade LUT Baker** is present.

ReShade must be installed in a configuration that permits third-party add-ons. If the panel is absent, check `ReShade.log` and the ReShade installation variant before troubleshooting the baker.

## Usage

1. Configure the grading techniques and their uniforms as desired.
2. Open **Add-ons > ReShade LUT Baker**.
3. Under **Techniques**, tick exactly the techniques to bake (click the checkbox or anywhere on the row). They are listed in ReShade execution order; the **Active** dot shows whether each one is currently enabled in ReShade.
4. Under **Output**, pick **CUBE** or **PNG** from **Common formats**, or **Monster Hunter Rise** from **Games**. CUBE offers 16³, 32³, 64³, 128³ and **Custom**. PNG has a separate size preference, a **Layout** selector and **Bit depth** (8-bit by default, optionally 16-bit). Rise always uses 32³. Switching formats preserves the other formats' preferences.
5. Optionally enter a **File name** (basename or complete matching suffix). The line below the field shows the exact name that will be written. Leaving it empty creates `ReShade_LUT_YYYYMMDD_HHMMSS.cube`, `.png` or `.tex.28`.
6. Press the bake button. Its label states what will be written, for example **Bake 3 techniques - CUBE 64x64x64**. With nothing selected it reads **Export identity LUT (GPU validation)**.

**Select active** replaces the current selection with exactly the techniques that are enabled at that moment, **Clear** deselects everything and **Refresh** re-reads the technique list. The filter field narrows the list by effect or technique name without changing the selection.

While a bake runs, the settings are locked and a progress bar shows the current step. **Cancel export** stops a queued bake, compilation wait or GPU readback wait without writing a LUT. It is disabled once the validated CPU result starts file writing; that atomic writer must finish. Cancellation does not interrupt ReShade's already queued shader compilation or submitted GPU commands. GPU resources are retained until their completion fence is observed. The settings backup is also retained until queued compilation finishes and live settings can be recovered, even after cancellation or a timeout. Another export stays blocked during that recovery. A late completion restores settings but does not restart the cancelled bake. The active request snapshot is never edited by cancellation. The **Result** section then reports the written file, format, technique count and duration, any warnings and validation metrics, with **Open output folder** and **Copy file name**. The accuracy limitations are listed under **How it works and limitations**.

After effects are reloaded, valid selections are preserved and techniques that no longer exist are removed automatically. An active bake keeps its own immutable selection snapshot, so a later catalog refresh cannot silently change the requested export.

An effect that has never been enabled may be compiled but still lack its normal GPU resources. ReShade 6.8.0 creates the shared constant buffer (which holds shader parameters) only when initializing the normal effect, not an offscreen permutation (a shader version for a different target). The baker requests that initialization first and restores the disabled state in the same callback, before any rendering. It waits for ReShade's **effects-reloaded** event and verifies readiness before requesting an offscreen permutation. A failure or an add-on veto stops the export without writing a LUT. If another add-on blocks restoration, the error explicitly tells you which technique to disable. See [ReShade's resource initialization](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp) and [technique state API](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp).

ReShade may then need to compile a custom offscreen effect permutation. The baker stops at the first technique without a render event, waits for the effects-reloaded event and verifies it once. If that exact technique still does not execute, the export fails explicitly rather than repeatedly requesting its compilation. Default resource initialization and offscreen compilation have separate bounded waits for each exact technique. Every full-chain attempt starts with a fresh identity lattice and retains ReShade's relative order. A 60-second overall deadline covers both stages, reload and GPU waits. It is checked from the panel even if matching presentation callbacks stop. A separate 30-second GPU submission timeout retains the in-flight resources safely. The baker also fails without writing a file if a requested technique disappears, the execution sequence differs from the expected order, allocation/readback fails, a non-finite result is found, or the file cannot be committed.

### Preserving unsaved settings

ReShade 6.8.0 reapplies the saved preset when a new offscreen permutation finishes compiling. Without protection, that replaces unsaved parameters, enabled states and sorting, and can bake the saved grading instead of the live grading. Cached permutations do not take that reload path.

The baker captures ordinary shader parameters, enabled technique states and the complete runtime order before each attempt. Once compilation completes, it queries fresh API handles, checks identities/types/dimensions, restores changed values and verifies them before executing the chain again. No preset is saved and no backup file is created. The in-memory parameter data is limited to 64 MiB. Dynamic uniforms with a `source` annotation, such as time or frame count, are not frozen. Private shader/add-on state and temporal GPU history are not part of this backup.

Duplicate identities, a changed parameter layout or a blocked restoration stop the export explicitly. A shader that failed to compile is never forced to run. Changing to another preset cancels the request and discards the old backup without applying it to the new preset. An explicit effect reload/replacement also discards a pending backup rather than guessing that newly created effects are the old ones. Avoid changing the preset or manually reloading effects while a bake/recovery is pending.

**Performance mode** embeds saved parameter values as compile-time constants. Restoring ordinary uniforms would not reliably change those constants. The baker therefore rejects selected-technique exports in that mode. Turn it off and let ReShade reload before baking. The baker never changes the mode automatically, and zero-selection identity exports remain available.

For example, Lilium's SDR TRC fix conditionally defines its technique based on buffer format/color space. Its usual SDR configuration does **not** define that technique on this baker's RGBA32F/RGBA16F offscreen target. ReShade 6.8.0 rejects permutations with different technique/uniform declarations. Such a shader cannot safely be baked unchanged with the public API: the baker stops and logs the exact non-executing technique, without lowering precision or changing shader preprocessor settings to manufacture a different transformation. ReShade itself may mark an effect as failed/disabled when its permutation fails; the baker cannot cancel or roll back ReShade's internal compiler through the public API. If necessary, reload effects after stopping the bake. See [ReShade's permutation validation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp) and [Lilium's shader](https://github.com/EndlesslyFlowering/ReShade_HDR_shaders/blob/master/Shaders/lilium__sdr_trc_fix.fx).

## Output

Exports are written to:

```text
<ReShade base directory>/LUT_Bakes/
```

Existing exports are not overwritten. If a requested filename already exists, a numeric suffix such as `_001` is added automatically.

For Rise the full suffix is retained: `MyGrade.tex.28`, then `MyGrade_001.tex.28`. A collision during the final atomic commit fails explicitly, never overwrites. Format, effective size, PNG layout/bit depth, name, output directory and range policy are captured when the request is queued. Editing preferences cannot change an active export.

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

### CUBE size and storage limits

The size is the number of points per color axis, not the color bit depth. **64³** remains the recommended default. **128³** has eight times as many samples and can reduce interpolation error for demanding grading. It does not recover information already lost by an effect. The size labels use cubic notation, such as **64³**; recommendations appear only in their tooltips.

**Custom** accepts integers from **2 to 128**, including sizes such as 33, 48, 65 and 96. Invalid values disable export and are rejected again at queue time. Other applications may impose different size limits.

A 128³ lattice has 2,097,152 samples. One four-channel, 32-bit float buffer needs 32 MiB, compared with 4 MiB for 64³. The baker and ReShade need several buffers, so this is not total memory use. MiB means 1,048,576 bytes.

The panel shows a conservative **File budget**, not a prediction of compression or exact file size. Free space is checked on the output volume before the bake and again on the writer, with an additional 16 MiB reserve. The 128³ CUBE data budget is about 96 MiB before that reserve. Space can change after either check; write failures still use the existing temporary-file cleanup. These limits do not prevent many accumulated exports from filling a drive. The baker never deletes older exports automatically.

### PNG compatibility export

Choose **PNG**, then choose a layout and **Bit depth**. Both layouts support 8-bit and 16-bit RGB. PNG and CUBE are separate output formats; a bake writes only the requested file.

| Size | Horizontal strip | Square tiles |
|---|---|---|
| 16³ | 256 × 16 | 64 × 64, 4 × 4 tiles |
| 32³ | 1024 × 32 | Not available without padding |
| 64³ | 4096 × 64 | 512 × 512, 8 × 8 tiles |
| 128³ | 16384 × 128 | Not available without padding |

Each tile is one blue-channel slice. Red increases from left to right within a tile; green increases from top to bottom. Blue slices progress left to right, then top to bottom in Square tiles. Both layouts contain the same samples. **Square tiles is not Hald layout**, even when both images measure 512 × 512. The output shader must support the exact layout and size. The baker does not add unused tiles or silently change size when switching layouts. If the existing PNG size is unsupported, choose 16 or 64 to enable export.

For the common horizontal `LUT.fx`, add the output folder to ReShade's texture search paths and configure:

```hlsl
#define fLUT_TextureName "MyGrade.png"
#define fLUT_TileSizeXY 64
#define fLUT_TileAmount 64
```

Reload effects after changing the file or definitions. `LUT.fx` expects the horizontal layout, not Square tiles. Check the reader's texture limits before choosing a 128³ strip: it is 16384 pixels wide. Square tiles must be used with a reader that supports that grid.

**8-bit is the default** for ReShade compatibility. **16-bit** is intended for external applications that preserve 16-bit PNG samples. ReShade 6.8.0 reduces PNG input to 8 bits when loading; choosing a float texture does not bypass that earlier conversion. The standard `LUT.fx` also uses an 8-bit texture. A 16-bit export therefore does not improve precision through that ReShade loading path. See [ReShade's image loader](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L3050-L3067), its [PNG decoding path](https://github.com/nothings/stb/blob/28d546d5eb77d4585506a20480f4de2e706dff4c/stb_image.h#L1167-L1179) and [LUT.fx](https://github.com/crosire/reshade-shaders/blob/slim/Shaders/LUT.fx#L40).

PNG stores integer RGB samples, not 16-bit floating point, with no alpha. The GPU bake and readback still use floating point. Final conversion rounds `value * 255` for 8-bit or `value * 65535` for 16-bit to the nearest integer, with half values rounded up. The 16-bit route never passes through 8-bit pixels. No gamma conversion, dithering or color profile is added. Windows' built-in PNG encoder compresses those samples losslessly. The writer checks the requested bit depth and fails rather than silently writing a lower-precision file. PNG still does not preserve every possible float value. The image includes the exporter version in its Software text metadata. Bit depth, verified technique order and range/quantization metrics are recorded in ReShade's log.

Values outside 0 to 1 fail by default. **Clamp to 0-1** explicitly clips them and reports the original range, affected components/samples and quantization error. The quantization error excludes clipping loss and is separate from GPU identity validation. Prefer CUBE when retaining float values or outputs outside 0 to 1 matters. No screenshot, PNG intermediate or display-bit-depth conversion is involved in producing CUBE or Rise.

The PNG path uses [Windows Imaging Component](https://learn.microsoft.com/en-us/windows/win32/wic/png-format-overview), Windows' built-in image encoder. It requires no bundled image library. The real PNG writer and both layouts are tested offline; final visual comparison through third-party shaders still requires ReShade testing.

### Monster Hunter Rise output

Choose **Monster Hunter Rise (.tex.28)** for a direct 32 × 32 × 32 bake. The resulting file is exactly **131,128 bytes**: the verified 56-byte TEX28 header followed by 32,768 RGBA samples. Red changes fastest, then green, then blue; alpha is always 255. This is only the verified Rise profile, not a general RE Engine texture exporter. See [the binary profile](docs/RISE_TEX28.md).

Rise stores normalized RGB only. NaN/infinity always fails. Finite values outside 0–1 fail by default with the original range in the error. The optional **Clamp to 0-1** checkbox explicitly clips those values; it does not normalize the lattice or modify the float samples. The result/UI/log reports source range, clipped RGB component/sample counts and maximum/mean/RMS quantization error. These quantization metrics are against the policy-adjusted float input, separate from GPU identity validation; they do not include the loss caused by clipping. Quantization uses `floor(double(value) * 255 + 0.5)` without an added gamma/sRGB transform, even though the verified header declares DXGI 29 (`R8G8B8A8_UNORM_SRGB`).

Only the requested TEX is written, with no sidecar or automatically managed alternate filename. Its verified technique order and range/quantization metrics are logged to `ReShade.log`; CUBE retains its embedded metadata. You can additionally export the same selection as a 32³ CUBE for numerical node comparison, keeping uniforms unchanged between the two exports.

For the future Rise Rehydrated custom-LUT manager, the intended paths are:

```text
Physical: natives/STM/rise_rehydrated/custom_lut/MyGrade.tex.28
Logical:  rise_rehydrated/custom_lut/MyGrade.tex
```

External loading requires REFramework's **Enable Loose File Loader**. The published Rise Rehydrated 1.1.0 manager does **not** discover custom LUTs yet: copying a file there is not a loading test. Custom discovery, selection/presets, refresh and fallback belong to that separate project. The baker neither searches for nor installs into a game.

The profile is backed by inspected native neutral/Color Boost textures and offline numerical evidence, but no file from this new exporter has yet been tested in Rise. Start with an identity and an asymmetric axes fixture using an authorized loading path when available, then compare a real RGB grading. Disable the original ReShade grading while applying the native TEX, keep other overrides fixed and neutralize participating original engine LUTs to avoid double grading/mixing. Check primaries, ramps, shadows, skin and highlights in SDR first, then HDR independently. The engine applies LUTs at a different render stage and may blend scene LUTs; valid bytes do not prove the same look. HDR is not disabled or presumed equivalent.

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

GPU work is submitted with a completion fence and polled on later presentations. The add-on does not perform a blocking GPU wait from inside ReShade's present callback. Once the fence completes, samples are copied to CPU memory and CUBE serialization or PNG/TEX quantization and writing runs on a background worker that owns its CPU data and makes no ReShade/runtime calls. Runtime reset cancels queued GPU work and releases resources only after ReShade's queue-idle teardown; a started CPU writer can finish its already validated snapshot independently.

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
- `tools/validate_png.py` decodes and checks PNG files using only Python's standard library. Specify `--layout horizontal` or `--layout square`; dimensions alone cannot distinguish Square tiles from Hald.

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

The C++ tests cover technique-selection reconciliation, duplicate identity changes, select-currently-enabled semantics, lattice dimensions/order, identity metrics, FP16 conversion, filename safety and atomic non-overwriting output. Technique-preparation tests exercise the production helper against a model of ReShade's initialization branches: a never-enabled effect must prepare its normal constant buffer before any offscreen request. They cover immediate state restoration, separate bounded waits, allocation/compile failure, add-on vetoes, exceptions and cancellation. These are policy tests, not a live ReShade/GPU test. Format tests cover all presets, custom boundaries and odd sizes, immutable snapshots, independent preferences and storage budgets. PNG tests cover every 8-bit and 16-bit RGB sample in both layouts, low-byte preservation, quantization, non-finite/range rejection, explicit clipping and the real Windows encoder. Rise tests retain their exact header/payload and failure-cleanup checks.

Runtime-settings tests use the production restoration helper with a fake settings API. They reproduce the saved-preset reset across multiple compilation cycles, verify exact float/integer/boolean/vector/matrix/array values and full technique order, protect unselected effects, and exercise late recovery after cancellation/timeout. They also check new presets, changed or ambiguous identities/layouts, deferred resource creation and rejected writes. This does not replace in-game validation of actual ReShade callbacks and GPU execution.

Python tests cover strict parsing, ordering, identity/comparison metrics and malformed-file rejection. The PNG decoder checks chunk checksums, all five row filters at 8-bit and 16-bit, opaque alpha, decoded-size bounds and absence of color-space metadata. It retains both bytes of 16-bit samples and selects its default tolerance from the file's bit depth. CTest generates files with the real C++ writers and verifies every TEX byte, every 8-bit and 16-bit PNG sample and CUBE float round trips at 2³, 16³, 32³, 33³, 64³, 65³ and 128³ against independent references. The optional `rise_tex_fixtures` executable generates validation fixtures for all formats; no auxiliary grading shaders or game assets are included. See [BUILDING.md](BUILDING.md) for commands.

Useful PNG checks for a real export:

```powershell
python -B tools/validate_png.py identity Identity64.png --layout horizontal
python -B tools/validate_png.py identity Identity64Square.png --layout square
python -B tools/validate_png.py compare-cube MyGrade.png MyGrade.cube --layout horizontal
```

The validator detects 8-bit or 16-bit samples from the PNG header. Use `--clamp` on `compare-cube` only when that PNG was exported with explicit clamping. The default tolerance includes half a step at the detected bit depth (`0.5/255` or `0.5/65535`) plus float32 identity error. For an actual 16-bit float GPU fallback, add its separately measured identity error to the comparison tolerance; do not interpret it as PNG quantization alone. PNG bit depth and GPU float precision are independent choices.

## Technical references

The implementation follows the official ReShade 6.8.0 source and headers:

- [ReShade add-on API header](https://github.com/crosire/reshade/blob/v6.8.0/include/reshade.hpp)
- [`effect_runtime::render_technique` implementation](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime_api.cpp#L1245-L1314)
- [Effect technique execution and implicit COLOR copy](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L4066-L4325)
- [Native CUBE loader](https://github.com/crosire/reshade/blob/v6.8.0/source/runtime.cpp#L2931-L3114)

## License

ReShade LUT Baker is available under the [MIT License](LICENSE).
