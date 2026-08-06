#include "CommandDispatcher.h"
#include "CommandParser.h"
#include "Session.h"
#include "Globals.h"
#include "TimeFormat.h"
#include "PassiveModeHandler.h"
#include "ActiveModeHandler.h"
#include "DataChannelSession.h"
#include "RdtReceiver.h"
#include "RdtSender.h"
#include "Sha256Hasher.h"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <windows.h>

namespace {
void clearDataState(ClientSession& session) {
    if (session.pendingDataSocket != INVALID_SOCKET) {
        closesocket(session.pendingDataSocket);
        session.pendingDataSocket = INVALID_SOCKET;
    }
    session.pendingPeerAddr = {};
    session.dataChannelMode = DataChannelMode::None;
}

bool portMatchesControlPeer(const ClientSession& session,
                            const sockaddr_in& advertised) {
    return advertised.sin_addr.s_addr == session.controlPeerAddr.sin_addr.s_addr &&
           advertised.sin_port != 0;
}

void sessionReply(ClientSession& session, int code, const std::string& message) {
    std::lock_guard<std::mutex> lock(session.replyMutex);
    if (session.socket != INVALID_SOCKET) {
        Session::replyWithCode(session.socket, code, message);
    }
}

void joinCompletedTransfer(ClientSession& session) {
    if (!session.transferActive.load() && session.transferWorker.joinable()) {
        session.transferWorker.join();
    }
}

bool createSiblingTemporaryPath(const std::filesystem::path& destination,
                                const wchar_t* prefix,
                                std::filesystem::path& outPath) {
    const std::filesystem::path parent = destination.parent_path();
    if (parent.empty()) return false;
    wchar_t buffer[MAX_PATH]{};
    if (GetTempFileNameW(parent.c_str(), prefix, 0, buffer) == 0) return false;
    outPath = buffer;
    return true;
}

bool appendAtomically(const std::filesystem::path& destination,
                      const std::filesystem::path& incoming) {
    std::filesystem::path combined;
    if (!createSiblingTemporaryPath(destination, L"hfa", combined)) return false;

    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{combined};

    std::ofstream output(combined, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
        if (error || !std::filesystem::is_regular_file(destination, error)) {
            return false;
        }
        std::ifstream existing(destination, std::ios::binary);
        if (!existing) return false;
        output << existing.rdbuf();
        if (existing.bad() || !output.good()) return false;
    } else if (error) {
        return false;
    }

    std::ifstream addition(incoming, std::ios::binary);
    if (!addition) return false;
    output << addition.rdbuf();
    if (addition.bad() || !output.good()) return false;
    output.flush();
    if (!output.good()) return false;
    output.close();
    if (output.fail()) return false;

    if (!MoveFileExW(combined.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return false;
    }
    cleanup.path.clear();
    return true;
}

bool reserveUniqueUpload(const std::filesystem::path& currentDir,
                         std::filesystem::path& resolved,
                         std::string& filename) {
    static std::atomic_uint64_t nextId{1};
    for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
        const std::uint64_t id = nextId.fetch_add(1);
        std::ostringstream name;
        name << "upload_" << std::setw(4) << std::setfill('0') << id << ".bin";
        std::filesystem::path candidate;
        if (!g_pathResolver.resolve(currentDir, name.str(), candidate)) {
            return false;
        }
        HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            resolved = std::move(candidate);
            filename = name.str();
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
}

enum class UploadKind { Store, Unique, Append };

std::string transferCompleteMessage(const std::string& hash,
                                    const std::string& uniqueFilename = {}) {
    std::string message = "Transfer complete.";
    if (!uniqueFilename.empty()) message += " Filename=" + uniqueFilename;
    if (!hash.empty()) message += " SHA256=" + hash;
    return message;
}

void finishTransfer(ClientSession& session, bool success,
                    const std::string& successMessage) {
    std::lock_guard<std::mutex> lock(session.replyMutex);
    const bool aborted = session.abortRequested.load();
    if (!aborted && session.socket != INVALID_SOCKET) {
        Session::replyWithCode(
            session.socket,
            success ? ReplyCode::TransferComplete : ReplyCode::TransferAborted,
            success ? successMessage : "Transfer failed.");
    }
    session.transferActive.store(false);
}

void startDownloadTransfer(ClientSession& session,
                           const std::filesystem::path& source) {
    if (session.dataChannelMode == DataChannelMode::None) {
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Use PORT or PASV before RETR.");
        return;
    }

    joinCompletedTransfer(session);
    const bool passive = session.dataChannelMode == DataChannelMode::Passive;
    std::unique_ptr<RdtSender> transport;
    if (passive) {
        const SOCKET dataSocket = std::exchange(
            session.pendingDataSocket, INVALID_SOCKET);
        transport = std::make_unique<RdtSender>(dataSocket);
    } else {
        char peerIp[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &session.pendingPeerAddr.sin_addr,
                  peerIp, sizeof(peerIp));
        transport = std::make_unique<RdtSender>(
            peerIp, ntohs(session.pendingPeerAddr.sin_port));
    }
    session.dataChannelMode = DataChannelMode::None;
    session.pendingPeerAddr = {};
    if (!transport->isValid()) {
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Could not open data connection.");
        return;
    }

    session.abortRequested.store(false);
    session.transferActive.store(true);
    const TransferMode transferMode = session.transferMode;
    sessionReply(session, ReplyCode::FileStatusOkay, "Opening data connection.");

    try {
        session.transferWorker = std::thread(
            [&session, source, transferMode, passive,
             transport = std::move(transport)]() mutable {
                const auto aborted = [&session] {
                    return session.abortRequested.load();
                };
                transport->setAbortPredicate(aborted);
                bool ready = !passive || transport->waitForClientReady();
                DataChannelSession channel(*transport, aborted);
                SendResult result;
                if (ready && !aborted()) {
                    result = channel.sendFile(source, transferMode);
                }
                finishTransfer(
                    session, result.success,
                    transferCompleteMessage(
                        transferMode == TransferMode::Binary
                            ? result.sha256 : std::string{}));
            });
    } catch (const std::system_error&) {
        session.transferActive.store(false);
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Could not start transfer worker.");
    }
}

void startUploadTransfer(ClientSession& session,
                         const std::filesystem::path& destination,
                         UploadKind kind,
                         const std::string& uniqueFilename = {}) {
    if (session.dataChannelMode == DataChannelMode::None) {
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Use PORT or PASV before upload.");
        if (kind == UploadKind::Unique) {
            std::error_code ignored;
            std::filesystem::remove(destination, ignored);
        }
        return;
    }

    std::filesystem::path stagingPath;
    if (kind == UploadKind::Append &&
        !createSiblingTemporaryPath(destination, L"hfu", stagingPath)) {
        clearDataState(session);
        sessionReply(session, ReplyCode::ActionNotTaken,
                     "Could not create append staging file.");
        return;
    }

    joinCompletedTransfer(session);
    const bool passive = session.dataChannelMode == DataChannelMode::Passive;
    std::unique_ptr<RdtReceiver> transport;
    if (passive) {
        const SOCKET dataSocket = std::exchange(
            session.pendingDataSocket, INVALID_SOCKET);
        transport = std::make_unique<RdtReceiver>(dataSocket);
    } else {
        transport = std::make_unique<RdtReceiver>(static_cast<uint16_t>(0));
    }
    const sockaddr_in activePeer = session.pendingPeerAddr;
    session.dataChannelMode = DataChannelMode::None;
    session.pendingPeerAddr = {};
    if (!transport->isValid()) {
        std::error_code ignored;
        if (!stagingPath.empty()) std::filesystem::remove(stagingPath, ignored);
        if (kind == UploadKind::Unique) {
            std::filesystem::remove(destination, ignored);
        }
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Could not open data connection.");
        return;
    }

    session.abortRequested.store(false);
    session.transferActive.store(true);
    session.pendingUniqueFilename = uniqueFilename;
    const TransferMode transferMode = session.transferMode;
    sessionReply(session, ReplyCode::FileStatusOkay, "Opening data connection.");

    try {
        session.transferWorker = std::thread(
            [&session, destination, stagingPath, transferMode, passive,
             activePeer, kind, uniqueFilename,
             transport = std::move(transport)]() mutable {
                const auto aborted = [&session] {
                    return session.abortRequested.load();
                };
                transport->setAbortPredicate(aborted);
                bool ready = passive ||
                    transport->initiateActiveHandshake(activePeer);
                const std::filesystem::path receivePath =
                    kind == UploadKind::Append ? stagingPath : destination;
                DataChannelSession channel(*transport, aborted);
                bool success = ready && !aborted() &&
                    channel.receiveFile(receivePath, transferMode);
                if (success && kind == UploadKind::Append && !aborted()) {
                    success = appendAtomically(destination, stagingPath);
                }

                std::error_code ignored;
                if (!stagingPath.empty()) {
                    std::filesystem::remove(stagingPath, ignored);
                }
                if (!success && kind == UploadKind::Unique) {
                    std::filesystem::remove(destination, ignored);
                }

                std::string hash;
                if (success && transferMode == TransferMode::Binary) {
                    hash = Sha256Hasher::hashFile(destination);
                }
                finishTransfer(
                    session, success,
                    transferCompleteMessage(hash, uniqueFilename));
                session.pendingUniqueFilename.clear();
            });
    } catch (const std::system_error&) {
        session.transferActive.store(false);
        std::error_code ignored;
        if (!stagingPath.empty()) std::filesystem::remove(stagingPath, ignored);
        if (kind == UploadKind::Unique) {
            std::filesystem::remove(destination, ignored);
        }
        session.pendingUniqueFilename.clear();
        sessionReply(session, ReplyCode::CantOpenDataConnection,
                     "Could not start transfer worker.");
    }
}
}

namespace CommandDispatcher{
    std::map<std::string, std::string> helpMap = {
        { "USER", 
            "Syntax: USER <username>\n"
            "Send the client's username to initiate an authentication session."
        },
        { "PASS", 
            "Syntax: PASS <password>\n"
            "Send the client's password to complete authentication."
        },
        { "QUIT", 
            "Syntax: QUIT\n"
            "Gracefully terminate the control connection and end the session."
        },
        { "NOOP", 
            "Syntax: NOOP\n"
            "No-operation; used as a keep-alive ping to prevent session timeout."
        },
        { "PWD", 
            "Syntax: PWD\n"
            "Print the server's current working directory path."
        },
        { "CWD", 
            "Syntax: CWD <path>\n"
            "Change the server's current working directory to the specified path."
        },
        { "CDUP", 
            "Syntax: CDUP\n"
            "Change the server's working directory to its parent directory."
        },
        { "MKD", 
            "Syntax: MKD <dirname>\n"
            "Create a new directory on the server at the current path."
        },
        { "RMD", 
            "Syntax: RMD <dirname>\n"
            "Remove an empty directory from the server."
        },
        { "LIST", 
            "Syntax: LIST [path]\n"
            "Return a detailed listing (name, size, type, permissions) of files and directories in the current or specified path."
        },
        { "NLST", 
            "Syntax: NLST [path]\n"
            "Return a plain name-only listing of files in the current or specified path."
        },
        { "STAT", 
            "Syntax: STAT [path]\n"
            "Return server status or, if a path is given, file/directory metadata."
        },
        { "SIZE", 
            "Syntax: SIZE <filename>\n"
            "Return the exact byte size of the specified file on the server."
        },
        { "MDTM", 
            "Syntax: MDTM <filename>\n"
            "Return the last modification timestamp of the specified file (format: YYYYMMDDhhmmss)."
        },
        { "TYPE", 
            "Syntax: TYPE {A | I}\n"
            "Set the data transfer type: A = ASCII (text), I = Image/Binary."
        },
        { "MODE",
            "Syntax: MODE {S | B | C}\n"
            "Set the transfer mode. Stream (S) is supported; Block (B) and Compressed (C) return 504."
        },
        { "PORT", 
            "Syntax: PORT <h1,h2,h3,h4,p1,p2>\n"
            "Active Mode: Client specifies its IP and port for the server to open the data connection back to. "
        },
        { "PASV", 
            "Syntax: PASV\n"
            "Passive Mode: Server opens a random port and returns its IP + port for the client to connect to."
        },
        { "RETR", 
            "Syntax: RETR <filename>\n"
            "Retrieve (download) the specified file from the server to the client via the data channel."
        },
        { "STOR", 
            "Syntax: STOR <filename>\n"
            "Store (upload) a file from the client to the server using the current filename."
        },
        { "STOU", 
            "Syntax: STOU\n"
            "Store a file with a guaranteed unique server-generated filename to prevent overwrites."
        },
        { "APPE", 
            "Syntax: APPE <filename>\n"
            "Append the uploaded data to an existing file on the server; create it if absent."
        },
        { "DELE", 
            "Syntax: DELE <filename>\n"
            "Delete the specified file from the server."
        },
        { "RNFR", 
            "Syntax: RNFR <oldname>\n"
            "Rename From: specify the file to be renamed (must be followed by RNTO)."
        },
        { "RNTO", 
            "Syntax: RNTO <newname>\n"
            "Rename To: complete the rename operation initiated by RNFR."
        },
        { "HASH", 
            "Syntax: HASH <filename>\n"
            "Request a cryptographic hash (MD5 or SHA-256) of the specified file for post-transfer integrity verification."
        },
        { "ABOR", 
            "Syntax: ABOR\n"
            "Abort the current data transfer in progress; data channel is reset."
        },
        { "HELP", 
            "Syntax: HELP [command]\n"
            "Return help text for all supported commands, or detailed usage for a specific command."
        },
    };

