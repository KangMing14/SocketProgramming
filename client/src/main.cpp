#include "control/Session.h"

#include <charconv>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

bool parsePort(std::string_view text, int& port) {
    int value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        value < 1 || value > 65535) {
        return false;
    }

    port = value;
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: ftp_client [server-ip] [port]\n";
        return 1;
    }

    const std::string serverIp = argc >= 2 ? argv[1] : "127.0.0.1";
    int serverPort = 4567;
    if (argc == 3 && !parsePort(argv[2], serverPort)) {
        std::cerr << "Invalid port. Expected a number from 1 to 65535.\n";
        return 1;
    }

    SOCKET serverSock = Session::connectToServer(serverIp, serverPort);
    if (serverSock == INVALID_SOCKET) return 1;

    Session::runClientSession(serverSock);
    closesocket(serverSock);
    WSACleanup();
    return 0;
}
