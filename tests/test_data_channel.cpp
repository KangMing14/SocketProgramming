#include "DataChannelSession.h"
#include "PassiveModeHandler.h"
#include "ActiveModeHandler.h"

#include <cassert>
#include <iostream>

#include "helper.h"

// ------Session------
void test_send_then_receive_round_trip() {
    createTestFile("dc_source.bin", 2600);

    FakeRdtTransport transport;
    //sockaddr_in dummyAddr{};
    //DataChannelSession sender(INVALID_SOCKET, dummyAddr, transport);
    DataChannelSession sender(transport);

    assert(sender.sendFile("dc_source.bin") == true);
    assert(!transport.sentChunks.empty());

    //DataChannelSession receiver(INVALID_SOCKET, dummyAddr, transport);
    DataChannelSession receiver(transport);
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

void test_format_pasv_reply() {
    uint32_t ip = (192u << 24) | (168u << 16) | (1u << 8) | 10u; // 192.168.1.10
    std::string reply = formatPasvReply(ip, 50000);

    // 50000 = p1*256 + p2  ->  p1 = 195, p2 = 80  (50000 = 195*256 + 80)
    assert(reply == "192,168,1,10,195,80");
    std::cout << "[PASS] test_format_pasv_reply\n";
}

void test_format_pasv_reply_low_port() {
    uint32_t ip = (10u << 24) | (0u << 16) | (0u << 8) | 5u;
    std::string reply = formatPasvReply(ip, 80); // p1=0, p2=80
    assert(reply == "10,0,0,5,0,80");
    std::cout << "[PASS] test_format_pasv_reply_low_port\n";
}

// ------ActiveMode------
void test_parse_port_command_valid() {
    sockaddr_in addr{};
    bool ok = parsePortCommand("192,168,1,5,195,80", addr);
    assert(ok == true);

    char ipBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
    assert(std::string(ipBuf) == "192.168.1.5");
    assert(ntohs(addr.sin_port) == 50000); // 195*256 + 80 = 50000

    std::cout << "[PASS] test_parse_port_command_valid\n";
}

void test_parse_port_command_wrong_field_count() {
    sockaddr_in addr{};
    assert(parsePortCommand("192,168,1,5,195", addr) == false);
    assert(parsePortCommand("192,168,1,5,195,80,1", addr) == false);
    assert(parsePortCommand("192,168,1,5,195,80,", addr) == false);
    assert(parsePortCommand("192,168,1,5,195,80abc", addr) == false);
    std::cout << "[PASS] test_parse_port_command_wrong_field_count\n";
}

void test_parse_port_command_out_of_range() {
    sockaddr_in addr{};
    assert(parsePortCommand("999,168,1,5,195,80", addr) == false);
    assert(parsePortCommand("192,168,1,5,-1,80", addr) == false); 
    std::cout << "[PASS] test_parse_port_command_out_of_range\n";
}

void test_parse_port_command_garbage() {
    sockaddr_in addr{};
    assert(parsePortCommand("not,a,port,command,at,all", addr) == false);
    assert(parsePortCommand("", addr) == false);
    std::cout << "[PASS] test_parse_port_command_garbage\n";
}

void test_open_active_connection_succeeds() {
    // Connect to localhost on some arbitrary port — since UDP connect() does
    // no handshake, this should succeed even with nothing listening there.
    sockaddr_in addr{};
    assert(parsePortCommand("127,0,0,1,4,210", addr) == true); // port 4*256+210=1234

    SOCKET sock;
    bool ok = openActiveDataConnection(addr, sock);
    assert(ok == true);
    assert(sock != INVALID_SOCKET);

    std::cout << "[PASS] test_open_active_connection_succeeds with no listener (no handshake)\n";
    closesocket(sock);
}

int main() {
    std::cout << "DataChannelSession test:\n";
    test_send_then_receive_round_trip();

    WSADATA wsaData;
    int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return 1;
    }

    std::cout << "\nPassiveModeHandler tests:\n";
    test_open_passive_port_succeeds();
    test_two_calls_get_different_ports();
    test_format_pasv_reply();
    test_format_pasv_reply_low_port();

    std::cout << "\nActiveModeHandler tests:\n";
    test_parse_port_command_valid();
    test_parse_port_command_wrong_field_count();
    test_parse_port_command_out_of_range();
    test_parse_port_command_garbage();
    test_open_active_connection_succeeds();

    WSACleanup();

    std::cout << "\n---All Datachannel tests passed---\n";

    for (const auto& name : { "dc_source.bin", "dc_result.bin" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }
}