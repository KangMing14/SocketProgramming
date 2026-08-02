#pragma once
#include "ChunkedFileReader.h"
#include "AsciiTranslator.h"
#include <vector>
#include <filesystem>

namespace hybridftp::client {

class AsciiChunkedReader {
public:
    explicit AsciiChunkedReader(const std::filesystem::path& filePath);
    bool isOpen() const;
    bool nextChunk(std::vector<char>& chunk);

private:
    ChunkedFileReader reader;
    AsciiTranslator translator;
    std::vector<char> pendingOutput;
};

}
