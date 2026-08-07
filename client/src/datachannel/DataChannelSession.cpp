#include "DataChannelSession.h"

#include "BlockMode.h"
#include "CompressedMode.h"
#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "../crypto/Sha256Hasher.h"

#include <windows.h>
#include <algorithm>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

namespace hybridftp::client {

DataChannelSession::DataChannelSession(IRdtTransport& transport,
                                       AbortPredicate abortRequested)
    : transport(transport), abortRequested(std::move(abortRequested)) {}

bool DataChannelSession::isAborted() const {
    return abortRequested && abortRequested();
}

namespace {
class SourceSnapshot {
public:
    explicit SourceSnapshot(const std::filesystem::path& source) {
        std::error_code error;
        const auto sourceSize = std::filesystem::file_size(source, error);
        if (error) return;

        const auto temporaryDirectory =
            std::filesystem::temp_directory_path(error);
        if (error) return;

        const auto space = std::filesystem::space(temporaryDirectory, error);
        if (!error && space.available < sourceSize) return;
        error.clear();

        wchar_t buffer[MAX_PATH]{};
        if (GetTempFileNameW(temporaryDirectory.c_str(), L"hfc", 0, buffer) == 0) {
            return;
        }
        path_ = buffer;
        if (!std::filesystem::copy_file(
                source, path_, std::filesystem::copy_options::overwrite_existing,
                error) || error) {
            std::filesystem::remove(path_, error);
            path_.clear();
        }
    }

