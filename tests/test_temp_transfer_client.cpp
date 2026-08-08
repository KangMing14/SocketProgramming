#include "../client/src/crypto/Sha256Hasher.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "../client/src/fileio/ChunkedFileWriter.h"
#include "helper.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace fs = std::filesystem;

namespace {
class MutatingTransport : public FakeRdtTransport {
public:
    explicit MutatingTransport(fs::path source) : source(std::move(source)) {}

    bool sendChunk(uint32_t sequence, const char* data, size_t length,
                   bool isFinal = false) override {
        if (!mutated) {
            std::ofstream replacement(source,
                                      std::ios::binary | std::ios::trunc);
            replacement << "client source changed";
            mutated = true;
        }
        return FakeRdtTransport::sendChunk(sequence, data, length, isFinal);
    }

private:
    fs::path source;
    bool mutated = false;
};

class CommitObservingTransport : public FakeRdtTransport {
public:
    explicit CommitObservingTransport(fs::path destinationPath)
        : destination(std::move(destinationPath)) {}

    bool confirmReceive(std::uint32_t, bool accepted) override {
        confirmationCalled = true;
        confirmationAccepted = accepted;
        std::ifstream input(destination, std::ios::binary);
        committedBeforeConfirmation = input &&
            std::string(std::istreambuf_iterator<char>(input), {}) == "committed";
        return accepted;
    }

    bool confirmationCalled = false;
    bool confirmationAccepted = false;
    bool committedBeforeConfirmation = false;

private:
    fs::path destination;
};

class LockingFinalTransport : public FakeRdtTransport {
public:
    explicit LockingFinalTransport(fs::path destinationPath)
        : destination(fs::absolute(std::move(destinationPath))) {}

    ~LockingFinalTransport() {
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }

    bool receiveNext(std::uint32_t& sequence, std::vector<char>& data,
                     bool& isFinal) override {
        if (!FakeRdtTransport::receiveNext(sequence, data, isFinal)) return false;
        if (lock == INVALID_HANDLE_VALUE) {
            lock = CreateFileW(
                destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        return lock != INVALID_HANDLE_VALUE;
    }

    bool confirmReceive(std::uint32_t, bool accepted) override {
        confirmationCalled = true;
        confirmationAccepted = accepted;
        return accepted;
    }

    bool confirmationCalled = false;
    bool confirmationAccepted = true;

private:
    fs::path destination;
    HANDLE lock = INVALID_HANDLE_VALUE;
};

void testClientSenderUsesSnapshotHash() {
    const fs::path source = "client_temp_source.bin";
    const fs::path expected = "client_temp_expected.bin";
    createTestFile(source, 3073);
    fs::copy_file(source, expected, fs::copy_options::overwrite_existing);

    MutatingTransport transport(source);
    hybridftp::client::DataChannelSession session(transport);
    const hybridftp::client::SendResult result =
        session.sendFile(source, TransferMode::Binary);

    assert(result.success);
    assert(result.sha256 == Sha256Hasher::hashFile(expected));
    std::cout << "[PASS] client sender hashes its stable snapshot\n";
}

void testClientReceiveFailurePreservesDestination() {
    const fs::path destination = "client_temp_destination.bin";
    const fs::path expected = "client_temp_destination_expected.bin";
    createTestFile(destination, 211);
    fs::copy_file(destination, expected, fs::copy_options::overwrite_existing);

    FakeRdtTransport transport;
    transport.sentChunks.emplace_back(0, std::vector<char>(1024, 'q'));
    transport.sentFinalFlags.push_back(false);

    hybridftp::client::DataChannelSession session(transport);
    assert(!session.receiveFile(destination, TransferMode::Binary));
    assert(filesAreIdentical(destination, expected));
    std::cout << "[PASS] client failed receive preserves destination\n";
}

void testClientConfirmsOnlyAfterCommit() {
    const fs::path destination = "client_commit_before_ack.bin";
    CommitObservingTransport transport(destination);
    transport.sentChunks.emplace_back(
        0, std::vector<char>{'c', 'o', 'm', 'm', 'i', 't', 't', 'e', 'd'});
    transport.sentFinalFlags.push_back(true);

    hybridftp::client::DataChannelSession session(transport);
    assert(session.receiveFile(destination, TransferMode::Binary));
    assert(transport.confirmationCalled);
    assert(transport.confirmationAccepted);
    assert(transport.committedBeforeConfirmation);
    std::cout << "[PASS] client confirms RETR only after local commit\n";
}

void testClientRejectsFailedFinalCommit() {
    const fs::path destination = "client_locked_destination.bin";
    const fs::path expected = "client_locked_destination_expected.bin";
    createTestFile(destination, 127);
    fs::copy_file(destination, expected, fs::copy_options::overwrite_existing);

    LockingFinalTransport transport(destination);
    transport.sentChunks.emplace_back(
        0, std::vector<char>{'r', 'e', 'p', 'l', 'a', 'c', 'e'});
    transport.sentFinalFlags.push_back(true);

    std::string failureReason;
    hybridftp::client::DataChannelSession session(transport);
    assert(!session.receiveFile(
        destination, TransferMode::Binary, &failureReason));
    assert(transport.confirmationCalled);
    assert(!transport.confirmationAccepted);
    assert(failureReason.find("Could not replace the local destination") !=
           std::string::npos);
    assert(filesAreIdentical(destination, expected));
    std::cout << "[PASS] failed local commit rejects RETR completion\n";
}

void testDestinationPreflightRejectsDirectory() {
    const fs::path directory = "client_download_directory";
    fs::create_directories(directory);
    std::string failureReason;
    assert(!hybridftp::client::ChunkedFileWriter::validateDestination(
        directory, failureReason));
    assert(failureReason.find("is a directory") != std::string::npos);
    std::cout << "[PASS] destination preflight rejects a bare directory\n";
}
}

int main() {
    testClientSenderUsesSnapshotHash();
    testClientReceiveFailurePreservesDestination();
    testClientConfirmsOnlyAfterCommit();
    testClientRejectsFailedFinalCommit();
    testDestinationPreflightRejectsDirectory();

    for (const auto& name : {
             "client_temp_source.bin", "client_temp_expected.bin",
             "client_temp_destination.bin",
             "client_temp_destination_expected.bin",
             "client_commit_before_ack.bin",
             "client_locked_destination.bin",
             "client_locked_destination_expected.bin"}) {
        std::error_code ignored;
        fs::remove(name, ignored);
    }
    std::error_code ignored;
    fs::remove_all("client_download_directory", ignored);
    return 0;
}
