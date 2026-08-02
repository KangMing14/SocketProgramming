#include <fstream>
#include <random>
#include <filesystem>
#include "RdtHeader.h"

namespace fs = std::filesystem;

class FakeRdtTransport : public IRdtTransport {
public:
    std::vector<std::pair<uint32_t, std::vector<char>>> sentChunks;
    std::vector<bool> sentFinalFlags;
    size_t receiveIndex = 0;

    bool sendChunk(uint32_t seq, const char* data, size_t len,
                   bool isFinal = false) override {
        std::vector<char> bytes;
        if (len > 0) bytes.assign(data, data + len);
        sentChunks.emplace_back(seq, std::move(bytes));
        sentFinalFlags.push_back(isFinal);
        return true; // no real network, always "succeeds"
    }

    bool receiveNext(uint32_t& seq, std::vector<char>& data, bool& isFinal) override {
        if (receiveIndex >= sentChunks.size()) return false;
        seq = sentChunks[receiveIndex].first;
        data = sentChunks[receiveIndex].second;
        isFinal = sentFinalFlags[receiveIndex];
        receiveIndex++;
        return true;
    }
};

void createTestFile(const fs::path& path, size_t sizeBytes) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937 rng(42); // fixed seed
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < sizeBytes; i++) {
        char byte = static_cast<char>(dist(rng));
        out.write(&byte, 1);
    }
}

bool filesAreIdentical(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    if (fs::file_size(a) != fs::file_size(b)) return false;

    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}