    ~SourceSnapshot() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    bool isValid() const { return !path_.empty(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

template <typename Reader>
bool sendFromReader(Reader& reader, IRdtTransport& transport,
                    const DataChannelSession::AbortPredicate& abortRequested) {
    const auto aborted = [&] { return abortRequested && abortRequested(); };
    if (!reader.isOpen() || aborted()) return false;

    std::vector<char> current;
    std::vector<char> next;
    if (!reader.nextChunk(current)) {
        return !aborted() && transport.sendChunk(0, nullptr, 0, true) &&
               !aborted() && transport.flush();
    }

    std::uint32_t sequence = 0;
    while (true) {
        const bool hasNext = reader.nextChunk(next);
        if (aborted() ||
            !transport.sendChunk(sequence, current.data(), current.size(),
                                 !hasNext)) {
            return false;
        }
        ++sequence;
        if (!hasNext) break;
        current.swap(next);
        next.clear();
    }
    return !aborted() && transport.flush() && !aborted();
}

bool sendPayload(const std::vector<char>& payload, bool final,
                 std::uint32_t& sequence, IRdtTransport& transport,
                 const DataChannelSession::AbortPredicate& abortRequested) {
    const auto aborted = [&] { return abortRequested && abortRequested(); };
    std::size_t offset = 0;
    do {
        const std::size_t remaining = payload.size() - offset;
        const std::size_t size = std::min(remaining, MAX_PAYLOAD);
        const bool fragmentIsFinal = final && offset + size == payload.size();
        if (aborted() ||
            !transport.sendChunk(sequence,
                                 size == 0 ? nullptr : payload.data() + offset,
                                 size, fragmentIsFinal)) {
            return false;
        }
        ++sequence;
        offset += size;
    } while (offset < payload.size());
    return !aborted();
}

template <typename Reader>
bool sendBlockFromReader(
    Reader& reader, IRdtTransport& transport,
    const DataChannelSession::AbortPredicate& abortRequested) {
    const auto aborted = [&] { return abortRequested && abortRequested(); };
    if (!reader.isOpen() || aborted()) return false;

    BlockEncoder encoder;
    std::uint32_t sequence = 0;
    std::vector<char> input;
    while (reader.nextChunk(input)) {
        if (aborted()) return false;
        std::vector<std::vector<char>> records;
        if (!encoder.appendData(input, records)) return false;
        for (const auto& record : records) {
            if (!sendPayload(record, false, sequence, transport,
                             abortRequested)) return false;
        }
        input.clear();
    }

    std::vector<std::vector<char>> eofRecords;
    if (!encoder.appendEndOfFile(eofRecords) || eofRecords.empty()) return false;
    for (std::size_t index = 0; index < eofRecords.size(); ++index) {
        if (!sendPayload(eofRecords[index], index + 1 == eofRecords.size(),
                         sequence, transport, abortRequested)) return false;
    }
    return !aborted() && transport.flush() && !aborted();
}

template <typename Reader>
bool sendCompressedFromReader(
    Reader& reader, TransferType type, IRdtTransport& transport,
    const DataChannelSession::AbortPredicate& abortRequested) {
    const auto aborted = [&] { return abortRequested && abortRequested(); };
    if (!reader.isOpen() || aborted()) return false;

    CompressedEncoder encoder(type);
    std::uint32_t sequence = 0;
    std::vector<char> input;
    while (reader.nextChunk(input)) {
        if (aborted()) return false;
        std::vector<char> encoded;
        if (!encoder.appendData(input, encoded) ||
            !sendPayload(encoded, false, sequence, transport,
                         abortRequested)) return false;
        input.clear();
    }

    std::vector<char> eof;
    if (!encoder.appendEndOfFile(eof) ||
        !sendPayload(eof, true, sequence, transport, abortRequested)) {
        return false;
    }
    return !aborted() && transport.flush() && !aborted();
}
}

SendResult DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                        TransferType type,
                                        TransferMode mode) {
    SendResult result;
    SourceSnapshot snapshot(filePath);
    if (!snapshot.isValid()) return result;

    if (type == TransferType::ASCII) {
        AsciiChunkedReader reader(snapshot.path());
        if (mode == TransferMode::Block) {
            result.success = sendBlockFromReader(reader, transport,
                                                 abortRequested);
        } else if (mode == TransferMode::Compressed) {
            result.success = sendCompressedFromReader(
                reader, type, transport, abortRequested);
        } else {
            result.success = sendFromReader(reader, transport, abortRequested);
        }
        return result;
    }
    result.sha256 = Sha256Hasher::hashFile(snapshot.path());
    if (result.sha256.empty()) return result;
    ChunkedFileReader reader(snapshot.path());
    if (mode == TransferMode::Block) {
        result.success = sendBlockFromReader(reader, transport,
                                             abortRequested);
    } else if (mode == TransferMode::Compressed) {
        result.success = sendCompressedFromReader(
            reader, type, transport, abortRequested);
    } else {
        result.success = sendFromReader(reader, transport, abortRequested);
    }
    return result;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destination,
                                     TransferType type, TransferMode mode) {
    ChunkedFileWriter writer(destination);
    if (!writer.isValid()) return false;

    AsciiTranslator translator;
    if (mode == TransferMode::Block) {
        BlockDecoder decoder;
        std::uint32_t writeSequence = 0;
        while (true) {
            std::uint32_t transportSequence = 0;
            std::vector<char> payload;
            bool isFinal = false;
            if (isAborted() ||
                !transport.receiveNext(transportSequence, payload, isFinal) ||
                isAborted() || !decoder.appendPayload(payload)) return false;

            std::vector<char> data;
            bool endOfFile = false;
            bool endOfRecord = false;
            while (decoder.nextRecord(data, endOfFile, endOfRecord)) {
                if (isAborted()) return false;
                if (type == TransferType::ASCII) data = translator.decode(data);
                if (!data.empty() &&
                    !writer.appendChunk(writeSequence++, data)) return false;
                if (endOfFile) break;
            }
            if (!decoder.isValid()) return false;
            if (decoder.sawEndOfFile() && !isFinal) return false;
            if (isFinal) {
                if (!decoder.finish()) return false;
                break;
            }
        }
        return !isAborted() && writer.commit();
    }

    if (mode == TransferMode::Compressed) {
        CompressedDecoder decoder(type);
        std::uint32_t writeSequence = 0;
        while (true) {
            std::uint32_t transportSequence = 0;
            std::vector<char> payload;
            bool isFinal = false;
            if (isAborted() ||
                !transport.receiveNext(transportSequence, payload, isFinal) ||
                isAborted() || !decoder.appendPayload(payload)) return false;

            std::vector<char> data;
            bool endOfFile = false;
            bool endOfRecord = false;
            bool suspectedError = false;
            while (decoder.nextData(data, endOfFile, endOfRecord,
                                    suspectedError)) {
                if (isAborted()) return false;
                if (type == TransferType::ASCII) data = translator.decode(data);
                if (!data.empty() &&
                    !writer.appendChunk(writeSequence++, data)) return false;
                if (endOfFile) break;
            }
            if (!decoder.isValid()) return false;
            if (decoder.sawEndOfFile() && !isFinal) return false;
            if (isFinal) {
                if (!decoder.finish()) return false;
                break;
            }
        }
        return !isAborted() && writer.commit();
    }

    while (true) {
        std::uint32_t sequence = 0;
        std::vector<char> data;
        bool isFinal = false;
        if (isAborted() ||
            !transport.receiveNext(sequence, data, isFinal) ||
            isAborted()) return false;
        if (type == TransferType::ASCII) data = translator.decode(data);
        if (!writer.appendChunk(sequence, data)) return false;
        if (isFinal) break;
    }

    return !isAborted() && writer.commit();
}

}
