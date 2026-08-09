Good — this confirms terminology to keep consistent with. Now here's the full, ordered wiring guide.

---

# Master Wiring Guide — Fixing, Completing, and Connecting All Three Members' Work for Week 2

Follow this in order — each step depends on the one before it. Owner is noted per step; several early steps are quick enough that any member (or you, pairing with them) can do them to unblock the team fast.

---

## Phase 0 — Repository Cleanup (do this before anyone writes new code)

### Step 0.1 — Resolve the duplicate `ReplyCodes.h` (Owner: you or Member A, 5 minutes)

Two files currently exist: `server/src/common/ReplyCodes.h` and `server/src/common/ ReplyCodes.h` (note the leading space — a merge artifact). Delete the stray one and merge every constant from both into a single canonical file:

```cpp
// server/src/common/ReplyCodes.h — the ONE canonical version
#pragma once

namespace ReplyCode {
    // 1xx - Positive Preliminary
    constexpr int FileStatusOkay = 150;

    // 2xx - Positive Completion
    constexpr int ServiceReady   = 220;
    constexpr int Goodbye        = 221;
    constexpr int LoggedIn       = 230;   // PASS success
    constexpr int ActionCompleted = 250;
    constexpr int PathnameCreated = 257;  // PWD, MKD
    constexpr int TransferComplete = 226;

    // 3xx - Positive Intermediate
    constexpr int AuthNeedPass  = 331;    // USER success, awaiting PASS
    constexpr int PendingRNTO   = 350;

    // 4xx - Transient Negative
    constexpr int CantOpenDataConnection = 425;

    // 5xx - Permanent Negative
    constexpr int SyntaxError    = 500;
    constexpr int ActionNotTaken = 550;
    constexpr int FilenameNotAllowed = 553;
}
```

```bash
git rm "server/src/common/ ReplyCodes.h"
```

### Step 0.2 — Re-enable the actual server executable (Owner: you or Member A)

```cmake
# CMakeLists.txt
add_executable(ftp_server server/src/main.cpp)
target_link_libraries(ftp_server PRIVATE ftp_core)
```

Right now nobody can run a real end-to-end test because this target is commented out — fix this before anything else in this guide can be verified.

### Step 0.3 — Fix the buffer-parsing bug (Owner: Member A)

```cpp
// Session.cpp, inside handleClient — currently parses the WRONG variable
ParsedCommand cmd = CommandParser::parseCommand(command);   // was: inBuffer
```

This is silent right now because a single-command-per-`recv()` test client never exposes it — but it will corrupt parsing the moment two commands arrive in one TCP segment, which is normal, not an edge case, for real clients.

### Step 0.4 — Add the missing `common/Logger.h` (Owner: whoever needs it first, per your original plan)

Your file plan allocated this and it was never created. A minimal version is enough for Week 2's audit-log requirement later:

```cpp
// server/src/common/Logger.h
#pragma once
#include <iostream>
#include <string>
#include <mutex>

namespace Logger {
    inline std::mutex logMutex;
    inline void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMutex);
        std::cout << "[LOG] " << msg << std::endl;
    }
}
```

---

## Phase 1 — Give the Server Real Per-Client State (Owner: Member A)

This is the actual missing link blocking every command handler below. `DirectoryService` methods need a `currentDir` per client; login-gated commands need to know if the client authenticated. None of that state currently exists anywhere in `Session`.

### Step 1.1 — Define `ClientSession`

```cpp
// server/src/control/Session.h — add this struct
#pragma once
// ... existing includes ...
#include <filesystem>

struct ClientSession {
    SOCKET socket;
    bool authenticated = false;
    std::string username;
    std::filesystem::path currentDir;

    // Populated by PASV/PORT, consumed by STOR/RETR — see Phase 4
    bool dataChannelIsPassive = false;
    SOCKET pendingDataSocket = INVALID_SOCKET;
    sockaddr_in pendingPeerAddr{};

    // Populated by RNFR, consumed by RNTO — see Phase 3
    std::filesystem::path pendingRenameSource;
    bool hasPendingRename = false;
};
```

