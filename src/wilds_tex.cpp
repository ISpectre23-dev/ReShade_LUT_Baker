#include "wilds_tex.hpp"
#include "atomic_output.hpp"

#include <GDeflate.h>
#include <libdeflate.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace
{
std::uint32_t read_u32(const std::uint8_t *bytes) noexcept
{
    return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void append_u32(std::vector<std::uint8_t> &bytes, const std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool compress_payload(const std::vector<std::uint8_t> &payload, std::vector<std::uint8_t> &compressed, std::string &error)
{
    constexpr std::size_t tile_size = 65536;
    constexpr std::size_t tiles = (lut_baker::wilds_payload_size + tile_size - 1) / tile_size;
    constexpr std::size_t data_start = 8 + tiles * 4;
    // Use the reference stream framing and page codec directly so allocation
    // failures are checked, rather than relying on the sample wrapper's
    // unchecked compressor allocation. The existing export worker owns this.
    std::unique_ptr<libdeflate_gdeflate_compressor, decltype(&libdeflate_free_gdeflate_compressor)> compressor(
        libdeflate_alloc_gdeflate_compressor(6), &libdeflate_free_gdeflate_compressor);
    if (!compressor)
    {
        error = "Could not allocate the Wilds compressor. No file was written. Free memory and try again.";
        return false;
    }
    std::size_t pages = 0;
    const auto scratch_size = libdeflate_gdeflate_compress_bound(compressor.get(), tile_size, &pages);
    const auto bound = GDeflate::CompressBound(payload.size());
    if (pages != 1 || bound > lut_baker::wilds_file_budget - 64 || scratch_size == 0 || scratch_size > bound)
    {
        error = "Internal error: the Wilds compressor is misconfigured in this build. No file was written. Reinstall the official LUT Baker release.";
        return false;
    }
    std::vector<std::uint8_t> scratch(scratch_size);
    compressed.reserve(bound);
    compressed.assign(data_start, 0);
    compressed[0] = 4;
    compressed[1] = 0xfb;
    compressed[2] = static_cast<std::uint8_t>(tiles);
    const std::uint32_t flags = 1u | (static_cast<std::uint32_t>(payload.size() % tile_size) << 2);
    for (unsigned int shift = 0; shift < 32; shift += 8)
        compressed[4 + shift / 8] = static_cast<std::uint8_t>(flags >> shift);
    std::array<std::uint32_t, tiles> offsets {};
    std::uint32_t last_size = 0;
    for (std::size_t tile = 0; tile < tiles; ++tile)
    {
        offsets[tile] = static_cast<std::uint32_t>(compressed.size() - data_start);
        libdeflate_gdeflate_out_page page { scratch.data(), scratch.size() };
        const auto input_size = std::min(tile_size, payload.size() - tile * tile_size);
        const auto written = libdeflate_gdeflate_compress(compressor.get(), payload.data() + tile * tile_size, input_size, &page, 1);
        if (written == 0 || written != page.nbytes || written > scratch.size() || written > bound - compressed.size())
        {
            error = "Wilds GDeflate compression failed. No file was written. Free memory and try again.";
            return false;
        }
        compressed.insert(compressed.end(), scratch.begin(), scratch.begin() + written);
        last_size = static_cast<std::uint32_t>(written);
    }
    // Table entry zero stores the LAST tile's size, not the first tile offset.
    offsets[0] = last_size;
    for (std::size_t tile = 0; tile < tiles; ++tile)
        for (unsigned int shift = 0; shift < 32; shift += 8)
            compressed[8 + tile * 4 + shift / 8] = static_cast<std::uint8_t>(offsets[tile] >> shift);
    return true;
}
}

namespace lut_baker
{
bool prepare_wilds_payload(const std::uint32_t size, const std::vector<float4> &samples,
    std::vector<std::uint8_t> &payload, quantization_metrics &metrics, std::string &error)
{
    payload.clear();
    error.clear();
    metrics = {};
    if (size != wilds_lut_size || samples.size() != wilds_sample_count)
    {
        error = "Wilds TEX requires exactly 33x33x33 RGB samples. No file was written. Choose the Wilds export format and bake again.";
        return false;
    }
    metrics.source_minimum = std::numeric_limits<double>::infinity();
    metrics.source_maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        for (const float value : { samples[index].r, samples[index].g, samples[index].b })
        {
            if (!std::isfinite(value) || std::abs(value) > 65504.0f)
            {
                error = "Wilds TEX cannot store RGB sample " + std::to_string(index) +
                    ": the value is non-finite or outside the 16-bit float range [-65504, 65504]. No file was written. Adjust the grading or use CUBE for finite values outside that range.";
                return false;
            }
            metrics.source_minimum = std::min(metrics.source_minimum, static_cast<double>(value));
            metrics.source_maximum = std::max(metrics.source_maximum, static_cast<double>(value));
        }
    }

    payload.assign(wilds_payload_size, 0); // Deterministic row padding.
    double absolute_sum = 0.0, squared_sum = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const std::size_t offset = (index / 33) * wilds_row_pitch + (index % 33) * 8;
        const float rgba[] = { samples[index].r, samples[index].g, samples[index].b, 1.0f };
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            const auto half = float_to_half(rgba[channel]);
            payload[offset + channel * 2] = static_cast<std::uint8_t>(half);
            payload[offset + channel * 2 + 1] = static_cast<std::uint8_t>(half >> 8);
            if (channel < 3)
            {
                const double difference = std::abs(static_cast<double>(half_to_float(half)) - rgba[channel]);
                metrics.quantization.maximum_absolute = std::max(metrics.quantization.maximum_absolute, difference);
                absolute_sum += difference;
                squared_sum += difference * difference;
            }
        }
    }
    metrics.quantization.mean_absolute = absolute_sum / (wilds_sample_count * 3.0);
    metrics.quantization.rms = std::sqrt(squared_sum / (wilds_sample_count * 3.0));
    return true;
}

