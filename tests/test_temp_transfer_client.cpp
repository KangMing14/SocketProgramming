#include "../client/src/crypto/Sha256Hasher.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "helper.h"

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

void testClientSenderUsesSnapshotHash() {
    const fs::path source = "client_temp_source.bin";
    const fs::path expected = "client_temp_expected.bin";
    createTestFile(source, 3073);
    fs::copy_file(source, expected, fs::copy_options::overwrite_existing);

    MutatingTransport transport(source);
    hybridftp::client::DataChannelSession session(transport);
    const hybridftp::client::SendResult result =
        session.sendFile(source, TransferType::Binary);

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
    assert(!session.receiveFile(destination, TransferType::Binary));
    assert(filesAreIdentical(destination, expected));
    std::cout << "[PASS] client failed receive preserves destination\n";
}
}

int main() {
    testClientSenderUsesSnapshotHash();
    testClientReceiveFailurePreservesDestination();

    for (const auto& name : {
             "client_temp_source.bin", "client_temp_expected.bin",
             "client_temp_destination.bin",
             "client_temp_destination_expected.bin"}) {
        std::error_code ignored;
        fs::remove(name, ignored);
    }
    return 0;
}
