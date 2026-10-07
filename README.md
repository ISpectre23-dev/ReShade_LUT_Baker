# ReShade LUT Baker

ReShade LUT Baker is a ReShade add-on that bakes the color grading of the techniques you select into a 3D LUT. Set up your grading in ReShade, pick the techniques, and export a single LUT file that reproduces them.

It exports standard `.cube` and PNG files, plus native LUTs for Monster Hunter Rise.

The bake runs the techniques on a neutral color grid in floating point, in ReShade's real execution order. It replaces the usual manual workflow (neutral LUT, screenshot, external converter) and its 8-bit limit with a direct floating-point export. Your preset is never saved or changed.

## Features

- Exports CUBE, PNG and Monster Hunter Rise (`.tex.28`) LUTs.
- Bakes several techniques together, in the order ReShade runs them.
- Bakes disabled techniques too, without leaving them enabled.
- Keeps unsaved shader parameters, technique states and order intact during the bake.
- Bakes in 32-bit float when the renderer allows it, with 16-bit float as a fallback.
- Never overwrites an existing export.
- Includes a GPU identity test and offline validation tools.

## What can be baked

A 3D LUT maps one RGB color to another. It can capture any technique whose output depends only on the color of the input pixel: color grading, tone curves, saturation, white balance and similar.

It cannot capture techniques that depend on anything else:

- screen position, resolution or aspect ratio (vignette, lens effects)
- neighboring pixels (sharpening, blur, bloom, chromatic aberration)
- depth, motion or scene geometry
- previous frames or time
- random noise, film grain or dithering

The baker does not check this for you. Compare the exported LUT against the original techniques before relying on it.

## Requirements

- Windows x64
- ReShade 6.8.0 or newer, with full add-on support
- ReShade's Performance mode turned off when baking techniques

The add-on is built against ReShade 6.8.0 (add-on API 20). Newer versions should work while they stay API-compatible. Older versions are not supported.

## Installation

1. Download or [build](BUILDING.md) `ReShadeLUTBaker.addon64`.
2. Copy it next to the game's ReShade DLL, normally the folder of the game executable.
3. Start the game and open ReShade's **Add-ons** tab.
4. Check that **ReShade LUT Baker** is listed.

If it is missing, check `ReShade.log` and make sure your ReShade build supports add-ons.

## Usage

1. Set up your grading techniques and their parameters in ReShade.
2. Open **Add-ons > ReShade LUT Baker**.
3. Under **Techniques**, select the techniques to bake. They are listed in ReShade's execution order, and the **Active** dot shows which ones are enabled.
4. Under **Output**, choose the format and its options.
5. Optionally type a **File name**. If you leave it empty, the file is named with the date and time.
6. Press the bake button. Its label shows what will be written, for example **Bake 3 techniques - CUBE 64x64x64**.

**Select active** selects exactly the techniques that are enabled in ReShade. **Clear** deselects everything, and **Refresh** reloads the list.

While a bake runs, the settings are locked and a progress bar shows the current step. **Cancel export** stops the bake without writing a file, and is available until the file starts being written. When it finishes, the **Result** section shows the file, any warnings and the validation metrics.

With nothing selected, the button exports an identity LUT (no color change) and reports how exactly the GPU reproduced it. Use it to test a new setup.

## Output formats

Files are written to `\LUT_Bakes` inside ReShade's base folder. If the name already exists, a suffix such as `_001` is added.

| Format | LUT size | Precision | Values outside 0-1 |
| --- | --- | --- | --- |
| CUBE (`.cube`) | 16³, 32³, 64³ (default), 128³ or custom from 2 to 128 | Float | Kept |
| PNG (`.png`) | 16³, 32³, 64³ or 128³ | 8-bit (default) or 16-bit | Rejected, or clamped with **Clamp to 0-1** |
| Monster Hunter Rise (`.tex.28`) | 32³, fixed | 8-bit | Rejected, or clamped with **Clamp to 0-1** |

The size is the number of points per color axis. 64³ is a good default; 128³ has eight times as many samples and is rarely needed.

### CUBE

Each file also records the baked technique order, version and buffer information as comments. Choose CUBE when you need float precision or values outside 0-1.

To load a CUBE file in ReShade, use a shader that reads the format, such as `CubeLUT3D.fx` from [BX-Shade](https://github.com/liuxd17thu/BX-Shade), which the ReShade installer can install. Move the baked LUT to ReShade's `reshade-shaders\Textures` folder or add the output folder to ReShade's texture search paths, then set:

```hlsl
#define SOURCE_CUBELUT3D_FILE "MyGrade.cube"
#define CUBE_3D_SIZE 64
```

`CUBE_3D_SIZE` must match the size of the exported LUT.

### PNG

PNG stores the LUT as an image, in one of two layouts:

| Size | Horizontal strip | Square tiles |
| --- | --- | --- |
| 16³ | 256 × 16 | 64 × 64 |
| 32³ | 1024 × 32 | Not available |
| 64³ | 4096 × 64 | 512 × 512 |
| 128³ | 16384 × 128 | Not available |

The shader or application that reads the file must support the same layout and size. Square tiles is not the Hald layout.

To load a Horizontal strip with the standard `LUT.fx`, move the baked LUT to ReShade's `reshade-shaders\Textures` folder or add the output folder to ReShade's texture search paths, then set:

```hlsl
#define fLUT_TextureName "MyGrade.png"
#define fLUT_TileSizeXY 64
#define fLUT_TileAmount 64
```

`fLUT_TileSizeXY` and `fLUT_TileAmount` must both match the size of the exported LUT.

ReShade 6.8.0 loads PNG files as 8-bit, so a 16-bit export only adds precision in applications that read 16-bit PNG.

## Game support

Besides CUBE and PNG, the add-on writes the native LUT format of these games:

| Game | Format | Status | Reference |
| --- | --- | --- | --- |
| Monster Hunter Rise | `.tex.28` | Supported | [RISE_TEX28.md](docs/RISE_TEX28.md) |

The add-on only writes the file. It does not install it or modify the game, so use a LUT manager or mod loader to load it.

## Limitations

- The bake runs on an offscreen floating-point target, not on the game image. Shaders that depend on the buffer size, format or color space (`BUFFER_*`), or on sRGB conversion, can behave differently from gameplay, especially in HDR.
- A shader that cannot compile for that target cannot be baked. The export stops with an error and nothing is written.
- Other add-ons that react to effect rendering events can alter the bake.
- Dynamic shader inputs such as time or frame count are not frozen.
- If two effect files share the same file and technique name, they are listed as numbered instances. Check the selection again after reordering them.

## Building and validation

See [BUILDING.md](BUILDING.md) for build commands, tests and the offline validation tools.

## License

ReShade LUT Baker is available under the [MIT License](LICENSE), which also includes the notices for the ReShade add-on SDK headers and Dear ImGui.
