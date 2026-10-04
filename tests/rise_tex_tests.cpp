#include "atomic_output.hpp"
#include "rise_tex.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
int failures = 0;
void expect(const bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

std::filesystem::path make_test_directory()
{
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto directory = std::filesystem::temp_directory_path() /
            ("reshade_lut_rise_tests_" + std::to_string(seed) + '_' + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(directory, error))
            return directory;
    }
    return {};
}
}

int main()
{
    using namespace lut_baker;
    const auto directory = make_test_directory();
    expect(!directory.empty(), "create owned test directory");
    if (directory.empty())
        return 1;

    const auto layout = choose_lattice_layout(32);
    const auto identity = make_identity_lattice(32, layout.first, layout.second);
    expect(layout.first == 256 && layout.second == 128 && identity.size() == 32768, "32-cubed rectangular lattice");
    std::vector<std::uint8_t> bytes;
    rise_export_metrics metrics;
    std::string error;
    expect(serialize_rise_tex(32, identity, range_policy::reject, bytes, metrics, error), "serialize identity");
    expect(bytes.size() == 131128, "exact TEX file size");
    // Independently specified bytes, not an assertion against the writer's own constant.
    const std::vector<std::uint8_t> reference_header {
        84,69,88,0,28,0,0,0,32,0,32,0,32,0,1,16,29,0,0,0,255,255,255,255,0,0,0,0,0,8,0,0,
        0,0,0,0,0,0,0,0,56,0,0,0,0,0,0,0,128,0,0,0,0,16,0,0
    };
    expect(bytes.size() >= 56 && std::equal(reference_header.begin(), reference_header.end(), bytes.begin()), "all 56 header bytes match verified profile");
    bool order_correct = bytes.size() == rise_file_size;
    double ideal_maximum = 0.0;
    for (std::uint32_t b = 0; order_correct && b < 32; ++b)
        for (std::uint32_t g = 0; g < 32; ++g)
            for (std::uint32_t r = 0; r < 32; ++r)
            {
                const std::size_t offset = 56 + 4 * ((b * 32 + g) * 32 + r);
                const double ideal[] = { r / 31.0, g / 31.0, b / 31.0 };
                const float rgb[] = { static_cast<float>(ideal[0]), static_cast<float>(ideal[1]), static_cast<float>(ideal[2]) };
                for (std::size_t c = 0; c < 3; ++c)
                {
                    order_correct &= bytes[offset + c] == static_cast<std::uint8_t>(std::floor(static_cast<double>(rgb[c]) * 255.0 + 0.5));
                    ideal_maximum = std::max(ideal_maximum, std::abs(bytes[offset + c] / 255.0 - ideal[c]));
                }
                order_correct &= bytes[offset + 3] == 255;
            }
    expect(order_correct, "all nodes, corners, axes and fixed alpha are ordered RGB red-fastest");
    expect(ideal_maximum <= 0.5 / 255.0 + measure_identity_error(identity, 32).maximum_absolute, "identity quantization bound includes FP32 input error");
    expect(metrics.quantization.maximum_absolute <= 0.5 / 255.0 + 1e-15, "quantization error measured against actual float input");
    expect(metrics.source_minimum == 0 && metrics.source_maximum == 1 && metrics.clipped_components == 0, "identity range and counts");

    auto half_identity = identity;
    for (auto &sample : half_identity)
    {
        sample.r = half_to_float(float_to_half(sample.r));
        sample.g = half_to_float(float_to_half(sample.g));
        sample.b = half_to_float(float_to_half(sample.b));
    }
    const auto half_error = measure_identity_error(half_identity, 32);
    expect(half_error.maximum_absolute <= 5e-4 && serialize_rise_tex(32, half_identity, range_policy::reject, bytes, metrics, error), "FP16 identity input is valid independently of TEX quantization");
    expect(metrics.quantization.maximum_absolute <= 0.5 / 255.0 + 1e-15, "FP16 quantization metrics exclude prior bake error");

    auto exchanged = identity;
    for (auto &sample : exchanged)
        std::swap(sample.r, sample.b);
    expect(serialize_rise_tex(32, exchanged, range_policy::reject, bytes, metrics, error), "channel exchange fixture");
    expect(bytes[56 + 4 * 31] == 0 && bytes[56 + 4 * 31 + 2] == 255 && bytes[56 + 4 * 31 * 32 * 32] == 255, "channel exchange does not reorder sample axes");

    auto rounding = identity;
    rounding[0] = { 0.5f, 0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN() };
    expect(serialize_rise_tex(32, rounding, range_policy::reject, bytes, metrics, error) && bytes[56] == 128 && bytes[57] == 0 && bytes[58] == 255 && bytes[59] == 255, "half-up rounding, endpoints, shader alpha ignored");

    const auto invalid_output = directory / "invalid.tex.28";
    expect(!write_rise_tex_atomic(invalid_output, 64, identity, range_policy::reject, metrics, error), "reject non-32 size");
    auto incomplete = identity;
    incomplete.pop_back();
    expect(!serialize_rise_tex(32, incomplete, range_policy::reject, bytes, metrics, error) && bytes.empty(), "reject incomplete buffer with no bytes");
    incomplete = identity;
    incomplete.push_back({});
    expect(!serialize_rise_tex(32, incomplete, range_policy::reject, bytes, metrics, error), "reject extra samples");
    expect(!serialize_rise_tex(32, identity, static_cast<range_policy>(99), bytes, metrics, error), "reject invalid policy");
    for (const float value : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() })
    {
        auto invalid = identity;
        invalid[9].g = value;
        for (const auto policy : { range_policy::reject, range_policy::clamp })
            expect(!write_rise_tex_atomic(invalid_output, 32, invalid, policy, metrics, error) && !std::filesystem::exists(invalid_output), "non-finite RGB always rejects, no final file");
    }
    auto out_of_range = identity;
    out_of_range[0] = { -0.25f, 1.25f, 0.5f, 0.0f };
    out_of_range[7].b = 2.0f;
    const auto original = out_of_range;
    expect(!write_rise_tex_atomic(invalid_output, 32, out_of_range, range_policy::reject, metrics, error) && !std::filesystem::exists(invalid_output), "out-of-range rejection leaves no file");
    expect(error.find("Source range") != std::string::npos && error.find("Clamp") != std::string::npos, "range failure explains original range and choice");
    expect(serialize_rise_tex(32, out_of_range, range_policy::clamp, bytes, metrics, error), "explicit clipping succeeds");
    expect(metrics.clipped_components == 3 && metrics.clipped_samples == 2 && metrics.source_minimum == -0.25 && metrics.source_maximum == 2.0, "exact clipping counts and original range");
    expect(bytes[56] == 0 && bytes[57] == 255 && bytes[58] == 128 && bytes[56 + 7 * 4 + 2] == 255, "clipping endpoints without normalization");
    expect(std::memcmp(original.data(), out_of_range.data(), original.size() * sizeof(float4)) == 0, "Rise clipping never mutates shared float samples");
    cube_metadata metadata;
    metadata.techniques = { "First.fx :: B", "Second.fx :: A" };
    metadata.warnings = { "Test warning" };
    // This is the first successful atomic write in this process. Simulate a
    // stale temporary from PID reuse and prove it is not overwritten/deleted.
    const auto stale = directory / ("range.cube.tmp." + std::to_string(GetCurrentProcessId()) + ".0");
    { std::ofstream sentinel(stale); sentinel << "stale"; }
    expect(write_cube_atomic(directory / "range.cube", 32, out_of_range, metadata, false, error), "CUBE still accepts original negative and super-white samples");
    expect(read_bytes(stale) == std::vector<std::uint8_t>({ 's', 't', 'a', 'l', 'e' }), "temporary reservation preserves a stale file not owned by this writer");
    const auto cube_bytes = read_bytes(directory / "range.cube");
    const std::string cube_text(cube_bytes.begin(), cube_bytes.end());
    expect(cube_text.find("#   1. First.fx :: B\n#   2. Second.fx :: A\n") != std::string::npos &&
        cube_text.find("# WARNING: Test warning") != std::string::npos && cube_text.find("-0.25 1.25 0.5") != std::string::npos,
        "CUBE preserves verified order, metadata and unclipped samples");

    for (const auto format : { output_format::cube, output_format::rise_tex })
    {
        std::string normalized;
        const std::string extension = output_extension(format);
        expect(validate_output_filename("MyGrade", normalized, error, format) && normalized == "MyGrade" + extension, "format-specific suffix addition");
        expect(validate_output_filename("MyGrade" + extension, normalized, error, format) && normalized == "MyGrade" + extension, "complete suffix retained");
        expect(validate_output_filename("", normalized, error, format) && normalized.find("ReShade_LUT_") == 0 && normalized.substr(normalized.size() - extension.size()) == extension, "empty name timestamps correct format");
        expect(validate_output_filename("Color_\xc3\xa1" + extension, normalized, error, format), "valid UTF-8 filename");
        for (const std::string &invalid : { "CON", "con", "AUX", "COM1", "LPT9", "CLOCK$", "CONIN$", "CONOUT$", ".", "..", "../escape", "a/b", "a\\b", "a:b", "bad\nname", "name.dds" })
            expect(!validate_output_filename(invalid, normalized, error, format), "unsafe or incompatible filename rejected");
        expect(!validate_output_filename(std::string("bad\xff", 4), normalized, error, format), "invalid UTF-8 rejected");
        const auto output = directory / std::filesystem::u8path("MyGrade" + extension);
        { std::ofstream existing(output); existing << "keep"; }
        expect(make_unique_output_path(directory, "MyGrade" + extension, format).filename() == std::filesystem::u8path("MyGrade_001" + extension), "collision suffix before full format extension");
    }
    std::string normalized;
    expect(!validate_output_filename("bad.cube.tex.28", normalized, error, output_format::rise_tex), "double known extension rejected");
    expect(filename_for_format("MyGrade.cube", output_format::cube, output_format::rise_tex) == "MyGrade.tex.28", "switch suffix to Rise");
    expect(filename_for_format("MyGrade.tex.28", output_format::rise_tex, output_format::cube) == "MyGrade.cube", "switch suffix back to CUBE");
    expect(filename_for_format("MyGrade", output_format::cube, output_format::rise_tex) == "MyGrade", "bare name preserved on switch");

    export_preferences preferences;
    export_request snapshot;
    expect(snapshot_export_request(preferences, "queued", directory, snapshot, error) && snapshot.lattice_size == 64 && snapshot.format == output_format::cube, "default CUBE snapshot");
    preferences.format = output_format::rise_tex;
    preferences.rise_range = range_policy::clamp;
    expect(effective_lattice_size(preferences) == 32 && snapshot.lattice_size == 64 && snapshot.filename == "queued.cube", "queued snapshot independent of edited preferences");
    export_request rise_snapshot;
    expect(snapshot_export_request(preferences, "rise", directory, rise_snapshot, error) && rise_snapshot.lattice_size == 32 && rise_snapshot.range == range_policy::clamp, "Rise snapshot is 32-cubed with explicit policy");
    preferences.format = output_format::cube;
    expect(effective_lattice_size(preferences) == 64 && preferences.cube_size == 64, "CUBE -> Rise -> CUBE retains saved CUBE size");
    preferences.cube_size = 16;
    preferences.rise_range = range_policy::reject;
    expect(rise_snapshot.format == output_format::rise_tex && rise_snapshot.lattice_size == 32 && rise_snapshot.filename == "rise.tex.28" && rise_snapshot.directory == directory && rise_snapshot.range == range_policy::clamp, "active format, size, directory, name, policy immutable across switch");
    export_request cube16_snapshot;
    expect(snapshot_export_request(preferences, "next", directory, cube16_snapshot, error) && cube16_snapshot.lattice_size == 16, "next CUBE request uses new preferences independently");
    preferences.cube_size = 128;
    expect(!snapshot_export_request(preferences, "invalid", directory, cube16_snapshot, error) && cube16_snapshot.filename == "next.cube", "invalid settings do not overwrite snapshot");

    const auto output = directory / "identity.tex.28";
    expect(write_rise_tex_atomic(output, 32, identity, range_policy::reject, metrics, error), "atomic TEX writer succeeds");
    const auto existing = read_bytes(output);
    expect(existing.size() == rise_file_size, "actual binary file byte count");
    expect(!write_rise_tex_atomic(output, 32, exchanged, range_policy::reject, metrics, error) && read_bytes(output) == existing, "existing TEX is never overwritten");
    const auto race_output = directory / "race.tex.28";
    expect(!write_file_atomic(race_output, false, [&race_output](std::ostream &stream, std::string &) {
        stream << "new";
        std::ofstream collision(race_output, std::ios::binary);
        collision << "old";
        return true;
    }, error) && read_bytes(race_output) == std::vector<std::uint8_t>({ 'o', 'l', 'd' }), "commit race preserves concurrently-created destination");
    expect(!write_file_atomic(directory / "fail.tex.28", false, [](std::ostream &stream, std::string &) {
        stream.setstate(std::ios::badbit);
        return true;
    }, error) && !std::filesystem::exists(directory / "fail.tex.28"), "failed stream does not install partial output");
    try
    {
        (void)write_file_atomic(directory / "throw.tex.28", false, [](std::ostream &, std::string &) -> bool { throw std::runtime_error("test failure"); }, error);
        expect(false, "serializer exception propagates");
    }
    catch (const std::runtime_error &) {}
    expect(!write_rise_tex_atomic(output / "child.tex.28", 32, identity, range_policy::reject, metrics, error), "file used as parent fails cleanly");
    const auto locked = directory / "locked.tex.28";
    const HANDLE lock = CreateFileW(locked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(lock != INVALID_HANDLE_VALUE, "create locked destination");
    expect(!write_rise_tex_atomic(locked, 32, identity, range_policy::reject, metrics, error), "locked/pre-existing destination rejected");
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    bool temporary_left = false;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        temporary_left |= entry.path() != stale && entry.path().filename().string().find(".tmp.") != std::string::npos;
    expect(!temporary_left && !std::filesystem::exists(directory / "throw.tex.28"), "failure and exception paths remove only owned temporary");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0)
        std::cout << "All Rise TEX / export state / atomic output tests passed. Identity ideal max error: " << ideal_maximum << '\n';
    return failures == 0 ? 0 : 1;
}
