#include "PathResolver.h"
#include <algorithm>

PathResolver::PathResolver(fs::path serverRoot)
    : serverRoot(fs::weakly_canonical(fs::absolute(std::move(serverRoot)))) {
}

bool PathResolver::resolve(const fs::path& currentDir, const std::string& userInput, fs::path& outPath) const {
    
    // Path join
    fs::path candidate = currentDir / userInput;

    // Normalize
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) return false;
    
    // Boundary check
    auto rootEnd = serverRoot.end();
    auto [rootIt, resolvedIt] = std::mismatch(serverRoot.begin(), rootEnd, resolved.begin(), resolved.end());

    if (rootIt != rootEnd) {
        return false;
    }

    outPath = resolved;
    return true;
}