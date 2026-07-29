// tests/test_rdt_integration.cpp
// Step 5.3 from tmp.md: real sender<->receiver integration test over loopback UDP.
// Uses a background thread to simulate the separate sender and receiver processes.
#include <iostream>
#include <thread>
#include <cassert>
#include <string>
#include <winsock2.h>
#include "RdtSender.h"
#include "RdtReceiver.h"

// -------------------------------------------------------------------
// TEST 1: Basic loopback — send "hello", receive "hello"
// -------------------------------------------------------------------
void test_sender_receiver_over_loopback() {
    RdtReceiver receiver((uint16_t)9999);

    std::thread receiverThread([&]() {
        uint32_t seq;
        std::vector<char> data;
        bool isFinal;
        assert(receiver.receiveNext(seq, data, isFinal));
        assert(seq == 0);
        std::string received(data.begin(), data.end());
        assert(received == "hello");
        std::cout << "  [Thread] Received: \"" << received
                  << "\" (seq=" << seq << ")\n";
    });

    RdtSender sender("127.0.0.1", 9999);
    assert(sender.sendChunk(0, "hello", 5) == true);
    receiverThread.join();

    std::cout << "[PASS] test_sender_receiver_over_loopback\n";
}

// -------------------------------------------------------------------
// TEST 2: Multi-chunk round-trip — send 3 sequential chunks
// -------------------------------------------------------------------
void test_multi_chunk_round_trip() {
    RdtReceiver receiver((uint16_t)9997);

    std::vector<std::string> expected = {"chunk0", "chunk1", "chunk2"};
    std::vector<std::string> received_chunks;

    std::thread receiverThread([&]() {
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t seq;
            std::vector<char> data;
            bool isFinal;
            assert(receiver.receiveNext(seq, data, isFinal));
            received_chunks.push_back(std::string(data.begin(), data.end()));
        }
    });

    RdtSender sender("127.0.0.1", 9997);
    for (uint32_t i = 0; i < 3; ++i) {
        assert(sender.sendChunk(i, expected[i].data(), expected[i].size()) == true);
    }
    receiverThread.join();

    assert(received_chunks == expected);
    std::cout << "[PASS] test_multi_chunk_round_trip\n";
}

// -------------------------------------------------------------------
// TEST 3: Large payload — 1024 bytes (MAX_PAYLOAD boundary)
// -------------------------------------------------------------------
void test_max_payload_boundary() {
    RdtReceiver receiver((uint16_t)9996);

    std::thread receiverThread([&]() {
        uint32_t seq;
        std::vector<char> data;
        bool isFinal;
        assert(receiver.receiveNext(seq, data, isFinal));
        assert(data.size() == 1024);
        // Verify data is intact
        for (char c : data) assert(c == 'Z');
    });

    std::vector<char> payload(1024, 'Z');
    RdtSender sender("127.0.0.1", 9996);
    assert(sender.sendChunk(7, payload.data(), payload.size()) == true);
    receiverThread.join();

    std::cout << "[PASS] test_max_payload_boundary\n";
}

// -------------------------------------------------------------------
// MAIN
// -------------------------------------------------------------------
int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    std::cout << "--- RDT Integration Tests (real loopback UDP) ---\n\n";

    test_sender_receiver_over_loopback();
    test_multi_chunk_round_trip();
    test_max_payload_boundary();

    std::cout << "\nAll integration tests PASSED!\n";

    WSACleanup();
    return 0;
}
