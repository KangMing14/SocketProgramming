#include "../server/src/datachannel/DataChannelSession.h"
#include "../server/src/datachannel/PassiveModeHandler.h"
#include "../server/src/rdt/RdtReceiver.h"
#include "../server/src/rdt/RdtSender.h"

#include "../client/src/datachannel/ActiveModeClient.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "../client/src/datachannel/PassiveModeClient.h"
#include "../client/src/rdt/RdtReceiver.h"
#include "../client/src/rdt/RdtSender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " +
                                     std::to_string(result));
        }
    }
    ~WinsockSession() { WSACleanup(); }
};

sockaddr_in loopbackAddress(unsigned short port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

in_addr loopbackIp() {
    in_addr address{};
    inet_pton(AF_INET, "127.0.0.1", &address);
    return address;
}

void writeFixture(const fs::path& path, std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Could not create fixture");
    for (std::size_t index = 0; index < size; ++index) {
        const char value = static_cast<char>((index * 37U + 11U) & 0xffU);
        output.write(&value, 1);
    }
}

bool filesEqual(const fs::path& left, const fs::path& right) {
    std::ifstream first(left, std::ios::binary);
    std::ifstream second(right, std::ios::binary);
    if (!first || !second) return false;
    return std::vector<char>(std::istreambuf_iterator<char>(first), {}) ==
           std::vector<char>(std::istreambuf_iterator<char>(second), {});
}

bool activeStore(const fs::path& source, const fs::path& destination) {
    SOCKET clientSocket = INVALID_SOCKET;
    unsigned short clientPort = 0;
    require(hybridftp::client::openActiveListenPort(clientSocket, clientPort),
            "Could not open Active client socket");

    RdtReceiver serverReceiver(static_cast<uint16_t>(0));
    require(serverReceiver.isValid(), "Could not create Active server receiver");
    const sockaddr_in clientAddress = loopbackAddress(clientPort);

    auto handshake = std::async(std::launch::async, [&]() {
        return serverReceiver.initiateActiveHandshake(clientAddress);
    });

    hybridftp::client::RdtSender clientSender(clientSocket);
    require(clientSender.waitForServerReady(loopbackIp()),
            "Client did not receive server SYN");
    require(handshake.get(), "Server Active handshake failed");

    auto receive = std::async(std::launch::async, [&]() {
        DataChannelSession channel(serverReceiver);
        return channel.receiveFile(destination, TransferMode::Binary);
    });
    hybridftp::client::DataChannelSession channel(clientSender);
    return channel.sendFile(source, TransferMode::Binary) && receive.get();
}

void activeHandshakeRetriesAfterDroppedSyn() {
    SOCKET clientSocket = INVALID_SOCKET;
    unsigned short clientPort = 0;
    require(hybridftp::client::openActiveListenPort(clientSocket, clientPort),
            "Could not open retry-test client socket");
    DWORD timeout = 2000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    RdtReceiver serverReceiver(static_cast<uint16_t>(0));
    require(serverReceiver.isValid(), "Could not create retry-test receiver");
    const sockaddr_in clientAddress = loopbackAddress(clientPort);
    auto handshake = std::async(std::launch::async, [&]() {
        return serverReceiver.initiateActiveHandshake(clientAddress);
    });

    char dropped[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int received = recvfrom(
        clientSocket, dropped, sizeof(dropped), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
    require(received >= static_cast<int>(HEADER_SIZE),
            "Did not receive first SYN to drop");

    hybridftp::client::RdtSender clientSender(clientSocket);
    require(clientSender.waitForServerReady(loopbackIp()),
            "Client did not receive retried SYN");
    require(handshake.get(), "Server did not accept ACK after SYN retry");
}

bool passiveStore(const fs::path& source, const fs::path& destination) {
    SOCKET serverSocket = INVALID_SOCKET;
    unsigned short serverPort = 0;
    require(openPassiveDataPort(serverSocket, serverPort),
            "Could not open PASV server socket");
    const sockaddr_in serverAddress = loopbackAddress(serverPort);

    SOCKET clientSocket = INVALID_SOCKET;
    require(hybridftp::client::connectToPassiveDataPort(
                serverAddress, clientSocket),
            "Could not open PASV client socket");

    RdtReceiver serverReceiver(serverSocket);
    hybridftp::client::RdtSender clientSender(clientSocket, serverAddress);
    auto receive = std::async(std::launch::async, [&]() {
        DataChannelSession channel(serverReceiver);
        return channel.receiveFile(destination, TransferMode::Binary);
    });
    hybridftp::client::DataChannelSession channel(clientSender);
    return channel.sendFile(source, TransferMode::Binary) && receive.get();
}

bool activeRetrieve(const fs::path& source, const fs::path& destination) {
    SOCKET clientSocket = INVALID_SOCKET;
    unsigned short clientPort = 0;
    require(hybridftp::client::openActiveListenPort(clientSocket, clientPort),
            "Could not open Active client socket");

    hybridftp::client::RdtReceiver clientReceiver(clientSocket);
    clientReceiver.expectPeerIp(loopbackIp());
    RdtSender serverSender("127.0.0.1", clientPort);

    auto receive = std::async(std::launch::async, [&]() {
        hybridftp::client::DataChannelSession channel(clientReceiver);
        return channel.receiveFile(destination, TransferMode::Binary);
    });
    DataChannelSession channel(serverSender);
    return channel.sendFile(source, TransferMode::Binary) && receive.get();
}

bool passiveRetrieve(const fs::path& source, const fs::path& destination) {
    SOCKET serverSocket = INVALID_SOCKET;
    unsigned short serverPort = 0;
    require(openPassiveDataPort(serverSocket, serverPort),
            "Could not open PASV server socket");
    const sockaddr_in serverAddress = loopbackAddress(serverPort);

    SOCKET clientSocket = INVALID_SOCKET;
    require(hybridftp::client::connectToPassiveDataPort(
                serverAddress, clientSocket),
            "Could not open PASV client socket");

    RdtSender serverSender(serverSocket);
    hybridftp::client::RdtReceiver clientReceiver(clientSocket);
    auto handshake = std::async(std::launch::async, [&]() {
        return serverSender.waitForClientReady();
    });
    require(clientReceiver.signalClientReady(serverAddress),
            "Client PASV SYN failed");
    require(handshake.get(), "Server did not accept client PASV SYN");

    auto receive = std::async(std::launch::async, [&]() {
        hybridftp::client::DataChannelSession channel(clientReceiver);
        return channel.receiveFile(destination, TransferMode::Binary);
    });
    DataChannelSession channel(serverSender);
    return channel.sendFile(source, TransferMode::Binary) && receive.get();
}

} // namespace

int main() {
    fs::path testDirectory;
    try {
        WinsockSession winsock;
        testDirectory = fs::temp_directory_path() /
            ("hybridftp_matrix_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(testDirectory);

        const fs::path source = testDirectory / "source.bin";
        writeFixture(source, MAX_PAYLOAD * 4 + 37);

        const fs::path activeStoreResult = testDirectory / "active_store.bin";
        require(activeStore(source, activeStoreResult), "PORT STOR failed");
        require(filesEqual(source, activeStoreResult),
                "PORT STOR integrity mismatch");
        std::cout << "[PASS] PORT STOR\n";

        const fs::path passiveStoreResult = testDirectory / "passive_store.bin";
        require(passiveStore(source, passiveStoreResult), "PASV STOR failed");
        require(filesEqual(source, passiveStoreResult),
                "PASV STOR integrity mismatch");
        std::cout << "[PASS] PASV STOR\n";

        const fs::path activeRetrieveResult = testDirectory / "active_retr.bin";
        require(activeRetrieve(source, activeRetrieveResult), "PORT RETR failed");
        require(filesEqual(source, activeRetrieveResult),
                "PORT RETR integrity mismatch");
        std::cout << "[PASS] PORT RETR\n";

        const fs::path passiveRetrieveResult = testDirectory / "passive_retr.bin";
        require(passiveRetrieve(source, passiveRetrieveResult), "PASV RETR failed");
        require(filesEqual(source, passiveRetrieveResult),
                "PASV RETR integrity mismatch");
        std::cout << "[PASS] PASV RETR\n";

        const fs::path empty = testDirectory / "empty.bin";
        writeFixture(empty, 0);
        const fs::path emptyResult = testDirectory / "empty_result.bin";
        require(activeStore(empty, emptyResult), "Empty PORT STOR failed");
        require(filesEqual(empty, emptyResult),
                "Empty PORT STOR integrity mismatch");
        std::cout << "[PASS] empty DATA|FIN\n";

        activeHandshakeRetriesAfterDroppedSyn();
        std::cout << "[PASS] dropped SYN retry\n";

        fs::remove_all(testDirectory);
        return 0;
    } catch (const std::exception& error) {
        if (!testDirectory.empty()) {
            std::error_code ignored;
            fs::remove_all(testDirectory, ignored);
        }
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
