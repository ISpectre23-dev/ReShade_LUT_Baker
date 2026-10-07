#include "rise_tex.hpp"
#include "atomic_output.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace lut_baker
{
bool serialize_rise_tex(const std::uint32_t size, const std::vector<float4> &samples, const range_policy policy,
    std::vector<std::uint8_t> &bytes, rise_export_metrics &metrics, std::string &error)
{
    bytes.clear();
    metrics = {};
    if (size != rise_lut_size || samples.size() != rise_sample_count)
    {
        error = "Monster Hunter Rise requires exactly 32 x 32 x 32 RGB samples (32768). No TEX was written.";
        return false;
    }
    if (policy != range_policy::reject && policy != range_policy::clamp)
    {
        error = "Invalid Rise range policy. No TEX was written.";
        return false;
    }

    metrics.source_minimum = std::numeric_limits<double>::infinity();
    metrics.source_maximum = -std::numeric_limits<double>::infinity();
    // Validate the complete buffer before producing any bytes. Alpha is not
    // baked into this profile: it is always 255, irrespective of shader alpha.
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const float rgb[] = { samples[index].r, samples[index].g, samples[index].b };
        bool affected = false;
        for (const float value : rgb)
        {
            if (!std::isfinite(value))
            {
                error = "The GPU result contains NaN or infinity at sample " + std::to_string(index) + ". No TEX was written.";
                return false;
            }
            metrics.source_minimum = std::min(metrics.source_minimum, static_cast<double>(value));
            metrics.source_maximum = std::max(metrics.source_maximum, static_cast<double>(value));
            if (value < 0.0f || value > 1.0f)
            {
                ++metrics.clipped_components;
                affected = true;
            }
        }
        if (affected)
            ++metrics.clipped_samples;
    }
    if (metrics.clipped_components != 0 && policy == range_policy::reject)
    {
        std::ostringstream message;
        message << std::setprecision(9) << "Rise TEX cannot store RGB outside 0-1. Source range: ["
                << metrics.source_minimum << ", " << metrics.source_maximum << "]; " << metrics.clipped_components
                << " value(s) in " << metrics.clipped_samples << " sample(s) out of range. No file was written. "
                << "Export as CUBE to keep them, or enable Clamp to 0-1.";
        error = message.str();
        return false;
    }

    bytes.reserve(rise_file_size);
    bytes.insert(bytes.end(), rise_tex_header.begin(), rise_tex_header.end());
    double absolute_sum = 0.0;
    double squared_sum = 0.0;
    for (const float4 &sample : samples)
    {
        const float rgb[] = { sample.r, sample.g, sample.b };
        for (const float component : rgb)
        {
            const double value = std::clamp(static_cast<double>(component), 0.0, 1.0);
            // No gamma or color-space conversion. Double arithmetic makes the
            // half-up rule deterministic for the actual binary32 input.
            const auto quantized = static_cast<std::uint8_t>(std::floor(value * 255.0 + 0.5));
            bytes.push_back(quantized);
            const double difference = std::abs(static_cast<double>(quantized) / 255.0 - value);
            metrics.quantization.maximum_absolute = std::max(metrics.quantization.maximum_absolute, difference);
            absolute_sum += difference;
            squared_sum += difference * difference;
        }
        bytes.push_back(255);
    }
    metrics.quantization.mean_absolute = absolute_sum / static_cast<double>(rise_sample_count * 3);
    metrics.quantization.rms = std::sqrt(squared_sum / static_cast<double>(rise_sample_count * 3));
    return true;
}

bool write_rise_tex_atomic(const std::filesystem::path &destination, const std::uint32_t size,
    const std::vector<float4> &samples, const range_policy policy, rise_export_metrics &metrics, std::string &error)
{
    std::vector<std::uint8_t> bytes;
    if (!serialize_rise_tex(size, samples, policy, bytes, metrics, error))
        return false;
    return write_file_atomic(destination, false, [&bytes](std::ostream &stream, std::string &) {
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return true;
    }, error);
}
}
