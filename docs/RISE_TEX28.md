# Monster Hunter Rise LUT format

The Rise export writes one fixed profile: TEX version 28, 32 × 32 × 32, 8-bit RGBA. It matches the game's neutral LUT, `systems/rendering/ColorCubeLinear.tex.28`. It is not a general RE Engine texture writer.

## File layout

The file is exactly 131,128 bytes: a 56-byte header followed by 131,072 bytes of RGBA data. The header is always the same:

```text
54 45 58 00 1c 00 00 00 20 00 20 00 20 00 01 10
1d 00 00 00 ff ff ff ff 00 00 00 00 00 08 00 00
00 00 00 00 00 00 00 00 38 00 00 00 00 00 00 00
80 00 00 00 00 10 00 00
```

All fields are little-endian.

| Offset | Field | Value |
| --- | --- | --- |
| 0 | Signature | `TEX\0` |
| 4 | Version, uint32 | 28 |
| 8 / 10 / 12 | Width / height / depth, uint16 | 32 / 32 / 32 |
| 14 | Control bytes | `01 10` |
| 16 | DXGI format, uint32 | 29, `R8G8B8A8_UNORM_SRGB` |
| 20–39 | Control fields | Copied as they are; meaning unknown |
| 40 | Data offset, uint64 | 56 |
| 48 | Row pitch, uint32 | 128 |
| 52 | Size of one depth slice, uint32 | 4096 |

## Sample order

Red changes fastest, then green, then blue. Alpha is always 255.

```text
sample_index = (b * 32 + g) * 32 + r
byte_offset  = 56 + 4 * sample_index
bytes        = R, G, B, 255
```

## Conversion

Each RGB value is converted with `floor(value * 255 + 0.5)`. No gamma conversion is applied, even though the header declares an sRGB format.

Values outside 0-1 are rejected by default. With **Clamp to 0-1**, they are clamped first. NaN and infinity are always rejected.
