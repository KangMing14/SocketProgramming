#include "Session.h"

#include "ActiveModeClient.h"
#include "DataChannelSession.h"
#include "PassiveModeClient.h"
#include "RdtReceiver.h"
#include "RdtSender.h"
#include "ReplyCodes.h"
#include "TransferMode.h"
#include "../crypto/Sha256Hasher.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Session {
namespace {

enum class DataMode { None, Passive, Active };

struct Reply {
    int code = 0;
    std::string line;
    std::vector<std::string> lines;
};

struct ClientState {
    DataMode dataMode = DataMode::None;
    SOCKET pendingDataSocket = INVALID_SOCKET;
    sockaddr_in passiveAddress{};
    sockaddr_in serverAddress{};
    TransferMode transferMode = TransferMode::Binary;
    std::string replyBuffer;
    std::atomic_bool transferActive{false};
    std::atomic_bool abortRequested{false};
    std::thread transferWorker;
};

void closeDataState(ClientState& state) {
    if (state.pendingDataSocket != INVALID_SOCKET) {
        closesocket(state.pendingDataSocket);
        state.pendingDataSocket = INVALID_SOCKET;
    }
    state.passiveAddress = {};
    state.dataMode = DataMode::None;
}

void joinCompletedTransfer(ClientState& state) {
    if (!state.transferActive.load() && state.transferWorker.joinable()) {
        state.transferWorker.join();
    }
}

bool sendAll(SOCKET socket, const std::string& bytes) {
    std::size_t sentTotal = 0;
    while (sentTotal < bytes.size()) {
        const int sent = send(socket, bytes.data() + sentTotal,
                              static_cast<int>(bytes.size() - sentTotal), 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        sentTotal += static_cast<std::size_t>(sent);
    }
    return true;
}

bool sendCommand(SOCKET socket, const std::string& command) {
    return sendAll(socket, command + "\r\n");
}

bool readReply(SOCKET socket, ClientState& state, Reply& reply) {
    reply = Reply{};
    bool complete = false, multiline = false;

    while (!complete) {
        std::size_t newline;
        while ((newline = state.replyBuffer.find('\n')) != std::string::npos) {
            reply.line = state.replyBuffer.substr(0, newline);
            state.replyBuffer.erase(0, newline + 1);
            
            if (!reply.line.empty() && reply.line.back() == '\r') {
                reply.line.pop_back();
            }

            if (!reply.code) {
                if (reply.line.size() < 3 ||
                    !std::isdigit(static_cast<unsigned char>(reply.line[0])) ||
                    !std::isdigit(static_cast<unsigned char>(reply.line[1])) ||
                    !std::isdigit(static_cast<unsigned char>(reply.line[2]))) {
                    return false;
                }

                reply.code = std::stoi(reply.line.substr(0, 3));

                if (reply.line.size() > 3 && reply.line[3] == '-') multiline = true;
                else complete = true;
            }
            else if (multiline) {
                std::string codeStr = std::to_string(reply.code);

                if (reply.line.size() > 3 && reply.line.compare(0, 3, codeStr) == 0 && reply.line[3] == ' ') complete = true;
            }
            
            reply.lines.push_back(reply.line);
            if (complete) return true;
        }

        char buffer[512];
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) return false;
        state.replyBuffer.append(buffer, received);
    }

    return true;
}

bool printReply(SOCKET socket, ClientState& state, Reply& reply) {
    if (!readReply(socket, state, reply)) return false;

    for (const std::string& line : reply.lines)  {
        std::cout << line << '\n';
    }
    
    return true;
}

std::vector<std::string> splitCommand(const std::string& command) {
    std::istringstream input(command);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) tokens.push_back(token);
    return tokens;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string canonicalizeCommandVerb(std::string command) {
    const std::size_t first = command.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return command;
    const std::size_t end = command.find_first_of(" \t\r\n", first);
    const std::size_t length = end == std::string::npos
        ? command.size() - first : end - first;
    std::transform(command.begin() + static_cast<std::ptrdiff_t>(first),
                   command.begin() + static_cast<std::ptrdiff_t>(first + length),
                   command.begin() + static_cast<std::ptrdiff_t>(first),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    return command;
}

bool extractHashField(const std::string& replyLine,
                      const std::string& field,
                      std::string& outHash) {
    std::istringstream tokens(replyLine);
    std::string token;
    const std::string prefix = field + "=";
    while (tokens >> token) {
        if (token.rfind(prefix, 0) != 0) continue;
        outHash = token.substr(prefix.size());
        return outHash.size() == 64 &&
            std::all_of(outHash.begin(), outHash.end(), [](unsigned char ch) {
                return std::isxdigit(ch) != 0;
            });
    }
    return false;
}

bool enterActiveMode(SOCKET controlSocket, ClientState& state,
                     const std::vector<std::string>& tokens) {
    if (tokens.size() != 1) {
        std::cerr << "Usage: PORT\n";
        return true;
    }
    closeDataState(state);

    SOCKET dataSocket = INVALID_SOCKET;
    unsigned short dataPort = 0;
    if (!hybridftp::client::openActiveListenPort(dataSocket, dataPort)) {
        std::cerr << "Could not bind an Active-mode UDP socket.\n";
        return true;
    }

    char serverIp[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &state.serverAddress.sin_addr, serverIp, sizeof(serverIp));
    std::uint32_t localIp = 0;
    if (!hybridftp::client::getLocalIPv4ForServer(serverIp, localIp)) {
        closesocket(dataSocket);
        std::cerr << "Could not determine the local IPv4 address.\n";
        return true;
    }

    const std::string portArgument =
        hybridftp::client::formatPortCommand(localIp, dataPort);
    if (!sendCommand(controlSocket, "PORT " + portArgument)) {
        closesocket(dataSocket);
        return false;
    }

    Reply reply;
    if (!printReply(controlSocket, state, reply)) {
        closesocket(dataSocket);
        return false;
    }
    if (reply.code == ReplyCode::CommandOkay) {
        state.pendingDataSocket = dataSocket;
        state.dataMode = DataMode::Active;
    } else {
        closesocket(dataSocket);
    }
    return true;
}

bool enterPassiveMode(SOCKET controlSocket, ClientState& state,
                      const std::vector<std::string>& tokens) {
    if (tokens.size() != 1) {
        std::cerr << "Usage: PASV\n";
        return true;
    }
    closeDataState(state);
    if (!sendCommand(controlSocket, "PASV")) return false;

    Reply reply;
    if (!printReply(controlSocket, state, reply)) return false;
    if (reply.code != ReplyCode::EnterPasvMode ||
        !hybridftp::client::parsePasvReply(reply.line, state.passiveAddress)) {
        return true;
    }

    SOCKET dataSocket = INVALID_SOCKET;
    if (!hybridftp::client::connectToPassiveDataPort(
            state.passiveAddress, dataSocket)) {
        std::cerr << "Could not open a Passive-mode UDP socket.\n";
        return true;
    }
    state.pendingDataSocket = dataSocket;
    state.dataMode = DataMode::Passive;
    return true;
}

bool storeFile(SOCKET controlSocket, ClientState& state,
               const std::vector<std::string>& tokens) {
    const std::string verb = upper(tokens.empty() ? "" : tokens[0]);
    const bool unique = verb == "STOU";
    if ((unique && tokens.size() != 2) ||
        (!unique && (tokens.size() < 2 || tokens.size() > 3))) {
        std::cerr << (unique
            ? "Usage: STOU <local-path>\n"
            : "Usage: " + verb + " <local-path> [remote-path]\n");
        return true;
    }
    const std::filesystem::path localPath = tokens[1];
    if (!std::filesystem::is_regular_file(localPath)) {
        std::cerr << "Local file not found: " << localPath.string() << '\n';
        return true;
    }
    if (state.dataMode == DataMode::None ||
        state.pendingDataSocket == INVALID_SOCKET) {
        std::cerr << "Use PORT or PASV before STOR.\n";
        return true;
    }

    const std::string remotePath = unique ? std::string{} :
        (tokens.size() == 3 ? tokens[2] : localPath.filename().string());
    if (!unique && remotePath.empty()) {
        std::cerr << "A remote filename is required.\n";
        return true;
    }
    const std::string serverCommand = unique ? "STOU" : verb + " " + remotePath;
    if (!sendCommand(controlSocket, serverCommand)) return false;

    Reply preliminary;
    if (!printReply(controlSocket, state, preliminary)) return false;
    if (preliminary.code != ReplyCode::FileStatusOkay) {
        closeDataState(state);
        return true;
    }

    const DataMode mode = state.dataMode;
    const SOCKET dataSocket = std::exchange(
        state.pendingDataSocket, INVALID_SOCKET);
    const sockaddr_in passiveAddress = state.passiveAddress;
    const sockaddr_in serverAddress = state.serverAddress;
    const TransferMode transferMode = state.transferMode;
    state.dataMode = DataMode::None;
    state.passiveAddress = {};
    joinCompletedTransfer(state);
    state.abortRequested.store(false);
    state.transferActive.store(true);

    try {
        state.transferWorker = std::thread(
            [controlSocket, &state, localPath, mode, dataSocket,
             passiveAddress, serverAddress, transferMode, verb]() {
                const auto aborted = [&state] {
                    return state.abortRequested.load();
                };
                std::unique_ptr<hybridftp::client::RdtSender> sender;
                if (mode == DataMode::Passive) {
                    sender = std::make_unique<hybridftp::client::RdtSender>(
                        dataSocket, passiveAddress);
                } else {
                    sender = std::make_unique<hybridftp::client::RdtSender>(
                        dataSocket);
                }
                sender->setAbortPredicate(aborted);
                hybridftp::client::DataChannelSession channel(*sender, aborted);
                hybridftp::client::SendResult sendResult;
                const bool ready = mode == DataMode::Passive ||
                    sender->waitForServerReady(serverAddress.sin_addr);
                if (ready && !aborted()) {
                    sendResult = channel.sendFile(localPath, transferMode);
                }

                Reply completion;
                const bool replyOkay = printReply(controlSocket, state, completion);
                if (replyOkay && !aborted() &&
                    (!sendResult.success ||
                     completion.code != ReplyCode::TransferComplete)) {
                    std::cerr << "Upload failed.\n";
                } else if (replyOkay && !aborted() &&
                           transferMode == TransferMode::Binary &&
                           !sendResult.sha256.empty()) {
                    std::string serverHash;
                    if (extractHashField(completion.line, "SHA256", serverHash)) {
                        if (serverHash == sendResult.sha256) {
                            std::cout << "Integrity verified: SHA-256 matches ("
                                      << serverHash << ")\n";
                        } else {
                            std::cerr << "WARNING: hash mismatch! Local="
                                      << sendResult.sha256 << " Server="
                                      << serverHash << "\n";
                        }
                    }
                    if (verb == "APPE") {
                        std::string finalHash;
                        if (extractHashField(
                                completion.line, "FINAL_SHA256", finalHash)) {
                            std::cout << "Final remote SHA-256: "
                                      << finalHash << "\n";
                        }
                    }
                }
                state.transferActive.store(false);
            });
    } catch (const std::system_error&) {
        closesocket(dataSocket);
        state.transferActive.store(false);
        std::cerr << "Could not start the upload worker.\n";
    }
    return true;
}

bool retrieveFile(SOCKET controlSocket, ClientState& state,
                  const std::vector<std::string>& tokens) {
    if (tokens.size() < 2 || tokens.size() > 3) {
        std::cerr << "Usage: RETR <remote-path> [local-path]\n";
        return true;
    }
    if (state.dataMode == DataMode::None ||
        state.pendingDataSocket == INVALID_SOCKET) {
        std::cerr << "Use PORT or PASV before RETR.\n";
        return true;
    }

    const std::string remotePath = tokens[1];
    const std::filesystem::path localPath = tokens.size() == 3
        ? std::filesystem::path(tokens[2])
        : std::filesystem::path(remotePath).filename();
    if (localPath.empty()) {
        std::cerr << "A local filename is required.\n";
        return true;
    }
    if (!sendCommand(controlSocket, "RETR " + remotePath)) return false;

    Reply preliminary;
    if (!printReply(controlSocket, state, preliminary)) return false;
    if (preliminary.code != ReplyCode::FileStatusOkay) {
        closeDataState(state);
        return true;
    }

    const DataMode mode = state.dataMode;
    const SOCKET dataSocket = std::exchange(
        state.pendingDataSocket, INVALID_SOCKET);
    const sockaddr_in passiveAddress = state.passiveAddress;
    const sockaddr_in serverAddress = state.serverAddress;
    const TransferMode transferMode = state.transferMode;
    state.dataMode = DataMode::None;
    state.passiveAddress = {};
    joinCompletedTransfer(state);
    state.abortRequested.store(false);
    state.transferActive.store(true);

    try {
        state.transferWorker = std::thread(
            [controlSocket, &state, localPath, mode, dataSocket,
             passiveAddress, serverAddress, transferMode]() {
                const auto aborted = [&state] {
                    return state.abortRequested.load();
                };
                hybridftp::client::RdtReceiver receiver(dataSocket);
                receiver.setAbortPredicate(aborted);
                receiver.expectPeerIp(serverAddress.sin_addr);
                bool ready = receiver.isValid();
                if (ready && mode == DataMode::Passive) {
                    ready = receiver.signalClientReady(passiveAddress);
                }
                hybridftp::client::DataChannelSession channel(receiver, aborted);
                const bool transferOkay = ready && !aborted() &&
                    channel.receiveFile(localPath, transferMode);

                Reply completion;
                const bool replyOkay = printReply(controlSocket, state, completion);
                if (replyOkay && !aborted() &&
                    (!transferOkay ||
                     completion.code != ReplyCode::TransferComplete)) {
                    std::cerr << "Download failed.\n";
                } else if (replyOkay && !aborted() &&
                           transferMode == TransferMode::Binary) {
                    std::string serverHash;
                    if (extractHashField(completion.line, "SHA256", serverHash)) {
                        const std::string localHash =
                            Sha256Hasher::hashFile(localPath);
                        if (localHash == serverHash) {
                            std::cout << "Integrity verified: SHA-256 matches ("
                                      << serverHash << ")\n";
                        } else {
                            std::cerr << "WARNING: hash mismatch! Local="
                                      << localHash << " Server="
                                      << serverHash << "\n";
                        }
                    }
                }
                state.transferActive.store(false);
            });
    } catch (const std::system_error&) {
        closesocket(dataSocket);
        state.transferActive.store(false);
        std::cerr << "Could not start the download worker.\n";
    }
    return true;
}

bool forwardCommand(SOCKET controlSocket, ClientState& state,
                    const std::string& command,
                    const std::vector<std::string>& tokens) {
    if (!sendCommand(controlSocket, canonicalizeCommandVerb(command))) return false;
    Reply reply;
    if (!printReply(controlSocket, state, reply)) return false;

    if (!tokens.empty() && upper(tokens[0]) == "TYPE" &&
        tokens.size() == 2 && reply.code >= 200 && reply.code < 300) {
        const std::string type = upper(tokens[1]);
        if (type == "A") state.transferMode = TransferMode::ASCII;
        if (type == "I") state.transferMode = TransferMode::Binary;
    }
    return true;
}

} // namespace

SOCKET connectToServer(const std::string& ipAddress, int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "WSAStartup failed\n";
        return INVALID_SOCKET;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        std::cout << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return INVALID_SOCKET;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, ipAddress.c_str(), &serverAddress.sin_addr) != 1 ||
        connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddress),
                sizeof(serverAddress)) == SOCKET_ERROR) {
        std::cerr << "connect() failed: " << WSAGetLastError() << "\n";
        closesocket(clientSocket);
        WSACleanup();
        return INVALID_SOCKET;
    }

    std::cout << "Connected to " << ipAddress << ':' << port << "\n";
    return clientSocket;
}

