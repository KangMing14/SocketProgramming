#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>

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

template <typename Function>
bool runReceiverAndJoin(std::thread& thread, std::future<bool>& result,
                        Function&& senderAction) {
    try {
        const bool sendSucceeded = senderAction();
        const bool receiveSucceeded = result.get();
        thread.join();
        return sendSucceeded && receiveSucceeded;
    } catch (...) {
        if (thread.joinable()) {
            thread.join();
        }
        throw;
    }
}

void testSenderReceiverOverLoopback() {
    constexpr uint16_t port = 9999;
    RdtReceiver receiver(port);

    std::packaged_task<bool()> receiveTask([&]() {
        uint32_t sequenceNumber = 0;
        std::vector<char> data;
        bool isFinal = false;

        return receiver.receiveNext(sequenceNumber, data, isFinal) &&
               sequenceNumber == 0 &&
               std::string(data.begin(), data.end()) == "hello";
    });
    std::future<bool> receiveResult = receiveTask.get_future();
    std::thread receiverThread(std::move(receiveTask));

    const bool succeeded = runReceiverAndJoin(
        receiverThread,
        receiveResult,
        [&]() {
            RdtSender sender("127.0.0.1", port);
            return sender.sendChunk(0, "hello", 5) && sender.flush();
        });

    require(succeeded, "Basic loopback transfer failed");
    std::cout << "[PASS] basic loopback transfer\n";
}

void testMultiChunkRoundTrip() {
    constexpr uint16_t port = 9997;
    const std::vector<std::string> expected{"chunk0", "chunk1", "chunk2"};
    RdtReceiver receiver(port);

    std::packaged_task<bool()> receiveTask([&]() {
        std::vector<std::string> received;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            uint32_t sequenceNumber = 0;
            std::vector<char> data;
            bool isFinal = false;

            if (!receiver.receiveNext(sequenceNumber, data, isFinal)) {
                return false;
            }
            if (sequenceNumber != i) {
                return false;
            }
            received.emplace_back(data.begin(), data.end());
        }
        return received == expected;
    });
    std::future<bool> receiveResult = receiveTask.get_future();
    std::thread receiverThread(std::move(receiveTask));

    const bool succeeded = runReceiverAndJoin(
        receiverThread,
        receiveResult,
        [&]() {
            RdtSender sender("127.0.0.1", port);
            for (uint32_t sequence = 0; sequence < expected.size(); ++sequence) {
                if (!sender.sendChunk(
                        sequence,
                        expected[sequence].data(),
                        expected[sequence].size())) {
                    return false;
                }
            }
            return sender.flush();
        });

    require(succeeded, "Multi-chunk loopback transfer failed");
    std::cout << "[PASS] multi-chunk round trip\n";
}

void testMaxPayloadBoundary() {
    constexpr uint16_t port = 9996;
    std::vector<char> payload(1024, 'Z');
    RdtReceiver receiver(port);

    std::packaged_task<bool()> receiveTask([&]() {
        uint32_t sequenceNumber = 0;
        std::vector<char> data;
        bool isFinal = false;

        return receiver.receiveNext(sequenceNumber, data, isFinal) &&
               sequenceNumber == 0 && data == payload;
    });
    std::future<bool> receiveResult = receiveTask.get_future();
    std::thread receiverThread(std::move(receiveTask));

    const bool succeeded = runReceiverAndJoin(
        receiverThread,
        receiveResult,
        [&]() {
            RdtSender sender("127.0.0.1", port);
            return sender.sendChunk(0, payload.data(), payload.size()) &&
                   sender.flush();
        });

    require(succeeded, "MAX_PAYLOAD round trip failed");
    std::cout << "[PASS] MAX_PAYLOAD boundary\n";
}

void testRetransmissionAbort() {
    constexpr uint16_t unusedPort = 9995;
    RdtSender sender("127.0.0.1", unusedPort, 50);

    bool succeeded = sender.sendChunk(0, "abort", 5);
    if (succeeded) {
        succeeded = sender.flush();
    }

    require(!succeeded,
            "Sender should abort after MAX_RETRIES without a receiver");
    std::cout << "[PASS] retransmission abort\n";
}

}  // namespace

int main() {
    try {
        WinsockSession winsock;

        std::cout << "--- RDT Integration Tests ---\n\n";
        testSenderReceiverOverLoopback();
        testMultiChunkRoundTrip();
        testMaxPayloadBoundary();
        testRetransmissionAbort();
        std::cout << "\nAll RDT integration tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
