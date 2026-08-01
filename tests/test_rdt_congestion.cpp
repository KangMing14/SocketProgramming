#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "RdtSender.h"
#include "RdtHeader.h"
#include "CheckSum.h"
#include "../common/ProtocolConstants.h"

#pragma comment(lib, "ws2_32.lib")

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

    assert(sender.getCongestionWindow() == 4.0);
    
    std::thread senderThread([&]() {
        for (int i = 0; i < 4; ++i) {
            sender.sendChunk(i, "a", 1);
        }
        sender.flush();
    });

    for (int i = 0; i < 4; ++i) {
        RdtHeader hdr = receiver.recvPacket();
        receiver.sendAck(hdr.seq_num);
    }

    senderThread.join();
    assert(sender.getCongestionWindow() == 5.0);
    assert(sender.getCleanAckCount() == 0);
    std::cout << "[PASS] test_additive_increase\n";
}

void test_duplicate_ack_protection() {
    MockReceiver receiver(9002);
    RdtSender sender("127.0.0.1", 9002, 50);

    assert(sender.getCongestionWindow() == 4.0);

    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.flush();
    });

    RdtHeader hdr = receiver.recvPacket();
    receiver.sendAck(hdr.seq_num);
    
    // Duplicate ACK
    receiver.sendAck(hdr.seq_num);
    receiver.sendAck(hdr.seq_num);

    senderThread.join();
    assert(sender.getCleanAckCount() == 1);
    assert(sender.getCongestionWindow() == 4.0);
    std::cout << "[PASS] test_duplicate_ack_protection\n";
}

void test_multiplicative_decrease() {
    MockReceiver receiver(9003);
    RdtSender sender("127.0.0.1", 9003, 50);

    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.flush();
    });

    // Receive first packet, ignore it to cause timeout
    receiver.recvPacket();
    
    // Receive retransmission
    RdtHeader hdr2 = receiver.recvPacket();
    if (hdr2.seq_num != 0xFFFFFFFF) {
        receiver.sendAck(hdr2.seq_num);
    }
    
    senderThread.join();

    if (sender.getCongestionWindow() != 2.0) {
        std::cerr << "Expected cwnd 2.0, got " << sender.getCongestionWindow() << "\n";
    }
    assert(sender.getCongestionWindow() == 2.0);
    std::cout << "[PASS] test_multiplicative_decrease\n";
}

void test_concurrent_timeouts() {
    MockReceiver receiver(9004);
    RdtSender sender("127.0.0.1", 9004, 50);

    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.sendChunk(1, "b", 1);
        sender.sendChunk(2, "c", 1);
        sender.flush();
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

    senderThread.join();

    if (sender.getCongestionWindow() != 2.0) {
        std::cerr << "Expected cwnd 2.0 (concurrent), got " << sender.getCongestionWindow() << "\n";
    }
    assert(sender.getCongestionWindow() == 2.0);
    std::cout << "[PASS] test_concurrent_timeouts\n";
}

void test_minimum_congestion_window() {
    MockReceiver receiver(9005);
    RdtSender sender("127.0.0.1", 9005, 50);

    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.flush();
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

    senderThread.join();

    assert(sender.getCongestionWindow() == 1.0);
    std::cout << "[PASS] test_minimum_congestion_window\n";
}

void test_karns_algorithm() {
    MockReceiver receiver(9006);
    RdtSender sender("127.0.0.1", 9006, 50);

    double initialEst = sender.getEstimatedRttMs();
    
    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.flush();
    });

    receiver.recvPacket(); // Ignore first
    RdtHeader r1_retry = receiver.recvPacket(); // Receive retry
    
    if (r1_retry.seq_num != 0xFFFFFFFF) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        receiver.sendAck(r1_retry.seq_num);
    }

    senderThread.join();

    assert(sender.getEstimatedRttMs() == initialEst);
    std::cout << "[PASS] test_karns_algorithm\n";
}

void test_rto_bounds() {
    MockReceiver receiver(9007);
    RdtSender sender("127.0.0.1", 9007, 50);

    std::thread senderThread([&]() {
        sender.sendChunk(0, "a", 1);
        sender.flush();
    });

    receiver.recvPacket(); // Ignore
    receiver.recvPacket(); // Retry 1 (RTO: 50 -> 100)
    receiver.recvPacket(); // Retry 2 (RTO: 100 -> 200)
    receiver.recvPacket(); // Retry 3 (RTO: 200 -> 400)
    receiver.recvPacket(); // Retry 4 (RTO: 400 -> 800)
    receiver.recvPacket(); // Retry 5 (RTO: 800 -> 1600)
    receiver.recvPacket(); // Retry 6 (RTO: 1600 -> 2000)
    RdtHeader r7 = receiver.recvPacket(); // Retry 7 (RTO: 2000 -> 2000)

    if (r7.seq_num != 0xFFFFFFFF) receiver.sendAck(r7.seq_num);

    senderThread.join();

    assert(sender.getTimeoutMs() == 2000); // Max bound
    std::cout << "[PASS] test_rto_bounds and test_exponential_backoff\n";
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    std::cout << "--- RDT Congestion Tests ---\n\n";

    test_additive_increase();
    test_duplicate_ack_protection();
    test_multiplicative_decrease();
    test_concurrent_timeouts();
    test_minimum_congestion_window();
    test_karns_algorithm();
    test_rto_bounds();

    std::cout << "\nAll congestion tests PASSED!\n";

    WSACleanup();
    return 0;
}
