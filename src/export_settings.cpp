#include "export_settings.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace
{
bool ends_with(const std::string_view value, const std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool preset_size(const std::uint32_t size)
{
    return size == 16 || size == 32 || size == 64 || size == 128;
}
}

namespace lut_baker
{
const char *output_extension(const output_format format) noexcept
{
    return format == output_format::png ? ".png" : format == output_format::rise_tex ? ".tex.28" : ".cube";
}

const char *output_format_name(const output_format format) noexcept
{
    return format == output_format::png ? "PNG" : format == output_format::rise_tex ? "Monster Hunter Rise TEX" : "CUBE";
}

std::uint32_t effective_lattice_size(const export_preferences &preferences) noexcept
{
    if (preferences.format == output_format::rise_tex)
        return 32;
    if (preferences.format == output_format::png)
        return preferences.png_size;
    return preferences.cube_custom
        ? (preferences.custom_cube_size > 0 ? static_cast<std::uint32_t>(preferences.custom_cube_size) : 0u)
        : preferences.cube_size;
}

bool validate_output_filename(const std::string_view value, std::string &normalized, std::string &error,
    const output_format format)
{
    normalized.assign(value.begin(), value.end());
    if (normalized == "." || normalized.find("..") != std::string::npos)
    {
        error = "Use a file name only, without path traversal.";
        return false;
    }
    while (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '.'))
        normalized.pop_back();

    if (normalized.empty())
    {
        normalized = make_timestamped_filename(format);
        return true;
    }

    const bool contains_control_character = std::any_of(normalized.begin(), normalized.end(), [](const unsigned char character) {
        return character < 0x20u;
    });
    if (normalized == "." || normalized == ".." || contains_control_character ||
        normalized.find_first_of("<>:\"/\\|?*") != std::string::npos || normalized.find("..") != std::string::npos)
    {
        error = "Use a file name only: no path separators, '..' or Windows-reserved characters.";
        return false;
    }

    if (normalized.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(), static_cast<int>(normalized.size()), nullptr, 0) == 0)
    {
        error = "The output filename is not valid UTF-8.";
        return false;
    }

    const std::string suffix = output_extension(format);
    if (normalized.find('.') == std::string::npos)
        normalized += suffix;
    else if (!ends_with(normalized, suffix))
    {
        error = "The file name must end in " + suffix + " (lowercase), or have no extension so it is added automatically.";
        return false;
    }
    // A known suffix left inside a compound filename is almost certainly a
    // format-switch mistake, not a meaningful Rise basename.
    if (format == output_format::rise_tex && ends_with(normalized.substr(0, normalized.size() - suffix.size()), ".cube"))
    {
        error = "Use a Rise basename without the .cube extension.";
        return false;
    }
    const auto basename = normalized.substr(0, normalized.size() - suffix.size());
    for (const char *const other_suffix : { ".cube", ".png", ".tex.28" })
    {
        if (suffix != other_suffix && ends_with(basename, other_suffix))
        {
            error = "The name contains another export format's extension. Use a name without extension or the matching one.";
            return false;
        }
    }

    std::string stem = normalized.substr(0, normalized.find('.'));
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    const bool numbered_device = stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
        (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0);
    if (stem.empty() || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CLOCK$" || stem == "CONIN$" || stem == "CONOUT$" || numbered_device)
    {
        error = "The output filename is reserved by Windows.";
        return false;
    }
    return true;
}

std::string make_timestamped_filename(const output_format format)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
    localtime_s(&local, &time);
    std::ostringstream stream;
    stream << "ReShade_LUT_" << std::put_time(&local, "%Y%m%d_%H%M%S") << output_extension(format);
    return stream.str();
}

std::filesystem::path make_unique_output_path(const std::filesystem::path &directory, const std::string &filename,
    const output_format format)
{
    const std::string suffix = output_extension(format);
    if (!ends_with(filename, suffix))
        return {};
    const std::filesystem::path requested = directory / std::filesystem::u8path(filename);
    std::error_code error;
    const bool exists = std::filesystem::exists(requested, error);
    if (error)
        return {};
    if (!exists)
        return requested;

    const std::string stem = filename.substr(0, filename.size() - suffix.size());
    for (std::uint32_t index = 1; index < 10000; ++index)
    {
        std::ostringstream candidate_name;
        candidate_name << stem << '_' << std::setw(3) << std::setfill('0') << index << suffix;
        const std::filesystem::path candidate = directory / std::filesystem::u8path(candidate_name.str());
        const bool candidate_exists = std::filesystem::exists(candidate, error);
        if (error)
            return {};
        if (!candidate_exists)
            return candidate;
    }
    return {};
}

std::string filename_for_format(const std::string_view filename, const output_format previous, const output_format next)
{
    const std::string suffix = output_extension(previous);
    if (!ends_with(filename, suffix))
        return std::string(filename); // Empty/bare names stay bare; invalid input stays visible.
    return std::string(filename.substr(0, filename.size() - suffix.size())) + output_extension(next);
}

bool snapshot_export_request(const export_preferences &preferences, const std::string_view filename,
    const std::filesystem::path &directory, export_request &request, std::string &error)
{
    if (!validate_export_preferences(preferences, error))
        return false;
    export_request snapshot;
    snapshot.format = preferences.format;
    snapshot.lattice_size = effective_lattice_size(preferences);
    snapshot.range = preferences.format == output_format::rise_tex ? preferences.rise_range :
        preferences.format == output_format::png ? preferences.png_range : range_policy::reject;
    snapshot.png_distribution = preferences.png_distribution;
    snapshot.png_depth = preferences.png_depth;
    snapshot.directory = directory;
    if (!validate_output_filename(filename, snapshot.filename, error, snapshot.format))
        return false;
    request = std::move(snapshot);
    return true;
}

