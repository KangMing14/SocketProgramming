#include "DataChannelSession.h"

#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include "../crypto/Sha256Hasher.h"

#include <windows.h>
#include <system_error>
#include <vector>

namespace hybridftp::client {

DataChannelSession::DataChannelSession(IRdtTransport& transport)
    : transport(transport) {}

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

SendResult DataChannelSession::sendFile(const std::filesystem::path& filePath,
                                        TransferMode mode) {
    SendResult result;
    SourceSnapshot snapshot(filePath);
    if (!snapshot.isValid()) return result;

    if (mode == TransferMode::ASCII) {
        AsciiChunkedReader reader(snapshot.path());
        result.success = sendFromReader(reader, transport);
        return result;
    }
    result.sha256 = Sha256Hasher::hashFile(snapshot.path());
    if (result.sha256.empty()) return result;
    ChunkedFileReader reader(snapshot.path());
    result.success = sendFromReader(reader, transport);
    return result;
}

bool DataChannelSession::receiveFile(const std::filesystem::path& destination,
                                     TransferMode mode) {
    ChunkedFileWriter writer(destination);
    if (!writer.isValid()) return false;

    AsciiTranslator translator;
    while (true) {
        std::uint32_t sequence = 0;
        std::vector<char> data;
        bool isFinal = false;
        if (!transport.receiveNext(sequence, data, isFinal)) return false;
        if (mode == TransferMode::ASCII) data = translator.decode(data);
        if (!writer.appendChunk(sequence, data)) return false;
        if (isFinal) break;
    }

    return writer.commit();
}

}