bool decode_wilds_tex(const std::vector<std::uint8_t> &bytes, std::vector<std::uint8_t> &payload, std::string &error)
{
    payload.clear();
    error.clear();
    constexpr std::size_t tile_size = 65536;
    constexpr std::size_t tile_count = (wilds_payload_size + tile_size - 1) / tile_size;
    constexpr std::size_t table_end = 64 + 8 + tile_count * 4;
    if (bytes.size() <= table_end || bytes.size() > wilds_file_budget ||
        !std::equal(wilds_tex_header.begin(), wilds_tex_header.end(), bytes.begin()) ||
        read_u32(bytes.data() + 56) != bytes.size() - 64 || read_u32(bytes.data() + 60) != 0)
    {
        error = "The file does not match the supported Wilds 33x33x33 RGBA16F TEX profile. Check the file format and export it again.";
        return false;
    }
    const auto *stream = bytes.data() + 64;
    // TileStream: codec 4/complement, 9 tiles, 64 KiB tile size, exact tail size,
    // and zero reserved bits. Its declared size must match the 3D row padding.
    const std::uint32_t stream_flags = 1u | (static_cast<std::uint32_t>(wilds_payload_size % tile_size) << 2);
    if (stream[0] != 4 || stream[1] != 0xfb || stream[2] != tile_count || stream[3] != 0 || read_u32(stream + 4) != stream_flags)
    {
        error = "The Wilds GDeflate header has an unexpected codec or decompressed size. Export the LUT again.";
        return false;
    }
    const auto *table = stream + 8;
    const std::size_t data_size = bytes.size() - table_end;
    std::array<std::size_t, tile_count + 1> offsets {};
    offsets[tile_count] = data_size;
    for (std::size_t tile = 1; tile < tile_count; ++tile)
        offsets[tile] = read_u32(table + tile * 4);
    for (std::size_t tile = 0; tile < tile_count; ++tile)
    {
        if (offsets[tile] >= offsets[tile + 1] || offsets[tile + 1] > data_size || (offsets[tile + 1] % 4) != 0)
        {
            error = "A Wilds GDeflate tile extends outside the file or has an invalid offset. Export the LUT again.";
            return false;
        }
    }
    if (read_u32(table) != data_size - offsets[tile_count - 1])
    {
        error = "The final Wilds GDeflate tile has an invalid size. Export the LUT again.";
        return false;
    }
    std::unique_ptr<libdeflate_gdeflate_decompressor, decltype(&libdeflate_free_gdeflate_decompressor)> decoder(
        libdeflate_alloc_gdeflate_decompressor(), &libdeflate_free_gdeflate_decompressor);
    if (!decoder)
    {
        error = "Could not allocate the Wilds decompression verifier. No file was written. Free memory and try again.";
        return false;
    }
    std::vector<std::uint8_t> decoded(wilds_payload_size);
    for (std::size_t tile = 0; tile < tile_count; ++tile)
    {
        libdeflate_gdeflate_in_page page { bytes.data() + table_end + offsets[tile], offsets[tile + 1] - offsets[tile] };
        const std::size_t expected = std::min(tile_size, wilds_payload_size - tile * tile_size);
        std::size_t written = 0;
        if (libdeflate_gdeflate_decompress(decoder.get(), &page, 1, decoded.data() + tile * tile_size, expected, &written) != LIBDEFLATE_SUCCESS || written != expected)
        {
            error = "A Wilds GDeflate tile could not be decoded to its expected size. Export the LUT again.";
            return false;
        }
    }
    payload = std::move(decoded);
    return true;
}

bool serialize_wilds_tex(const std::uint32_t size, const std::vector<float4> &samples,
    std::vector<std::uint8_t> &bytes, quantization_metrics &metrics, std::string &error)
{
    bytes.clear();
    std::vector<std::uint8_t> payload;
    if (!prepare_wilds_payload(size, samples, payload, metrics, error))
        return false;
    std::vector<std::uint8_t> compressed;
    if (!compress_payload(payload, compressed, error))
        return false;
    std::vector<std::uint8_t> candidate(wilds_tex_header.begin(), wilds_tex_header.end());
    append_u32(candidate, static_cast<std::uint32_t>(compressed.size()));
    append_u32(candidate, 0);
    candidate.insert(candidate.end(), compressed.begin(), compressed.end());
    std::vector<std::uint8_t> roundtrip;
    if (!decode_wilds_tex(candidate, roundtrip, error) || roundtrip != payload)
    {
        error = "Wilds file verification failed after compression. No file was written. " + error;
        return false;
    }
    bytes = std::move(candidate);
    return true;
}

bool write_wilds_tex_atomic(const std::filesystem::path &destination, const std::uint32_t size,
    const std::vector<float4> &samples, quantization_metrics &metrics, std::string &error)
{
    std::vector<std::uint8_t> bytes;
    if (!serialize_wilds_tex(size, samples, bytes, metrics, error))
        return false;
    return write_file_atomic(destination, false, [&bytes](std::ostream &stream, std::string &) {
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return true;
    }, error);
}
}
