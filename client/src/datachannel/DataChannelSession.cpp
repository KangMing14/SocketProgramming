#include "DataChannelSession.h"

#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"

#include <vector>

namespace hybridftp::client {

DataChannelSession::DataChannelSession(IRdtTransport& transport)
    : transport(transport) {}

namespace {
template <typename Reader>
bool sendFromReader(Reader& reader, IRdtTransport& transport) {
    if (!reader.isOpen()) return false;

    std::vector<char> current;
    std::vector<char> next;
    if (!reader.nextChunk(current)) {
        return transport.sendChunk(0, nullptr, 0, true) && transport.flush();
    }

    std::uint32_t sequence = 0;
    while (true) {
        const bool hasNext = reader.nextChunk(next);
        if (!transport.sendChunk(sequence, current.data(), current.size(),
                                 !hasNext)) {
            return false;
        }
        ++sequence;
        if (!hasNext) break;
        current.swap(next);
        next.clear();
    }
    return transport.flush();
}
}

bool DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                  TransferMode mode) {
    if (mode == TransferMode::ASCII) {
        AsciiChunkedReader reader(filePath);
        return sendFromReader(reader, transport);
    }
    ChunkedFileReader reader(filePath);
    return sendFromReader(reader, transport);
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destination,
                                     TransferMode mode) {
    ChunkedFileWriter writer(destination);
    if (!writer.isValid()) return false;

    std::uint32_t chunkCount = 0;
    while (true) {
        std::uint32_t sequence = 0;
        std::vector<char> data;
        bool isFinal = false;
        if (!transport.receiveNext(sequence, data, isFinal)) return false;
        writer.addChunk(sequence, data);
        ++chunkCount;
        if (isFinal) break;
    }

    if (mode == TransferMode::ASCII) {
        AsciiTranslator translator;
        return writer.finalize(
            chunkCount, [&translator](const std::vector<char>& bytes) {
                return translator.decode(bytes);
            });
    }
    return writer.finalize(chunkCount);
}

}
