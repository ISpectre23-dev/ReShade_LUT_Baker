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
}

namespace lut_baker
{
const char *output_extension(const output_format format) noexcept
{
    return format == output_format::rise_tex ? ".tex.28" : ".cube";
}

const char *output_format_name(const output_format format) noexcept
{
    return format == output_format::rise_tex ? "Monster Hunter Rise TEX" : "CUBE";
}

std::uint32_t effective_lattice_size(const export_preferences &preferences) noexcept
{
    return preferences.format == output_format::rise_tex ? 32u : preferences.cube_size;
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
    if ((preferences.format != output_format::cube && preferences.format != output_format::rise_tex) ||
        (preferences.cube_size != 16 && preferences.cube_size != 32 && preferences.cube_size != 64) ||
        (preferences.rise_range != range_policy::reject && preferences.rise_range != range_policy::clamp))
    {
        error = "Invalid export format, LUT size or range policy.";
        return false;
    }
    export_request snapshot;
    snapshot.format = preferences.format;
    snapshot.lattice_size = effective_lattice_size(preferences);
    snapshot.range = preferences.format == output_format::rise_tex ? preferences.rise_range : range_policy::reject;
    snapshot.directory = directory;
    if (!validate_output_filename(filename, snapshot.filename, error, snapshot.format))
        return false;
    request = std::move(snapshot);
    return true;
}
}