### Step 1.2 — Construct one shared `PathResolver`/`DirectoryService` at server startup

These are stateless services (by design, from your earlier work) — exactly one instance should exist for the whole server's lifetime, shared safely across every client thread.

```cpp
// server/src/main.cpp
#include "control/Session.h"
#include "filesystem/PathResolver.h"
#include "filesystem/DirectoryService.h"
#include <filesystem>

PathResolver g_pathResolver(std::filesystem::absolute("server_root"));
DirectoryService g_dirService(g_pathResolver);

int main() {
    std::filesystem::create_directories(g_pathResolver.root()); // ensure sandbox exists
    Session::runSession();
    return 0;
}
```

Declare `extern` references to these two in a shared header (e.g. `common/Globals.h`) so `CommandDispatcher.cpp` can reach them:

```cpp
// server/src/common/Globals.h
#pragma once
#include "../filesystem/PathResolver.h"
#include "../filesystem/DirectoryService.h"

extern PathResolver g_pathResolver;
extern DirectoryService g_dirService;
```

### Step 1.3 — Thread `ClientSession` through the accept loop

```cpp
// Session.cpp
void handleClient(SOCKET clientSock) {
    ClientSession session;
    session.socket = clientSock;
    session.currentDir = g_pathResolver.root();

    replyWithCode(clientSock, ReplyCode::ServiceReady, "Service ready.");

    std::string inBuffer;
    char buffer[512];
    while (true) {
        int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) break;
        inBuffer.append(buffer, bytesReceived);

        size_t newlinePos;
        while ((newlinePos = inBuffer.find('\n')) != std::string::npos) {
            std::string command = inBuffer.substr(0, newlinePos);
            if (!command.empty() && command.back() == '\r') command.pop_back();

            ParsedCommand cmd = CommandParser::parseCommand(command);
            if (cmd.type.empty()) replyWithCode(clientSock, ReplyCode::SyntaxError, "");
            else CommandDispatcher::executeCommand(session, cmd);   // <-- pass session, not just socket

            inBuffer.erase(0, newlinePos + 1);
            if (session.socket == INVALID_SOCKET) return;  // QUIT already closed it
        }
    }
    if (session.socket != INVALID_SOCKET) closesocket(session.socket);
    printf("Client disconnected.\n");
}
```

### Step 1.4 — Update `CommandDispatcher`'s signature everywhere

```cpp
// CommandDispatcher.h
namespace CommandDispatcher {
    void executeCommand(ClientSession&, const ParsedCommand&);
    void quitSession(ClientSession&, const std::vector<std::string>&);
}
```

```cpp
// CommandDispatcher.cpp
void quitSession(ClientSession& s, const std::vector<std::string>& args) {
    Session::replyWithCode(s.socket, ReplyCode::Goodbye, "Service closing control connection.");
    shutdown(s.socket, SD_SEND);
    closesocket(s.socket);
    s.socket = INVALID_SOCKET;
}
```

---

## Phase 2 — Wire Authentication + Directory Commands (Owner: Member A, using your already-built `DirectoryService`)

This closes the exact Week 1 merge-point gap identified in the evaluation.

```cpp
// CommandDispatcher.cpp
#include "../common/Globals.h"

std::map<std::string, std::function<void(ClientSession&, const std::vector<std::string>&)>> commandMap = {

    { "USER", [](ClientSession& s, const std::vector<std::string>& args) {
        s.username = args.empty() ? "" : args[0];
        Session::replyWithCode(s.socket, ReplyCode::AuthNeedPass, "Username OK, need password.");
    }},

    { "PASS", [](ClientSession& s, const std::vector<std::string>&) {
        s.authenticated = true;   // Basic Level: no real credential check required
        Session::replyWithCode(s.socket, ReplyCode::LoggedIn, "User logged in.");
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
        Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, body); // send over control channel for now
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

    { "QUIT", [](ClientSession& s, const std::vector<std::string>& args) { quitSession(s, args); }},
};
```

