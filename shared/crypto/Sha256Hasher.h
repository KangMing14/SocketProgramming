#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace Sha256Hasher {
// Returns a lowercase hexadecimal digest, or an empty string on failure.
std::string hashFile(const fs::path& filePath);
}
