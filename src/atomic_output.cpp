#include "atomic_output.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

namespace
{
std::atomic<std::uint64_t> s_temp_counter { 0 };

struct owned_temporary
{
    std::filesystem::path path;
    bool owned = false;
    ~owned_temporary()
    {
        if (owned)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};
}

namespace lut_baker
{
bool write_file_atomic(const std::filesystem::path &destination, const bool overwrite,
    const std::function<bool(std::ostream &, std::string &)> &serialize, std::string &error)
{
    std::error_code filesystem_error;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        error = "Unable to create the LUT_Bakes output directory: " + filesystem_error.message();
        return false;
    }
    const bool exists = std::filesystem::exists(destination, filesystem_error);
    if (filesystem_error)
    {
        error = "Unable to inspect the output destination: " + filesystem_error.message();
        return false;
    }
    if (!overwrite && exists)
    {
        error = "The destination already exists.";
        return false;
    }

    owned_temporary temporary;
    // CREATE_NEW reserves a new temporary file. Leave existing files alone,
    // even if an earlier process used the same process ID.
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::wostringstream suffix;
        suffix << L".tmp." << GetCurrentProcessId() << L'.' << s_temp_counter.fetch_add(1, std::memory_order_relaxed);
        temporary.path = destination.parent_path() / (destination.filename().wstring() + suffix.str());
        const HANDLE file = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            temporary.owned = true;
            CloseHandle(file);
            break;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
        {
            error = "Unable to create the temporary LUT file (Windows error " + std::to_string(code) + ").";
            return false;
        }
    }
    if (!temporary.owned)
    {
        error = "Unable to reserve a unique temporary LUT file.";
        return false;
    }

    // Declare the stream after the guard so it closes before cleanup, even if
    // serialization throws.
    std::ofstream stream(temporary.path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        error = "Unable to open the temporary LUT file.";
        return false;
    }
    if (!serialize(stream, error))
        return false;
    stream.flush();
    if (!stream)
    {
        error = "The LUT write failed before all samples were committed.";
        return false;
    }
    stream.close();
    if (!stream)
    {
        error = "The LUT file could not be closed cleanly.";
        return false;
    }

    // A same-directory rename publishes the complete file. Without replacement,
    // it also rejects destinations created after the initial existence check.
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0u);
    if (!MoveFileExW(temporary.path.c_str(), destination.c_str(), flags))
    {
        const DWORD code = GetLastError();
        error = "Unable to install output file (Windows error " + std::to_string(code) + ").";
        if (!overwrite && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS))
            error += " The destination already exists.";
        return false;
    }
    temporary.owned = false;
    return true;
}
}
