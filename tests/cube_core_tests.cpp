#include "cube_lut.hpp"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace
{
int s_failures = 0;

void expect(const bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++s_failures;
    }
}

bool nearly_equal(const double left, const double right, const double tolerance = 1.0e-7)
{
    return std::abs(left - right) <= tolerance;
}

bool same_float_bits(const float left, const float right)
{
    std::uint32_t left_bits = 0;
    std::uint32_t right_bits = 0;
    std::memcpy(&left_bits, &left, sizeof(left_bits));
    std::memcpy(&right_bits, &right, sizeof(right_bits));
    return left_bits == right_bits;
}
}

int main()
{
    const auto layout = lut_baker::choose_lattice_layout(64);
    expect(layout.first == 512 && layout.second == 512, "64^3 must use a 512 x 512 texture");

    const auto identity = lut_baker::make_identity_lattice(64, layout.first, layout.second);
    expect(identity.size() == 262144, "identity lattice sample count");
    expect(nearly_equal(identity.front().r, 0.0) && nearly_equal(identity.front().g, 0.0) && nearly_equal(identity.front().b, 0.0), "first sample is black");
    expect(nearly_equal(identity[1].r, 1.0 / 63.0) && nearly_equal(identity[1].g, 0.0), "red is the fastest-changing CUBE axis");
    expect(nearly_equal(identity[64].g, 1.0 / 63.0) && nearly_equal(identity[64].r, 0.0), "green is the middle CUBE axis");
    expect(nearly_equal(identity.back().r, 1.0) && nearly_equal(identity.back().g, 1.0) && nearly_equal(identity.back().b, 1.0), "last sample is white");

    const auto metrics = lut_baker::measure_identity_error(identity, 64);
    expect(metrics.maximum_absolute <= 3.0e-8, "float identity maximum error");
    expect(metrics.mean_absolute <= 1.0e-8, "float identity mean error");
    expect(metrics.rms <= 2.0e-8, "float identity RMS error");

    for (const float value : { -2.0f, -0.0f, 0.0f, 0.125f, 0.5f, 1.0f, 3.5f })
    {
        const float round_trip = lut_baker::half_to_float(lut_baker::float_to_half(value));
        expect(std::abs(round_trip - value) <= std::max(1.0e-6f, std::abs(value) * 0.001f), "half conversion round trip");
    }

    bool every_half_round_trips = true;
    for (std::uint32_t bits = 0; bits <= 0xffffu; ++bits)
    {
        const std::uint16_t half = static_cast<std::uint16_t>(bits);
        const bool is_nan = (half & 0x7c00u) == 0x7c00u && (half & 0x03ffu) != 0;
        const std::uint16_t round_trip = lut_baker::float_to_half(lut_baker::half_to_float(half));
        if ((!is_nan && round_trip != half) ||
            (is_nan && ((round_trip & 0x7c00u) != 0x7c00u || (round_trip & 0x03ffu) == 0)))
        {
            every_half_round_trips = false;
            break;
        }
    }
    expect(every_half_round_trips, "all finite/infinite half values round trip exactly and NaNs remain NaN");

    std::string normalized;
    std::string error;
    expect(lut_baker::validate_output_filename("Example", normalized, error) && normalized == "Example.cube", "filename extension is added");
    expect(!lut_baker::validate_output_filename("../escape.cube", normalized, error), "path traversal is rejected");
    expect(!lut_baker::validate_output_filename(std::string("bad\nname.cube"), normalized, error), "control characters are rejected");
    expect(!lut_baker::validate_output_filename("CON.cube", normalized, error), "Windows device filenames are rejected");
    expect(!lut_baker::validate_output_filename("Example.CUBE", normalized, error), "uppercase extension is rejected for ReShade compatibility");

    std::filesystem::path test_directory;
    std::error_code ignored;
    const auto unique_seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100 && test_directory.empty(); ++attempt)
    {
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
            ("reshade_lut_baker_core_tests_" + std::to_string(unique_seed) + '_' + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ignored))
            test_directory = candidate;
        ignored.clear();
    }
    expect(!test_directory.empty(), "unique test directory can be created");
    if (test_directory.empty())
        return 1;
    const std::filesystem::path output = test_directory / "identity.cube";

    lut_baker::cube_metadata metadata;
    metadata.title = "Core identity test";
    metadata.exporter_version = "test";
    metadata.reshade_api = "20";
    expect(lut_baker::write_cube_atomic(output, 64, identity, metadata, false, error), "CUBE writer succeeds");
    expect(std::filesystem::exists(output), "CUBE output exists");
    expect(!lut_baker::write_cube_atomic(output, 64, identity, metadata, false, error), "CUBE writer does not overwrite by default");
    expect(lut_baker::make_unique_output_path(test_directory, "identity.cube").filename() == "identity_001.cube", "existing exports receive a non-overwriting numeric suffix");
    const std::filesystem::path alias = test_directory / "latest.cube";
    expect(lut_baker::copy_file_atomic(output, alias, true, error), "preview alias copy succeeds");
    expect(lut_baker::copy_file_atomic(output, alias, true, error), "preview alias overwrite is explicit and repeatable");

    auto non_finite = identity;
    non_finite[7].g = std::numeric_limits<float>::quiet_NaN();
    expect(!lut_baker::write_cube_atomic(test_directory / "invalid.cube", 64, non_finite, metadata, false, error), "CUBE writer rejects non-finite samples");

    std::ifstream stream(output);
    std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    expect(contents.find("LUT_3D_SIZE 64") != std::string::npos, "CUBE size header");
    expect(contents.find("DOMAIN_MIN 0.0 0.0 0.0") != std::string::npos, "CUBE domain header");

    auto round_trip_samples = lut_baker::make_identity_lattice(2, 4, 2);
    round_trip_samples[0] = { -0.123456791f, 1.23456788f, 0.333333343f, 1.0f };
    const std::filesystem::path round_trip_output = test_directory / "roundtrip.cube";
    expect(lut_baker::write_cube_atomic(round_trip_output, 2, round_trip_samples, metadata, false, error), "round-trip CUBE writer succeeds");
    std::ifstream round_trip_stream(round_trip_output);
    std::string line;
    std::vector<lut_baker::float4> parsed;
    while (std::getline(round_trip_stream, line))
    {
        std::istringstream row(line);
        lut_baker::float4 value;
        if (row >> value.r >> value.g >> value.b)
            parsed.push_back(value);
    }
    bool every_written_float_round_trips = parsed.size() == round_trip_samples.size();
    for (std::size_t index = 0; every_written_float_round_trips && index < parsed.size(); ++index)
    {
        every_written_float_round_trips = same_float_bits(parsed[index].r, round_trip_samples[index].r) &&
            same_float_bits(parsed[index].g, round_trip_samples[index].g) &&
            same_float_bits(parsed[index].b, round_trip_samples[index].b);
    }
    expect(every_written_float_round_trips, "max_digits10 CUBE rows round trip every binary32 RGB value");

    std::filesystem::remove_all(test_directory, ignored);
    if (s_failures == 0)
        std::cout << "All cube core tests passed.\n";
    return s_failures == 0 ? 0 : 1;
}
