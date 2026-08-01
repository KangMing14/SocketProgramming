#include "AsciiChunkedReader.h"

#include <algorithm>

namespace hybridftp::client {

AsciiChunkedReader::AsciiChunkedReader(const std::filesystem::path& filePath) : reader(filePath) {}

bool AsciiChunkedReader::isOpen() const { return reader.isOpen(); }

bool AsciiChunkedReader::nextChunk(std::vector<char>& chunk) {
    while (pendingOutput.size() < ChunkedFileReader::CHUNK_SIZE) {
        std::vector<char> raw;
        if (!reader.nextChunk(raw)) break;
        std::vector<char> translated = translator.encode(raw);
        pendingOutput.insert(pendingOutput.end(), translated.begin(), translated.end());
    }

    if (pendingOutput.empty()) return false;

    size_t take = std::min(pendingOutput.size(), ChunkedFileReader::CHUNK_SIZE);
    chunk.assign(pendingOutput.begin(), pendingOutput.begin() + take);
    pendingOutput.erase(pendingOutput.begin(), pendingOutput.begin() + take);
    return true;
}

}
