#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <vector>
#include <future>
#include <string>
#include <cmath>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "RdtSender.h"
#include "RdtHeader.h"
#include "CheckSum.h"
#include "../common/ProtocolConstants.h"

#pragma comment(lib, "ws2_32.lib")

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class RdtTestAccess {
public:
    static void updateRtt(RdtSender& sender, double sampleRttMs) {
        sender.updateRtt(sampleRttMs);
    }
    static void applyCongestionDecrease(RdtSender& sender) {
        sender.applyCongestionDecrease();
    }
    static void setInjectedSendTo(RdtSender& sender, std::function<int(SOCKET, const char*, int, int, const sockaddr*, int)> fn) {
        sender.injectedSendTo = fn;
    }
    static void setInjectedPreSendDelay(RdtSender& sender, std::function<void()> fn) {
        sender.injectedPreSendDelay = fn;
    }
    static void setEstimatedRtt(RdtSender& sender, double rtt) {
        sender.estimatedRttMs = rtt;
    }
    static void setDevRtt(RdtSender& sender, double dev) {
        sender.devRttMs = dev;
    }
};

class MockReceiver {
public:
    SOCKET sock;
    sockaddr_in senderAddr{};
    int senderLen = sizeof(senderAddr);

    MockReceiver(uint16_t port) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (sockaddr*)&addr, sizeof(addr));

        DWORD timeout = 2000; // 2 seconds
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    }

    ~MockReceiver() {
        closesocket(sock);
    }

    RdtHeader recvPacket() {
        char buf[HEADER_SIZE + MAX_PAYLOAD];
        senderLen = sizeof(senderAddr);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&senderAddr, &senderLen);
        if (n >= HEADER_SIZE) {
            return deserializeHeader(buf);
        }
        // Return dummy packet on timeout
        RdtHeader dummy{};
        dummy.seq_num = 0xFFFFFFFF;
        return dummy;
    }

    void sendAck(uint32_t ackNum) {
        RdtHeader ackHdr{};
        ackHdr.seq_num = 0;
        ackHdr.ack_num = ackNum;
        ackHdr.flags = FLAG_ACK;
        ackHdr.window_size = 10;
        ackHdr.payload_len = 0;
        ackHdr.reserved = 0;
        ackHdr.checksum = 0;

        char buf[HEADER_SIZE];
        serializeHeader(ackHdr, buf);
        ackHdr.checksum = internetChecksum((const uint8_t*)buf, HEADER_SIZE);
        serializeHeader(ackHdr, buf);

        sendto(sock, buf, HEADER_SIZE, 0, (sockaddr*)&senderAddr, senderLen);
    }
};

void test_additive_increase() {
    MockReceiver receiver(9001);
    RdtSender sender("127.0.0.1", 9001, 50);

    require(sender.getCongestionWindow() == 4.0, "Initial cwnd should be 4.0");
    
    std::promise<bool> p;
    std::thread senderThread([&]() {
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            ok = ok && sender.sendChunk(i, "a", 1);
        }
        p.set_value(ok && sender.flush());
    });

    for (int i = 0; i < 4; ++i) {
        RdtHeader hdr = receiver.recvPacket();
        receiver.sendAck(hdr.seq_num);
    }

    require(p.get_future().get(), "Sender thread failed");
    senderThread.join();
    
    require(sender.getCongestionWindow() == 5.0, "cwnd should be 5.0");
    require(sender.getCleanAckCount() == 0, "cleanAcks should be 0");
    std::cout << "[PASS] test_additive_increase\n";
}

void test_duplicate_ack_protection() {
    MockReceiver receiver(9002);
    RdtSender sender("127.0.0.1", 9002, 500);

    require(sender.getCongestionWindow() == 4.0, "Initial cwnd should be 4.0");

    std::promise<bool> p;
    std::thread senderThread([&]() {
        bool s1 = sender.sendChunk(0, "a", 1);
        bool s2 = sender.sendChunk(1, "b", 1);
        p.set_value(s1 && s2 && sender.flush());
    });

    RdtHeader hdr1 = receiver.recvPacket();
    RdtHeader hdr2 = receiver.recvPacket();
    require(hdr1.seq_num != 0xFFFFFFFF && hdr2.seq_num != 0xFFFFFFFF, "Did not receive both packets");

    receiver.sendAck(hdr1.seq_num);
    
    // Duplicate ACKs
    receiver.sendAck(hdr1.seq_num);
    receiver.sendAck(hdr1.seq_num);

    receiver.sendAck(hdr2.seq_num);

    require(p.get_future().get(), "Sender thread failed");
    senderThread.join();
    
    require(sender.getCleanAckCount() == 2, "cleanAcks should be 2");
    require(sender.getCongestionWindow() == 4.0, "cwnd should remain 4.0");
    std::cout << "[PASS] test_duplicate_ack_protection\n";
}

