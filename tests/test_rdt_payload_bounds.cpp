#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>

#include "ChunkedFileReader.h"
#include "ProtocolConstants.h"
#include "RdtReceiver.h"
#include "RdtSender.h"

namespace {

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
};

void testFullSizeChunkSurvivesRoundTrip() {
    constexpr uint16_t port = 9998;
    std::vector<char> input(ChunkedFileReader::CHUNK_SIZE, 'X');

    RdtReceiver receiver(port);
    std::packaged_task<bool()> receiveTask([&]() {
        uint32_t sequenceNumber = 0;
        std::vector<char> output;
        bool isFinal = false;

        if (!receiver.receiveNext(sequenceNumber, output, isFinal)) {
            return false;
        }

        return sequenceNumber == 0 &&
               output.size() == ChunkedFileReader::CHUNK_SIZE &&
               output == input;
    });

    std::future<bool> receiveResult = receiveTask.get_future();
    std::thread receiverThread(std::move(receiveTask));

    bool sendSucceeded = false;
    try {
        RdtSender sender("127.0.0.1", port);
        sendSucceeded =
            sender.sendChunk(0, input.data(), input.size()) && sender.flush();

        const bool receiveSucceeded = receiveResult.get();
        receiverThread.join();

        require(sendSucceeded, "Sender failed for a MAX_PAYLOAD chunk");
        require(receiveSucceeded,
                "MAX_PAYLOAD data changed during the round trip");
    } catch (...) {
        if (receiverThread.joinable()) {
            receiverThread.join();
        }
        throw;
    }

    std::cout
        << "[PASS] full CHUNK_SIZE payload survives the RDT round trip\n";
}

}  // namespace

int main() {
    try {
        WinsockSession winsock;
        testFullSizeChunkSurvivesRoundTrip();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
