#include "DataChannelSession.h"
#include "Sha256Hasher.h"
#include "helper.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <utility>

namespace fs = std::filesystem;

namespace {
std::set<fs::path> hybridTempFiles(const fs::path& directory,
                                   const std::string& prefix) {
    std::set<fs::path> result;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) == 0) result.insert(it->path());
    }
    return result;
}

class MutatingTransport : public FakeRdtTransport {
public:
    explicit MutatingTransport(fs::path source) : source(std::move(source)) {}

    bool sendChunk(uint32_t sequence, const char* data, size_t length,
                   bool isFinal = false) override {
        if (!mutated) {
            std::ofstream replacement(source,
                                      std::ios::binary | std::ios::trunc);
            replacement << "changed while transfer was active";
            mutated = true;
        }
        return FakeRdtTransport::sendChunk(sequence, data, length, isFinal);
    }

private:
    fs::path source;
    bool mutated = false;
};

void testSenderUsesStableSnapshotAndCleansIt() {
    const fs::path source = "temp_sender_source.bin";
    const fs::path expected = "temp_sender_expected.bin";
    const fs::path captured = "temp_sender_captured.bin";
    createTestFile(source, 4097);
    fs::copy_file(source, expected, fs::copy_options::overwrite_existing);

    std::error_code error;
    const fs::path temporaryDirectory = fs::temp_directory_path(error);
    assert(!error);
    const auto before = hybridTempFiles(temporaryDirectory, "hfs");

    MutatingTransport transport(source);
    DataChannelSession sender(transport);
    const SendResult result = sender.sendFile(source, TransferMode::Binary);
    assert(result.success);
    assert(result.sha256 == Sha256Hasher::hashFile(expected));

    std::ofstream output(captured, std::ios::binary);
    for (const auto& sent : transport.sentChunks) {
        output.write(sent.second.data(),
                     static_cast<std::streamsize>(sent.second.size()));
    }
    output.close();

    assert(filesAreIdentical(expected, captured));
    assert(hybridTempFiles(temporaryDirectory, "hfs") == before);
    std::cout << "[PASS] sender snapshot is stable and cleaned\n";
}

void testReceiveFailurePreservesDestinationAndCleansTemp() {
    const fs::path destination = "temp_existing_destination.bin";
    const fs::path expected = "temp_existing_expected.bin";
    createTestFile(destination, 333);
    fs::copy_file(destination, expected, fs::copy_options::overwrite_existing);

    const auto before = hybridTempFiles(fs::current_path(), "hft");
    FakeRdtTransport transport;
    transport.sentChunks.emplace_back(0, std::vector<char>(1024, 'x'));
    transport.sentFinalFlags.push_back(false);

    DataChannelSession receiver(transport);
    assert(!receiver.receiveFile(destination, TransferMode::Binary));
    assert(filesAreIdentical(destination, expected));
    assert(hybridTempFiles(fs::current_path(), "hft") == before);
    std::cout << "[PASS] failed receive preserves destination and cleans temp\n";
}
}

int main() {
    testSenderUsesStableSnapshotAndCleansIt();
    testReceiveFailurePreservesDestinationAndCleansTemp();

    for (const auto& name : {
             "temp_sender_source.bin", "temp_sender_expected.bin",
             "temp_sender_captured.bin", "temp_existing_destination.bin",
             "temp_existing_expected.bin"}) {
        std::error_code ignored;
        fs::remove(name, ignored);
    }
    return 0;
}
