#include "ChunkedFileWriter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace hybridftp::client {

namespace {
bool createSiblingTemporaryFile(const fs::path& destination,
                                fs::path& temporaryPath) {
    const fs::path parent = destination.parent_path();
    wchar_t buffer[MAX_PATH]{};
    if (GetTempFileNameW(parent.c_str(), L"hft", 0, buffer) == 0) {
        return false;
    }
    temporaryPath = buffer;
    return true;
}
}

ChunkedFileWriter::ChunkedFileWriter(const fs::path& filePath) {
    if (filePath.empty()) return;

    std::error_code error;
    destinationPath = fs::absolute(filePath, error);
    if (error || destinationPath.parent_path().empty() ||
        !createSiblingTemporaryFile(destinationPath, temporaryPath)) {
        return;
    }

    output.open(temporaryPath, std::ios::binary | std::ios::trunc);
    valid = output.is_open();
    if (!valid) {
        fs::remove(temporaryPath, error);
        temporaryPath.clear();
    }
}

ChunkedFileWriter::~ChunkedFileWriter() { abort(); }

bool ChunkedFileWriter::isValid() const { return valid; }

bool ChunkedFileWriter::appendChunk(uint32_t seqNum,
                                    const std::vector<char>& data) {
    if (!valid || committed || seqNum != nextExpectedSequence) return false;
    if (!data.empty()) {
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!output.good()) return false;
    }
    ++nextExpectedSequence;
    return true;
}

bool ChunkedFileWriter::commit() {
    if (!valid || committed) return false;

    output.flush();
    if (!output.good()) return false;
    output.close();
    if (output.fail()) return false;

    if (!MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return false;
    }

    committed = true;
    valid = false;
    temporaryPath.clear();
    return true;
}

void ChunkedFileWriter::abort() {
    if (committed) return;
    if (output.is_open()) output.close();
    if (!temporaryPath.empty()) {
        std::error_code ignored;
        fs::remove(temporaryPath, ignored);
        temporaryPath.clear();
    }
    valid = false;
}

}