    std::map<std::string, std::function<void(ClientSession&, const std::vector<std::string>&)>> commandMap = {
        { "USER", [](ClientSession& s, const std::vector<std::string>& args) {
            std::string user = args.empty() ? "" : args[0];
            auto it = Users::database.find(user);
            if (it != Users::database.end()) {
                s.username = user;
                ClientRegistry::setUsername(s.socket, s.username);
                Logger::log("Client " + ClientRegistry::clients[s.socket].address + " identified as \"" + s.username + "\".");
                Session::replyWithCode(s.socket, ReplyCode::AuthNeedPass, "Username OK, need password.");
            }
            else {
                Session::replyWithCode(s.socket, ReplyCode::NotLoggedIn, "Not logged in, username incorrect.");
            }
            
        }},

        { "PASS", [](ClientSession& s, const std::vector<std::string>& args) {
            if (s.username.empty()) return Session::replyWithCode(s.socket, ReplyCode::BadSequence, "Log in with USER first.");
            std::string pw = args.empty() ? "" : args[0];
            if (pw == Users::database.find(s.username)->second) {
                s.authenticated = true;   // Basic Level
                Session::replyWithCode(s.socket, ReplyCode::LoggedIn, "User logged in.");
            }
            else Session::replyWithCode(s.socket, ReplyCode::NotLoggedIn, "Not logged in, password incorrect.");
        }},

        { "QUIT", [](ClientSession& s, const std::vector<std::string>&) {
            Session::replyWithCode(s.socket, ReplyCode::Goodbye, "Service closing control connection.");
            shutdown(s.socket, SD_SEND);
            closesocket(s.socket);
            s.socket = INVALID_SOCKET;
        }},

        { "NOOP", [](ClientSession& s, const std::vector<std::string>&) {
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, "NOOP OK.");
        }},

