#include "../server/src/common/Globals.h"
#include "../server/src/common/ReplyCodes.h"
#include "../server/src/control/Session.h"
#include "../server/src/crypto/Sha256Hasher.h"

#include "../client/src/datachannel/ActiveModeClient.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "../client/src/rdt/RdtSender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

PathResolver g_pathResolver(fs::absolute("remaining_commands_test_root"));
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

struct Reply {
    int code = 0;
    std::string line;
    std::vector<std::string> lines;
};

class ReplyReader {
public:
    Reply read(SOCKET socket) {
        Reply reply;
        reply.line = readLine(socket);
        require(hasReplyCode(reply.line),
                "Malformed control reply: " + reply.line);
        reply.code = std::stoi(reply.line.substr(0, 3));
        reply.lines.push_back(reply.line);

        if (reply.line.size() <= 3 || reply.line[3] != '-') return reply;

        const std::string terminator = std::to_string(reply.code) + " ";
        while (true) {
            reply.line = readLine(socket);
            reply.lines.push_back(reply.line);
            if (reply.line.rfind(terminator, 0) == 0) return reply;
        }
    }

private:
    static bool hasReplyCode(const std::string& line) {
        return line.size() >= 3 &&
               std::isdigit(static_cast<unsigned char>(line[0])) &&
               std::isdigit(static_cast<unsigned char>(line[1])) &&
               std::isdigit(static_cast<unsigned char>(line[2]));
    }

    std::string readLine(SOCKET socket) {
        while (true) {
            const std::size_t newline = buffered_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffered_.substr(0, newline);
                buffered_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }

            char bytes[512];
            const int received = recv(socket, bytes, sizeof(bytes), 0);
            require(received > 0, "Control connection closed unexpectedly");
            buffered_.append(bytes, static_cast<std::size_t>(received));
        }
    }

    std::string buffered_;
};

void sendCommand(SOCKET socket, const std::string& command) {
    const std::string line = command + "\r\n";
    std::size_t sentTotal = 0;
    while (sentTotal < line.size()) {
        const int sent = send(
            socket, line.data() + sentTotal,
            static_cast<int>(line.size() - sentTotal), 0);
        require(sent > 0, "Could not send control command");
        sentTotal += static_cast<std::size_t>(sent);
    }
}

bool replyContains(const Reply& reply, const std::string& text) {
    return std::any_of(reply.lines.begin(), reply.lines.end(),
                       [&](const std::string& line) {
                           return line.find(text) != std::string::npos;
                       });
}

Reply readListing(SOCKET control, ReplyReader& replies,
                  const std::string& command) {
    sendCommand(control, command);
    Reply listing = replies.read(control);
    require(listing.code == ReplyCode::ActionCompleted,
            command + " did not return 250");

    sendCommand(control, "NOOP");
    const Reply noop = replies.read(control);
    require(noop.code == ReplyCode::ActionCompleted &&
                replyContains(noop, "NOOP OK"),
            command + " left the control reply stream out of sync");
    return listing;
}

std::pair<SOCKET, SOCKET> createControlPair() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Could not create TCP listener");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(listener, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != SOCKET_ERROR,
            "Could not bind TCP listener");
    require(listen(listener, 1) != SOCKET_ERROR, "Could not listen");
    int length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                        &length) != SOCKET_ERROR,
            "Could not inspect TCP listener");

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "Could not create TCP client");
    require(connect(client, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) != SOCKET_ERROR,
            "Could not connect TCP client");
    SOCKET server = accept(listener, nullptr, nullptr);
    closesocket(listener);
    require(server != INVALID_SOCKET, "Could not accept TCP client");
    return {client, server};
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Could not write fixture");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not read file");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

