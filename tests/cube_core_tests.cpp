#include "cube_lut.hpp"
#include "technique_catalog.hpp"

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
    using lut_baker::technique_key;
    using lut_baker::technique_selection;

    struct catalog_entry
    {
        technique_key key;
        bool enabled = false;
    };

    const technique_key retained { "Current.fx", "Retained", 0, 1 };
    const technique_key disappeared { "Previous.fx", "Gone", 0, 1 };
    const technique_key duplicate_second { "Duplicate.fx", "SameName", 1, 2 };
    const technique_key new_technique { "Current.fx", "New", 0, 1 };
    const technique_selection previous_selection { retained, disappeared, duplicate_second };
    const std::vector<technique_key> refreshed_catalog { duplicate_second, new_technique, retained };

    const auto reconciled = lut_baker::reconcile_catalog_selection(refreshed_catalog, previous_selection, false);
    expect(reconciled.accepted, "non-empty refreshed catalog is authoritative");
    expect(reconciled.selected.size() == 2, "catalog reconciliation removes only disappeared selections");
    expect(reconciled.selected.find(retained) != reconciled.selected.end(), "catalog reconciliation preserves an existing selection");
    expect(reconciled.selected.find(duplicate_second) != reconciled.selected.end(), "stable duplicate ordinal remains selected");
    expect(reconciled.selected.find(disappeared) == reconciled.selected.end(), "disappeared selection is removed");
    expect(reconciled.selected.find(new_technique) == reconciled.selected.end(), "new catalog entries are not auto-selected");

    const technique_key duplicate_after_cardinality_change { "Duplicate.fx", "SameName", 0, 1 };
    const technique_selection ambiguous_previous { technique_key { "Duplicate.fx", "SameName", 0, 2 } };
    expect(!lut_baker::selection_contains_exact(ambiguous_previous, duplicate_after_cardinality_change),
        "active request lookup rejects duplicate cardinality changes");
    expect(lut_baker::selection_contains_exact(previous_selection, duplicate_second),
        "active request lookup accepts a stable duplicate identity");
    const auto ambiguous = lut_baker::reconcile_catalog_selection({ duplicate_after_cardinality_change }, ambiguous_previous, false);
    expect(ambiguous.accepted && ambiguous.selected.empty(), "duplicate cardinality changes are not ambiguously reselected");

    const auto transient_empty = lut_baker::reconcile_catalog_selection({}, previous_selection, false);
    expect(!transient_empty.accepted, "empty enumeration with a previous selection is treated as transient");
    const auto pending_empty = lut_baker::reconcile_catalog_selection({}, {}, true);
    expect(!pending_empty.accepted, "empty enumeration cannot replace a pending bake catalog");
    const auto authoritative_empty = lut_baker::reconcile_catalog_selection({}, {}, false);
    expect(authoritative_empty.accepted && authoritative_empty.selected.empty(), "empty idle catalog without selection is accepted");

    technique_selection requested_snapshot = previous_selection;
    technique_selection ui_selection = reconciled.selected;
    expect(requested_snapshot.find(disappeared) != requested_snapshot.end(), "catalog reconciliation does not mutate an active requested snapshot");
    expect(ui_selection.find(disappeared) == ui_selection.end(), "future UI selection is reconciled independently of requested snapshot");

    const std::vector<catalog_entry> enabled_catalog {
        { retained, true },
        { new_technique, false }
    };
    technique_selection enabled_selection { disappeared, new_technique };
    enabled_selection = lut_baker::select_currently_enabled(enabled_catalog);
    expect(enabled_selection.size() == 1 && enabled_selection.find(retained) != enabled_selection.end(),
        "Select currently enabled replaces the selection with exactly enabled techniques");
    expect(enabled_selection.find(disappeared) == enabled_selection.end() && enabled_selection.find(new_technique) == enabled_selection.end(),
        "Select currently enabled does not accumulate stale or disabled selections");

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
