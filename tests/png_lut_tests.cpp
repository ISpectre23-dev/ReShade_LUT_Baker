#include "png_lut.hpp"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace
{
int failures = 0;
void expect(const bool condition, const char *const label)
{
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
std::uint16_t component(const lut_baker::png_rgb_image &image, const std::size_t index)
{
    if (image.depth == lut_baker::png_bit_depth::eight)
        return image.pixels[index];
    return static_cast<std::uint16_t>(image.pixels[index * 2] | (static_cast<std::uint16_t>(image.pixels[index * 2 + 1]) << 8));
}
}

int main()
{
    using namespace lut_baker;
    std::string error;
    quantization_metrics metrics;
    png_rgb_image image;
    for (const auto depth : { png_bit_depth::eight, png_bit_depth::sixteen })
    for (const std::uint32_t size : { 16u, 32u, 64u, 128u })
    {
        const auto layout = choose_lattice_layout(size);
        const auto samples = make_identity_lattice(size, layout.first, layout.second);
        for (const auto distribution : { png_layout::horizontal, png_layout::square })
        {
            if (distribution == png_layout::square && size != 16 && size != 64)
            {
                expect(!prepare_png_lut(size, samples, distribution, range_policy::reject, image, metrics, error, depth) && image.pixels.empty(), "unsupported square size produces no bytes");
                continue;
            }
            expect(prepare_png_lut(size, samples, distribution, range_policy::reject, image, metrics, error, depth), "supported PNG lattice prepares");
            const std::size_t component_bytes = depth == png_bit_depth::sixteen ? 2u : 1u;
            const double maximum = depth == png_bit_depth::sixteen ? 65535.0 : 255.0;
            const std::uint32_t columns = distribution == png_layout::horizontal ? size : size == 16 ? 4u : 8u;
            expect(image.width == size * columns && image.height == size * (size / columns) &&
                image.depth == depth && image.pixels.size() == samples.size() * 3 * component_bytes, "layout dimensions, depth and payload exact");
            bool correct = true;
            // Independently walk destination rows, invert the layout, and verify every byte.
            for (std::uint32_t y = 0; y < image.height; ++y)
                for (std::uint32_t x = 0; x < image.width; ++x)
                {
                    const std::uint32_t input[] = { x % size, y % size, (y / size) * columns + x / size };
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        const float value = static_cast<float>(input[c]) / static_cast<float>(size - 1);
                        const auto expected = static_cast<std::uint16_t>(std::floor(static_cast<double>(value) * maximum + 0.5));
                        correct &= component(image, (static_cast<std::size_t>(y) * image.width + x) * 3 + c) == expected;
                    }
                }
            expect(correct, "all RGB bytes match the independent axes reference");
            expect(metrics.quantization.maximum_absolute <= 0.5 / maximum + 1e-12 && metrics.clipped_components == 0, "identity quantization bounded separately from GPU error");
        }
    }
    const auto layout = choose_lattice_layout(16);
    auto samples = make_identity_lattice(16, layout.first, layout.second);
    samples[0] = { -1.0f, 2.0f, 0.5f, std::numeric_limits<float>::quiet_NaN() };
    const auto before = samples;
    expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::reject, image, metrics, error) && image.pixels.empty() && metrics.clipped_components == 2 && metrics.clipped_samples == 1, "reject mode reports original range without producing pixels");
    expect(prepare_png_lut(16, samples, png_layout::horizontal, range_policy::clamp, image, metrics, error) && image.pixels[0] == 0 && image.pixels[1] == 255 && image.pixels[2] == 128, "explicit clamp and half-up quantization, alpha ignored");
    expect(std::memcmp(before.data(), samples.data(), samples.size() * sizeof(float4)) == 0, "PNG never modifies the float samples");
    expect(metrics.source_minimum == -1.0 && metrics.source_maximum == 2.0 && metrics.clipped_samples == 1, "clipping statistics retain original values");
    expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::reject, image, metrics, error, png_bit_depth::sixteen) &&
        image.pixels.empty() && metrics.clipped_components == 2, "16-bit PNG still rejects values outside 0-1 by default");
    expect(prepare_png_lut(16, samples, png_layout::horizontal, range_policy::clamp, image, metrics, error, png_bit_depth::sixteen) &&
        component(image, 0) == 0 && component(image, 1) == 65535 && component(image, 2) == 32768, "16-bit clamp uses full-range integer half-up quantization");
    expect(std::memcmp(before.data(), samples.data(), samples.size() * sizeof(float4)) == 0, "16-bit export never modifies float samples");
    for (const float invalid : { std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
    {
        samples[100].r = invalid;
        expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::clamp, image, metrics, error) && image.pixels.empty(), "non-finite RGB always fails");
        expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::clamp, image, metrics, error, png_bit_depth::sixteen) &&
            image.pixels.empty(), "16-bit non-finite RGB always fails");
    }
    samples = make_identity_lattice(16, layout.first, layout.second);
    expect(!prepare_png_lut(16, samples, static_cast<png_layout>(99), range_policy::reject, image, metrics, error), "unknown layout rejected");
    expect(!prepare_png_lut(16, samples, png_layout::horizontal, static_cast<range_policy>(99), image, metrics, error), "unknown range policy rejected");
    expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::reject, image, metrics, error, static_cast<png_bit_depth>(12)), "unknown bit depth rejected");
    samples[0] = { 4660.0f / 65535.0f, 43981.0f / 65535.0f, 1.0f / 65535.0f, 1.0f };
    expect(prepare_png_lut(16, samples, png_layout::horizontal, range_policy::reject, image, metrics, error, png_bit_depth::sixteen) &&
        component(image, 0) == 0x1234 && component(image, 1) == 0xabcd && component(image, 2) == 1,
        "low 16-bit sample bytes survive conversion without an 8-bit intermediate");
    samples.pop_back();
    expect(!prepare_png_lut(16, samples, png_layout::horizontal, range_policy::reject, image, metrics, error), "incomplete lattice rejected");
    std::vector<std::uint8_t> bytes;
    expect(!encode_png({}, "", bytes, error) && bytes.empty(), "empty image rejected before the codec");
    expect(prepare_png_lut(16, before, png_layout::horizontal, range_policy::clamp, image, metrics, error, png_bit_depth::sixteen),
        "prepare a valid 16-bit encoder input");
    image.depth = static_cast<png_bit_depth>(12);
    expect(!encode_png(image, "", bytes, error) && bytes.empty(), "invalid bit depth rejected before the codec");
    image.depth = png_bit_depth::sixteen;
    image.pixels.pop_back();
    expect(!encode_png(image, "", bytes, error) && bytes.empty(), "truncated 16-bit component rejected before the codec");
    std::filesystem::path directory;
    for (unsigned int attempt = 0; attempt < 100; ++attempt)
    {
        const auto candidate = std::filesystem::temp_directory_path() / ("lut_baker_png_tests_" + std::to_string(GetCurrentProcessId()) +
            '_' + std::to_string(GetTickCount64()) + '_' + std::to_string(attempt));
        if (CreateDirectoryW(candidate.c_str(), nullptr))
        {
            directory = candidate;
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    if (directory.empty())
    {
        std::cerr << "Could not create a private test directory\n";
        return 1;
    }
    samples = make_identity_lattice(16, layout.first, layout.second);
    cube_metadata metadata;
    metadata.exporter_version = "test";
    const auto output = directory / "identity.png";
    const bool written = write_png_lut_atomic(output, 16, samples, png_layout::square, range_policy::reject, metadata, metrics, error);
    expect(written, error.c_str());
    const auto original = read_bytes(output);
    expect(original.size() > 8 && original[0] == 137 && original[1] == 'P' && original[2] == 'N' && original[3] == 'G', "real Windows encoder writes PNG");
    expect(original.size() >= 33 && original[24] == 8 && original[25] == 2, "default writer remains true-color 8-bit RGB");
    expect(!write_png_lut_atomic(output, 16, samples, png_layout::horizontal, range_policy::reject, metadata, metrics, error) && read_bytes(output) == original, "existing PNG is never overwritten");
    const auto output16 = directory / "identity16.png";
    expect(write_png_lut_atomic(output16, 16, samples, png_layout::square, range_policy::reject, metadata, metrics, error, png_bit_depth::sixteen), error.c_str());
    const auto original16 = read_bytes(output16);
    expect(original16.size() >= 33 && original16[24] == 16 && original16[25] == 2, "real Windows encoder preserves 16-bit RGB without alpha");
    expect(!write_png_lut_atomic(output16, 16, samples, png_layout::horizontal, range_policy::reject, metadata, metrics, error, png_bit_depth::sixteen) &&
        read_bytes(output16) == original16, "16-bit PNG never overwrites an existing file");
    samples[0].r = -0.1f;
    const auto invalid_output = directory / "invalid.png";
    expect(!write_png_lut_atomic(invalid_output, 16, samples, png_layout::square, range_policy::reject, metadata, metrics, error) && !std::filesystem::exists(invalid_output), "failed range validation creates no file");
    expect(!write_png_lut_atomic(invalid_output, 16, samples, png_layout::square, range_policy::reject, metadata, metrics, error, png_bit_depth::sixteen) &&
        !std::filesystem::exists(invalid_output), "failed 16-bit range validation creates no file");
    expect(!write_png_lut_atomic(output / "child.png", 16, before, png_layout::square, range_policy::clamp, metadata, metrics, error), "invalid parent fails cleanly");
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        expect(entry.path().filename().u8string().find(".tmp.") == std::string::npos, "no temporary remains after failures");
    std::filesystem::remove(output);
    std::filesystem::remove(output16);
    std::filesystem::remove(directory);
    std::cout << (failures == 0 ? "PNG tests passed\n" : "PNG tests failed\n");
    return failures == 0 ? 0 : 1;
}
