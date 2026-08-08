#include "ClientRegistry.h"
#include "Globals.h"
#include "ReplyCodes.h"
#include "Session.h"

#include <winsock2.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

PathResolver g_pathResolver(fs::absolute("control_session_test_root"));
DirectoryService g_dirService(g_pathResolver);

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
                "WSAStartup failed");
    }
    ~WinsockSession() { WSACleanup(); }
};

std::pair<SOCKET, SOCKET> createControlPair() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Could not create listener");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != SOCKET_ERROR,
            "Could not bind listener");
    require(listen(listener, 1) != SOCKET_ERROR, "Could not listen");
    int length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                        &length) != SOCKET_ERROR,
            "Could not inspect listener");

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "Could not create client socket");
    require(connect(client, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) != SOCKET_ERROR,
            "Could not connect client socket");
    SOCKET server = accept(listener, nullptr, nullptr);
    closesocket(listener);
    require(server != INVALID_SOCKET, "Could not accept client socket");
    return {client, server};
}

std::string receiveExactly(SOCKET socket, std::size_t expectedSize) {
    std::string received;
    std::vector<char> buffer(4096);
    while (received.size() < expectedSize) {
        const int count = recv(socket, buffer.data(),
                               static_cast<int>(buffer.size()), 0);
        require(count > 0, "Control reply ended before all bytes arrived");
        received.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return received;
}

void testCompleteControlReplies() {
    const auto sockets = createControlPair();
    const SOCKET client = sockets.first;
    const SOCKET server = sockets.second;

    Session::multilineReplyWithCode(server, ReplyCode::SystemStatus, "");
    require(receiveExactly(client, 6) == "211 \r\n",
            "Empty multiline response was malformed");

    int sendBuffer = 128;
    setsockopt(server, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));

    std::vector<std::string> lines;
    lines.reserve(2000);
    for (int index = 0; index < 2000; ++index) {
        lines.push_back("line-" + std::to_string(index) + "-" +
                        std::string(64, 'x'));
    }

    std::string message;
    std::string expected;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) message.push_back('\n');
        message += lines[index];
        expected += "211";
        expected += index + 1 == lines.size() ? " " : "-";
        expected += lines[index] + "\r\n";
    }

    std::thread writer([&] {
        Session::multilineReplyWithCode(
            server, ReplyCode::SystemStatus, message);
    });
    const std::string received = receiveExactly(client, expected.size());
    writer.join();
    require(received == expected, "Long control reply was truncated");

    closesocket(client);
    closesocket(server);
    std::cout << "[PASS] complete empty and long control replies\n";
}

void testBindFailureIsReported() {
    SOCKET first = Session::initializeSession(0);
    require(first != INVALID_SOCKET, "Could not create first listen socket");

    sockaddr_in address{};
    int length = sizeof(address);
    require(getsockname(first, reinterpret_cast<sockaddr*>(&address),
                        &length) != SOCKET_ERROR,
            "Could not inspect first listen socket");

    SOCKET second = Session::initializeSession(ntohs(address.sin_port));
    require(second == INVALID_SOCKET,
            "Second server unexpectedly ignored bind failure");

    closesocket(first);
    WSACleanup();
    std::cout << "[PASS] bind failure prevents server startup\n";
}

void testConcurrentRegistryAccess() {
    constexpr int THREADS = 4;
    constexpr int CLIENTS_PER_THREAD = 100;
    std::atomic_bool valid{true};
    std::vector<std::thread> workers;

    for (int threadIndex = 0; threadIndex < THREADS; ++threadIndex) {
        workers.emplace_back([&, threadIndex] {
            for (int index = 0; index < CLIENTS_PER_THREAD; ++index) {
                const SOCKET socket = static_cast<SOCKET>(
                    10000 + threadIndex * CLIENTS_PER_THREAD + index);
                ClientRegistry::addClient(socket, "loopback");
                ClientRegistry::setUsername(socket, "User1");
                const auto client = ClientRegistry::findClient(socket);
                if (!client || client->address != "loopback" ||
                    client->username != "User1") {
                    valid.store(false);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    require(valid.load(), "Registry returned a torn or missing client record");
    require(ClientRegistry::count() == THREADS * CLIENTS_PER_THREAD,
            "Registry count was corrupted by concurrent writes");
    require(ClientRegistry::snapshot().size() ==
                THREADS * CLIENTS_PER_THREAD,
            "Registry snapshot was incomplete");

    for (int index = 0; index < THREADS * CLIENTS_PER_THREAD; ++index) {
        ClientRegistry::removeClient(static_cast<SOCKET>(10000 + index));
    }
    require(ClientRegistry::count() == 0,
            "Registry cleanup left stale clients");
    std::cout << "[PASS] synchronized client registry access\n";
}

}

int main() {
    try {
        WinsockSession winsock;
        testCompleteControlReplies();
        testBindFailureIsReported();
        testConcurrentRegistryAccess();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
