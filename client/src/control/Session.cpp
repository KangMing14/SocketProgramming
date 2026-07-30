#include "Session.h"
#include <winsock2.h>

namespace Session {
    SOCKET connectToServer(const std::string& ipAddress, int port){
        // Every Winsock program must call this once before using any socket function.
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cout << "WSAStartup failed\n";
            return INVALID_SOCKET;
        }

        // Create a listen socket for the server.
        SOCKET clientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSock == INVALID_SOCKET) {
            std::cout << "socket() failed: " << WSAGetLastError() << "\n";
            WSACleanup();
            return INVALID_SOCKET;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, ipAddress.c_str(), &serverAddr.sin_addr);

        std::cout << "Connecting to server at " << ipAddress << ":" << port << "...\n";
        if (connect(clientSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "connect() failed: " << WSAGetLastError() << "\n";
            closesocket(clientSock);
            WSACleanup();
            return INVALID_SOCKET;
        }

        std::cout << "Connected!\n";
        return clientSock;
    }

    bool isSocketConnected(SOCKET clientSock) {
        if (clientSock == INVALID_SOCKET) return false;

        // Set up socket set for select()
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(clientSock, &readSet);

        timeval timeout{ 0, 0 }; // select() returns immediately
        int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);

        // No activity (still connected and no data pending)
        if (selectResult <= 0) {
            return (selectResult == 0); // true if 0 (connected/quiet), false if SOCKET_ERROR
        }

        // Peek at 1 byte without consuming it from the OS queue
        char peekBuffer;
        int bytesPeeked = recv(clientSock, &peekBuffer, 1, MSG_PEEK);

        if (bytesPeeked == 0) return false; 
        else if (bytesPeeked == SOCKET_ERROR) {
            int err = WSAGetLastError();
            // WSAEWOULDBLOCK means socket is fine, but no data ready right now
            if (err == WSAEWOULDBLOCK) return true; 
            
            // Any other socket error (e.g., WSAECONNRESET) means disconnected
            return false; 
        }
        return true; 
    }

    void runClientSession(SOCKET serverSock) {
        char buffer[512];
        std::string inBuffer;

        // Read the initial "Service ready" greeting from the server
        int bytesReceived = recv(serverSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::cout << "Server: " << buffer;
        }

        std::string userInput;
        while (true) {
            if (!isSocketConnected(serverSock)) {
                closesocket(serverSock);
                serverSock = INVALID_SOCKET;
                return;
            }
            
            std::cout << "ftp> ";
            std::getline(std::cin, userInput);

            std::string commandToSend = userInput + "\r\n";
            send(serverSock, commandToSend.c_str(), (int)commandToSend.length(), 0);

            bool replyReceived = false;
            while (!replyReceived) {
                bytesReceived = recv(serverSock, buffer, sizeof(buffer) - 1, 0);
                if (bytesReceived <= 0) break;

                inBuffer.append(buffer, bytesReceived);

                size_t newlinePos;
                while ((newlinePos = inBuffer.find('\n')) != std::string::npos) {
                    std::string reply = inBuffer.substr(0, newlinePos);
                    if (!reply.empty() && reply.back() == '\r') {
                        reply.pop_back();
                    }

                    std::cout << reply << "\n";
                    inBuffer.erase(0, newlinePos + 1);
                    replyReceived = true; 
                }
            }
        }
    }
}