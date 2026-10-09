#pragma once

#include "cube_lut.hpp"

#include <array>
#include <cstddef>

namespace lut_baker
{
inline constexpr std::uint32_t rise_lut_size = 32;
inline constexpr std::size_t rise_sample_count = 32u * 32u * 32u;
inline constexpr std::size_t rise_file_size = 56u + rise_sample_count * 4u;

// Fixed Rise ColorCubeLinear / Color Boost header, not a generic TEX serializer.
// Explicit bytes preserve control fields and avoid struct padding and endian
// ambiguity.
inline constexpr std::array<std::uint8_t, 56> rise_tex_header {{
    0x54, 0x45, 0x58, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x01, 0x10,
    0x1d, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00
}};

using rise_export_metrics = quantization_metrics;

[[nodiscard]] bool serialize_rise_tex(std::uint32_t size, const std::vector<float4> &samples, range_policy policy,
    std::vector<std::uint8_t> &bytes, rise_export_metrics &metrics, std::string &error);
[[nodiscard]] bool write_rise_tex_atomic(const std::filesystem::path &destination, std::uint32_t size,
    const std::vector<float4> &samples, range_policy policy, rise_export_metrics &metrics, std::string &error);
}
