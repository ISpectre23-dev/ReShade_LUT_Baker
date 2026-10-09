#pragma once

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>

namespace lut_baker
{
// Write to a temporary file in the destination directory, then rename it.
// This keeps partial files out of the destination if writing fails.
[[nodiscard]] bool write_file_atomic(const std::filesystem::path &destination, bool overwrite,
    const std::function<bool(std::ostream &, std::string &)> &serialize, std::string &error);
}
