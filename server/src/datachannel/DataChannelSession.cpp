#include "DataChannelSession.h"

#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "AsciiChunkedReader.h"


DataChannelSession::DataChannelSession(IRdtTransport& transport)
    : transport(transport) {}

bool DataChannelSession::sendFile(const std::filesystem::path& filePath, TransferMode mode) {
    std::vector<char> chunk;
    uint32_t seq = 0;

    if (mode == TransferMode::Binary) {
        ChunkedFileReader reader(filePath);
        if (!reader.isOpen()) return false;
        while (reader.nextChunk(chunk)) {
            if (!transport.sendChunk(seq, chunk.data(), chunk.size())) return false;
            seq++;
        }
    }
    else {
        AsciiChunkedReader reader(filePath);
        if (!reader.isOpen()) return false;
        while (reader.nextChunk(chunk)) {
            if (!transport.sendChunk(seq, chunk.data(), chunk.size())) return false;
            seq++;
        }
    }
    return true;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destPath, TransferMode mode) {
    ChunkedFileWriter writer(destPath);

    uint32_t seq = 0, chunkCount = 0;
    std::vector<char> data;
    bool isFinal = false;

    while (transport.receiveNext(seq, data, isFinal)) {
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