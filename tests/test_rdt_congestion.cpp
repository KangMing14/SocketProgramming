#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "RdtSender.h"
#include "RdtHeader.h"
#include "CheckSum.h"
#include "../common/ProtocolConstants.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string socketError(const std::string& operation) {
    return operation + " failed with Winsock error " +
           std::to_string(WSAGetLastError());
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

class SenderTask {
public:
    template <typename Function>
    explicit SenderTask(Function&& function) {
        std::packaged_task<bool()> task(std::forward<Function>(function));
        result_ = task.get_future();
        thread_ = std::thread(std::move(task));
    }

    ~SenderTask() {
        join();
    }

    bool get() {
        try {
            const bool result = result_.get();
            join();
            return result;
        } catch (...) {
            join();
            throw;
        }
    }

    SenderTask(const SenderTask&) = delete;
    SenderTask& operator=(const SenderTask&) = delete;

private:
    void join() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::future<bool> result_;
    std::thread thread_;
};

}  // namespace

class RdtTestAccess {
public:
    static void updateRtt(RdtSender& sender, double sampleRttMs) {
        sender.updateRtt(sampleRttMs);
    }

    static void applyCongestionDecrease(RdtSender& sender) {
        sender.applyCongestionDecrease();
    }

    static void setInjectedSendTo(
        RdtSender& sender,
        std::function<int(
            SOCKET,
            const char*,
            int,
            int,
            const sockaddr*,
            int)> function) {
        sender.injectedSendTo = std::move(function);
    }

    static void setInjectedPreSendDelay(
        RdtSender& sender,
        std::function<void()> function) {
        sender.injectedPreSendDelay = std::move(function);
    }

    static void setEstimatedRtt(RdtSender& sender, double value) {
        sender.estimatedRttMs = value;
    }

    static void setDevRtt(RdtSender& sender, double value) {
        sender.devRttMs = value;
    }
};

namespace {

class MockReceiver {
public:
    MockReceiver() {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error(socketError("socket"));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(0);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(
                socket_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
            const std::string message = socketError("bind");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            throw std::runtime_error(message);
        }

        int addressLength = sizeof(address);
        if (getsockname(
                socket_,
                reinterpret_cast<sockaddr*>(&address),
                &addressLength) == SOCKET_ERROR) {
            const std::string message = socketError("getsockname");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            throw std::runtime_error(message);
        }

        port_ = ntohs(address.sin_port);

        const DWORD timeoutMs = 3000;
        if (setsockopt(
                socket_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs)) == SOCKET_ERROR) {
            const std::string message = socketError("setsockopt");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            throw std::runtime_error(message);
        }
    }

    ~MockReceiver() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }

    MockReceiver(const MockReceiver&) = delete;
    MockReceiver& operator=(const MockReceiver&) = delete;

    uint16_t port() const noexcept {
        return port_;
    }

    RdtHeader receivePacket() {
        std::array<char, HEADER_SIZE + MAX_PAYLOAD> buffer{};
        senderAddressLength_ = sizeof(senderAddress_);

        const int received = recvfrom(
            socket_,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&senderAddress_),
            &senderAddressLength_);

        require(received != SOCKET_ERROR, socketError("recvfrom"));
        require(received >= static_cast<int>(HEADER_SIZE),
                "Received an RDT datagram shorter than its header");

        std::vector<char> checksumBuffer(
            buffer.begin(), buffer.begin() + received);
        checksumBuffer[13] = 0;
        checksumBuffer[14] = 0;

        uint16_t receivedChecksum = 0;
        std::memcpy(&receivedChecksum, buffer.data() + 13, sizeof(receivedChecksum));
        receivedChecksum = ntohs(receivedChecksum);

        const uint16_t computedChecksum = internetChecksum(
            reinterpret_cast<const uint8_t*>(checksumBuffer.data()),
            checksumBuffer.size());

        require(computedChecksum == receivedChecksum,
                "Received a packet with an invalid checksum");

        return deserializeHeader(buffer.data());
    }

    void sendAck(uint32_t acknowledgementNumber) {
        require(senderAddressLength_ > 0,
                "Cannot send an ACK before receiving a packet");

        RdtHeader acknowledgement{};
        acknowledgement.ack_num = acknowledgementNumber;
        acknowledgement.flags = FLAG_ACK;
        acknowledgement.window_size = 10;

        std::array<char, HEADER_SIZE> buffer{};
        serializeHeader(acknowledgement, buffer.data());
        acknowledgement.checksum = internetChecksum(
            reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
        serializeHeader(acknowledgement, buffer.data());

        const int sent = sendto(
            socket_,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<const sockaddr*>(&senderAddress_),
            senderAddressLength_);

        require(sent == static_cast<int>(buffer.size()), socketError("sendto"));
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    sockaddr_in senderAddress_{};
    int senderAddressLength_ = 0;
};

void testAdditiveIncrease() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    require(sender.getCongestionWindow() == 4.0,
            "Initial cwnd should be 4.0");

    SenderTask task([&]() {
        for (uint32_t sequence = 0; sequence < 4; ++sequence) {
            if (!sender.sendChunk(sequence, "a", 1)) {
                return false;
            }
        }
        return sender.flush();
    });

    for (int i = 0; i < 4; ++i) {
        const RdtHeader header = receiver.receivePacket();
        receiver.sendAck(header.seq_num);
    }

    require(task.get(), "Sender failed during additive-increase test");
    require(sender.getCongestionWindow() == 5.0,
            "Four clean ACKs should increase cwnd from 4.0 to 5.0");
    require(sender.getCleanAckCount() == 0,
            "cleanAcks should reset after cwnd grows");

    std::cout << "[PASS] additive increase\n";
}

