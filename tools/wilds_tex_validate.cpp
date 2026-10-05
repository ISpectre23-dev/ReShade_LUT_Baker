#include "atomic_output.hpp"
#include "wilds_tex.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv)
{
    if (argc != 2 && (argc != 4 || std::string(argv[2]) != "--payload"))
    {
        std::cerr << "Usage: wilds_tex_validate <file.tex.241106027> [--payload <new-output.bin>]\n";
        return 2;
    }
    try
    {
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary | std::ios::ate);
        const auto length = input.tellg();
        if (!input || length < 0 || length > static_cast<std::streamoff>(lut_baker::wilds_file_budget))
            throw std::runtime_error("Cannot read the TEX file or it exceeds the supported profile's size limit.");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("The TEX file could not be read completely.");
        std::vector<std::uint8_t> payload;
        std::string error;
        if (!lut_baker::decode_wilds_tex(bytes, payload, error))
            throw std::runtime_error(error);
        std::vector<lut_baker::float4> samples;
        samples.reserve(lut_baker::wilds_sample_count);
        for (std::size_t index = 0; index < lut_baker::wilds_sample_count; ++index)
        {
            const std::size_t offset = (index / 33) * lut_baker::wilds_row_pitch + (index % 33) * 8;
            float rgba[4] {};
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const auto *value = payload.data() + offset + channel * 2;
                const auto half = static_cast<std::uint16_t>(value[0] | (static_cast<std::uint16_t>(value[1]) << 8));
                rgba[channel] = lut_baker::half_to_float(half);
                if (!std::isfinite(rgba[channel]))
                    throw std::runtime_error("The LUT contains a non-finite channel value.");
            }
            if (rgba[3] != 1.0f)
                throw std::runtime_error("The LUT does not have the export profile's opaque alpha.");
            samples.push_back({ rgba[0], rgba[1], rgba[2], rgba[3] });
        }
        if (argc == 4 && !lut_baker::write_file_atomic(std::filesystem::u8path(argv[3]), false,
            [&payload](std::ostream &stream, std::string &) {
                stream.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
                return true;
            }, error))
            throw std::runtime_error(error);
        const auto identity = lut_baker::measure_identity_error(samples, 33);
        std::cout << "Wilds TEX v241106027: 33x33x33, RGBA16F, GDeflate, " << bytes.size()
                  << " file bytes, " << payload.size() << " decoded bytes.\n"
                  << std::setprecision(12) << "Numeric identity max/mean/RMS: " << identity.maximum_absolute << " / "
                  << identity.mean_absolute << " / " << identity.rms << "\n"
                  << "File validation only. In-game loading and color-domain equivalence are not established by this test.\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
