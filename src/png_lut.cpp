#include "png_lut.hpp"
#include "atomic_output.hpp"

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace
{
struct com_apartment
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~com_apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool encoding_failure(const HRESULT result, const char *const operation, std::string &error)
{
    if (SUCCEEDED(result))
        return false;
    std::ostringstream message;
    message << "The Windows PNG encoder failed while " << operation << " (Windows error 0x"
            << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(result)
            << "). No PNG was written. Try CUBE or check the Windows imaging components.";
    error = message.str();
    return true;
}

bool remove_color_metadata(std::vector<std::uint8_t> &bytes, std::string &error)
{
    constexpr std::uint8_t signature[] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    const auto invalid = [&error]() {
        error = "The Windows encoder returned an invalid PNG container. No PNG was written. Try CUBE or check the Windows imaging components.";
        return false;
    };
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0)
        return invalid();
    std::vector<std::uint8_t> untagged;
    untagged.reserve(bytes.size());
    untagged.insert(untagged.end(), bytes.begin(), bytes.begin() + 8);
    bool header = false, pixels = false, end = false;
    for (std::size_t offset = 8; offset < bytes.size();)
    {
        if (bytes.size() - offset < 12)
            return invalid();
        const std::uint32_t length = static_cast<std::uint32_t>(bytes[offset]) << 24 |
            static_cast<std::uint32_t>(bytes[offset + 1]) << 16 |
            static_cast<std::uint32_t>(bytes[offset + 2]) << 8 | bytes[offset + 3];
        const std::size_t total = static_cast<std::size_t>(length) + 12;
        if (total > bytes.size() - offset)
            return invalid();
        const std::string_view type(reinterpret_cast<const char *>(bytes.data() + offset + 4), 4);
        if (!header && type != "IHDR")
            return invalid();
        if (type == "IHDR")
        {
            if (header || length != 13)
                return invalid();
            header = true;
        }
        if (type == "IDAT") pixels = true;
        if (type == "IEND")
        {
            if (length != 0 || offset + total != bytes.size())
                return invalid();
            end = true;
        }
        // WIC can add color metadata at commit even when none was requested.
        // Drop those chunks whole. The pixel data, the Software text chunk and
        // the checksums of every chunk that stays are left untouched.
        if (type != "sRGB" && type != "gAMA" && type != "iCCP" && type != "cHRM" &&
            type != "cICP" && type != "mDCV" && type != "cLLI")
            untagged.insert(untagged.end(), bytes.begin() + offset, bytes.begin() + offset + total);
        offset += total;
    }
    if (!header || !pixels || !end)
        return invalid();
    bytes.swap(untagged);
    return true;
}
}

