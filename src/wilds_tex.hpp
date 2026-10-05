#pragma once

#include "cube_lut.hpp"

#include <array>

namespace lut_baker
{
inline constexpr std::uint32_t wilds_lut_size = 33;
inline constexpr std::size_t wilds_sample_count = 33u * 33u * 33u;
inline constexpr std::size_t wilds_row_pitch = 512;
inline constexpr std::size_t wilds_payload_size = wilds_row_pitch * 33u * 33u;
// Covers the pinned GDeflate CompressBound and all TEX headers, even for noise.
inline constexpr std::size_t wilds_file_budget = wilds_payload_size + 65536u + 64u;

// Single-mip RGBA16F profile observed in Wilds 1.42.0.2 map/event LUTs.
// Preserve the unknown control bytes; this is not a generic TEX serializer.
// mipOffset points to the compressed image header at 56, NOT to pixel data.
inline constexpr std::array<std::uint8_t, 56> wilds_tex_header {{
    0x54, 0x45, 0x58, 0x00, 0x6b, 0xfc, 0x5e, 0x0e, 0x21, 0x00, 0x21, 0x00, 0x21, 0x00, 0x01, 0x10,
    0x0a, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00
}};

[[nodiscard]] bool prepare_wilds_payload(std::uint32_t size, const std::vector<float4> &samples,
    std::vector<std::uint8_t> &payload, quantization_metrics &metrics, std::string &error);
[[nodiscard]] bool serialize_wilds_tex(std::uint32_t size, const std::vector<float4> &samples,
    std::vector<std::uint8_t> &bytes, quantization_metrics &metrics, std::string &error);
[[nodiscard]] bool write_wilds_tex_atomic(const std::filesystem::path &destination, std::uint32_t size,
    const std::vector<float4> &samples, quantization_metrics &metrics, std::string &error);

// Bounded decoder for this exact profile. Validate stream sizes/offsets before
// calling the page codec; never pass an unchecked file to the sample decoder.
[[nodiscard]] bool decode_wilds_tex(const std::vector<std::uint8_t> &bytes,
    std::vector<std::uint8_t> &payload, std::string &error);
}
