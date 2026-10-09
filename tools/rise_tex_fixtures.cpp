// Generate synthetic LUT samples for independent checks of the production writers.
#include "rise_tex.hpp"
#include "png_lut.hpp"
#include "version.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

int main(const int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: rise_tex_fixtures <new-or-empty-output-directory>\n";
        return 2;
    }
    try
    {
        const auto directory = std::filesystem::u8path(argv[1]);
        lut_baker::cube_metadata metadata;
        metadata.exporter_version = LUT_BAKER_VERSION_STRING;
        metadata.reshade_api = "Offline writer fixture (no GPU)";
        metadata.warnings = { "Synthetic writer fixture, not a grading preset." };
        std::string error;
        for (const std::uint32_t size : { 2u, 16u, 32u, 33u, 64u, 65u, 128u })
        {
            const auto layout = lut_baker::choose_lattice_layout(size);
            const auto samples = lut_baker::make_identity_lattice(size, layout.first, layout.second);
            metadata.title = "Identity " + std::to_string(size);
            if (!lut_baker::write_cube_atomic(directory / ("Identity" + std::to_string(size) + ".cube"), size, samples, metadata, false, error))
            {
                std::cerr << error << '\n';
                return 1;
            }
            if (size == 32)
            {
                lut_baker::rise_export_metrics metrics;
                if (!lut_baker::write_rise_tex_atomic(directory / "Identity32.tex.28", size, samples, lut_baker::range_policy::reject, metrics, error))
                {
                    std::cerr << error << '\n';
                    return 1;
                }
                std::cout << std::setprecision(12) << "Identity32 quantization max/mean/RMS: " << metrics.quantization.maximum_absolute
                          << " / " << metrics.quantization.mean_absolute << " / " << metrics.quantization.rms << '\n';
            }
        }
        const auto layout = lut_baker::choose_lattice_layout(32);
        auto asymmetric = lut_baker::make_identity_lattice(32, layout.first, layout.second);
        // Compute from ideal coordinates in double, then make the float32
        // samples explicit. The Python reference independently does the same.
        for (std::uint32_t b = 0; b < 32; ++b)
            for (std::uint32_t g = 0; g < 32; ++g)
                for (std::uint32_t r = 0; r < 32; ++r)
                    asymmetric[(b * 32 + g) * 32 + r] = {
                        static_cast<float>(0.1 + 0.8 * (b / 31.0)),
                        static_cast<float>(0.1 + 0.8 * (r / 31.0) * (r / 31.0)),
                        static_cast<float>(0.1 + 0.8 * (g / 31.0)), 1.0f
                    };
        metadata.title = "Asymmetric axes fixture";
        metadata.techniques = { "Offline F(r,g,b) = (0.1+0.8*b, 0.1+0.8*r*r, 0.1+0.8*g)" };
        lut_baker::rise_export_metrics metrics;
        if (!lut_baker::write_rise_tex_atomic(directory / "Asymmetric32.tex.28", 32, asymmetric, lut_baker::range_policy::reject, metrics, error) ||
            !lut_baker::write_cube_atomic(directory / "Asymmetric32.cube", 32, asymmetric, metadata, false, error))
        {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << std::setprecision(12) << "Asymmetric32 quantization max/mean/RMS: " << metrics.quantization.maximum_absolute
                  << " / " << metrics.quantization.mean_absolute << " / " << metrics.quantization.rms << '\n';
        for (const std::uint32_t size : { 16u, 32u, 64u, 128u })
        {
            const auto png_dimensions = lut_baker::choose_lattice_layout(size);
            const auto identity = lut_baker::make_identity_lattice(size, png_dimensions.first, png_dimensions.second);
            for (const auto distribution : { lut_baker::png_layout::horizontal, lut_baker::png_layout::square })
            {
                if (distribution == lut_baker::png_layout::square && size != 16 && size != 64)
                    continue;
                const std::string suffix = distribution == lut_baker::png_layout::square ? "_square.png" : "_horizontal.png";
                for (const auto depth : { lut_baker::png_bit_depth::eight, lut_baker::png_bit_depth::sixteen })
                {
                    const std::string depth_suffix = depth == lut_baker::png_bit_depth::sixteen ? "_16bit" : "";
                    lut_baker::quantization_metrics png_metrics;
                    if (!lut_baker::write_png_lut_atomic(directory / ("Identity" + std::to_string(size) + depth_suffix + suffix), size,
                        identity, distribution, lut_baker::range_policy::reject, metadata, png_metrics, error, depth))
                    {
                        std::cerr << error << '\n';
                        return 1;
                    }
                }
            }
        }
        auto asymmetric64 = lut_baker::make_identity_lattice(64, 512, 512);
        for (std::uint32_t b = 0; b < 64; ++b)
            for (std::uint32_t g = 0; g < 64; ++g)
                for (std::uint32_t r = 0; r < 64; ++r)
                    asymmetric64[(b * 64 + g) * 64 + r] = {
                        static_cast<float>(0.1 + 0.8 * (b / 63.0)),
                        static_cast<float>(0.1 + 0.8 * (r / 63.0) * (r / 63.0)),
                        static_cast<float>(0.1 + 0.8 * (g / 63.0)), 1.0f
                    };
        if (!lut_baker::write_cube_atomic(directory / "Asymmetric64.cube", 64, asymmetric64, metadata, false, error))
        {
            std::cerr << error << '\n';
            return 1;
        }
        for (const auto distribution : { lut_baker::png_layout::horizontal, lut_baker::png_layout::square })
        {
            const std::string suffix = distribution == lut_baker::png_layout::square ? "_square.png" : "_horizontal.png";
            for (const auto depth : { lut_baker::png_bit_depth::eight, lut_baker::png_bit_depth::sixteen })
            {
                const std::string depth_suffix = depth == lut_baker::png_bit_depth::sixteen ? "_16bit" : "";
                lut_baker::quantization_metrics png_metrics;
                if (!lut_baker::write_png_lut_atomic(directory / ("Asymmetric64" + depth_suffix + suffix), 64, asymmetric64,
                    distribution, lut_baker::range_policy::reject, metadata, png_metrics, error, depth))
                {
                    std::cerr << error << '\n';
                    return 1;
                }
            }
        }
        std::cout << "Fixtures written to " << directory.u8string() << ".\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