**Note on LIST/NLST above:** sending the listing over the *control* channel (as shown) is a fine placeholder to get the merge-point criteria passing immediately. Per your project spec, LIST/NLST output for a real grade should go out over the *data* channel like RETR does — that upgrade happens naturally once Phase 4 (PASV/PORT) and Phase 5 (data transfer) are wired, so treat this as a Week-1-passable stub, not the final form.

---

## Phase 3 — Wire the Metadata & Mutation Commands (Owner: Member A, using your `getMetadata`/`deleteFile`/`renameFrom`/`renameTo`)

```cpp
{ "SIZE", [](ClientSession& s, const std::vector<std::string>& args) {
    DirectoryService::PathMetadata meta;
    if (args.empty() || !g_dirService.getMetadata(s.currentDir, args[0], meta) || meta.isDirectory) {
        Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not get file size.");
        return;
    }
    Session::replyWithCode(s.socket, 213, std::to_string(meta.sizeBytes));
}},

{ "MDTM", [](ClientSession& s, const std::vector<std::string>& args) {
    DirectoryService::PathMetadata meta;
    if (args.empty() || !g_dirService.getMetadata(s.currentDir, args[0], meta) || meta.isDirectory) {
        Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Could not get modification time.");
        return;
    }
    Session::replyWithCode(s.socket, 213, formatMdtmTimestamp(meta.lastModified)); // helper from earlier discussion
}},

{ "STAT", [](ClientSession& s, const std::vector<std::string>& args) {
    if (args.empty()) {
        Session::replyWithCode(s.socket, 211, "Server status: OK.");  // no-path case: control-plane info, Member A's own
        return;
    }
    DirectoryService::PathMetadata meta;
    if (!g_dirService.getMetadata(s.currentDir, args[0], meta)) {
        Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Path not found.");
        return;
    }
    std::string typeStr = meta.isDirectory ? "directory" : "file";
    Session::replyWithCode(s.socket, 213, args[0] + ": " + typeStr + ", " + std::to_string(meta.sizeBytes) + " bytes");
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
```

`formatMdtmTimestamp` is the small helper flagged as genuinely fiddly back when `PathMetadata` was designed — implement it now if it doesn't exist yet, since MDTM can't work without it:

```cpp
// common/TimeFormat.h — new small helper
#pragma once
#include <filesystem>
#include <string>
#include <chrono>

inline std::string formatMdtmTimestamp(std::filesystem::file_time_type ft) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    gmtime_s(&tm, &tt); // MSVC-safe variant
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm);
    return std::string(buf);
}
```

---

## Phase 4 — Wire PASV/PORT (Owner: Member A, calling your already-built handlers)

```cpp
{ "PASV", [](ClientSession& s, const std::vector<std::string>&) {
    SOCKET dataSock;
    unsigned short port;
    if (!openPassiveDataPort(dataSock, port)) {
        Session::replyWithCode(s.socket, ReplyCode::CantOpenDataConnection, "Could not open data port.");
        return;
    }
    s.pendingDataSocket = dataSock;
    s.dataChannelIsPassive = true;

    sockaddr_in localAddr{}; int len = sizeof(localAddr);
    getsockname(s.socket, (sockaddr*)&localAddr, &len); // control socket's local IP = server's IP
    uint32_t serverIp = ntohl(localAddr.sin_addr.s_addr);

    std::string body = formatPasvReply(serverIp, port);
    Session::replyWithCode(s.socket, 227, "Entering Passive Mode (" + body + ").");
}},

{ "PORT", [](ClientSession& s, const std::vector<std::string>& args) {
    sockaddr_in peerAddr{};
    if (args.empty() || !parsePortCommand(args[0], peerAddr)) {
        Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "Bad PORT argument.");
        return;
    }
    s.pendingPeerAddr = peerAddr;
    s.dataChannelIsPassive = false;
    Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, "PORT command successful.");
}},
```

