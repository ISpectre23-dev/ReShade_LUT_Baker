#pragma once

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>

namespace lut_baker
{
// Serializers share only filesystem handling, not sample processing. The
// callback and all filesystem work run on the CPU writer, never per frame.
[[nodiscard]] bool write_file_atomic(const std::filesystem::path &destination, bool overwrite,
    const std::function<bool(std::ostream &, std::string &)> &serialize, std::string &error);
}
