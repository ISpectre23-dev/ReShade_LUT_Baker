#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lut_baker
{
enum class output_format { cube, rise_tex, png };
enum class range_policy { reject, clamp };
enum class png_layout { horizontal, square };

inline constexpr std::uint32_t minimum_lut_size = 2;
inline constexpr std::uint32_t maximum_lut_size = 128;

[[nodiscard]] constexpr bool valid_lut_size(const std::uint32_t size) noexcept
{
    return size >= minimum_lut_size && size <= maximum_lut_size;
}

struct export_preferences
{
    output_format format = output_format::cube;
    std::uint32_t cube_size = 64;
    range_policy rise_range = range_policy::reject;
    bool cube_custom = false;
    int custom_cube_size = 64;
    std::uint32_t png_size = 64;
    png_layout png_distribution = png_layout::horizontal;
    range_policy png_range = range_policy::reject;
};

// Captured once at queue time. GPU allocation, readback and the CPU worker
// must consume this snapshot, never the editable preferences above.
struct export_request
{
    output_format format = output_format::cube;
    std::uint32_t lattice_size = 64;
    range_policy range = range_policy::reject;
    png_layout png_distribution = png_layout::horizontal;
    std::filesystem::path directory;
    std::string filename;
};

struct export_estimate
{
    std::uint64_t samples = 0;
    std::uint64_t float_buffer_bytes = 0; // One RGBA32F buffer, not total memory use.
    std::uint64_t file_bytes = 0; // Conservative budget, not a predicted compressed size.
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
};

[[nodiscard]] const char *output_extension(output_format format) noexcept;
[[nodiscard]] const char *output_format_name(output_format format) noexcept;
[[nodiscard]] std::uint32_t effective_lattice_size(const export_preferences &preferences) noexcept;
[[nodiscard]] bool validate_output_filename(std::string_view value, std::string &normalized, std::string &error,
    output_format format = output_format::cube);
[[nodiscard]] std::string make_timestamped_filename(output_format format = output_format::cube);
[[nodiscard]] std::filesystem::path make_unique_output_path(const std::filesystem::path &directory,
    const std::string &filename, output_format format = output_format::cube);
[[nodiscard]] std::string filename_for_format(std::string_view filename, output_format previous, output_format next);
[[nodiscard]] bool snapshot_export_request(const export_preferences &preferences, std::string_view filename,
    const std::filesystem::path &directory, export_request &request, std::string &error);
[[nodiscard]] bool validate_export_preferences(const export_preferences &preferences, std::string &error);
[[nodiscard]] bool estimate_export(const export_request &request, export_estimate &estimate, std::string &error);
[[nodiscard]] bool check_export_capacity(std::uint64_t available, const export_estimate &estimate, std::string &error);
[[nodiscard]] bool check_export_space(const export_request &request, std::string &error);
}
