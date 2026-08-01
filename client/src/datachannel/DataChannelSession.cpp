#include "DataChannelSession.h"

#include "../fileio/ChunkedFileReader.h"
#include "../fileio/ChunkedFileWriter.h"
#include "../fileio/AsciiChunkedReader.h"

#include <iostream>
#include <vector>

DataChannelSession::DataChannelSession(IRdtTransport& transport)
    : transport(transport) {
}

bool DataChannelSession::sendFile(const std::filesystem::path& filePath, TransferMode mode) {
    if (!transport.establishConnection()) {
        std::cerr << "[DataChannelSession] Handshake failed -- aborting send.\n";
        return false;
    }

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
    if (!transport.establishConnection()) {
        std::cerr << "[DataChannelSession] Handshake failed -- aborting receive.\n";
        return false;
    }

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