void test_multiplicative_decrease() {
    MockReceiver receiver(9003);
    RdtSender sender("127.0.0.1", 9003, 50);

    std::promise<bool> p;
    std::thread senderThread([&]() {
        p.set_value(sender.sendChunk(0, "a", 1) && sender.flush());
    });

    // Receive first packet, ignore it to cause timeout
    receiver.recvPacket();
    
    // Receive retransmission
    RdtHeader hdr2 = receiver.recvPacket();
    if (hdr2.seq_num != 0xFFFFFFFF) {
        receiver.sendAck(hdr2.seq_num);
    }
    
    require(p.get_future().get(), "Sender thread failed");
    senderThread.join();

    require(sender.getCongestionWindow() == 2.0, "Expected cwnd 2.0");
    std::cout << "[PASS] test_multiplicative_decrease\n";
}

void test_concurrent_timeouts() {
    MockReceiver receiver(9004);
    RdtSender sender("127.0.0.1", 9004, 50);

    std::promise<bool> p;
    std::thread senderThread([&]() {
        bool ok = sender.sendChunk(0, "a", 1);
        ok = ok && sender.sendChunk(1, "b", 1);
        ok = ok && sender.sendChunk(2, "c", 1);
        p.set_value(ok && sender.flush());
    });

    // Receive 3 initial packets, ignore all to cause concurrent timeouts
    receiver.recvPacket();
    receiver.recvPacket();
    receiver.recvPacket();

    // Receive retransmissions
    for (int i=0; i<10; i++) {
        RdtHeader r = receiver.recvPacket();
        if (r.seq_num != 0xFFFFFFFF) {
            receiver.sendAck(r.seq_num);
        } else {
            break;
        }
    }

    require(p.get_future().get(), "Sender thread failed");
    senderThread.join();

    require(sender.getCongestionWindow() == 2.0, "Expected cwnd 2.0 (concurrent)");
    std::cout << "[PASS] test_concurrent_timeouts\n";
}

void test_minimum_congestion_window() {
    MockReceiver receiver(9005);
    RdtSender sender("127.0.0.1", 9005, 50);

    std::promise<bool> p;
    std::thread senderThread([&]() {
        p.set_value(sender.sendChunk(0, "a", 1) && sender.flush());
    });

    // 1st transmission (ignore) -> triggers timeout, cwnd 4->2
    receiver.recvPacket();
    
    // 2nd transmission (ignore) -> triggers timeout, cwnd 2->1
    receiver.recvPacket();
    
    // 3rd transmission (ignore) -> triggers timeout, cwnd 1->1
    receiver.recvPacket();

    // 4th transmission (ACK it)
    RdtHeader r4 = receiver.recvPacket();
    if (r4.seq_num != 0xFFFFFFFF) {
        receiver.sendAck(r4.seq_num);
    }

    require(p.get_future().get(), "Sender thread failed");
    senderThread.join();

    require(sender.getCongestionWindow() == 1.0, "cwnd should be 1.0");
    std::cout << "[PASS] test_minimum_congestion_window\n";
}

void test_jacobson_karels() {
    RdtSender sender("127.0.0.1", 9000, 500); 
    RdtTestAccess::setEstimatedRtt(sender, 500.0);
    RdtTestAccess::setDevRtt(sender, 0.0);
    
    RdtTestAccess::updateRtt(sender, 100.0);
    
    require(std::abs(sender.getDevRttMs() - 100.0) < 0.001, "DevRTT mismatch");
    require(std::abs(sender.getEstimatedRttMs() - 450.0) < 0.001, "EstimatedRTT mismatch");
    require(sender.getTimeoutMs() == 850, "RTO mismatch");
    std::cout << "[PASS] test_jacobson_karels\n";
}

void test_rto_lower_bound() {
    RdtSender sender("127.0.0.1", 9000, 500);
    for(int i=0; i<100; i++) {
        RdtTestAccess::updateRtt(sender, 1.0);
    }
    require(sender.getTimeoutMs() == 50, "RTO did not clamp to lower bound");
    std::cout << "[PASS] test_rto_lower_bound\n";
}

