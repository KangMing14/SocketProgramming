#pragma once

#include "PathResolver.h"
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct DirEntryInfo {
    std::string formatPermissions;
    std::string name;
    bool isDirectory;
    uintmax_t sizeBytes;
};

class DirectoryService {
public:
    explicit DirectoryService(const PathResolver& resolver);

    struct Result { bool ok; int code; std::string message; };
    struct PathMetadata { bool exists; bool isDirectory; uintmax_t sizeBytes; fs::file_time_type lastModified; };

    Result printWorkingDir(const fs::path& currentDir) const;                                         // PWD
    Result changeDir(fs::path& currentDir, const std::string& target) const;                          // CWD
    Result changeToParent(fs::path& currentDir) const;                                                // DCUP
    Result makeDir(const fs::path& currentDir, const std::string& name) const;                        // MKD
    Result removeDir(const fs::path& currentDir, const std::string& name) const;                      // RMD
    bool listDir(const fs::path& currentDir, std::vector<DirEntryInfo>& outEntries) const;            // LIST/NLST
    bool getMetadata(const fs::path& currentDir, const std::string& target, PathMetadata& out) const; // STAT <path>

private:
    const PathResolver& resolver;

    std::string getFormatPermissions(const fs::path& path) const;
};