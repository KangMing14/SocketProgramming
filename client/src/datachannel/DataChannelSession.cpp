#include "DataChannelSession.h"

#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "../crypto/Sha256Hasher.h"

#include <windows.h>
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
}

SendResult DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                        TransferMode mode) {
    SendResult result;
    SourceSnapshot snapshot(filePath);
    if (!snapshot.isValid()) return result;

    if (mode == TransferMode::ASCII) {
        AsciiChunkedReader reader(snapshot.path());
        result.success = sendFromReader(reader, transport, abortRequested);
        return result;
    }
    result.sha256 = Sha256Hasher::hashFile(snapshot.path());
    if (result.sha256.empty()) return result;
    ChunkedFileReader reader(snapshot.path());
    result.success = sendFromReader(reader, transport, abortRequested);
    return result;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destination,
                                     TransferMode mode,
                                     std::string* failureReason) {
    if (failureReason != nullptr) failureReason->clear();
    const auto fail = [&](const std::string& reason) {
        if (failureReason != nullptr) *failureReason = reason;
        return false;
    };

    ChunkedFileWriter writer(destination);
    if (!writer.isValid()) return fail(writer.errorMessage());

    AsciiTranslator translator;
    while (true) {
        std::uint32_t sequence = 0;
        std::vector<char> data;
        bool isFinal = false;
        if (isAborted()) return fail("Download was aborted.");
        if (!transport.receiveNext(sequence, data, isFinal)) {
            return fail("The data connection closed or timed out before the final packet.");
        }
        if (isAborted()) {
            transport.confirmReceive(sequence, false);
            return fail("Download was aborted.");
        }
        if (mode == TransferMode::ASCII) data = translator.decode(data);
        if (!writer.appendChunk(sequence, data)) {
            transport.confirmReceive(sequence, false);
            return fail(writer.errorMessage());
        }
        if (!isFinal) continue;

        const bool committed = !isAborted() && writer.commit();
        const bool confirmed = transport.confirmReceive(sequence, committed);
        if (!committed) {
            return fail(isAborted() ? "Download was aborted."
                                    : writer.errorMessage());
        }
        if (!confirmed) {
            return fail("The file was saved, but the final acknowledgement could not be sent.");
        }
        return true;
    }
}

}
