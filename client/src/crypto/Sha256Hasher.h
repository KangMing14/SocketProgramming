#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace Sha256Hasher {
    // @return Lowercase hex digest, or empty string on failure.
    std::string hashFile(const fs::path& filePath);
}