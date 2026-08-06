#include "DataChannelSession.h"

#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "Sha256Hasher.h"

#include <windows.h>
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
}

SendResult DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                        TransferMode mode) {
    SendResult result;
    SourceSnapshot snapshot(filePath);
    if (!snapshot.isValid()) return result;

    if (mode == TransferMode::Binary) {
        result.sha256 = Sha256Hasher::hashFile(snapshot.path());
        if (result.sha256.empty()) return result;
        ChunkedFileReader reader(snapshot.path());
        result.success = sendFromReader(reader, transport, abortRequested);
    } else {
        AsciiChunkedReader reader(snapshot.path());
        result.success = sendFromReader(reader, transport, abortRequested);
    }
    return result;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destPath, TransferMode mode) {
    ChunkedFileWriter writer(destPath);
    if (!writer.isValid()) return false;

    uint32_t seq = 0;
    std::vector<char> data;
    bool isFinal = false;
    AsciiTranslator translator;

    while (true) {
        if (isAborted() || !transport.receiveNext(seq, data, isFinal) ||
            isAborted()) return false;
        if (mode == TransferMode::ASCII) data = translator.decode(data);
        if (!writer.appendChunk(seq, data)) return false;

        if (isFinal) break;
    }

    return !isAborted() && writer.commit();
}
