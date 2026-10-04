#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lut_baker
{
enum class output_format { cube, rise_tex };
enum class range_policy { reject, clamp };

struct export_preferences
{
    output_format format = output_format::cube;
    std::uint32_t cube_size = 64;
    range_policy rise_range = range_policy::reject;
};

// Captured once at queue time. GPU allocation, readback and the CPU worker
// must consume this snapshot, never the editable preferences above.
struct export_request
{
    output_format format = output_format::cube;
    std::uint32_t lattice_size = 64;
    range_policy range = range_policy::reject;
    std::filesystem::path directory;
    std::string filename;
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
}