void runClientSession(SOCKET serverSocket) {
    ClientState state;
    int serverLength = sizeof(state.serverAddress);
    if (getpeername(serverSocket,
                    reinterpret_cast<sockaddr*>(&state.serverAddress),
                    &serverLength) == SOCKET_ERROR) {
        std::cerr << "Could not inspect the control server address.\n";
        return;
    }

    Reply greeting;
    if (!printReply(serverSocket, state, greeting)) return;

    std::string input;
    while (std::cout << "ftp> " && std::getline(std::cin, input)) {
        const std::vector<std::string> tokens = splitCommand(input);
        if (tokens.empty()) continue;
        const std::string verb = upper(tokens[0]);

        bool connected = true;
        if (state.transferActive.load()) {
            if (verb != "ABOR" || tokens.size() != 1) {
                std::cerr << "Transfer already in progress; use ABOR first.\n";
                continue;
            }
            state.abortRequested.store(true);
            connected = sendCommand(serverSocket, "ABOR");
            if (state.transferWorker.joinable()) state.transferWorker.join();
            state.transferActive.store(false);
            if (!connected) break;
            continue;
        }

        joinCompletedTransfer(state);
        if (verb == "PORT") {
            connected = enterActiveMode(serverSocket, state, tokens);
        } else if (verb == "PASV") {
            connected = enterPassiveMode(serverSocket, state, tokens);
        } else if (verb == "STOR" || verb == "STOU" || verb == "APPE") {
            connected = storeFile(serverSocket, state, tokens);
        } else if (verb == "RETR") {
            connected = retrieveFile(serverSocket, state, tokens);
        } else {
            connected = forwardCommand(serverSocket, state, input, tokens);
        }

        if (!connected || verb == "QUIT") break;
    }
    if (state.transferActive.load()) {
        state.abortRequested.store(true);
        sendCommand(serverSocket, "ABOR");
    }
    if (state.transferWorker.joinable()) state.transferWorker.join();
    closeDataState(state);
}

}