**Team decision needed here (per your own build plan's open item):** whether to support both PORT and PASV live in the demo, or default to PASV only given Active mode's NAT fragility in a shared lab. Settle this with the team before Week 2 testing, not during the demo.

---

## Phase 5 — Wire STOR/RETR (Owner: Member A + Member B jointly — this is the real integration point)

**Before writing this code, hold the 15-minute interface confirmation your own `DataChannelSession.cpp` comments are waiting on:**
1. Does `RdtSender::sendChunk` returning `false` mean "gave up entirely" (abort transfer) or "still retrying" (current code assumes the former)?
2. Can `RdtReceiver::receiveNext` return `true` twice for the same `seqNum`, or is deduplication guaranteed before it returns (current code assumes the latter)?

Looking at Member B's actual code: `sendChunk` only returns `false` after `MAX_RETRIES` attempts — confirming assumption 1 is correct as written. `receiveNext` does not deduplicate — it returns whatever arrives and lets the caller's `ChunkedFileWriter` handle duplicates via its own idempotent `addChunk` — so assumption 2 is also fine, since your `chunkCount++` in `receiveFile` would only overcount if `receiveNext` returned `true` for a genuine retransmit duplicate. **This is worth a real test**, not just a read of the code — add the integration test in Step 5.3 below before trusting it live.

```cpp
{ "RETR", [](ClientSession& s, const std::vector<std::string>& args) {
    if (args.empty()) { Session::replyWithCode(s.socket, ReplyCode::SyntaxError, ""); return; }
    std::filesystem::path resolved;
    if (!g_pathResolver.resolve(s.currentDir, args[0], resolved) || !std::filesystem::exists(resolved)) {
        Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "File not found.");
        return;
    }

    char peerIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s.pendingPeerAddr.sin_addr, peerIp, sizeof(peerIp));
    unsigned short peerPort = ntohs(s.pendingPeerAddr.sin_port);

    RdtSender transport(peerIp, peerPort);
    DataChannelSession channel(s.pendingDataSocket, s.pendingPeerAddr, transport);

    Session::replyWithCode(s.socket, ReplyCode::FileStatusOkay, "Opening data connection.");
    bool ok = channel.sendFile(resolved);
    Session::replyWithCode(s.socket, ok ? ReplyCode::TransferComplete : ReplyCode::ActionNotTaken,
                            ok ? "Transfer complete." : "Transfer failed.");
}},

{ "STOR", [](ClientSession& s, const std::vector<std::string>& args) {
    if (args.empty()) { Session::replyWithCode(s.socket, ReplyCode::SyntaxError, ""); return; }
    std::filesystem::path resolved;
    if (!g_pathResolver.resolve(s.currentDir, args[0], resolved)) {
        Session::replyWithCode(s.socket, ReplyCode::ActionNotTaken, "Path outside server root.");
        return;
    }

    RdtReceiver transport(/* the port this session's PASV/PORT actually bound */);
    DataChannelSession channel(s.pendingDataSocket, s.pendingPeerAddr, transport);

    Session::replyWithCode(s.socket, ReplyCode::FileStatusOkay, "Opening data connection.");
    bool ok = channel.receiveFile(resolved);
    Session::replyWithCode(s.socket, ok ? ReplyCode::TransferComplete : ReplyCode::ActionNotTaken,
                            ok ? "Transfer complete." : "Transfer failed.");
}},
```

**One real gap to flag to the team here:** `RdtSender`'s constructor takes a target IP/port at construction, but for PASV mode the server *already has* a bound socket (`s.pendingDataSocket`) it should reuse rather than opening a second one inside `RdtSender`'s constructor. This is exactly the "who owns the socket" ambiguity flagged in `DataChannelSession.h`'s own comments — resolve it with Member B now: either give `RdtSender`/`RdtReceiver` an overload/constructor that accepts an already-open `SOCKET`, or accept that `PassiveModeHandler`'s socket goes unused and `RdtSender` opens its own — but pick one deliberately rather than shipping both by accident.

### Step 5.1 — Wire the HASH command too (currently unbuilt by anyone)

```cpp
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
    Session::replyWithCode(s.socket, 213, "SHA-256 " + hash);
}},
```

### Step 5.2 — Wire TYPE (currently unbuilt by anyone, needed for ASCII/Binary mode)

```cpp
{ "TYPE", [](ClientSession& s, const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "A" && args[0] != "I")) {
        Session::replyWithCode(s.socket, ReplyCode::SyntaxError, "TYPE must be A or I.");
        return;
    }
    // s.transferModeBinary = (args[0] == "I");  // add this field to ClientSession
    Session::replyWithCode(s.socket, ReplyCode::ActionCompleted, "Type set to " + args[0] + ".");
}},
```

### Step 5.3 — Add the missing sender↔receiver integration test (Owner: Member B, before building further)

```cpp
// tests/test_rdt_integration.cpp — real socket, not just struct round-tripping
void test_sender_receiver_over_loopback() {
    RdtReceiver receiver(9999);
    std::thread receiverThread([&]() {
        uint32_t seq; std::vector<char> data; bool isFinal;
        assert(receiver.receiveNext(seq, data, isFinal));
        assert(seq == 0);
        assert(std::string(data.begin(), data.end()) == "hello");
    });

    RdtSender sender("127.0.0.1", 9999);
    assert(sender.sendChunk(0, "hello", 5) == true);
    receiverThread.join();
    std::cout << "[PASS] RdtSender <-> RdtReceiver over real loopback UDP\n";
}
```

Add this to `CMakeLists.txt` alongside the existing `test_rdt` target. This is the test your Week 1 merge point actually asked for and doesn't yet exist.

---

## Phase 6 — Start the Client (Owner: Member A for `main.cpp`/`cli/`/`control/`; Member B for `rdt/` duplication; Member C for `datachannel/`/`fileio/`/`crypto/` duplication)

`client/` is currently 0% built. This needs to start now, in parallel with the server-side wiring above, since Week 2's merge point (*"Full RETR/STOR works client↔server"*) needs both sides.

**Suggested minimal split to start immediately:**

- **Member A:** `client/src/main.cpp` + `client/src/control/ControlConnection.h/.cpp` — a TCP client that connects, sends a command line, reads back one reply line. This mirrors the server bootstrap from your own `MemberA_Guide.md` Step 0, just as a client instead.
- **Member C (you):** copy `ChunkedFileReader`/`ChunkedFileWriter`/`Sha256Hasher` unchanged into `client/src/fileio/` and `client/src/crypto/` (per `ExpectedLayout.md`, these are deliberately duplicated, not shared, given your team's earlier decision that a shared `core/` library isn't worth the CMake complexity for a 3-person team). Then build `client/src/datachannel/ActiveModeClient.h/.cpp` and `PassiveModeClient.h/.cpp` — the client-side mirror of your server's `PassiveModeHandler`/`ActiveModeHandler`, but with connect-direction reversed (client connects out for PASV, client binds/listens for PORT).
- **Member B:** copy `RdtHeader`/`CheckSum`/`RdtSender`/`RdtReceiver` into `client/src/rdt/` unchanged (same duplication reasoning).

This can start in parallel with Phases 1-5 above — it has no dependency on the server-side wiring being finished first.

---

## Phase 7 — Verify, Then Move Into Week 2's Chaos Checklist

1. Build `ftp_server`, run it, connect with raw `telnet 127.0.0.1 4567` (or PuTTY raw mode), and manually confirm the exact Week 1 merge-point sequence: `USER x` → `331`, `PASS y` → `230`, `PWD` → `257`. This retroactively closes the Week 1 gap.
2. Confirm `PWD`/`CWD`/`MKD`/`RMD`/`LIST` all behave correctly against a real `server_root/` folder with some test files/subfolders placed in it.
3. Once STOR/RETR work over loopback (client and server both running on the same machine), run your team's Week 2 chaos checklist: inject 15% artificial UDP packet loss in `sendRawPacket`, confirm the file still arrives byte-identical; inject 200-400ms latency, confirm the timeout doesn't fire spuriously; flip one bit in ~5% of payloads, confirm the checksum catches it and triggers a silent-drop-and-retransmit rather than corrupting the file.
4. Only after 1-3 pass, move into Week 2's sliding-window/Selective-Repeat upgrade for Member B, and the client-side build-out from Phase 6.