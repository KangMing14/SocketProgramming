#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

class PathResolver {
public:
    explicit PathResolver(fs::path serverRoot);

    // Resolves userInput against currentDir; returns false (and leaves outPath untouched) if the result would escape serverRoot.
    bool resolve(const fs::path& currentDir, const std::string& userInput, fs::path& outPath) const;

    const fs::path& root() const { return serverRoot; }

private:
    fs::path serverRoot;

};