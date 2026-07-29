#include <vector>
#include <thread>
#include <cassert>
#include <iostream>
#include "RdtSender.h"
#include "RdtReceiver.h"
#include "ChunkedFileReader.h"
#include "ProtocolConstants.h"


void test_full_size_chunk_survives_round_trip() {
    // Deliberately send a FULL CHUNK_SIZE (1024-byte) payload -- the exact
    // scenario that previously overflowed the sender's stack buffers.
    std::vector<char> data(ChunkedFileReader::CHUNK_SIZE, 'X');

    RdtReceiver receiver((uint16_t)9998);
    std::thread t([&]() {
        uint32_t seq; std::vector<char> out; bool isFinal;
        assert(receiver.receiveNext(seq, out, isFinal));
        assert(out.size() == ChunkedFileReader::CHUNK_SIZE);
        });

    RdtSender sender("127.0.0.1", 9998);
    assert(sender.sendChunk(0, data.data(), data.size()) == true);
    t.join();
    std::cout << "[PASS] full CHUNK_SIZE payload survives sender/receiver round trip\n";
}

#include <winsock2.h>

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    test_full_size_chunk_survives_round_trip();

    WSACleanup();
    return 0;
}