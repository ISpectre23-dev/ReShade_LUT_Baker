# Verified Monster Hunter Rise LUT profile

This writer implements one measured profile: Rise's neutral `systems/rendering/ColorCubeLinear.tex.28` and the three working Rise Rehydrated Color Boost LUTs share this layout. The supplied investigation dated 2026-10-04 verified their header/data order and synthetic node conversion offline. Native assets are **not** distributed or required by tests. This is not a specification of all RE Engine TEX variants, nor evidence of in-game equivalence of new baker exports.

The entire 56-byte header is fixed:

```text
54 45 58 00 1c 00 00 00 20 00 20 00 20 00 01 10
1d 00 00 00 ff ff ff ff 00 00 00 00 00 08 00 00
00 00 00 00 00 00 00 00 38 00 00 00 00 00 00 00
80 00 00 00 00 10 00 00
```

Header SHA-256: `2b77b7325e98114c3fdaf22233d4a7cd1183791e8ec95ca7e7587c0904ad9c04`.

| Offset | Field | Value |
| --- | --- | --- |
| 0 | Signature | `TEX\0` |
| 4 | UInt32 LE version | 28 |
| 8 / 10 / 12 | UInt16 LE width / height / depth | 32 / 32 / 32 |
| 14 | Control bytes | `01 10` |
| 16 | UInt32 LE DXGI format | 29, `R8G8B8A8_UNORM_SRGB` |
| 20–39 | Control fields | Exact reference bytes; full meaning not established |
| 40 | UInt64 LE payload offset | 56 |
| 48 | UInt32 LE row pitch | 128 |
| 52 | UInt32 LE mip size field | 4096, per-depth-slice value, **not** the entire payload |

No struct is dumped to disk. All control bytes are preserved verbatim. The file is exactly 131128 bytes with one 131072-byte RGBA payload and no extra mips or metadata.

```text
sample_index = (b * 32 + g) * 32 + r
byte_offset = 56 + 4 * sample_index
payload = R, G, B, 255
```

For each finite RGB component, reject values outside [0, 1] by default; explicit clamp clips only those values. Quantize with `floor(double(value) * 255 + 0.5)`. No transfer function is added. The declaration of sRGB describes the verified texture profile, not permission to gamma-transform the source samples. The [official DXGI enum](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format) identifies value 29.

Tests use ideal identity nodes and the asymmetric fixture `F(r,g,b)=(0.1+0.8*b, 0.1+0.8*r*r, 0.1+0.8*g)`, explicitly rounded to binary32 before serialization. The independent Python reference checks every output byte, including alpha; it does not invoke the C++ quantizer to produce expected bytes. The ideal double fixture in the original investigation can differ by one byte at half-step ties after float32 rounding, which is not an axis or format error.

Quantization max/mean/RMS is measured against the float input **after** the selected range policy. Identity error against ideal RGB nodes additionally contains float32/float16 bake error; clipping loss is not hidden inside quantization metrics. An ideal rounded identity differs from the original game's neutral ramp by at most one byte according to the supplied investigation, so do not demand byte identity with that asset.

Native loading, engine LUT mixing/application stage and SDR/HDR visual equivalence remain separate manual tests. The baker must not compensate with an unmeasured gamma transform, alter this header or restrict HDR simply because the final container is 8-bit.