void test_rto_upper_bound() {
    RdtSender sender("127.0.0.1", 9000, 500);
    RdtTestAccess::updateRtt(sender, 10000.0);
    require(sender.getTimeoutMs() == 2000, "RTO did not clamp to upper bound");
    std::cout << "[PASS] test_rto_upper_bound\n";
}

void test_exponential_backoff() {
    RdtSender sender("127.0.0.1", 9000, 50);
    
    require(sender.getTimeoutMs() == 50, "Initial RTO mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 100, "Backoff 1 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 200, "Backoff 2 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 400, "Backoff 3 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 800, "Backoff 4 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 1600, "Backoff 5 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 2000, "Backoff 6 mismatch");
    RdtTestAccess::applyCongestionDecrease(sender);
    require(sender.getTimeoutMs() == 2000, "Backoff 7 mismatch");
    std::cout << "[PASS] test_exponential_backoff\n";
}

void test_karns_algorithm() {
    MockReceiver receiver(9006);
    RdtSender sender("127.0.0.1", 9006, 50);

    double initialEst = sender.getEstimatedRttMs();
    double initialDev = sender.getDevRttMs();

    std::promise<bool> p;
    std::thread senderThread([&]() {
        p.set_value(sender.sendChunk(0, "a", 1) && sender.flush());
    });

    receiver.recvPacket(); // Ignore first
    RdtHeader r1_retry = receiver.recvPacket(); // Receive retry
    
    int backedOffTimeout = sender.getTimeoutMs();
    require(backedOffTimeout == 100, "Timeout should back off");

    if (r1_retry.seq_num != 0xFFFFFFFF) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        receiver.sendAck(r1_retry.seq_num);
    }

    require(p.get_future().get(), "Sender failed");
    senderThread.join();

    require(sender.getEstimatedRttMs() == initialEst, "EstimatedRTT changed");
    require(sender.getDevRttMs() == initialDev, "DevRTT changed");
    require(sender.getTimeoutMs() == backedOffTimeout, "Timeout changed after retransmitted ACK");

    std::cout << "[PASS] test_karns_algorithm\n";
}

void test_retransmission_failure_propagation() {
    MockReceiver receiver(9007);
    RdtSender sender("127.0.0.1", 9007, 50);

    int sendCount = 0;
    RdtTestAccess::setInjectedSendTo(sender, [&](SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
        sendCount++;
        if (sendCount == 2) {
            WSASetLastError(WSAECONNRESET);
            return -1; // SOCKET_ERROR
        }
        return sendto(s, buf, len, flags, to, tolen);
    });

    std::promise<bool> p;
    std::thread senderThread([&]() {
        p.set_value(sender.sendChunk(0, "a", 1) && sender.flush());
    });

    receiver.recvPacket(); // Receive and ignore first packet to cause timeout
    
    bool success = p.get_future().get();
    senderThread.join();

    require(!success, "Sender should have failed");
    require(sendCount == 2, "Should have attempted exactly 2 sends");
    std::cout << "[PASS] test_retransmission_failure_propagation\n";
}

void test_initial_timestamp() {
    MockReceiver receiver(9008);
    RdtSender sender("127.0.0.1", 9008, 500);

    RdtTestAccess::setInjectedPreSendDelay(sender, [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    std::promise<bool> p;
    std::thread senderThread([&]() {
        p.set_value(sender.sendChunk(0, "a", 1) && sender.flush());
    });

    RdtHeader hdr = receiver.recvPacket();
    receiver.sendAck(hdr.seq_num);

    require(p.get_future().get(), "Sender failed");
    senderThread.join();

    double newEst = sender.getEstimatedRttMs();
    double inferredSample = (newEst - 0.875 * 500.0) / 0.125;
    require(inferredSample < 100.0, "RTT sample included pre-send delay: " + std::to_string(inferredSample));

    std::cout << "[PASS] test_initial_timestamp\n";
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    try {
        std::cout << "--- RDT Congestion Tests ---\n\n";

        test_additive_increase();
        test_duplicate_ack_protection();
        test_multiplicative_decrease();
        test_concurrent_timeouts();
        test_minimum_congestion_window();
        
        test_jacobson_karels();
        test_rto_lower_bound();
        test_rto_upper_bound();
        test_exponential_backoff();
        test_karns_algorithm();
        
        test_retransmission_failure_propagation();
        test_initial_timestamp();

        std::cout << "\nAll congestion tests PASSED!\n";
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n';
        WSACleanup();
        return 1;
    }

    WSACleanup();
    return 0;
}
