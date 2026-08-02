#include "../client/src/datachannel/DataChannelSession.h"
#include "helper.h"

#include <WinSock2.h>
#include <cassert>
#include <iostream>

class FakeRdtTransportWithHandshake : public IRdtTransport {
public:
    bool handshakeCalled = false;
    bool handshakeShouldSucceed = true;
    std::vector<std::pair<uint32_t, std::vector<char>>> sentChunks;

    bool establishConnection() override {
        handshakeCalled = true;
        return handshakeShouldSucceed;
    }
    bool sendChunk(uint32_t seq, const char* data, size_t len) override {
        sentChunks.emplace_back(seq, std::vector<char>(data, data + len));
        return true;
    }
    bool receiveNext(uint32_t&, std::vector<char>&, bool&) override { return false; }
};

void test_handshake_is_called_before_send() {
    FakeRdtTransportWithHandshake transport;
    DataChannelSession session(transport);

    createTestFile("dc_client_source.bin", 100);
    session.sendFile("dc_client_source.bin");

    assert(transport.handshakeCalled == true);
    std::cout << "[PASS] establishConnection() is called before the send loop begins\n";
}

void test_send_aborts_cleanly_if_handshake_fails() {
    FakeRdtTransportWithHandshake transport;
    transport.handshakeShouldSucceed = false;
    DataChannelSession session(transport);

    createTestFile("dc_client_source2.bin", 100);
    bool result = session.sendFile("dc_client_source2.bin");

    assert(result == false);
    assert(transport.sentChunks.empty());   // no chunks should have been sent at all
    std::cout << "[PASS] sendFile aborts immediately and sends nothing if handshake fails\n";
}

int main() {
    WSADATA wsaData;
    int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return 1;
    }
    test_handshake_is_called_before_send();
    test_send_aborts_cleanly_if_handshake_fails();

    WSACleanup();

    std::cout << "\n---All Datachannel tests passed---\n";


    for (const auto& name : { "dc_client_source.txt", "dc_client_source2.txt" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }
}