SOCKET prepareActiveEndpoint(SOCKET control, ReplyReader& replies) {
    SOCKET dataSocket = INVALID_SOCKET;
    unsigned short dataPort = 0;
    require(hybridftp::client::openActiveListenPort(dataSocket, dataPort),
            "Could not bind Active data socket");
    const std::string port = hybridftp::client::formatPortCommand(
        (127U << 24) | 1U, dataPort);
    sendCommand(control, "PORT " + port);
    require(replies.read(control).code == ReplyCode::CommandOkay,
            "Expected 200 after PORT");
    return dataSocket;
}

Reply activeUpload(SOCKET control, ReplyReader& replies,
                   const std::string& command, const fs::path& source) {
    SOCKET dataSocket = prepareActiveEndpoint(control, replies);
    sendCommand(control, command);
    require(replies.read(control).code == ReplyCode::FileStatusOkay,
            "Expected 150 before upload");

    in_addr loopback{};
    inet_pton(AF_INET, "127.0.0.1", &loopback);
    hybridftp::client::RdtSender sender(dataSocket);
    require(sender.waitForServerReady(loopback),
            "Client did not receive the server SYN");
    hybridftp::client::DataChannelSession channel(sender);
    require(channel.sendFile(source, TransferMode::Binary),
            "Client upload failed");
    return replies.read(control);
}

std::string extractFilename(const std::string& line) {
    const std::string marker = "Filename=";
    const std::size_t start = line.find(marker);
    require(start != std::string::npos, "STOU reply omitted Filename");
    const std::size_t valueStart = start + marker.size();
    const std::size_t end = line.find_first_of(" \r\n", valueStart);
    return line.substr(valueStart, end - valueStart);
}

std::string extractField(const std::string& line, const std::string& field) {
    const std::string marker = field + "=";
    const std::size_t start = line.find(marker);
    require(start != std::string::npos, "Reply omitted " + field);
    const std::size_t valueStart = start + marker.size();
    const std::size_t end = line.find_first_of(" \r\n", valueStart);
    return line.substr(valueStart, end - valueStart);
}

void login(SOCKET control, ReplyReader& replies) {
    require(replies.read(control).code == ReplyCode::ServiceReady,
            "Expected 220 greeting");
    sendCommand(control, "uSeR User1");
    require(replies.read(control).code == ReplyCode::AuthNeedPass,
            "Expected 331 after USER");
    sendCommand(control, "pAsS password");
    require(replies.read(control).code == ReplyCode::LoggedIn,
            "Expected 230 after PASS");

    sendCommand(control, "USER user1");
    require(replies.read(control).code == ReplyCode::NotLoggedIn,
            "Unknown USER did not clear the authenticated state");
    sendCommand(control, "PASS password");
    require(replies.read(control).code == ReplyCode::BadSequence,
            "PASS succeeded after an unknown USER");

    sendCommand(control, "USER User1");
    require(replies.read(control).code == ReplyCode::AuthNeedPass,
            "Valid USER was not accepted after reset");
    sendCommand(control, "PASS wrong");
    require(replies.read(control).code == ReplyCode::NotLoggedIn,
            "Wrong password was not rejected");
    sendCommand(control, "pWd");
    require(replies.read(control).code == ReplyCode::NotLoggedIn,
            "Failed PASS left the session authenticated");
    sendCommand(control, "PASS password");
    require(replies.read(control).code == ReplyCode::LoggedIn,
            "Valid password could not recover the pending USER attempt");

    sendCommand(control, "USER User1");
    require(replies.read(control).code == ReplyCode::AuthNeedPass,
            "USER while logged in did not start a new authentication attempt");
    sendCommand(control, "PWD");
    require(replies.read(control).code == ReplyCode::NotLoggedIn,
            "USER while logged in did not revoke authentication");
    sendCommand(control, "PASS password");
    require(replies.read(control).code == ReplyCode::LoggedIn,
            "Re-authentication failed");

    sendCommand(control, "pWd");
    require(replies.read(control).code == ReplyCode::PathnameCreated,
            "Mixed-case PWD command was not accepted");
}

} // namespace

