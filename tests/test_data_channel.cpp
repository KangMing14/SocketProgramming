#include "DataChannelSession.h"
#include "PassiveModeHandler.h"

#include <cassert>
#include <iostream>

#include "helper.h"

// ------Session------
class FakeRdtTransport : public IRdtTransport {
public:
    std::vector<std::pair<uint32_t, std::vector<char>>> sentChunks;

    bool sendChunk(uint32_t seq, const char* data, size_t len) override {
        sentChunks.emplace_back(seq, std::vector<char>(data, data + len));
        return true; // no real network, always "succeeds"
    }

    bool receiveNext(uint32_t& seq, std::vector<char>& data, bool& isFinal) override {
        static size_t i = 0;
        if (i >= sentChunks.size()) return false;
        seq = sentChunks[i].first;
        data = sentChunks[i].second;
        isFinal = (i == sentChunks.size() - 1);
        i++;
        return true;
    }
};

void test_send_then_receive_round_trip() {
    createTestFile("dc_source.bin", 2600); // reusing the fixture helper from earlier

    FakeRdtTransport transport;
    sockaddr_in dummyAddr{};
    DataChannelSession sender(INVALID_SOCKET, dummyAddr, transport);

    assert(sender.sendFile("dc_source.bin") == true);
    assert(!transport.sentChunks.empty());

    DataChannelSession receiver(INVALID_SOCKET, dummyAddr, transport);
    assert(receiver.receiveFile("dc_result.bin") == true);

    assert(filesAreIdentical("dc_source.bin", "dc_result.bin"));
    std::cout << "[PASS] test_send_then_receive_round_trip\n";
}

// ------PassiveMode------
void test_open_passive_port_succeeds() {
    SOCKET sock;
    unsigned short port;
    bool ok = openPassiveDataPort(sock, port);

    assert(ok == true);
    assert(sock != INVALID_SOCKET);
    // A real ephemeral port should be nonzero, and Windows typically hands
    // out ports well above the well-known range (0-1023).
    assert(port > 1023);

    std::cout << "[PASS] test_open_passive_port_succeeds, OS assigned port " << port << "\n";
    closesocket(sock);
}

// OS giving out different ports per call
void test_two_calls_get_different_ports() {
    SOCKET sock1, sock2;
    unsigned short port1, port2;
    assert(openPassiveDataPort(sock1, port1));
    assert(openPassiveDataPort(sock2, port2));

    assert(port1 != port2);
    std::cout << "[PASS] test_two_calls_get_different_ports ("
              << port1 << " vs " << port2 << ")\n";

    closesocket(sock1);
    closesocket(sock2);
}

// 192.168.1.10 packed as a 32-bit value: (192<<24)|(168<<16)|(1<<8)|10
void test_format_pasv_reply() {
    uint32_t ip = (192u << 24) | (168u << 16) | (1u << 8) | 10u;
    std::string reply = formatPasvReply(ip, 50000);

    // 50000 = p1*256 + p2  ->  p1 = 195, p2 = 80  (50000 = 195*256 + 80)
    assert(reply == "192,168,1,10,195,80");
    std::cout << "[PASS] test_format_pasv_reply\n";
}

// Edge case: a port small enough that p1 == 0
void test_format_pasv_reply_low_port() {
    uint32_t ip = (10u << 24) | (0u << 16) | (0u << 8) | 5u;
    std::string reply = formatPasvReply(ip, 80); // p1=0, p2=80
    assert(reply == "10,0,0,5,0,80");
    std::cout << "[PASS] test_format_pasv_reply_low_port\n";
}

int main() {
    std::cout << "DataChannelSession test:\n";
    test_send_then_receive_round_trip();

    std::cout << "\nPassiveModeHandler tests:\n";
    WSADATA wsaData;
    int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return 1;
    }
    test_open_passive_port_succeeds();
    test_two_calls_get_different_ports();
    test_format_pasv_reply();
    test_format_pasv_reply_low_port();
    WSACleanup();

    std::cout << "\n---All Datachannel tests passed---\n";

    for (const auto& name : { "dc_source.bin", "dc_result.bin" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }
}