bool validate_export_preferences(const export_preferences &preferences, std::string &error)
{
    if (preferences.format != output_format::cube && preferences.format != output_format::rise_tex && preferences.format != output_format::png)
    {
        error = "Unknown export format. Choose CUBE, PNG or Monster Hunter Rise.";
        return false;
    }
    if (preferences.format == output_format::cube)
    {
        if (preferences.cube_custom ? preferences.custom_cube_size < 2 || preferences.custom_cube_size > 128 : !preset_size(preferences.cube_size))
        {
            error = "CUBE size must be an integer from 2 to 128. Choose a preset or correct the custom size.";
            return false;
        }
    }
    else if (preferences.format == output_format::png)
    {
        if (!valid_png_bit_depth(preferences.png_depth))
        {
            error = "Invalid PNG bit depth. Choose 8 or 16 bits per channel before baking.";
            return false;
        }
        if (!preset_size(preferences.png_size) ||
            (preferences.png_distribution != png_layout::horizontal && preferences.png_distribution != png_layout::square))
        {
            error = "Invalid PNG size or layout. Choose a listed size and a layout.";
            return false;
        }
        if (preferences.png_distribution == png_layout::square && preferences.png_size != 16 && preferences.png_size != 64)
        {
            error = "Square tiles only supports sizes 16 and 64. Choose one of those, or switch to Horizontal strip.";
            return false;
        }
    }
    const range_policy policy = preferences.format == output_format::png ? preferences.png_range : preferences.rise_range;
    if (policy != range_policy::reject && policy != range_policy::clamp)
    {
        error = "Invalid range policy. Choose whether to reject or clamp values outside 0-1.";
        return false;
    }
    return true;
}

bool estimate_export(const export_request &request, export_estimate &estimate, std::string &error)
{
    estimate = {};
    if (!valid_lut_size(request.lattice_size))
    {
        error = "Export size must be from 2 to 128. No bake was started.";
        return false;
    }
    estimate.samples = static_cast<std::uint64_t>(request.lattice_size) * request.lattice_size * request.lattice_size;
    estimate.float_buffer_bytes = estimate.samples * 16;
    switch (request.format)
    {
    case output_format::cube:
        // At most 15 characters per finite float at max_digits10, plus separators.
        // Allow one MiB for comments/header. The capacity check adds a further reserve.
        estimate.file_bytes = estimate.samples * 48 + 1024 * 1024;
        return true;
    case output_format::rise_tex:
        if (request.lattice_size != 32)
            break;
        estimate.file_bytes = 56 + estimate.samples * 4;
        return true;
    case output_format::png:
        if (!valid_png_bit_depth(request.png_depth))
            break;
        if (request.png_distribution == png_layout::horizontal)
        {
            estimate.image_width = request.lattice_size * request.lattice_size;
            estimate.image_height = request.lattice_size;
        }
        else if (request.png_distribution == png_layout::square && (request.lattice_size == 16 || request.lattice_size == 64))
        {
            const std::uint32_t tiles = request.lattice_size == 16 ? 4u : 8u;
            estimate.image_width = estimate.image_height = request.lattice_size * tiles;
        }
        else
            break;
        // RGB scanlines and worst-case deflate/chunk overhead fit comfortably here.
        estimate.file_bytes = estimate.samples * (request.png_depth == png_bit_depth::sixteen ? 8 : 4) + 1024 * 1024;
        return true;
    }
    estimate = {};
    error = "Invalid export size, layout or bit depth. Correct the output settings before baking.";
    return false;
}

bool check_export_capacity(const std::uint64_t available, const export_estimate &estimate, std::string &error)
{
    constexpr std::uint64_t reserve = 16 * 1024 * 1024;
    if (estimate.file_bytes == 0 || available < estimate.file_bytes || available - estimate.file_bytes < reserve)
    {
        error = "Not enough free space for this export and a 16 MiB safety reserve. No file was written. Free space or choose a smaller size.";
        return false;
    }
    return true;
}

bool check_export_space(const export_request &request, std::string &error)
{
    export_estimate estimate;
    if (!estimate_export(request, estimate, error))
        return false;
    std::error_code filesystem_error;
    auto directory = std::filesystem::absolute(request.directory, filesystem_error);
    while (!filesystem_error && !directory.empty() && !std::filesystem::exists(directory, filesystem_error))
    {
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    const auto capacity = filesystem_error || directory.empty() ? std::filesystem::space_info {} : std::filesystem::space(directory, filesystem_error);
    if (!filesystem_error && !directory.empty() && !std::filesystem::is_directory(directory, filesystem_error))
    {
        error = "The output path contains a file where a folder is needed. No file was written. Move or rename the file blocking the output folder.";
        return false;
    }
    if (filesystem_error || directory.empty() || capacity.available == static_cast<std::uintmax_t>(-1))
    {
        error = "Cannot check free space in the output folder. No file was written. Check the folder's permissions and storage availability.";
        return false;
    }
    return check_export_capacity(capacity.available, estimate, error);
}
}
