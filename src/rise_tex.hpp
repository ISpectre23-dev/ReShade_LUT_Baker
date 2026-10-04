#pragma once

#include "cube_lut.hpp"

#include <array>
#include <cstddef>

namespace lut_baker
{
inline constexpr std::uint32_t rise_lut_size = 32;
inline constexpr std::size_t rise_sample_count = 32u * 32u * 32u;
inline constexpr std::size_t rise_file_size = 56u + rise_sample_count * 4u;

// Verified Rise ColorCubeLinear / Color Boost profile supplied with the format
// investigation. Control fields are preserved verbatim; this is NOT a generic
// RE Engine TEX serializer. Byte layout avoids struct padding/endian ambiguity.
inline constexpr std::array<std::uint8_t, 56> rise_tex_header {{
    0x54, 0x45, 0x58, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x01, 0x10,
    0x1d, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00
}};

struct rise_export_metrics
{
    double source_minimum = 0.0;
    double source_maximum = 0.0;
    std::size_t clipped_components = 0;
    std::size_t clipped_samples = 0;
    // Against the range-policy-adjusted float input, not the ideal lattice or
    // unclipped input. GPU identity and clipping error are separate concerns.
    error_metrics quantization;
};

[[nodiscard]] bool serialize_rise_tex(std::uint32_t size, const std::vector<float4> &samples, range_policy policy,
    std::vector<std::uint8_t> &bytes, rise_export_metrics &metrics, std::string &error);
[[nodiscard]] bool write_rise_tex_atomic(const std::filesystem::path &destination, std::uint32_t size,
    const std::vector<float4> &samples, range_policy policy, rise_export_metrics &metrics, std::string &error);
}
