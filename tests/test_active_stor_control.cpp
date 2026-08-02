#include "../server/src/common/Globals.h"
#include "../server/src/common/ReplyCodes.h"
#include "../server/src/control/Session.h"

#include "../client/src/datachannel/ActiveModeClient.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "../client/src/rdt/RdtSender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

PathResolver g_pathResolver(fs::absolute("control_test_root"));
DirectoryService g_dirService(g_pathResolver);

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~WinsockSession() { WSACleanup(); }
};

struct ReplyReader {
    std::string buffered;

    int readCode(SOCKET socket) {
        while (true) {
            const std::size_t newline = buffered.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                require(line.size() >= 3, "Malformed control reply");
                return std::stoi(line.substr(0, 3));
            }
            char bytes[512];
            const int received = recv(socket, bytes, sizeof(bytes), 0);
            require(received > 0, "Control connection closed unexpectedly");
            buffered.append(bytes, received);
        }
    }
};

void sendCommand(SOCKET socket, const std::string& command) {
    const std::string line = command + "\r\n";
    require(send(socket, line.data(), static_cast<int>(line.size()), 0) ==
                static_cast<int>(line.size()),
            "Could not send control command");
}

std::pair<SOCKET, SOCKET> createControlPair() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Could not create TCP listener");

    sockaddr_in listenAddress{};
    listenAddress.sin_family = AF_INET;
    listenAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listenAddress.sin_port = 0;
    require(bind(listener, reinterpret_cast<sockaddr*>(&listenAddress),
                 sizeof(listenAddress)) != SOCKET_ERROR,
            "Could not bind TCP listener");
    require(listen(listener, 1) != SOCKET_ERROR, "Could not listen");

    int length = sizeof(listenAddress);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&listenAddress),
                        &length) != SOCKET_ERROR,
            "Could not inspect TCP listener");

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "Could not create TCP client");
    require(connect(client, reinterpret_cast<sockaddr*>(&listenAddress),
                    sizeof(listenAddress)) != SOCKET_ERROR,
            "Could not connect TCP client");

    SOCKET server = accept(listener, nullptr, nullptr);
    closesocket(listener);
    require(server != INVALID_SOCKET, "Could not accept TCP client");
    return {client, server};
}

void writeFixture(const fs::path& path) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Could not create source file");
    for (std::size_t index = 0; index < MAX_PAYLOAD * 2 + 19; ++index) {
        const char value = static_cast<char>((index * 13U) & 0xffU);
        output.write(&value, 1);
    }
}

bool filesEqual(const fs::path& left, const fs::path& right) {
    std::ifstream first(left, std::ios::binary);
    std::ifstream second(right, std::ios::binary);
    return first && second &&
        std::vector<char>(std::istreambuf_iterator<char>(first), {}) ==
        std::vector<char>(std::istreambuf_iterator<char>(second), {});
}

} // namespace

int main() {
    fs::path source;
    SOCKET clientControl = INVALID_SOCKET;
    std::thread serverThread;
    try {
        WinsockSession winsock;
        std::error_code ignored;
        fs::remove_all(g_pathResolver.root(), ignored);
        fs::create_directories(g_pathResolver.root());
        source = fs::absolute("control_upload_source.bin");
        writeFixture(source);

        const auto sockets = createControlPair();
        clientControl = sockets.first;
        serverThread = std::thread(Session::handleClient, sockets.second);

        ReplyReader replies;
        require(replies.readCode(clientControl) == ReplyCode::ServiceReady,
                "Expected 220 greeting");

        SOCKET rejectedSocket = INVALID_SOCKET;
        unsigned short rejectedPort = 0;
        require(hybridftp::client::openActiveListenPort(
                    rejectedSocket, rejectedPort),
                "Could not bind rejected PORT socket");
        const std::string rejected = hybridftp::client::formatPortCommand(
            (127U << 24) | 2U, rejectedPort);
        sendCommand(clientControl, "PORT " + rejected);
        require(replies.readCode(clientControl) == ReplyCode::SyntaxError,
                "Mismatched PORT IP was not rejected");
        closesocket(rejectedSocket);

        SOCKET dataSocket = INVALID_SOCKET;
        unsigned short dataPort = 0;
        require(hybridftp::client::openActiveListenPort(dataSocket, dataPort),
                "Could not bind Active data socket");
        const std::string port = hybridftp::client::formatPortCommand(
            (127U << 24) | 1U, dataPort);
        sendCommand(clientControl, "PORT " + port);
        require(replies.readCode(clientControl) == ReplyCode::CommandOkay,
                "Expected 200 after PORT");

        sendCommand(clientControl, "STOR active-control.bin");
        require(replies.readCode(clientControl) == ReplyCode::FileStatusOkay,
                "Expected 150 before Active STOR");

        in_addr loopback{};
        inet_pton(AF_INET, "127.0.0.1", &loopback);
        hybridftp::client::RdtSender sender(dataSocket);
        require(sender.waitForServerReady(loopback),
                "Client did not receive server SYN");
        hybridftp::client::DataChannelSession channel(sender);
        require(channel.sendFile(source, TransferMode::Binary),
                "Client upload failed");
        require(replies.readCode(clientControl) == ReplyCode::TransferComplete,
                "Expected 226 after Active STOR");
        require(filesEqual(source, g_pathResolver.root() / "active-control.bin"),
                "Uploaded file failed integrity check");

        SOCKET abandonedSocket = INVALID_SOCKET;
        unsigned short abandonedPort = 0;
        require(hybridftp::client::openActiveListenPort(
                    abandonedSocket, abandonedPort),
                "Could not bind abandoned data socket");
        const std::string abandoned = hybridftp::client::formatPortCommand(
            (127U << 24) | 1U, abandonedPort);
        sendCommand(clientControl, "PORT " + abandoned);
        require(replies.readCode(clientControl) == ReplyCode::CommandOkay,
                "Expected 200 for abandoned PORT");
        closesocket(abandonedSocket);
        sendCommand(clientControl, "STOR abandoned.bin");
        require(replies.readCode(clientControl) == ReplyCode::FileStatusOkay,
                "Expected 150 before failed Active STOR");
        require(replies.readCode(clientControl) == ReplyCode::TransferAborted,
                "Expected 426 after failed Active handshake");

        sendCommand(clientControl, "STOR stale.bin");
        require(replies.readCode(clientControl) ==
                    ReplyCode::CantOpenDataConnection,
                "Data endpoint was not single-use");

        sendCommand(clientControl, "QUIT");
        require(replies.readCode(clientControl) == ReplyCode::Goodbye,
                "Expected 221 after QUIT");
        closesocket(clientControl);
        clientControl = INVALID_SOCKET;
        serverThread.join();

        fs::remove(source, ignored);
        fs::remove_all(g_pathResolver.root(), ignored);
        std::cout << "[PASS] Active STOR control flow\n";
        return 0;
    } catch (const std::exception& error) {
        if (clientControl != INVALID_SOCKET) closesocket(clientControl);
        if (serverThread.joinable()) serverThread.join();
        std::error_code ignored;
        if (!source.empty()) fs::remove(source, ignored);
        fs::remove_all(g_pathResolver.root(), ignored);
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
