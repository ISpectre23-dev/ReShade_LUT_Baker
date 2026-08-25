#include "cube_lut.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <cstring>
#include <sstream>
#include <system_error>

namespace
{
std::atomic<std::uint64_t> s_temp_counter { 0 };

std::filesystem::path make_temp_path(const std::filesystem::path &destination)
{
    std::wostringstream suffix;
    suffix << L".tmp." << GetCurrentProcessId() << L'.' << s_temp_counter.fetch_add(1, std::memory_order_relaxed);
    return destination.parent_path() / (destination.filename().wstring() + suffix.str());
}

bool install_temp_file(
    const std::filesystem::path &temporary,
    const std::filesystem::path &destination,
    const bool overwrite,
    std::string &error)
{
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite)
        flags |= MOVEFILE_REPLACE_EXISTING;

    if (MoveFileExW(temporary.c_str(), destination.c_str(), flags) != FALSE)
        return true;

    const DWORD code = GetLastError();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);

    std::ostringstream message;
    message << "Unable to install output file (Windows error " << code << ").";
    if (!overwrite && code == ERROR_ALREADY_EXISTS)
        message << " The destination already exists.";
    error = message.str();
    return false;
}

void write_comment(std::ostream &stream, const std::string &label, const std::string &value)
{
    if (value.empty())
        return;

    stream << "# " << label << ": ";
    for (const unsigned char character : value)
        stream << (character < 0x20u ? ' ' : static_cast<char>(character));
    stream << '\n';
}

std::string sanitize_cube_text(const std::string &value, const bool quoted)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value)
    {
        if (character < 0x20u || (quoted && (character == '\"' || character == '\\')))
            result.push_back('_');
        else
            result.push_back(static_cast<char>(character));
    }
    return result;
}
}

namespace lut_baker
{
std::pair<std::uint32_t, std::uint32_t> choose_lattice_layout(const std::uint32_t size)
{
    if (size < 2)
        return { 0, 0 };

    const std::uint64_t sample_count = static_cast<std::uint64_t>(size) * size * size;
    std::uint64_t factor = static_cast<std::uint64_t>(std::sqrt(static_cast<long double>(sample_count)));
    while (factor > 1 && sample_count % factor != 0)
        --factor;

    const std::uint64_t other = sample_count / factor;
    if (other > std::numeric_limits<std::uint32_t>::max())
        return { 0, 0 };

    return {
        static_cast<std::uint32_t>(std::max(factor, other)),
        static_cast<std::uint32_t>(std::min(factor, other))
    };
}

std::vector<float4> make_identity_lattice(
    const std::uint32_t size,
    const std::uint32_t width,
    const std::uint32_t height)
{
    const std::uint64_t sample_count = static_cast<std::uint64_t>(size) * size * size;
    if (size < 2 || static_cast<std::uint64_t>(width) * height < sample_count)
        return {};

    std::vector<float4> pixels(static_cast<std::size_t>(width) * height);
    const float denominator = static_cast<float>(size - 1);

    std::size_t index = 0;
    for (std::uint32_t blue = 0; blue < size; ++blue)
    {
        for (std::uint32_t green = 0; green < size; ++green)
        {
            for (std::uint32_t red = 0; red < size; ++red, ++index)
            {
                pixels[index] = {
                    static_cast<float>(red) / denominator,
                    static_cast<float>(green) / denominator,
                    static_cast<float>(blue) / denominator,
                    1.0f
                };
            }
        }
    }

    return pixels;
}

error_metrics measure_identity_error(const std::vector<float4> &samples, const std::uint32_t size)
{
    error_metrics result;
    const std::uint64_t sample_count = static_cast<std::uint64_t>(size) * size * size;
    if (size < 2 || samples.size() < sample_count)
    {
        result.maximum_absolute = std::numeric_limits<double>::infinity();
        result.mean_absolute = std::numeric_limits<double>::infinity();
        result.rms = std::numeric_limits<double>::infinity();
        return result;
    }

    const double denominator = static_cast<double>(size - 1);
    double absolute_sum = 0.0;
    double squared_sum = 0.0;
    std::size_t index = 0;

    for (std::uint32_t blue = 0; blue < size; ++blue)
    {
        for (std::uint32_t green = 0; green < size; ++green)
        {
            for (std::uint32_t red = 0; red < size; ++red, ++index)
            {
                const double expected[3] = { red / denominator, green / denominator, blue / denominator };
                const double actual[3] = { samples[index].r, samples[index].g, samples[index].b };
                for (int channel = 0; channel < 3; ++channel)
                {
                    const double difference = std::abs(actual[channel] - expected[channel]);
                    result.maximum_absolute = std::max(result.maximum_absolute, difference);
                    absolute_sum += difference;
                    squared_sum += difference * difference;
                }
            }
        }
    }

    const double component_count = static_cast<double>(sample_count) * 3.0;
    result.mean_absolute = absolute_sum / component_count;
    result.rms = std::sqrt(squared_sum / component_count);
    return result;
}

std::uint16_t float_to_half(const float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    const std::uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu)
    {
        if (mantissa == 0)
            return static_cast<std::uint16_t>(sign | 0x7c00u);
        return static_cast<std::uint16_t>(sign | 0x7e00u);
    }

