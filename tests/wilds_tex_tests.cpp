#include "wilds_tex.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace
{
int failures = 0;
void expect(const bool condition, const char *message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
}

int main()
{
    using namespace lut_baker;
    const auto layout = choose_lattice_layout(33);
    auto identity = make_identity_lattice(33, layout.first, layout.second);
    expect(identity.size() == 35937 && measure_identity_error(identity, 33).maximum_absolute == 0, "33-cubed identity exactly representable in FP32");
    std::vector<std::uint8_t> payload, bytes, decoded;
    quantization_metrics metrics;
    std::string error;
    expect(prepare_wilds_payload(33, identity, payload, metrics, error), "prepare identity payload");
    expect(payload.size() == 557568 && metrics.quantization.maximum_absolute == 0 && metrics.quantization.mean_absolute == 0 && metrics.quantization.rms == 0, "identity exactly representable in FP16, all three error metrics zero");
    bool order = payload.size() == 557568;
    for (std::uint32_t b = 0; order && b < 33; ++b)
        for (std::uint32_t g = 0; g < 33; ++g)
            for (std::uint32_t r = 0; r < 33; ++r)
            {
                const std::size_t offset = (b * 33 + g) * 512 + r * 8;
                const float expected[] = { r / 32.0f, g / 32.0f, b / 32.0f, 1.0f };
                for (std::size_t c = 0; c < 4; ++c)
                {
                    const auto value = static_cast<std::uint16_t>(payload[offset + c * 2] | (static_cast<std::uint16_t>(payload[offset + c * 2 + 1]) << 8));
                    order &= half_to_float(value) == expected[c];
                }
            }
    expect(order, "every sample is X/red-fastest, then Y/green, then Z/blue with opaque alpha");
    bool zero_padding = true;
    for (std::size_t row = 0; row < 33 * 33; ++row)
        zero_padding &= std::all_of(payload.begin() + row * 512 + 264, payload.begin() + (row + 1) * 512, [](std::uint8_t v) { return v == 0; });
    expect(zero_padding, "all 248 row-padding bytes are zero");
    expect(serialize_wilds_tex(33, identity, bytes, metrics, error), "serialize and verify compressed identity");
    const auto identity_bytes = bytes;
    const std::vector<std::uint8_t> expected_header {
        84,69,88,0,107,252,94,14,33,0,33,0,33,0,1,16,10,0,0,0,255,255,255,255,0,0,0,0,0,7,0,0,
        0,0,0,0,0,0,0,0,56,0,0,0,0,0,0,0,0,2,0,0,0,66,0,0
    };
    expect(bytes.size() > 108 && bytes.size() <= 623168 && std::equal(expected_header.begin(), expected_header.end(), bytes.begin()), "independent 56-byte stock profile, bounded compressed size");
    expect(decode_wilds_tex(bytes, decoded, error) && decoded == payload, "lossless GDeflate round trip includes padding");
    expect(serialize_wilds_tex(33, identity, bytes, metrics, error) && bytes == identity_bytes, "compression is deterministic");
    for (std::size_t length : { 0u, 4u, 55u, 56u, 63u, 64u, 71u, 107u })
        expect(!decode_wilds_tex(std::vector<std::uint8_t>(identity_bytes.begin(), identity_bytes.begin() + length), decoded, error) && decoded.empty(), "truncated headers rejected before codec access");
    for (std::size_t offset : { 0u, 4u, 8u, 10u, 12u, 14u, 15u, 16u, 20u, 28u, 29u, 32u, 40u, 48u, 52u, 56u, 60u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u })
    {
        auto invalid = identity_bytes;
        invalid[offset] ^= 0x80;
        expect(!decode_wilds_tex(invalid, decoded, error) && decoded.empty(), "profile/stream metadata corruption rejected");
    }
    for (std::size_t tile = 0; tile < 9; ++tile)
    {
        auto invalid = identity_bytes;
        std::fill_n(invalid.begin() + 72 + tile * 4, 4, static_cast<std::uint8_t>(0xff));
        expect(!decode_wilds_tex(invalid, decoded, error), "out-of-file tile offsets/size rejected");
    }
    auto invalid_bytes = identity_bytes;
    invalid_bytes.push_back(0);
    expect(!decode_wilds_tex(invalid_bytes, decoded, error), "trailing bytes rejected");

    auto grade = identity;
    grade[0] = { -0.25f, 1.25f, 0.33333334f, std::numeric_limits<float>::quiet_NaN() };
    grade[1] = { 65504.0f, -65504.0f, -0.0f, 0.0f };
    grade[2] = { 0x1p-24f, 0x1p-25f, 1.00048828125f, 0.0f };
    const auto before = grade;
    expect(serialize_wilds_tex(33, grade, bytes, metrics, error) && decode_wilds_tex(bytes, decoded, error), "negative/super-white/endpoints/subnormals accepted without clipping");
    expect(metrics.source_minimum == -65504 && metrics.source_maximum == 65504 && metrics.clipped_components == 0, "source range includes original finite float values");
    expect(decoded[0] == 0 && decoded[1] == 0xb4 && decoded[2] == 0 && decoded[3] == 0x3d && decoded[6] == 0 && decoded[7] == 0x3c, "negative and super-white float values retained, alpha forced to one");
    expect(decoded[16] == 1 && decoded[17] == 0 && decoded[18] == 0 && decoded[19] == 0 && decoded[20] == 0 && decoded[21] == 0x3c, "minimum subnormal and ties round to even");
    expect(std::memcmp(before.data(), grade.data(), grade.size() * sizeof(float4)) == 0, "serializer does not mutate float samples");
    std::uint32_t random = 1729;
    for (auto &sample : grade)
    {
        float channels[3] {};
        for (auto &channel : channels)
        {
            random = random * 1664525u + 1013904223u;
            channel = half_to_float(static_cast<std::uint16_t>(random & 0xfbffu));
        }
        sample = { channels[0], channels[1], channels[2], 1.0f };
    }
    expect(serialize_wilds_tex(33, grade, bytes, metrics, error) && bytes.size() <= wilds_file_budget &&
        decode_wilds_tex(bytes, decoded, error), "low-compressibility finite samples stay inside the file budget and round trip");
    expect(metrics.quantization.maximum_absolute == 0, "exact half-precision input stays exact under noisy compression");
    for (const float value : { 65505.0f, -65505.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
    {
        grade = identity;
        grade[5].r = value;
        expect(!serialize_wilds_tex(33, grade, bytes, metrics, error) && bytes.empty() && error.find("sample 5") != std::string::npos, "invalid float input rejected with sample and no bytes");
    }
    expect(!serialize_wilds_tex(32, identity, bytes, metrics, error) && bytes.empty(), "non-33 size rejected");
    identity.pop_back();
    expect(!serialize_wilds_tex(33, identity, bytes, metrics, error) && bytes.empty(), "incomplete sample buffer rejected");
    identity.push_back({ 1, 1, 1, 1 });
    identity.push_back({});
    expect(!serialize_wilds_tex(33, identity, bytes, metrics, error) && bytes.empty(), "extra samples rejected");
    identity.pop_back();

    const auto directory = std::filesystem::temp_directory_path() / ("reshade_lut_wilds_tests_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code filesystem_error;
    const bool owned = std::filesystem::create_directory(directory, filesystem_error);
    expect(owned, "create owned test directory");
    if (owned)
    {
        const auto path = directory / "identity.tex.241106027";
        expect(write_wilds_tex_atomic(path, 33, identity, metrics, error), "atomic Wilds export");
        const auto original = read_file(path);
        expect(original == identity_bytes, "actual file equals verified serialized bytes");
        expect(make_unique_output_path(directory, "identity.tex.241106027", output_format::wilds_tex).filename() ==
            "identity_001.tex.241106027", "collision suffix stays before the entire Wilds format extension");
        expect(!write_wilds_tex_atomic(path, 33, before, metrics, error) && read_file(path) == original, "existing file is never overwritten");
        expect(!write_wilds_tex_atomic(directory / "invalid.tex.241106027", 32, identity, metrics, error) && !std::filesystem::exists(directory / "invalid.tex.241106027"), "invalid request leaves no output file");
        bool temporary_left = false;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            temporary_left |= entry.path().filename().string().find(".tmp.") != std::string::npos;
        expect(!temporary_left, "no owned temporary remains after failures");
        std::filesystem::remove_all(directory, filesystem_error); // Only this successfully reserved test directory.
    }
    if (failures == 0)
        std::cout << "Wilds TEX tests passed. Numeric 33-cubed identity max/mean/RMS: 0 / 0 / 0.\n";
    return failures == 0 ? 0 : 1;
}