void testDuplicateAckProtection() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 500);

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) &&
               sender.sendChunk(1, "b", 1) &&
               sender.flush();
    });

    const RdtHeader first = receiver.receivePacket();
    const RdtHeader second = receiver.receivePacket();
    require(first.seq_num != second.seq_num,
            "The sender should transmit two distinct sequence numbers");

    receiver.sendAck(first.seq_num);
    receiver.sendAck(first.seq_num);
    receiver.sendAck(first.seq_num);
    receiver.sendAck(second.seq_num);

    require(task.get(), "Sender failed during duplicate-ACK test");
    require(sender.getCleanAckCount() == 2,
            "Duplicate ACKs must not increment cleanAcks");
    require(sender.getCongestionWindow() == 4.0,
            "Duplicate ACKs must not grow cwnd");

    std::cout << "[PASS] duplicate ACK protection\n";
}

void testMultiplicativeDecrease() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) && sender.flush();
    });

    receiver.receivePacket();
    const RdtHeader retransmission = receiver.receivePacket();
    receiver.sendAck(retransmission.seq_num);

    require(task.get(), "Sender failed after one controlled timeout");
    require(sender.getCongestionWindow() == 2.0,
            "One timeout should reduce cwnd from 4.0 to 2.0");

    std::cout << "[PASS] multiplicative decrease\n";
}

void testConcurrentTimeoutsCauseOneDecrease() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) &&
               sender.sendChunk(1, "b", 1) &&
               sender.sendChunk(2, "c", 1) &&
               sender.flush();
    });

    std::set<uint32_t> initialSequences;
    for (int i = 0; i < 3; ++i) {
        initialSequences.insert(receiver.receivePacket().seq_num);
    }
    require(initialSequences.size() == 3,
            "Expected three distinct initial packets");

    std::set<uint32_t> retransmittedSequences;
    for (int i = 0; i < 3; ++i) {
        const RdtHeader retransmission = receiver.receivePacket();
        retransmittedSequences.insert(retransmission.seq_num);
        receiver.sendAck(retransmission.seq_num);
    }

    require(task.get(), "Sender failed during concurrent-timeout test");
    require(retransmittedSequences == initialSequences,
            "Every expired packet should be retransmitted exactly once");
    require(sender.getCongestionWindow() == 2.0,
            "One polling cycle must halve cwnd only once");

    std::cout << "[PASS] concurrent timeouts cause one decrease\n";
}

void testMinimumCongestionWindow() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) && sender.flush();
    });

    receiver.receivePacket();
    receiver.receivePacket();
    receiver.receivePacket();

    const RdtHeader finalRetransmission = receiver.receivePacket();
    receiver.sendAck(finalRetransmission.seq_num);

    require(task.get(), "Sender failed while testing minimum cwnd");
    require(sender.getCongestionWindow() == 1.0,
            "cwnd must never fall below 1.0");

    std::cout << "[PASS] minimum congestion window\n";
}

void testJacobsonKarelsEstimator() {
    RdtSender sender("127.0.0.1", 9, 500);
    RdtTestAccess::setEstimatedRtt(sender, 500.0);
    RdtTestAccess::setDevRtt(sender, 0.0);

    RdtTestAccess::updateRtt(sender, 100.0);

    require(std::abs(sender.getDevRttMs() - 100.0) < 0.001,
            "DevRTT should use the previous EstimatedRTT");
    require(std::abs(sender.getEstimatedRttMs() - 450.0) < 0.001,
            "EstimatedRTT does not match the Jacobson/Karels formula");
    require(sender.getTimeoutMs() == 850,
            "RTO should equal ceil(EstimatedRTT + 4 * DevRTT)");

    std::cout << "[PASS] Jacobson/Karels estimator\n";
}