    const int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7c00u);

    if (half_exponent <= 0)
    {
        if (half_exponent < -10)
            return static_cast<std::uint16_t>(sign);

        std::uint32_t subnormal = mantissa | 0x800000u;
        const int shift = 14 - half_exponent;
        const std::uint32_t rounding = (1u << (shift - 1)) - 1u + ((subnormal >> shift) & 1u);
        subnormal = (subnormal + rounding) >> shift;
        return static_cast<std::uint16_t>(sign | subnormal);
    }

    std::uint32_t rounded_mantissa = mantissa + 0xfffu + ((mantissa >> 13) & 1u);
    std::uint32_t rounded_exponent = static_cast<std::uint32_t>(half_exponent);
    if ((rounded_mantissa & 0x800000u) != 0)
    {
        rounded_mantissa = 0;
        ++rounded_exponent;
        if (rounded_exponent >= 31)
            return static_cast<std::uint16_t>(sign | 0x7c00u);
    }

    return static_cast<std::uint16_t>(sign | (rounded_exponent << 10) | (rounded_mantissa >> 13));
}

float half_to_float(const std::uint16_t value) noexcept
{
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    int exponent = (value >> 10) & 0x1fu;
    std::uint32_t mantissa = value & 0x03ffu;
    std::uint32_t bits = 0;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            exponent = 1;
            while ((mantissa & 0x0400u) == 0)
            {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03ffu;
            const std::uint32_t float_exponent = static_cast<std::uint32_t>(exponent + (127 - 15));
            bits = sign | (float_exponent << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        const std::uint32_t float_exponent = static_cast<std::uint32_t>(exponent + (127 - 15));
        bits = sign | (float_exponent << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool validate_output_filename(std::string_view value, std::string &normalized, std::string &error)
{
    normalized.assign(value.begin(), value.end());
    while (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '.'))
        normalized.pop_back();

    if (normalized.empty())
    {
        normalized = make_timestamped_filename();
        return true;
    }

    const bool contains_control_character = std::any_of(normalized.begin(), normalized.end(), [](const unsigned char character) {
        return character < 0x20u;
    });
    if (normalized == "." || normalized == ".." || contains_control_character ||
        normalized.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        normalized.find("..") != std::string::npos)
    {
        error = "Use a file name only, without path separators, '..', or Windows-reserved characters.";
        return false;
    }

    if (normalized.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(), static_cast<int>(normalized.size()), nullptr, 0) == 0)
    {
        error = "The output filename is not valid UTF-8.";
        return false;
    }

    const std::size_t extension_position = normalized.find_last_of('.');
    if (extension_position == std::string::npos)
        normalized += ".cube";
    else if (normalized.substr(extension_position) != ".cube")
    {
        error = "The output file must use the lowercase .cube extension.";
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

std::string make_timestamped_filename()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
    localtime_s(&local, &time);

    std::ostringstream stream;
    stream << "ReShade_LUT_" << std::put_time(&local, "%Y%m%d_%H%M%S") << ".cube";
    return stream.str();
}

std::filesystem::path make_unique_output_path(const std::filesystem::path &directory, const std::string &filename)
{
    const std::filesystem::path requested = directory / std::filesystem::u8path(filename);
    std::error_code error;
    if (!std::filesystem::exists(requested, error))
        return requested;

    const std::filesystem::path stem = requested.stem();
    const std::filesystem::path extension = requested.extension();
    for (std::uint32_t index = 1; index < 10000; ++index)
    {
        std::ostringstream suffix;
        suffix << '_' << std::setw(3) << std::setfill('0') << index;
        const std::filesystem::path candidate = directory / (stem.wstring() + std::filesystem::path(suffix.str()).wstring() + extension.wstring());
        if (!std::filesystem::exists(candidate, error))
            return candidate;
    }

    return {};
}

bool write_cube_atomic(
    const std::filesystem::path &destination,
    const std::uint32_t size,
    const std::vector<float4> &samples,
    const cube_metadata &metadata,
    const bool overwrite,
    std::string &error)
{
    const std::uint64_t expected_count = static_cast<std::uint64_t>(size) * size * size;
    if (size < 2 || samples.size() != expected_count)
    {
        error = "The sample buffer does not contain exactly one complete 3D lattice.";
        return false;
    }

    for (std::size_t index = 0; index < expected_count; ++index)
    {
        if (!std::isfinite(samples[index].r) || !std::isfinite(samples[index].g) || !std::isfinite(samples[index].b))
        {
            std::ostringstream message;
            message << "The GPU result contains NaN or infinity at sample " << index << ". No LUT was written.";
            error = message.str();
            return false;
        }
    }

    std::error_code filesystem_error;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        error = "Unable to create the LUT_Bakes output directory: " + filesystem_error.message();
        return false;
    }

    if (!overwrite && std::filesystem::exists(destination, filesystem_error))
    {
        error = "The destination already exists.";
        return false;
    }

    const std::filesystem::path temporary = make_temp_path(destination);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        error = "Unable to create the temporary CUBE file.";
        return false;
    }

    stream.imbue(std::locale::classic());
    stream << "# ReShade LUT Baker\n";
    write_comment(stream, "Exporter version", metadata.exporter_version);
    write_comment(stream, "ReShade API", metadata.reshade_api);
    write_comment(stream, "Graphics API", metadata.graphics_api);
    write_comment(stream, "Source buffer", metadata.source_buffer);
    write_comment(stream, "Bake buffer", metadata.bake_buffer);
    stream << "# LUT size: " << size << '\n';
    stream << "# Selected techniques in verified execution order: " << metadata.techniques.size() << '\n';
    for (std::size_t index = 0; index < metadata.techniques.size(); ++index)
        stream << "#   " << (index + 1) << ". " << sanitize_cube_text(metadata.techniques[index], false) << '\n';
    for (const std::string &warning : metadata.warnings)
        stream << "# WARNING: " << sanitize_cube_text(warning, false) << '\n';

    stream << "TITLE \"" << sanitize_cube_text(metadata.title.empty() ? "ReShade LUT" : metadata.title, true) << "\"\n";
    stream << "LUT_3D_SIZE " << size << '\n';
    stream << "DOMAIN_MIN 0.0 0.0 0.0\n";
    stream << "DOMAIN_MAX 1.0 1.0 1.0\n\n";
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << std::defaultfloat;

    for (std::size_t index = 0; index < expected_count; ++index)
        stream << samples[index].r << ' ' << samples[index].g << ' ' << samples[index].b << '\n';

    stream.flush();
    if (!stream)
    {
        stream.close();
        std::filesystem::remove(temporary, filesystem_error);
        error = "The CUBE write failed before all samples were committed.";
        return false;
    }
    stream.close();
    if (!stream)
    {
        std::filesystem::remove(temporary, filesystem_error);
        error = "The CUBE file could not be closed cleanly.";
        return false;
    }

    return install_temp_file(temporary, destination, overwrite, error);
}
}