namespace lut_baker
{
const char *png_layout_name(const png_layout layout) noexcept
{
    return layout == png_layout::square ? "Square tiles" : "Horizontal strip";
}

bool prepare_png_lut(const std::uint32_t size, const std::vector<float4> &samples, const png_layout layout,
    const range_policy policy, png_rgb_image &image, quantization_metrics &metrics, std::string &error,
    const png_bit_depth depth)
{
    image = {};
    metrics = {};
    error.clear();
    export_request request;
    request.format = output_format::png;
    request.lattice_size = size;
    request.png_distribution = layout;
    request.png_depth = depth;
    export_estimate estimate;
    if (!estimate_export(request, estimate, error) || samples.size() != estimate.samples)
    {
        if (error.empty())
            error = "PNG requires one complete RGB lattice. No PNG was written. Bake again with a supported size and layout.";
        return false;
    }
    if (policy != range_policy::reject && policy != range_policy::clamp)
    {
        error = "Invalid PNG range policy. No PNG was written. Choose whether to reject or clamp values outside 0-1.";
        return false;
    }
    metrics.source_minimum = std::numeric_limits<double>::infinity();
    metrics.source_maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const float rgb[] = { samples[index].r, samples[index].g, samples[index].b };
        bool affected = false;
        for (const float value : rgb)
        {
            if (!std::isfinite(value))
            {
                error = "The GPU result contains NaN or infinity at sample " + std::to_string(index) + ". No PNG was written. Check the selected effects.";
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
        message << std::setprecision(9) << "PNG cannot store RGB outside 0-1. Source range: ["
                << metrics.source_minimum << ", " << metrics.source_maximum << "]; " << metrics.clipped_components
                << " values in " << metrics.clipped_samples << " samples out of range. No PNG was written. "
                << "Export CUBE to keep them, or enable Clamp to 0-1.";
        error = message.str();
        return false;
    }

    image.width = estimate.image_width;
    image.height = estimate.image_height;
    image.depth = depth;
    const std::size_t component_bytes = depth == png_bit_depth::sixteen ? 2u : 1u;
    const double maximum = depth == png_bit_depth::sixteen ? 65535.0 : 255.0;
    image.pixels.resize(samples.size() * 3 * component_bytes);
    const std::uint32_t columns = layout == png_layout::square ? image.width / size : size;
    double absolute_sum = 0.0;
    double squared_sum = 0.0;
    for (std::uint32_t b = 0; b < size; ++b)
        for (std::uint32_t g = 0; g < size; ++g)
            for (std::uint32_t r = 0; r < size; ++r)
            {
                const auto &sample = samples[(static_cast<std::size_t>(b) * size + g) * size + r];
                const std::uint32_t x = r + (b % columns) * size;
                const std::uint32_t y = g + (b / columns) * size;
                const auto pixel = (static_cast<std::size_t>(y) * image.width + x) * 3 * component_bytes;
                const float rgb[] = { sample.r, sample.g, sample.b };
                for (std::size_t c = 0; c < 3; ++c)
                {
                    const double value = std::clamp(static_cast<double>(rgb[c]), 0.0, 1.0);
                    const auto quantized = static_cast<std::uint16_t>(std::floor(value * maximum + 0.5));
                    image.pixels[pixel + c * component_bytes] = static_cast<std::uint8_t>(quantized & 0xffu);
                    if (component_bytes == 2)
                        image.pixels[pixel + c * component_bytes + 1] = static_cast<std::uint8_t>(quantized >> 8);
                    const double difference = std::abs(static_cast<double>(quantized) / maximum - value);
                    metrics.quantization.maximum_absolute = std::max(metrics.quantization.maximum_absolute, difference);
                    absolute_sum += difference;
                    squared_sum += difference * difference;
                }
            }
    const double components = static_cast<double>(samples.size() * 3);
    metrics.quantization.mean_absolute = absolute_sum / components;
    metrics.quantization.rms = std::sqrt(squared_sum / components);
    return true;
}

bool encode_png(const png_rgb_image &image, const std::string &software, std::vector<std::uint8_t> &bytes, std::string &error)
{
    bytes.clear();
    error.clear();
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    const std::uint32_t component_bytes = image.depth == png_bit_depth::sixteen ? 2u : 1u;
    if (image.width == 0 || image.height == 0 || pixels > static_cast<std::uint64_t>(maximum_lut_size) * maximum_lut_size * maximum_lut_size ||
        !valid_png_bit_depth(image.depth) || image.pixels.size() != pixels * 3 * component_bytes || software.size() > 4096)
    {
        error = "Invalid or oversized PNG image. No PNG was written. Choose a supported size and layout.";
        return false;
    }
    com_apartment apartment;
    // Reuse the thread's existing COM apartment if its mode differs. The guard
    // must call CoUninitialize only when our CoInitializeEx call succeeded.
    if (apartment.result != RPC_E_CHANGED_MODE && encoding_failure(apartment.result, "initializing the encoder", error))
        return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (encoding_failure(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())), "creating the imaging factory", error) ||
        encoding_failure(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()), "creating the image stream", error) ||
        encoding_failure(factory->CreateEncoder(GUID_ContainerFormatPng, &GUID_VendorMicrosoft, encoder.GetAddressOf()), "creating the PNG codec", error) ||
        encoding_failure(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "opening the image stream", error) ||
        encoding_failure(encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf()), "creating the PNG frame", error))
        return false;
    PROPBAG2 option {};
    wchar_t option_name[] = L"InterlaceOption";
    option.pstrName = option_name;
    VARIANT value {};
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_FALSE;
    if (encoding_failure(options->Write(1, &option, &value), "setting non-interlaced output", error) ||
        encoding_failure(frame->Initialize(options.Get()), "initializing the PNG frame", error) ||
        encoding_failure(frame->SetSize(image.width, image.height), "setting image dimensions", error))
        return false;
    // Request the exact integer precision. Never accept a downgraded format.
    // 8-bit input is BGR; 16-bit input is native little-endian RGB. The codec
    // writes PNG's big-endian 16-bit samples without a pixel converter.
    const WICPixelFormatGUID requested_format = component_bytes == 2 ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID pixel_format = requested_format;
    if (encoding_failure(frame->SetPixelFormat(&pixel_format), "setting RGB bit depth", error) ||
        encoding_failure(IsEqualGUID(pixel_format, requested_format) ? S_OK : E_FAIL, "verifying the RGB bit depth", error))
        return false;
    // Skip the pixel converter and SetColorContexts so the samples are written
    // as they are. WIC still adds default color tags; those are removed after
    // commit.
    if (!software.empty())
    {
        ComPtr<IWICMetadataQueryWriter> metadata;
        PROPVARIANT text {};
        text.vt = VT_LPSTR;
        text.pszVal = const_cast<char *>(software.c_str()); // Borrowed only for this call.
        if (encoding_failure(frame->GetMetadataQueryWriter(metadata.GetAddressOf()), "opening PNG metadata", error) ||
            encoding_failure(metadata->SetMetadataByName(L"/tEXt/{str=Software}", &text), "writing exporter metadata", error))
            return false;
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(image.width) * 3 * component_bytes);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        if (component_bytes == 2)
            std::memcpy(row.data(), image.pixels.data() + static_cast<std::size_t>(y) * row.size(), row.size());
        else for (std::size_t x = 0; x < row.size(); x += 3)
        {
            const auto source = static_cast<std::size_t>(y) * row.size() + x;
            row[x] = image.pixels[source + 2];
            row[x + 1] = image.pixels[source + 1];
            row[x + 2] = image.pixels[source];
        }
        if (encoding_failure(frame->WritePixels(1, static_cast<UINT>(row.size()), static_cast<UINT>(row.size()), row.data()), "encoding RGB samples", error))
            return false;
    }
    if (encoding_failure(frame->Commit(), "committing the PNG frame", error) ||
        encoding_failure(encoder->Commit(), "committing the PNG file", error))
        return false;
    STATSTG stat {};
    if (encoding_failure(stream->Stat(&stat, STATFLAG_NONAME), "reading the encoded size", error))
        return false;
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > pixels * 4 * component_bytes + 1024 * 1024)
    {
        error = "The encoded PNG exceeded its file budget. No PNG was written. Try a smaller size or CUBE.";
        return false;
    }
    LARGE_INTEGER start {};
    if (encoding_failure(stream->Seek(start, STREAM_SEEK_SET, nullptr), "reading the encoded stream", error))
        return false;
    bytes.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    if (encoding_failure(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "reading the PNG bytes", error) || read != bytes.size())
    {
        bytes.clear();
        if (error.empty())
            error = "The PNG encoder returned incomplete data. No PNG was written. Try CUBE or restart the game.";
        return false;
    }
    if (!remove_color_metadata(bytes, error))
        return false;
    // Verify the encoded IHDR, not just the format accepted by SetPixelFormat.
    if (bytes.size() < 33 || bytes[24] != static_cast<std::uint8_t>(image.depth) || bytes[25] != 2)
    {
        bytes.clear();
        error = "The PNG encoder did not preserve the requested RGB bit depth. No PNG was written. Try CUBE or check the Windows imaging components.";
        return false;
    }
    return true;
}

bool write_png_lut_atomic(const std::filesystem::path &destination, const std::uint32_t size,
    const std::vector<float4> &samples, const png_layout layout, const range_policy policy,
    const cube_metadata &metadata, quantization_metrics &metrics, std::string &error,
    const png_bit_depth depth)
{
    png_rgb_image image;
    std::vector<std::uint8_t> bytes;
    if (!prepare_png_lut(size, samples, layout, policy, image, metrics, error, depth) ||
        !encode_png(image, "ReShade LUT Baker " + metadata.exporter_version, bytes, error))
        return false;
    return write_file_atomic(destination, false, [&bytes](std::ostream &output, std::string &) {
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return true;
    }, error);
}
}
