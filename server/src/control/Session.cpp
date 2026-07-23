#include "Session.h"
#include "../common/ReplyCodes.h"
#include <winsock2.h>

void replyWithCode(SOCKET clientSocket, int replyCode){
    // Respond to the client with the reply code
    std::string response = std::to_string(replyCode) + "\r\n";
    send(clientSocket, response.c_str(), (int) response.length(), 0);
}

SOCKET initializeSession(){
    // Every Winsock program must call this once before using any socket function.
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "WSAStartup failed\n";
        return INVALID_SOCKET;
    }

    // Create a listen socket for the server.
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cout << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return INVALID_SOCKET;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;   // listen on all local interfaces
    serverAddr.sin_port = htons(4567);         // control channel port

    bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listenSock, SOMAXCONN);

    std::cout << "Server listening on port 4567...\n";
    return listenSock;
}

void handleClient(SOCKET clientSock){
    replyWithCode(clientSock, ReplyCode::ServiceReady);

    char buffer[512];
    while (true) {
        int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) break;          // 0 = client closed, <0 = error
        buffer[bytesReceived] = '\0';
        printf("Received: %s", buffer);
        // Echo back for now — real command parsing comes in Step 2
        replyWithCode(clientSock, ReplyCode::ActionCompleted);
    }

    closesocket(clientSock);
    printf("Client disconnected.\n");
}

void runSession(){
    // Initialize the listen socket
    SOCKET listenSock = initializeSession();
    if (listenSock == INVALID_SOCKET) return;

    // Listen for clients
    while (true) {
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSock == INVALID_SOCKET) continue; // one bad accept shouldn't kill the server

        // Create a new thread for the client
        std::thread(handleClient, clientSock).detach();
    }

    closesocket(listenSock);
    WSACleanup();
}