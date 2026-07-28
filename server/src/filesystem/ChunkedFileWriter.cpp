#include "ChunkedFileWriter.h"

ChunkedFileWriter::ChunkedFileWriter(const fs::path& filePath) : filePath(filePath) {}

bool ChunkedFileWriter::isValid() const { return !filePath.empty(); }

void ChunkedFileWriter::addChunk(uint32_t seqNum, const std::vector<char>& data) { pendingChunks[seqNum] = data; };

bool ChunkedFileWriter::finalize(uint32_t expectedChunkCount,
                                    const std::function<std::vector<char>(const std::vector<char>&)>& transform) {
    if (pendingChunks.size() != expectedChunkCount) return false;

    std::ofstream out(filePath, std::ios::binary);
    if (!out) return false;

    for (uint32_t i = 0; i < expectedChunkCount; i++) {
        auto it = pendingChunks.find(i);
        if (it == pendingChunks.end()) return false;

        if (transform) {
            std::vector<char> transformed = transform(it->second);
            out.write(transformed.data(), transformed.size());
        }
        else {
            out.write(it->second.data(), it->second.size());
        }
    }

    return true;
}