void testRtoLowerBound() {
    RdtSender sender("127.0.0.1", 9, 500);
    for (int i = 0; i < 100; ++i) {
        RdtTestAccess::updateRtt(sender, 1.0);
    }

    require(sender.getTimeoutMs() == 50,
            "RTO should clamp to the 50 ms lower bound");
    std::cout << "[PASS] RTO lower bound\n";
}

void testRtoUpperBound() {
    RdtSender sender("127.0.0.1", 9, 500);
    RdtTestAccess::updateRtt(sender, 10000.0);

    require(sender.getTimeoutMs() == 2000,
            "RTO should clamp to the 2000 ms upper bound");
    std::cout << "[PASS] RTO upper bound\n";
}

void testExponentialBackoff() {
    RdtSender sender("127.0.0.1", 9, 50);
    const std::vector<int> expectedTimeouts{
        100, 200, 400, 800, 1600, 2000, 2000};

    require(sender.getTimeoutMs() == 50, "Initial RTO should be 50 ms");
    for (const int expectedTimeout : expectedTimeouts) {
        RdtTestAccess::applyCongestionDecrease(sender);
        require(
            sender.getTimeoutMs() == expectedTimeout,
            "Unexpected backoff value: expected " +
                std::to_string(expectedTimeout) + " ms, got " +
                std::to_string(sender.getTimeoutMs()) + " ms");
    }

    std::cout << "[PASS] exponential backoff\n";
}

void testKarnsAlgorithm() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    const double initialEstimatedRtt = sender.getEstimatedRttMs();
    const double initialDevRtt = sender.getDevRttMs();

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) && sender.flush();
    });

    receiver.receivePacket();
    const RdtHeader retransmission = receiver.receivePacket();
    receiver.sendAck(retransmission.seq_num);

    require(task.get(), "Sender failed during Karn test");

    // Read sender state only after the sender thread has joined. This avoids
    // the data race that existed in the previous version of this test.
    require(sender.getEstimatedRttMs() == initialEstimatedRtt,
            "A retransmitted packet must not update EstimatedRTT");
    require(sender.getDevRttMs() == initialDevRtt,
            "A retransmitted packet must not update DevRTT");
    require(sender.getTimeoutMs() == 100,
            "The retransmitted ACK must preserve the backed-off RTO");

    std::cout << "[PASS] Karn's algorithm\n";
}

void testRetransmissionFailurePropagation() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 50);

    int sendCount = 0;
    RdtTestAccess::setInjectedSendTo(
        sender,
        [&](SOCKET socketHandle,
            const char* buffer,
            int length,
            int flags,
            const sockaddr* destination,
            int destinationLength) {
            ++sendCount;
            if (sendCount == 2) {
                WSASetLastError(WSAECONNRESET);
                return SOCKET_ERROR;
            }
            return sendto(
                socketHandle,
                buffer,
                length,
                flags,
                destination,
                destinationLength);
        });

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) && sender.flush();
    });

    receiver.receivePacket();

    require(!task.get(),
            "A retransmission send failure must propagate to the caller");
    require(sendCount == 2,
            "The sender should stop immediately after the failed retransmission");

    std::cout << "[PASS] retransmission failure propagation\n";
}

void testInitialTimestampExcludesPreSendDelay() {
    MockReceiver receiver;
    RdtSender sender("127.0.0.1", receiver.port(), 500);

    RdtTestAccess::setInjectedPreSendDelay(sender, []() {
        std::this_thread::sleep_for(250ms);
    });

    SenderTask task([&]() {
        return sender.sendChunk(0, "a", 1) && sender.flush();
    });

    const RdtHeader packet = receiver.receivePacket();
    receiver.sendAck(packet.seq_num);

    require(task.get(), "Sender failed during timestamp test");

    const double inferredSampleRtt =
        (sender.getEstimatedRttMs() - 0.875 * 500.0) / 0.125;

    require(inferredSampleRtt >= 0.0,
            "Inferred SampleRTT must not be negative");
    require(inferredSampleRtt < 175.0,
            "SampleRTT appears to include the injected 250 ms pre-send delay");

    std::cout << "[PASS] initial timestamp excludes pre-send delay\n";
}

}  // namespace

int main() {
    try {
        WinsockSession winsock;

        std::cout << "--- RDT Congestion Tests ---\n\n";

        testAdditiveIncrease();
        testDuplicateAckProtection();
        testMultiplicativeDecrease();
        testConcurrentTimeoutsCauseOneDecrease();
        testMinimumCongestionWindow();
        testJacobsonKarelsEstimator();
        testRtoLowerBound();
        testRtoUpperBound();
        testExponentialBackoff();
        testKarnsAlgorithm();
        testRetransmissionFailurePropagation();
        testInitialTimestampExcludesPreSendDelay();

        std::cout << "\nAll RDT congestion tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
