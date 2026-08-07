#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>

#include "../server/src/crypto/Sha256Hasher.h"
#include "../server/src/datachannel/DataChannelSession.h"
#include "../server/src/filesystem/ChunkedFileReader.h"
#include "../server/src/filesystem/ChunkedFileWriter.h"
#include "../server/src/rdt/RdtReceiver.h"
#include "../server/src/rdt/RdtSender.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error(
                "WSAStartup failed with error " + std::to_string(result));
        }
    }

    ~WinsockSession() {
        WSACleanup();
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto uniqueValue =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("hybrid_ftp_chaos_" + std::to_string(uniqueValue));

        std::error_code error;
        const bool created = fs::create_directories(path_, error);
        require(created && !error,
                "Could not create temporary test directory: " +
                    error.message());
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const noexcept {
        return path_;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

private:
    fs::path path_;
};

uint16_t findAvailableUdpPort() {
    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socketHandle != INVALID_SOCKET,
            "Could not create a socket for ephemeral-port discovery");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        closesocket(socketHandle);
        throw std::runtime_error(
            "Could not bind an ephemeral UDP socket: " +
            std::to_string(error));
    }

    int addressLength = sizeof(address);
    if (getsockname(
            socketHandle,
            reinterpret_cast<sockaddr*>(&address),
            &addressLength) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        closesocket(socketHandle);
        throw std::runtime_error(
            "Could not query the ephemeral UDP port: " +
            std::to_string(error));
    }

    const uint16_t port = ntohs(address.sin_port);
    closesocket(socketHandle);
    return port;
}

void createChaosTestFile(const fs::path& path, std::size_t sizeBytes) {
    std::ofstream output(path, std::ios::binary);
    require(output.is_open(), "Could not create chaos source file");

    std::mt19937 randomEngine(12345);
    std::uniform_int_distribution<int> byteDistribution(0, 255);

    for (std::size_t i = 0; i < sizeBytes; ++i) {
        const char byte = static_cast<char>(byteDistribution(randomEngine));
        output.write(&byte, 1);
        require(output.good(), "Failed while writing chaos source file");
    }
}

bool receiveKnownNumberOfChunks(
    RdtReceiver& receiver,
    const fs::path& destination,
    uint32_t expectedChunkCount) {
    ChunkedFileWriter writer(destination);
    if (!writer.isValid()) {
        return false;
    }

    for (uint32_t i = 0; i < expectedChunkCount; ++i) {
        uint32_t sequenceNumber = 0;
        std::vector<char> data;
        bool isFinal = false;

        if (!receiver.receiveNext(sequenceNumber, data, isFinal)) {
            return false;
        }

        if (!writer.appendChunk(sequenceNumber, data)) return false;
    }

    return writer.commit();
}

bool runOneChaosTransfer(
    const fs::path& source,
    const fs::path& destination,
    uint16_t port,
    uint32_t expectedChunkCount) {
    RdtReceiver receiver(port);

    std::packaged_task<bool()> receiveTask([&]() {
        return receiveKnownNumberOfChunks(
            receiver, destination, expectedChunkCount);
    });
    std::future<bool> receiveResult = receiveTask.get_future();
    std::thread receiverThread(std::move(receiveTask));

    try {
        std::this_thread::sleep_for(50ms);

        RdtSender sender("127.0.0.1", port);
        DataChannelSession channel(sender);
        const bool sendSucceeded =
            channel.sendFile(source, TransferType::Binary);

        const bool receiveSucceeded = receiveResult.get();
        receiverThread.join();
        return sendSucceeded && receiveSucceeded;
    } catch (...) {
        if (receiverThread.joinable()) {
            receiverThread.join();
        }
        throw;
    }
}

void testFileSurvivesChaosAcrossManyTrials() {
    constexpr int trialCount = 7;
    constexpr std::size_t fileSize = 7 * 1024;
    constexpr uint32_t expectedChunkCount = static_cast<uint32_t>(
        (fileSize + ChunkedFileReader::CHUNK_SIZE - 1) /
        ChunkedFileReader::CHUNK_SIZE);

    TemporaryDirectory temporaryDirectory;
    const fs::path source = temporaryDirectory.path() / "source.bin";
    createChaosTestFile(source, fileSize);

    const std::string expectedHash = Sha256Hasher::hashFile(source);
    require(!expectedHash.empty(), "Could not hash the chaos source file");

    for (int trial = 0; trial < trialCount; ++trial) {
        const fs::path destination = temporaryDirectory.path() /
            ("result_" + std::to_string(trial) + ".bin");
        const uint16_t port = findAvailableUdpPort();

        const bool transferSucceeded = runOneChaosTransfer(
            source, destination, port, expectedChunkCount);
        const std::string actualHash = transferSucceeded
            ? Sha256Hasher::hashFile(destination)
            : std::string{};

        const bool trialPassed =
            transferSucceeded && actualHash == expectedHash;

        std::cout << "  Trial " << (trial + 1) << '/' << trialCount
                  << ": " << (trialPassed ? "PASS" : "FAIL") << '\n';

        require(
            trialPassed,
            "Chaos trial " + std::to_string(trial + 1) +
                " did not produce a byte-identical file");

        std::error_code removeError;
        fs::remove(destination, removeError);
        require(!removeError,
                "Could not remove chaos trial output: " +
                    removeError.message());
    }

    std::cout << "[PASS] " << trialCount << '/' << trialCount
              << " chaos trials produced byte-identical files\n";
}

}  // namespace

int main() {
    try {
        WinsockSession winsock;
        testFileSurvivesChaosAcrossManyTrials();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
