#include "DataChannelSession.h"

#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "AsciiChunkedReader.h"


DataChannelSession::DataChannelSession(IRdtTransport& transport)
    : transport(transport) {}

bool DataChannelSession::sendFile(const std::filesystem::path& filePath, TransferMode mode) {
    std::vector<char> currentChunk;
    std::vector<char> nextChunk;
    uint32_t seq = 0;

    if (mode == TransferMode::Binary) {
        ChunkedFileReader reader(filePath);
        if (!reader.isOpen()) return false;
        if (!reader.nextChunk(currentChunk)) {
            return transport.sendChunk(0, nullptr, 0, true) && transport.flush();
        }
        while (true) {
            const bool hasNext = reader.nextChunk(nextChunk);
            if (!transport.sendChunk(seq, currentChunk.data(), currentChunk.size(), !hasNext)) return false;
            seq++;
            if (!hasNext) break;
            currentChunk.swap(nextChunk);
            nextChunk.clear();
        }
    }
    else {
        AsciiChunkedReader reader(filePath);
        if (!reader.isOpen()) return false;
        if (!reader.nextChunk(currentChunk)) {
            return transport.sendChunk(0, nullptr, 0, true) && transport.flush();
        }
        while (true) {
            const bool hasNext = reader.nextChunk(nextChunk);
            if (!transport.sendChunk(seq, currentChunk.data(), currentChunk.size(), !hasNext)) return false;
            seq++;
            if (!hasNext) break;
            currentChunk.swap(nextChunk);
            nextChunk.clear();
        }
    }
    return transport.flush();
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destPath, TransferMode mode) {
    ChunkedFileWriter writer(destPath);

    uint32_t seq = 0, chunkCount = 0;
    std::vector<char> data;
    bool isFinal = false;

    while (true) {
        if (!transport.receiveNext(seq, data, isFinal)) return false;
        writer.addChunk(seq, data);
        chunkCount++;

        if (isFinal) break;
    }

    if (mode == TransferMode::Binary) {
        return writer.finalize(chunkCount);
    }
    else {
        AsciiTranslator translator;
        return writer.finalize(chunkCount, [&translator](const std::vector<char>& raw) { return translator.decode(raw); });
    }
}
