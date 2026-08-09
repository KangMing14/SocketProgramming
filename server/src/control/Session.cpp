#include "Session.h"
#include "../common/ReplyCodes.h"
#include "../common/Logger.h"
#include "../common/ClientRegistry.h"
#include "CommandParser.h"
#include "CommandDispatcher.h"
#include "Globals.h"
#include <winsock2.h>
#include <sstream>
#include <vector>

namespace {
bool sendAll(SOCKET socket, const std::string& bytes) {
    std::size_t sentTotal = 0;
    while (sentTotal < bytes.size()) {
        const int sent = send(
            socket, bytes.data() + sentTotal,
            static_cast<int>(bytes.size() - sentTotal), 0);
        if (sent > 0) {
            sentTotal += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEINTR) continue;
        return false;
    }
    return true;
}

void stopFailedControlSocket(SOCKET socket) {
    Logger::log("Control-channel send failed: " +
                std::to_string(WSAGetLastError()) + ".");
    shutdown(socket, SD_BOTH);
}
}

namespace Session {
    void replyWithCode(SOCKET clientSocket, int replyCode, std::string message = ""){
        // Respond to the client with the reply code
        std::string response = std::to_string(replyCode) + " " + message + "\r\n";
        if (!sendAll(clientSocket, response)) stopFailedControlSocket(clientSocket);
    }

    void multilineReplyWithCode(SOCKET clientSocket, int replyCode, std::string message = ""){
        std::stringstream ss(message);
        std::string line, response;
        std::vector<std::string> lines;

        // Split input text by newlines, strip trailing \r
        while (std::getline(ss, line, '\n')) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }

        if (lines.empty()) lines.emplace_back();

        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            response = std::to_string(replyCode) + "-" + lines[i] + "\r\n";
            if (!sendAll(clientSocket, response)) {
                stopFailedControlSocket(clientSocket);
                return;
            }
        }

        response = std::to_string(replyCode) + " " + lines[lines.size()-1] + "\r\n";
        if (!sendAll(clientSocket, response)) stopFailedControlSocket(clientSocket);
    }

    SOCKET initializeSession(std::uint16_t port){
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
        serverAddr.sin_port = htons(port);          // control channel port

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr),
                 sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cout << "bind() failed: " << WSAGetLastError() << "\n";
            closesocket(listenSock);
            WSACleanup();
            return INVALID_SOCKET;
        }
        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::cout << "listen() failed: " << WSAGetLastError() << "\n";
            closesocket(listenSock);
            WSACleanup();
            return INVALID_SOCKET;
        }

        std::uint16_t listeningPort = port;
        if (listeningPort == 0) {
            sockaddr_in boundAddress{};
            int boundLength = sizeof(boundAddress);
            if (getsockname(listenSock,
                    reinterpret_cast<sockaddr*>(&boundAddress),
                    &boundLength) == 0) {
                listeningPort = ntohs(boundAddress.sin_port);
            }
        }
        std::cout << "Server listening on port " << listeningPort << "...\n";
        return listenSock;
    }

    void handleClient(SOCKET clientSock){
        ClientSession session;
        session.socket = clientSock;
        session.currentDir = g_pathResolver.root();
        int peerLength = sizeof(session.controlPeerAddr);
        getpeername(clientSock,
                    reinterpret_cast<sockaddr*>(&session.controlPeerAddr),
                    &peerLength);

        // Register this session in the shared connected-clients table
        char peerIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &session.controlPeerAddr.sin_addr, peerIp, sizeof(peerIp));
        std::string peerAddress = std::string(peerIp) + ":" +
            std::to_string(ntohs(session.controlPeerAddr.sin_port));
        ClientRegistry::addClient(clientSock, peerAddress);

        // Log each client that connects and disconnects
        Logger::log("Client connected: " + peerAddress + ". There are now " + 
            std::to_string(ClientRegistry::count()) + " connected client(s).");

        // Guarantees removal on exit without calling removeClient() at each return site
        struct RegistryGuard {
            SOCKET socket;
            std::string address;
            ~RegistryGuard() { 
                ClientRegistry::removeClient(socket); 
                Logger::log("Client disconnected: " + address + ". There are now " + 
                    std::to_string(ClientRegistry::count()) + " connected client(s).");
            }
        } registryGuard{clientSock, peerAddress};

        replyWithCode(clientSock, ReplyCode::ServiceReady, "Service ready.");

        std::string inBuffer;
        char buffer[512];
        while (true) {
            int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
            if (bytesReceived <= 0) break;

            inBuffer.append(buffer, bytesReceived);

            size_t newlinePos;
            // Only finish the command when the user presses newline
            while ((newlinePos = inBuffer.find('\n')) != std::string::npos) {
                // Extract the complete command (and trim potential trailing '\r')
                std::string command = inBuffer.substr(0, newlinePos);
                if (!command.empty() && command.back() == '\r') {
                    command.pop_back();
                }

                // Command processing
                ParsedCommand cmd = CommandParser::parseCommand(command);
                if (cmd.type.empty()) replyWithCode(clientSock, ReplyCode::SyntaxError);
                else CommandDispatcher::executeCommand(session, cmd);

                // Check if client disconnected from server
                if (session.socket == INVALID_SOCKET) {
                    CommandDispatcher::shutdownSession(session);
                    return;
                }

                // Remove the processed command from the session buffer
                inBuffer.erase(0, newlinePos + 1);
            }
        }

        CommandDispatcher::shutdownSession(session);
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
}
