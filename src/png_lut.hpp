#pragma once

#include "cube_lut.hpp"

namespace lut_baker
{
struct png_rgb_image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // Top-to-bottom rows; RGB, with no alpha or color conversion.
};

[[nodiscard]] const char *png_layout_name(png_layout layout) noexcept;
[[nodiscard]] bool prepare_png_lut(std::uint32_t size, const std::vector<float4> &samples, png_layout layout,
    range_policy policy, png_rgb_image &image, quantization_metrics &metrics, std::string &error);
[[nodiscard]] bool encode_png(const png_rgb_image &image, const std::string &software,
    std::vector<std::uint8_t> &bytes, std::string &error);
[[nodiscard]] bool write_png_lut_atomic(const std::filesystem::path &destination, std::uint32_t size,
    const std::vector<float4> &samples, png_layout layout, range_policy policy,
    const cube_metadata &metadata, quantization_metrics &metrics, std::string &error);
}
