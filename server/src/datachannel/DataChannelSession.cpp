#include "DataChannelSession.h"

#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"

DataChannelSession::DataChannelSession(SOCKET dataSocket, sockaddr_in peerAddr, IRdtTransport& transport)
    : dataSocket(dataSocket), peerAddr(peerAddr), transport(transport) {}

bool DataChannelSession::sendFile(const std::filesystem::path& filePath) {
    ChunkedFileReader reader(filePath);
    if (!reader.isOpen()) return false;

    std::vector<char> chunk;
    uint32_t seq = 0;
    while (reader.nextChunk(chunk)) {
        if (!transport.sendChunk(seq, chunk.data(), chunk.size())) {
            return false; // B's retry logic lives inside sendChunk — a false here
                          // means either transport gave up entirely or still retrying
                          // internally. Shall change based on member B's implementation.
        }
        seq++;
    }
    return true;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destPath) {
    ChunkedFileWriter writer(destPath);

    uint32_t seq = 0, chunkCount = 0;
    std::vector<char> data;
    bool isFinal = false;

    while (transport.receiveNext(seq, data, isFinal)) {
        writer.addChunk(seq, data);
        chunkCount++;       // Assumes receiveNext is called (and returns true) exactly
                            // once per unique chunk. Member B's receiveNext implementation 
                            // might return true many times, need change if really is.

        if (isFinal) break; // Also based on how member B's flagged the final chunk
    }

    return writer.finalize(chunkCount);
}