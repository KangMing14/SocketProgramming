#include "DataChannelSession.h"

#include "BlockMode.h"
#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "Sha256Hasher.h"

#include <windows.h>
#include <algorithm>
#include <functional>
#include <system_error>
#include <utility>

DataChannelSession::DataChannelSession(IRdtTransport& transport,
                                       AbortPredicate abortRequested)
    : transport(transport), abortRequested(std::move(abortRequested)) {}

bool DataChannelSession::isAborted() const {
    return abortRequested && abortRequested();
}

namespace {
class SourceSnapshot {
public:
    explicit SourceSnapshot(const fs::path& source) {
        std::error_code error;
        const auto sourceSize = fs::file_size(source, error);
        if (error) return;

        const fs::path temporaryDirectory = fs::temp_directory_path(error);
        if (error) return;

        const auto space = fs::space(temporaryDirectory, error);
        if (!error && space.available < sourceSize) return;
        error.clear();

        wchar_t buffer[MAX_PATH]{};
        if (GetTempFileNameW(temporaryDirectory.c_str(), L"hfs", 0, buffer) == 0) {
            return;
        }
        path_ = buffer;
        if (!fs::copy_file(source, path_, fs::copy_options::overwrite_existing,
                           error) || error) {
            fs::remove(path_, error);
            path_.clear();
        }
    }

    ~SourceSnapshot() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    bool isValid() const { return !path_.empty(); }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

template <typename Reader>
bool sendFromReader(Reader& reader, IRdtTransport& transport,
                    const DataChannelSession::AbortPredicate& abortRequested) {
    std::vector<char> currentChunk;
    std::vector<char> nextChunk;
    uint32_t seq = 0;

    const auto aborted = [&] { return abortRequested && abortRequested(); };

    if (!reader.isOpen() || aborted()) return false;
    if (!reader.nextChunk(currentChunk)) {
        return !aborted() && transport.sendChunk(0, nullptr, 0, true) &&
               !aborted() && transport.flush();
    }

    while (true) {
        const bool hasNext = reader.nextChunk(nextChunk);
        if (aborted() ||
            !transport.sendChunk(seq, currentChunk.data(), currentChunk.size(),
                                 !hasNext)) return false;
        ++seq;
        if (!hasNext) break;
        currentChunk.swap(nextChunk);
        nextChunk.clear();
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
}

SendResult DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                        TransferType type,
                                        TransferMode mode) {
    SendResult result;
    if (mode == TransferMode::Compressed) return result;
    SourceSnapshot snapshot(filePath);
    if (!snapshot.isValid()) return result;

    if (type == TransferType::Binary) {
        result.sha256 = Sha256Hasher::hashFile(snapshot.path());
        if (result.sha256.empty()) return result;
        ChunkedFileReader reader(snapshot.path());
        result.success = mode == TransferMode::Block
            ? sendBlockFromReader(reader, transport, abortRequested)
            : sendFromReader(reader, transport, abortRequested);
    } else {
        AsciiChunkedReader reader(snapshot.path());
        result.success = mode == TransferMode::Block
            ? sendBlockFromReader(reader, transport, abortRequested)
            : sendFromReader(reader, transport, abortRequested);
    }
    return result;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destPath,
                                     TransferType type, TransferMode mode) {
    if (mode == TransferMode::Compressed) return false;
    ChunkedFileWriter writer(destPath);
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

    while (true) {
        uint32_t seq = 0;
        std::vector<char> data;
        bool isFinal = false;
        if (isAborted() || !transport.receiveNext(seq, data, isFinal) ||
            isAborted()) return false;
        if (type == TransferType::ASCII) data = translator.decode(data);
        if (!writer.appendChunk(seq, data)) return false;

        if (isFinal) break;
    }

    return !isAborted() && writer.commit();
}
