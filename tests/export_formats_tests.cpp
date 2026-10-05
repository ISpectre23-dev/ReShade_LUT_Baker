#include "cube_lut.hpp"
#include "export_settings.hpp"

#include <filesystem>
#include <iostream>
#include <limits>

namespace
{
int failures = 0;
void expect(const bool condition, const char *const label)
{
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
}

int main()
{
    using namespace lut_baker;
    export_preferences preferences;
    std::string error, filename;
    export_request request;
    const auto directory = std::filesystem::temp_directory_path();
    expect(snapshot_export_request(preferences, "default", directory, request, error) && request.format == output_format::cube && request.lattice_size == 64, "default remains CUBE 64");
    for (const std::uint32_t size : { 16u, 32u, 64u, 128u })
    {
        preferences.cube_size = size;
        expect(snapshot_export_request(preferences, "size", directory, request, error) && request.lattice_size == size, "all CUBE presets accepted");
    }
    const auto layout128 = choose_lattice_layout(128);
    expect(layout128.first == 2048 && layout128.second == 1024, "128 has balanced offscreen dimensions");
    for (const int size : { 2, 3, 17, 33, 48, 65, 96, 127, 128 })
    {
        preferences.cube_custom = true;
        preferences.custom_cube_size = size;
        expect(snapshot_export_request(preferences, "custom", directory, request, error) && request.lattice_size == static_cast<std::uint32_t>(size), "custom sizes accepted");
        const auto layout = choose_lattice_layout(request.lattice_size);
        const auto identity = make_identity_lattice(request.lattice_size, layout.first, layout.second);
        const auto count = static_cast<std::uint64_t>(size) * size * size;
        expect(identity.size() == count && layout.first <= 16384 && layout.second <= 16384, "custom dimensions bounded and contain all samples");
        expect(measure_identity_error(identity, request.lattice_size).maximum_absolute <= 3e-8, "custom identity float tolerance");
    }
    const auto saved = request;
    for (const int invalid : { -1, 0, 1, 129, std::numeric_limits<int>::max() })
    {
        preferences.custom_cube_size = invalid;
        expect(!snapshot_export_request(preferences, "invalid", directory, request, error) && request.filename == saved.filename && request.lattice_size == saved.lattice_size, "invalid custom input does not mutate the snapshot");
    }
    expect(choose_lattice_layout(129).first == 0 && make_identity_lattice(129, 1, 1).empty(), "oversized core lattice rejected");
    preferences.custom_cube_size = 65;
    expect(snapshot_export_request(preferences, "saved", directory, request, error), "custom snapshot captured");
    const auto custom_snapshot = request;
    preferences.format = output_format::png;
    preferences.png_size = 32;
    preferences.png_range = range_policy::clamp;
    expect(snapshot_export_request(preferences, "png", directory, request, error) && request.filename == "png.png" && request.range == range_policy::clamp && request.lattice_size == 32, "PNG settings are independent from CUBE");
    const auto png_snapshot = request;
    preferences.png_distribution = png_layout::square;
    expect(!snapshot_export_request(preferences, "square", directory, request, error) && request.filename == png_snapshot.filename && request.png_distribution == png_layout::horizontal, "unsupported square size rejected without modifying snapshot");
    preferences.png_size = 64;
    expect(snapshot_export_request(preferences, "square", directory, request, error) && request.png_distribution == png_layout::square, "square 64 accepted");
    preferences.format = output_format::rise_tex;
    preferences.rise_range = range_policy::reject;
    expect(snapshot_export_request(preferences, "rise", directory, request, error) && request.lattice_size == 32 && request.range == range_policy::reject, "Rise size and policy unchanged");
    preferences.format = output_format::cube;
    expect(snapshot_export_request(preferences, "back", directory, request, error) && request.lattice_size == 65 && request.range == range_policy::reject, "custom CUBE choice preserved across format switches");
    expect(custom_snapshot.lattice_size == 65 && custom_snapshot.filename == "saved.cube" && png_snapshot.range == range_policy::clamp && png_snapshot.png_distribution == png_layout::horizontal, "active snapshots immutable across preference changes");
    expect(validate_output_filename("lut", filename, error, output_format::png) && filename == "lut.png", "PNG extension added");
    expect(!validate_output_filename("lut.PNG", filename, error, output_format::png) && !validate_output_filename("lut.cube.png", filename, error, output_format::png), "wrong and doubled extensions rejected");
    expect(filename_for_format("lut.png", output_format::png, output_format::cube) == "lut.cube" && filename_for_format("lut.tex.28", output_format::rise_tex, output_format::png) == "lut.png", "all format suffixes switch cleanly");
    export_estimate estimate;
    request = custom_snapshot;
    request.lattice_size = 128;
    expect(estimate_export(request, estimate, error) && estimate.samples == 2097152 && estimate.float_buffer_bytes == 33554432 && estimate.file_bytes >= 100663296, "128 memory and disk budget");
    expect(!check_export_capacity(estimate.file_bytes + 16 * 1024 * 1024 - 1, estimate, error), "free-space reserve enforced");
    expect(check_export_capacity(estimate.file_bytes + 16 * 1024 * 1024, estimate, error), "exact budget and reserve accepted");
    expect(check_export_space(request, error), "existing output volume checked without creating files");
    request.directory = directory / "lut_baker_uncreated_parent" / "child";
    expect(check_export_space(request, error) && !std::filesystem::exists(request.directory), "missing directory checks nearest existing volume without creating it");
    request.format = output_format::png;
    request.png_distribution = png_layout::horizontal;
    expect(estimate_export(request, estimate, error) && estimate.image_width == 16384 && estimate.image_height == 128, "PNG 128 strip dimensions");
    request.png_distribution = png_layout::square;
    expect(!estimate_export(request, estimate, error), "square 128 cannot silently add padding");
    std::cout << (failures == 0 ? "Export format tests passed\n" : "Export format tests failed\n");
    return failures == 0 ? 0 : 1;
}
