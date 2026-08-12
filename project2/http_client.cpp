#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <iostream>
#include <string>

namespace {

bool sendAll(SOCKET socketHandle, const std::string& data) {
    std::size_t totalSent = 0;

    while (totalSent < data.size()) {
        const int sent = send(
            socketHandle,
            data.data() + totalSent,
            static_cast<int>(data.size() - totalSent),
            0);

        if (sent == SOCKET_ERROR || sent == 0) return false;
        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

} // namespace

int main() {
    constexpr const char* host = "www.google.com";
    constexpr const char* service = "80";

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const int resolveResult = getaddrinfo(host, service, &hints, &addresses);
    if (resolveResult != 0) {
        std::cerr << "getaddrinfo failed: " << resolveResult << '\n';
        WSACleanup();
        return 1;
    }

    SOCKET socketHandle = INVALID_SOCKET;
    for (addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next) {
        socketHandle = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (socketHandle == INVALID_SOCKET) continue;

        if (connect(
                socketHandle,
                address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0) {
            break;
        }

        closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);

    if (socketHandle == INVALID_SOCKET) {
        std::cerr << "Could not connect to " << host << ":80.\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in peerAddress{};
    int peerLength = sizeof(peerAddress);
    if (getpeername(
            socketHandle,
            reinterpret_cast<sockaddr*>(&peerAddress),
            &peerLength) == 0) {
        char peerIp[INET_ADDRSTRLEN]{};
        if (inet_ntop(
                AF_INET,
                &peerAddress.sin_addr,
                peerIp,
                sizeof(peerIp)) != nullptr) {
            std::cout << "Connected to " << peerIp << ":80\n\n";
        }
    }

    const std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: www.google.com\r\n"
        "User-Agent: SocketLab/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (!sendAll(socketHandle, request)) {
        std::cerr << "send failed: " << WSAGetLastError() << '\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    char buffer[4096];
    while (true) {
        const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received == 0) break;
        if (received == SOCKET_ERROR) {
            std::cerr << "\nrecv failed: " << WSAGetLastError() << '\n';
            closesocket(socketHandle);
            WSACleanup();
            return 1;
        }

        std::cout.write(buffer, received);
    }

    closesocket(socketHandle);
    WSACleanup();
    return 0;
}
