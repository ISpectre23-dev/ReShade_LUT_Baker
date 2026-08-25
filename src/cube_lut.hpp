#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lut_baker
{
struct float4
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct error_metrics
{
    double maximum_absolute = 0.0;
    double mean_absolute = 0.0;
    double rms = 0.0;
};

struct cube_metadata
{
    std::string title;
    std::string exporter_version;
    std::string reshade_api;
    std::string graphics_api;
    std::string source_buffer;
    std::string bake_buffer;
    std::vector<std::string> techniques;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> choose_lattice_layout(std::uint32_t size);
[[nodiscard]] std::vector<float4> make_identity_lattice(std::uint32_t size, std::uint32_t width, std::uint32_t height);
[[nodiscard]] error_metrics measure_identity_error(const std::vector<float4> &samples, std::uint32_t size);
[[nodiscard]] std::uint16_t float_to_half(float value) noexcept;
[[nodiscard]] float half_to_float(std::uint16_t value) noexcept;

[[nodiscard]] bool validate_output_filename(std::string_view value, std::string &normalized, std::string &error);
[[nodiscard]] std::string make_timestamped_filename();
[[nodiscard]] std::filesystem::path make_unique_output_path(const std::filesystem::path &directory, const std::string &filename);

[[nodiscard]] bool write_cube_atomic(
    const std::filesystem::path &destination,
    std::uint32_t size,
    const std::vector<float4> &samples,
    const cube_metadata &metadata,
    bool overwrite,
    std::string &error);
}