int main() {
    SOCKET control = INVALID_SOCKET;
    std::thread serverThread;
    fs::path firstSource;
    fs::path secondSource;
    try {
        WinsockSession winsock;
        std::error_code ignored;
        fs::remove_all(g_pathResolver.root(), ignored);
        fs::create_directories(g_pathResolver.root() / "subdir");
        fs::create_directories(g_pathResolver.root() / "multiple");
        fs::create_directories(g_pathResolver.root() / "empty");
        writeText(g_pathResolver.root() / "subdir" / "inside.txt", "listed");
        writeText(g_pathResolver.root() / "multiple" / "first.txt", "first");
        writeText(g_pathResolver.root() / "multiple" / "second.bin", "second");

        firstSource = fs::absolute("remaining_first_source.bin");
        secondSource = fs::absolute("remaining_second_source.bin");
        writeText(firstSource, "alpha");
        writeText(secondSource, "beta");

        const auto sockets = createControlPair();
        control = sockets.first;
        serverThread = std::thread(Session::handleClient, sockets.second);
        ReplyReader replies;
        login(control, replies);

        sendCommand(control, "mOdE s");
        require(replies.read(control).code == ReplyCode::CommandOkay,
                "MODE S was not accepted");
        sendCommand(control, "MODE B");
        require(replies.read(control).code ==
                    ReplyCode::CommandNotImplementedForParameter,
                "MODE B was not rejected with 504");
        sendCommand(control, "MODE C");
        require(replies.read(control).code ==
                    ReplyCode::CommandNotImplementedForParameter,
                "MODE C was not rejected with 504");

        const Reply emptyList = readListing(control, replies, "LIST empty");
        require(emptyList.lines.size() == 1 && emptyList.line == "250 ",
                "LIST did not represent an empty directory cleanly");
        const Reply emptyNames = readListing(control, replies, "NLST empty");
        require(emptyNames.lines.size() == 1 && emptyNames.line == "250 ",
                "NLST did not represent an empty directory cleanly");

        const Reply listing = readListing(control, replies, "LIST subdir");
        require(listing.lines.size() == 1 &&
                    replyContains(listing, "inside.txt") &&
                    replyContains(listing, "rw"),
                "LIST did not return one detailed file entry");
        const Reply names = readListing(control, replies, "NLST subdir");
        require(names.lines.size() == 1 &&
                    replyContains(names, "inside.txt") &&
                    !replyContains(names, "rw"),
                "NLST did not return one name-only file entry");

        const Reply multipleList = readListing(control, replies, "LIST multiple");
        require(multipleList.lines.size() == 2 &&
                    replyContains(multipleList, "first.txt") &&
                    replyContains(multipleList, "second.bin"),
                "LIST did not frame all entries in a non-empty directory");
        const Reply multipleNames = readListing(control, replies, "NLST multiple");
        require(multipleNames.lines.size() == 2 &&
                    replyContains(multipleNames, "first.txt") &&
                    replyContains(multipleNames, "second.bin") &&
                    !replyContains(multipleNames, "rw"),
                "NLST did not frame all name-only entries");

        const Reply fileList = readListing(
            control, replies, "LIST subdir/inside.txt");
        require(fileList.lines.size() == 1 &&
                    replyContains(fileList, "inside.txt") &&
                    replyContains(fileList, "rw"),
                "LIST did not support a regular-file target");
        const Reply fileNames = readListing(
            control, replies, "NLST subdir/inside.txt");
        require(fileNames.lines.size() == 1 &&
                    replyContains(fileNames, "inside.txt") &&
                    !replyContains(fileNames, "rw"),
                "NLST did not support a regular-file target");

        const Reply firstUnique = activeUpload(
            control, replies, "STOU", firstSource);
        require(firstUnique.code == ReplyCode::TransferComplete,
                "First STOU did not complete");
        const std::string firstName = extractFilename(firstUnique.line);
        require(fs::path(firstName).parent_path().empty() &&
                    readText(g_pathResolver.root() / firstName) == "alpha",
                "First STOU filename or contents were invalid");

        const Reply secondUnique = activeUpload(
            control, replies, "STOU", secondSource);
        require(secondUnique.code == ReplyCode::TransferComplete,
                "Second STOU did not complete");
        const std::string secondName = extractFilename(secondUnique.line);
        require(firstName != secondName &&
                    fs::path(secondName).parent_path().empty() &&
                    readText(g_pathResolver.root() / secondName) == "beta",
                "STOU did not generate a second safe unique filename");

        writeText(g_pathResolver.root() / "append.txt", "base-");
        const Reply append = activeUpload(
            control, replies, "APPE append.txt", secondSource);
        require(append.code == ReplyCode::TransferComplete &&
                    readText(g_pathResolver.root() / "append.txt") == "base-beta",
                "APPE did not append atomically");
        require(extractField(append.line, "SHA256") ==
                    Sha256Hasher::hashFile(secondSource),
                "APPE payload hash did not match the uploaded fragment");
        require(extractField(append.line, "FINAL_SHA256") ==
                    Sha256Hasher::hashFile(g_pathResolver.root() / "append.txt"),
                "APPE final hash did not match the resulting destination");

        writeText(g_pathResolver.root() / "preserve.txt", "original");
        SOCKET abortUploadSocket = prepareActiveEndpoint(control, replies);
        sendCommand(control, "APPE preserve.txt");
        require(replies.read(control).code == ReplyCode::FileStatusOkay,
                "Expected 150 before abortable APPE");
        in_addr loopback{};
        inet_pton(AF_INET, "127.0.0.1", &loopback);
        hybridftp::client::RdtSender abortSender(abortUploadSocket);
        require(abortSender.waitForServerReady(loopback),
                "Abort test did not complete the Active handshake");
        sendCommand(control, "ABOR");
        require(replies.read(control).code == ReplyCode::TransferAborted,
                "ABOR did not cancel the upload");
        require(readText(g_pathResolver.root() / "preserve.txt") == "original",
                "Cancelled APPE changed the original destination");

        sendCommand(control, "STOR stale-after-abort.bin");
        require(replies.read(control).code ==
                    ReplyCode::CantOpenDataConnection,
                "ABOR did not reset the data endpoint");

        writeText(g_pathResolver.root() / "download.bin",
                  std::string(MAX_PAYLOAD * 3, 'x'));
        SOCKET abortDownloadSocket = prepareActiveEndpoint(control, replies);
        sendCommand(control, "RETR download.bin");
        require(replies.read(control).code == ReplyCode::FileStatusOkay,
                "Expected 150 before abortable RETR");
        sendCommand(control, "ABOR");
        require(replies.read(control).code == ReplyCode::TransferAborted,
                "ABOR did not cancel the download");
        closesocket(abortDownloadSocket);

        sendCommand(control, "ABOR");
        require(replies.read(control).code == ReplyCode::TransferComplete,
                "Idle ABOR did not return 226");

        sendCommand(control, "QUIT");
        require(replies.read(control).code == ReplyCode::Goodbye,
                "Expected 221 after QUIT");
        closesocket(control);
        control = INVALID_SOCKET;
        serverThread.join();

        fs::remove(firstSource, ignored);
        fs::remove(secondSource, ignored);
        fs::remove_all(g_pathResolver.root(), ignored);
        std::cout << "[PASS] Remaining FTP commands\n";
        return 0;
    } catch (const std::exception& error) {
        if (control != INVALID_SOCKET) closesocket(control);
        if (serverThread.joinable()) serverThread.join();
        std::error_code ignored;
        if (!firstSource.empty()) fs::remove(firstSource, ignored);
        if (!secondSource.empty()) fs::remove(secondSource, ignored);
        fs::remove_all(g_pathResolver.root(), ignored);
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
