#include "ChunkedFileWriter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <system_error>

namespace hybridftp::client {

namespace {
std::string windowsFailure(const std::string& operation, DWORD errorCode) {
    const std::error_code error(
        static_cast<int>(errorCode), std::system_category());
    return operation + ": " + error.message() + " (Windows error " +
           std::to_string(errorCode) + ").";
}

bool resolveDestination(const fs::path& filePath, fs::path& destination,
                        std::string& failureReason) {
    if (filePath.empty()) {
        failureReason = "The local destination path is empty.";
        return false;
    }

    std::error_code error;
    destination = fs::absolute(filePath, error);
    if (error) {
        failureReason = "Could not resolve the local destination: " +
                        error.message() + ".";
        return false;
    }

    const fs::path parent = destination.parent_path();
    if (parent.empty()) {
        failureReason = "The local destination has no parent directory.";
        return false;
    }
    if (!fs::exists(parent, error) || error) {
        failureReason = error
            ? "Could not inspect the parent directory: " + error.message() + "."
            : "The parent directory does not exist.";
        return false;
    }
    if (!fs::is_directory(parent, error) || error) {
        failureReason = error
            ? "Could not inspect the parent directory: " + error.message() + "."
            : "The destination parent is not a directory.";
        return false;
    }

    error.clear();
    const bool destinationExists = fs::exists(destination, error);
    if (error) {
        failureReason = "Could not inspect the local destination: " +
                        error.message() + ".";
        return false;
    }
    if (!destinationExists) return true;
    if (fs::is_directory(destination, error) && !error) {
        failureReason = "The local destination is a directory; specify a filename.";
        return false;
    }
    if (error) {
        failureReason = "Could not inspect the local destination: " +
                        error.message() + ".";
        return false;
    }

    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        failureReason = windowsFailure(
            "Could not inspect the local destination", GetLastError());
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        failureReason = "The local destination is read-only.";
        return false;
    }

    HANDLE destinationHandle = CreateFileW(
        destination.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destinationHandle == INVALID_HANDLE_VALUE) {
        failureReason = windowsFailure(
            "The local destination cannot be replaced", GetLastError());
        return false;
    }
    CloseHandle(destinationHandle);
    return true;
}

bool createSiblingTemporaryFile(const fs::path& destination,
                                fs::path& temporaryPath,
                                std::string& failureReason) {
    const fs::path parent = destination.parent_path();
    wchar_t buffer[MAX_PATH]{};
    if (GetTempFileNameW(parent.c_str(), L"hft", 0, buffer) == 0) {
        failureReason = windowsFailure(
            "Could not create a temporary download file", GetLastError());
        return false;
    }
    temporaryPath = buffer;
    return true;
}
}

ChunkedFileWriter::ChunkedFileWriter(const fs::path& filePath) {
    if (!resolveDestination(filePath, destinationPath, failureReason) ||
        !createSiblingTemporaryFile(
            destinationPath, temporaryPath, failureReason)) {
        return;
    }

    output.open(temporaryPath, std::ios::binary | std::ios::trunc);
    valid = output.is_open();
    if (!valid) {
        failureReason = "Could not open the temporary download file for writing.";
        std::error_code error;
        fs::remove(temporaryPath, error);
        temporaryPath.clear();
    }
}

ChunkedFileWriter::~ChunkedFileWriter() { abort(); }

bool ChunkedFileWriter::isValid() const { return valid; }

bool ChunkedFileWriter::validateDestination(
    const fs::path& filePath, std::string& failureReason) {
    failureReason.clear();
    fs::path destination;
    if (!resolveDestination(filePath, destination, failureReason)) return false;

    fs::path probe;
    if (!createSiblingTemporaryFile(destination, probe, failureReason)) return false;
    if (!DeleteFileW(probe.c_str())) {
        const DWORD errorCode = GetLastError();
        std::error_code ignored;
        fs::remove(probe, ignored);
        failureReason = windowsFailure(
            "Could not clean up the temporary destination probe", errorCode);
        return false;
    }
    return true;
}

bool ChunkedFileWriter::appendChunk(uint32_t seqNum,
                                    const std::vector<char>& data) {
    if (!valid) {
        if (failureReason.empty()) {
            failureReason = "The temporary download file is not available.";
        }
        return false;
    }
    if (committed) {
        failureReason = "The download file was already committed.";
        return false;
    }
    if (seqNum != nextExpectedSequence) {
        failureReason = "Received an unexpected data sequence number.";
        return false;
    }
    if (!data.empty()) {
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!output.good()) {
            failureReason = "Could not write data to the temporary download file.";
            return false;
        }
    }
    ++nextExpectedSequence;
    return true;
}

bool ChunkedFileWriter::commit() {
    if (!valid || committed) {
        if (failureReason.empty()) {
            failureReason = "The temporary download file cannot be committed.";
        }
        return false;
    }

    output.flush();
    if (!output.good()) {
        failureReason = "Could not flush the temporary download file.";
        return false;
    }
    output.close();
    if (output.fail()) {
        failureReason = "Could not close the temporary download file.";
        return false;
    }

    if (!MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        failureReason = windowsFailure(
            "Could not replace the local destination", GetLastError());
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