        { "PWD", [](ClientSession& s, const std::vector<std::string>&) {
            auto r = g_dirService.printWorkingDir(s.currentDir);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "CWD", [](ClientSession& s, const std::vector<std::string>& args) {
            auto r = g_dirService.changeDir(s.currentDir, args.empty() ? "" : args[0]);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "CDUP", [](ClientSession& s, const std::vector<std::string>&) {
            auto r = g_dirService.changeToParent(s.currentDir);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "MKD", [](ClientSession& s, const std::vector<std::string>& args) {
            auto r = g_dirService.makeDir(s.currentDir, args.empty() ? "" : args[0]);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "RMD", [](ClientSession& s, const std::vector<std::string>& args) {
            auto r = g_dirService.removeDir(s.currentDir, args.empty() ? "" : args[0]);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "LIST", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() > 1) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: LIST [path]");
                return;
            }
            std::vector<DirEntryInfo> entries;
            if (!g_dirService.listDir(
                    s.currentDir, args.empty() ? "" : args[0], entries)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not list directory.");
                return;
            }
            std::string body;
            for (auto& e : entries)
                body += e.formatPermissions + " " + std::to_string(e.sizeBytes) + " " + e.name + "\r\n";
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, body);
        }},

        { "NLST", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() > 1) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: NLST [path]");
                return;
            }
            std::vector<DirEntryInfo> entries;
            if (!g_dirService.listDir(
                    s.currentDir, args.empty() ? "" : args[0], entries)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not list directory.");
                return;
            }
            std::string body;
            for (auto& e : entries) body += e.name + "\r\n";
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, body);
        }},

        { "STAT", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.empty()) {
                std::string response = "Server status: OK." + std::to_string(ClientRegistry::count()) + " client(s) connected.";
                Session::replyWithCode(s.socket, ReplyCode::SystemStatus, response);  // no-path case
                return;
            }
            DirectoryService::PathMetadata meta;
            if (!g_dirService.getMetadata(s.currentDir, args[0], meta)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Path not found.");
                return;
            }
            std::string typeStr = meta.isDirectory ? "directory" : "file";
            Session::replyWithCode(s.socket, ReplyCode::FileStatus, args[0] + ": " + typeStr + ", " + std::to_string(meta.sizeBytes) + " bytes");
        }},

        { "SIZE", [](ClientSession& s, const std::vector<std::string>& args) {
            DirectoryService::PathMetadata meta;
            if (args.empty() || !g_dirService.getMetadata(s.currentDir, args[0], meta) || meta.isDirectory) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not get file size.");
                return;
            }
            Session::replyWithCode(s.socket, ReplyCode::FileStatus, std::to_string(meta.sizeBytes));
        }},

        { "MDTM", [](ClientSession& s, const std::vector<std::string>& args) {
            DirectoryService::PathMetadata meta;
            if (args.empty() || !g_dirService.getMetadata(s.currentDir, args[0], meta) || meta.isDirectory) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not get modification time.");
                return;
            }
            Session::replyWithCode(s.socket, ReplyCode::FileStatus, formatMdtmTimestamp(meta.lastModified));
        }},

        { "DELE", [](ClientSession& s, const std::vector<std::string>& args) {
            auto r = g_dirService.deleteFile(s.currentDir, args.empty() ? "" : args[0]);
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "RNFR", [](ClientSession& s, const std::vector<std::string>& args) {
            auto r = g_dirService.renameFrom(s.currentDir, args.empty() ? "" : args[0], s.pendingRenameSource);
            s.hasPendingRename = r.ok;
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "RNTO", [](ClientSession& s, const std::vector<std::string>& args) {
            if (!s.hasPendingRename) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "RNFR required first.");
                return;
            }
            auto r = g_dirService.renameTo(s.currentDir, s.pendingRenameSource, args.empty() ? "" : args[0]);
            s.hasPendingRename = false;
            Session::replyWithCode(s.socket, r.code, r.message);
        }},

        { "PASV", [](ClientSession& s, const std::vector<std::string>&) {
            clearDataState(s);
            SOCKET dataSock;
            unsigned short port;
            if (!openPassiveDataPort(dataSock, port)) {
                Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection, "Could not open data port.");
                return;
            }
            s.pendingDataSocket = dataSock;
            s.dataChannelMode = DataChannelMode::Passive;

            sockaddr_in localAddr{}; int len = sizeof(localAddr);
            getsockname(s.socket, (sockaddr*)&localAddr, &len); // control socket's local IP = server's IP
            uint32_t serverIp = ntohl(localAddr.sin_addr.s_addr);

            std::string body = formatPasvReply(serverIp, port);
            Session::replyWithCode(s.socket, ReplyCode::EnterPasvMode, "Entering Passive Mode (" + body + ").");
        } },

        { "PORT", [](ClientSession& s, const std::vector<std::string>& args) {
            clearDataState(s);
            sockaddr_in peerAddr{};
            if (args.size() != 1 || !parsePortCommand(args[0], peerAddr)) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "Bad PORT argument.");
                return;
            }
            if (!portMatchesControlPeer(s, peerAddr)) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "PORT address must match the control connection.");
                return;
            }
            s.pendingPeerAddr = peerAddr;
            s.dataChannelMode = DataChannelMode::Active;
            Session::replyWithCode(s.socket, ReplyCode::CommandOkay, "PORT command successful.");
        } },

        {"RETR", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: RETR <filename>");
                return;
            }
            std::filesystem::path resolved;
            std::error_code error;
            if (!g_pathResolver.resolve(s.currentDir, args[0], resolved) ||
                !std::filesystem::is_regular_file(resolved, error) || error) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "File not found.");
                return;
            }
            startDownloadTransfer(s, resolved);
        } },

        { "STOR", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: STOR <filename>");
                return;
            }
            std::filesystem::path resolved;
            if (!g_pathResolver.resolve(s.currentDir, args[0], resolved)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Path outside server root.");
                return;
            }
            std::error_code error;
            if (std::filesystem::is_directory(resolved, error)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::FilenameNotAllowed,
                                       "Destination is a directory.");
                return;
            }
            startUploadTransfer(s, resolved, UploadKind::Store);
        } },

        { "STOU", [](ClientSession& s, const std::vector<std::string>& args) {
            if (!args.empty()) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: STOU");
                return;
            }
            std::filesystem::path resolved;
            std::string filename;
            if (!reserveUniqueUpload(s.currentDir, resolved, filename)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::FilenameNotAllowed,
                                       "Could not reserve a unique filename.");
                return;
            }
            startUploadTransfer(s, resolved, UploadKind::Unique, filename);
        } },

        { "APPE", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() != 1) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: APPE <filename>");
                return;
            }
            std::filesystem::path resolved;
            if (!g_pathResolver.resolve(s.currentDir, args[0], resolved)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken,
                                       "Path outside server root.");
                return;
            }
            std::error_code error;
            if (std::filesystem::exists(resolved, error) &&
                !std::filesystem::is_regular_file(resolved, error)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::FilenameNotAllowed,
                                       "Destination is not a regular file.");
                return;
            }
            startUploadTransfer(s, resolved, UploadKind::Append);
        } },

        { "HASH", [](ClientSession& s, const std::vector<std::string>& args) {
            std::filesystem::path resolved;
            if (args.empty() || !g_pathResolver.resolve(s.currentDir, args[0], resolved) || !std::filesystem::exists(resolved)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "File not found.");
                return;
            }
            std::string hash = Sha256Hasher::hashFile(resolved);
            if (hash.empty()) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Hash computation failed.");
                return;
            }
            Session::replyWithCode(s.socket, ReplyCode::FileStatus, "SHA-256 " + hash);
        } },

        { "TYPE", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() != 1 || (args[0] != "A" && args[0] != "I")) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "TYPE must be A or I.");
                return;
            }
            s.transferMode = (args[0] == "A") ? TransferMode::ASCII : TransferMode::Binary;
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, "Type set to " + args[0] + ".");
        } },

        { "MODE", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.size() != 1 ||
                (args[0] != "S" && args[0] != "B" && args[0] != "C")) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "MODE must be S, B, or C.");
                return;
            }
            if (args[0] != "S") {
                Session::replyWithCode(
                    s.socket, ReplyCode::CommandNotImplementedForParameter,
                    "Only MODE S is supported.");
                return;
            }
            Session::replyWithCode(s.socket, ReplyCode::CommandOkay,
                                   "Stream mode enabled.");
        } },

        { "ABOR", [](ClientSession& s, const std::vector<std::string>& args) {
            if (!args.empty()) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError,
                                       "Syntax: ABOR");
                return;
            }
            if (!s.transferActive.load()) {
                joinCompletedTransfer(s);
                clearDataState(s);
                sessionReply(s, ReplyCode::TransferComplete,
                             "No transfer in progress.");
                return;
            }
            s.abortRequested.store(true);
            if (s.transferWorker.joinable()) s.transferWorker.join();
            s.transferActive.store(false);
            clearDataState(s);
            sessionReply(s, ReplyCode::TransferAborted, "Transfer aborted.");
        } },

        { "HELP", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.empty()) {
                Session::multilineReplyWithCode(s.socket, ReplyCode::HelpMessage,
                    "The following commands are recognized:\n"
                    "USER   PASS    QUIT    NOOP    PWD     CWD     CDUP    MKD \n"
                    "RMD    LIST    NLST    STAT    SIZE    MDTM    TYPE    MODE\n"
                    "PORT   PASV    RETR    STOR    STOU    APPE    DELE    RNFR\n"
                    "RNTO   HASH    ABOR    HELP"
                );
                return;
            }
            else {
                auto it = helpMap.find(args[0]);
                if (it != helpMap.end()) {
                    Session::multilineReplyWithCode(s.socket, ReplyCode::HelpMessage, it->second);
                }
                else {
                    Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "Command not found: " + args[0]);
                }
            }
        } }
    };

    void executeCommand(ClientSession& s, const ParsedCommand cmd){
        if (!s.authenticated && cmd.type != "USER" && cmd.type != "PASS" &&
            cmd.type != "HELP" && cmd.type != "NOOP" && cmd.type != "QUIT") {
            Session::replyWithCode(s.socket, ReplyCode::NotLoggedIn,
                                   "Please log in using USER and PASS.");
            return;
        }
        if (s.transferActive.load() && cmd.type != "ABOR") {
            sessionReply(s, ReplyCode::BadSequence,
                         "Transfer already in progress; use ABOR first.");
            return;
        }
        joinCompletedTransfer(s);

        auto it = commandMap.find(cmd.type);
        if (it != commandMap.end()) {
            it->second(s, cmd.args);
        }
        else {
            Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "Command not found: " + cmd.type);
        }
    }

    void shutdownSession(ClientSession& s) {
        s.abortRequested.store(true);
        if (s.transferWorker.joinable()) s.transferWorker.join();
        s.transferActive.store(false);
        clearDataState(s);
    }
}
