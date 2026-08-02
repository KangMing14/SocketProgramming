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
#include <memory>
#include <utility>

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
}

namespace CommandDispatcher{
    std::map<std::string, std::function<void(ClientSession&, const std::vector<std::string>&)>> commandMap = {
        { "USER", [](ClientSession& s, const std::vector<std::string>& args) {
            s.username = args.empty() ? "" : args[0];
            Session::replyWithCode(s.socket, ReplyCode::AuthNeedPass, "Username OK, need password.");
        }},

        { "PASS", [](ClientSession& s, const std::vector<std::string>&) {
            s.authenticated = true;   // Basic Level
            Session::replyWithCode(s.socket, ReplyCode::LoggedIn, "User logged in.");
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

        { "LIST", [](ClientSession& s, const std::vector<std::string>&) {
            std::vector<DirEntryInfo> entries;
            if (!g_dirService.listDir(s.currentDir, entries)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not list directory.");
                return;
            }
            std::string body;
            for (auto& e : entries)
                body += e.formatPermissions + " " + std::to_string(e.sizeBytes) + " " + e.name + "\r\n";
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, body);
        }},

        { "NLST", [](ClientSession& s, const std::vector<std::string>&) {
            std::vector<DirEntryInfo> entries;
            if (!g_dirService.listDir(s.currentDir, entries)) {
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not list directory.");
                return;
            }
            std::string body;
            for (auto& e : entries) body += e.name + "\r\n";
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, body);
        }},

        { "STAT", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.empty()) {
                Session::replyWithCode(s.socket, ReplyCode::SystemStatus, "Server status: OK.");  // no-path case
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
            if (args.empty()) { Session::replyWithCode(s.socket, ReplyCode::SyntaxError, ""); return; }
            std::filesystem::path resolved;
            if (!g_pathResolver.resolve(s.currentDir, args[0], resolved) || !std::filesystem::exists(resolved)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "File not found.");
                return;
            }
            if (s.dataChannelMode == DataChannelMode::None) {
                Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection,
                                       "Use PORT or PASV before RETR.");
                return;
            }

            std::unique_ptr<RdtSender> transport;
            const bool passive = s.dataChannelMode == DataChannelMode::Passive;
            if (passive) {
                const SOCKET dataSocket = std::exchange(
                    s.pendingDataSocket, INVALID_SOCKET);
                transport = std::make_unique<RdtSender>(dataSocket);
            } else {
                char peerIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &s.pendingPeerAddr.sin_addr, peerIp, sizeof(peerIp));
                unsigned short peerPort = ntohs(s.pendingPeerAddr.sin_port);
                transport = std::make_unique<RdtSender>(peerIp, peerPort);
            }
            s.dataChannelMode = DataChannelMode::None;
            s.pendingPeerAddr = {};
            if (!transport->isValid()) {
                Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection,
                                       "Could not open data connection.");
                return;
            }

            DataChannelSession channel(*transport);

            Session::replyWithCode(s.socket, ReplyCode::FileStatusOkay, "Opening data connection.");
            if (passive && !transport->waitForClientReady()) {
                Session::replyWithCode(s.socket, ReplyCode::TransferAborted,
                                       "Data handshake failed.");
                return;
            }
            bool ok = channel.sendFile(resolved, s.transferMode);
            Session::replyWithCode(s.socket, ok ? ReplyCode::TransferComplete : ReplyCode::TransferAborted,
                                    ok ? "Transfer complete." : "Transfer failed.");
        } },

        { "STOR", [](ClientSession& s, const std::vector<std::string>& args) {
            if (args.empty()) { Session::replyWithCode(s.socket, ReplyCode::SyntaxError, ""); return; }
            std::filesystem::path resolved;
            if (!g_pathResolver.resolve(s.currentDir, args[0], resolved)) {
                clearDataState(s);
                Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Path outside server root.");
                return;
            }
            if (s.dataChannelMode == DataChannelMode::None) {
                Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection,
                                       "Use PORT or PASV before STOR.");
                return;
            }

            const bool passive = s.dataChannelMode == DataChannelMode::Passive;
            std::unique_ptr<RdtReceiver> transport;
            if (passive) {
                const SOCKET dataSocket = std::exchange(
                    s.pendingDataSocket, INVALID_SOCKET);
                transport = std::make_unique<RdtReceiver>(dataSocket);
            } else {
                transport = std::make_unique<RdtReceiver>(
                    static_cast<uint16_t>(0));
            }
            const sockaddr_in activePeer = s.pendingPeerAddr;
            s.dataChannelMode = DataChannelMode::None;
            s.pendingPeerAddr = {};
            if (!transport->isValid()) {
                Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection,
                                       "Could not open data connection.");
                return;
            }

            DataChannelSession channel(*transport);

            Session::replyWithCode(s.socket, ReplyCode::FileStatusOkay, "Opening data connection.");
            if (!passive && !transport->initiateActiveHandshake(activePeer)) {
                Session::replyWithCode(s.socket, ReplyCode::TransferAborted,
                                       "Data handshake failed.");
                return;
            }
            bool ok = channel.receiveFile(resolved, s.transferMode);
            Session::replyWithCode(s.socket, ok ? ReplyCode::TransferComplete : ReplyCode::TransferAborted,
                                    ok ? "Transfer complete." : "Transfer failed.");
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
            if (args.empty() || (args[0] != "A" && args[0] != "I")) {
                Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "TYPE must be A or I.");
                return;
            }
            s.transferMode = (args[0] == "A") ? TransferMode::ASCII : TransferMode::Binary;
            Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, "Type set to " + args[0] + ".");
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

        // To Do: STOU, APPE, ABOR
    };
    
    void executeCommand(ClientSession& s, const ParsedCommand cmd){
        auto it = commandMap.find(cmd.type);
        if (it != commandMap.end()) {
            it->second(s, cmd.args);
        }
        else {
            Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "Command not found: " + cmd.type);
        }
    }
}
