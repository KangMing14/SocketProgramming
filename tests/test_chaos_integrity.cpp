#include "../server/src/filesystem/ChunkedFileReader.h"
#include "../server/src/filesystem/ChunkedFileWriter.h"
#include "../server/src/crypto/Sha256Hasher.h"
#include "../server/src/rdt/RdtSender.h"
#include "../server/src/rdt/RdtReceiver.h"
#include "../server/src/datachannel/DataChannelSession.h"
#include <cassert>
#include <iostream>
#include <random>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;

// Creates a multi-chunk binary file with pseudo-random content, large enough
// to exercise many packets (not just one), which matters here specifically:
// a single-packet file could pass by luck even with a real bug, since there's
// nothing to reorder or lose across multiple packets.
void createChaosTestFile(const fs::path& path, size_t sizeBytes) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < sizeBytes; i++) {
        char byte = static_cast<char>(dist(rng));
        out.write(&byte, 1);
    }
}

bool runOneChaosTransfer(const fs::path& sourceFile, const fs::path& destFile,
    uint16_t port) {
    RdtReceiver receiver(port);
    bool receiveOk = false;

    std::thread receiverThread([&]() {
        DataChannelSession channel(receiver);
        receiveOk = channel.receiveFile(destFile, TransferMode::Binary);
        });

    // Give the receiver a moment to bind and start listening before the
    // sender's first packet goes out.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RdtSender sender("127.0.0.1", port);
    DataChannelSession sendChannel(sender);
    bool sendOk = sendChannel.sendFile(sourceFile, TransferMode::Binary);

    receiverThread.join();
    return sendOk && receiveOk;
}

void test_file_survives_chaos_across_many_trials() {
    const int TRIALS = 7;
    const size_t FILE_SIZE = 7 * 1024; // ~20 chunks -- enough to exercise real reordering/retry paths
    fs::path source = "chaos_source.bin";
    createChaosTestFile(source, FILE_SIZE);
    std::string expectedHash = Sha256Hasher::hashFile(source);

    int passCount = 0;
    for (int trial = 0; trial < TRIALS; trial++) {
        fs::path dest = "chaos_result_" + std::to_string(trial) + ".bin";
        uint16_t port = static_cast<uint16_t>(20000 + trial); // distinct port per trial

        bool transferOk = runOneChaosTransfer(source, dest, port);
        std::string actualHash = transferOk ? Sha256Hasher::hashFile(dest) : "";

        bool trialPassed = transferOk && (actualHash == expectedHash);
        std::cout << "  Trial " << (trial + 1) << "/" << TRIALS
            << ": " << (trialPassed ? "PASS" : "FAIL") << "\n";

        // FAIL HARD immediately -- do not average this away. A single
        // mismatch under chaos is a real correctness bug, not noise.
        assert(trialPassed && "File corrupted under chaos conditions.");

        if (trialPassed) passCount++;
        fs::remove(dest);
    }

    fs::remove(source);
    std::cout << "[PASS] " << passCount << "/" << TRIALS
        << " chaos trials produced byte-identical, hash-verified transfers\n";
}

int main() {
    WSADATA wsaData;
    int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return 1;
    }

    test_file_survives_chaos_across_many_trials();
}