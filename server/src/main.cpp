#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsaData;
    // Every Winsock program must call this once before using any socket function.
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;   // listen on all local interfaces
    serverAddr.sin_port = htons(4567);         // control channel port

    bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listenSock, SOMAXCONN);             // start accepting connection attempts

    printf("Server listening on port 4567...\n");

    while (true) {
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSock == INVALID_SOCKET) continue; // one bad accept shouldn't kill the server

        char welcomeMsg[] = "220 Service ready\r\n";
        send(clientSock, welcomeMsg, (int)strlen(welcomeMsg), 0);
        closesocket(clientSock); // for now: close immediately, no threading yet
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}