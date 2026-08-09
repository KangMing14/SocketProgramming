# GenAI Usage & Code Refinement Log — Lam Hong Hai Hoang Le

## Entry 1

**Issue identified:** Without initial knowledge of TCP projects, it was difficult to start writing the Control segment.

**Prompt (verbatim):** "My Hybrid TCP project has a folder called "server" and in it are the files main.cpp and subfolder "control" with Session.h, CommandParser.h, CommandDispatcher.h. What should be included in these files?"

**Raw AI Output (Summary):**
- The control/ subfolder focuses exclusively on TCP session management and command processing before handoff to the UDP data channel.
- ``control/Session.h``: Represents a single connected client's control state over TCP. It holds the active SOCKET handle, tracks client authentication state, and manages current transfer state (e.g., active file being uploaded/downloaded).
- ``control/CommandParser.h``: Takes raw bytes received from Session::tcpSocket and deserializes/parses them into structured C++ data objects (or enums). It protects the server against malformed or truncated packets.
- ``control/CommandDispatcher.h``: The engine of the control channel. It takes a ParsedCommand and the active Session, routes it to the appropriate handler method, and executes the server-side logic (e.g., verifying credentials, allocating a UDP port for transfer, sending a response packet back over TCP).
- ``main.cpp``: The server entry point. It handles WSAStartup, creates a main TCP listener socket, accepts incoming client connections, creates a Session object for each client, and spins off worker threads to process commands using CommandParser and CommandDispatcher.

**Refinement / problem-solving:** Using suggestions from the AI and researching more about it online, a basic control was formed. Code suggested by AI was discarded due to not fitting the requirement of the prepared guide.

## Entry 2

**Issue identified:** The client was sending a command after every keystroke

**Prompt (verbatim):** Stop client socket from sending every keystroke

**Raw AI Output:** 
```cpp
#include <string>

std::string incomingBuffer;
char tempBuffer[512];

while (true) {
    int bytesReceived = recv(clientSock, tempBuffer, sizeof(tempBuffer), 0);
    if (bytesReceived <= 0) break; // Client disconnected or error

    // 1. Append newly received bytes to our session string buffer
    incomingBuffer.append(tempBuffer, bytesReceived);

    // 2. Extract and process all complete commands ending in '\n'
    size_t newlinePos;
    while ((newlinePos = incomingBuffer.find('\n')) != std::string::npos) {
        // Extract the complete command (and trim potential trailing '\r')
        std::string command = incomingBuffer.substr(0, newlinePos);
        if (!command.empty() && command.back() == '\r') {
            command.pop_back();
        }

        // 3. Process the full command string
        printf("Received full command: %s\n", command.c_str());
        
        // Echo back or run parser ONLY for complete commands
        replyWithCode(clientSock, ReplyCode::ActionCompleted);

        // Remove the processed command from the session buffer
        incomingBuffer.erase(0, newlinePos + 1);
    }
}
```

**Refinement / problem-solving:** I implemented the suggestion and reformatted the code to fit with the existing code.

## Entry 3

**Issue identified:** How the client should handle more arguments than needed was unknown.

**Prompt (verbatim):** What should happen when an FTP command has extra arguments?

**Raw AI Output (Summary):** Three approaches were given:
- Approach 1: Reject with a Syntax Error (Recommended for Custom/Hybrid Protocols)
- Approach 2: Ignore Extra Arguments (Legacy Standard FTP / RFC 959 Style)
- Approach 3: Approach 3: Space-in-Filename Special Case (Quotations / Trailing Payload)

**Refinement / problem-solving:** Approach 1 was chosen, so extra arguments will be rejected with a syntax error.

## Entry 4

**Issue identified:** The QUIT command doesn't properly quit the client session.

**Prompt (verbatim):** Check if client socket is closed after sending the quit command which runs
void quitSession(ClientSession& s, const std::vector<std::string>& args){
    Session::replyWithCode(s.socket, ReplyCode::Goodbye, "Service closing control connection.");
    shutdown(s.socket, SD_SEND);
    closesocket(s.socket);
    s.socket = INVALID_SOCKET;
} 

**Raw AI Output (Summary):** The socket is closed immediately on the server side when quitSession finishes, 
but there is a subtle bug in the connection loop that causes an unnecessary extra recv() attempt (and potential error logging) when the client disconnects.
The suggestion was to pass ``SOCKET&`` by reference to ``handleClient`` and making sure ``ClientSession`` holds a reference or check ``s.socket`` at the end of every command execution loop.

**Refinement / problem-solving:** The suggestion was implemented into the system and the QUIT command properly exits the client.

## Entry 5

**Issue identified:** The HELP command provides a multiline response, but the system can only receive single line responses.

**Prompt (verbatim):** Function to convert a string into a multiline response

**Raw AI Output (Code):**
```cpp
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

std::string format_ftp_multiline(int code, const std::string& input_text) {
    std::stringstream ss(input_text);
    std::string line;
    std::vector<std::string> lines;

    // Split input text by newlines
    while (std::getline(ss, line)) {
        // Strip trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    // Handle empty input case
    if (lines.empty()) {
        return std::to_string(code) + " OK\r\n";
    }

    std::string code_str = std::to_string(code);
    
    // Single-line response
    if (lines.size() == 1) {
        return code_str + " " + lines[0] + "\r\n";
    }

    // Multi-line response (RFC 959 compliant)
    std::string result = "";
    
    // Intermediate lines use hyphen 'code-'
    for (size_t i = 0; i < lines.size() - 1; ++i) {
        result += code_str + "-" + lines[i] + "\r\n";
    }
    
    // Final line uses space 'code '
    result += code_str + " " + lines.back() + "\r\n";

    return result;
}

int main() {
    std::string help_text = 
        "The following commands are recognized:\n"
        "USER  PASS  PORT  PASV\n"
        "QUIT  HELP  NOOP  EPRT\n"
        "Direct comments to admin@example.com.";

    std::string ftp_response = format_ftp_multiline(214, help_text);
    
    std::cout << "--- Raw Output Sent to Wire ---" << std::endl;
    std::cout << ftp_response;

    return 0;
}
```

**Refinement / problem-solving:** The current replyWithCode function was duplicated and modified to use a similar method as what AI suggested.

## Entry 6

**Issue identified:** Of all the commands available, the ones that don't need authentication was unknown.

**Prompt (verbatim):**  of the commands "USER   PASS    QUIT    NOOP    PWD     CWD     CDUP    MKD \n"
"RMD    LIST    NLST    STAT    SIZE    MDTM    TYPE    MODE\n"
 "PORT   PASV    RETR    STOR    STOU    APPE    DELE    RNFR\n"
"RNTO   HASH    ABOR    HELP", which do not need authentication?

**Raw AI Output (Summary):** 
1. Primary Authentication Commands
USER: Used to specify the username for login.
PASS: Used to supply the password following the USER command.
2. Informational & Utility Commands
HELP: Displays server help information or command syntax.
NOOP: "No Operation"—used by clients to check server responsiveness or keep the connection alive.
QUIT: Closes the FTP control connection.

**Refinement / problem-solving:** Only the five commands in the answer do not have an authentication check for them.

## Entry 7

**Issue identified:** Logging without race conditions has not been implemented.

**Prompt (verbatim):** Implement a function that tracks connected clients using std::mutex.

**Raw AI Output (Code):**
```cpp
#pragma once

#include <winsock2.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

// Tracks currently-connected control sessions. This is the ONE piece of
// state in the whole server that's genuinely shared and mutated across
// client threads — everything else (ClientSession) lives on a single
// thread's stack and needs no locking at all.
namespace ClientRegistry {
    struct ClientInfo {
        std::string address;   // "ip:port" of the control connection
        std::string username;  // empty until USER is sent
    };

    inline std::mutex registryMutex;
    inline std::unordered_map<SOCKET, ClientInfo> clients;

    inline void addClient(SOCKET socket, const std::string& address) {
        std::lock_guard<std::mutex> lock(registryMutex);
        clients[socket] = ClientInfo{address, ""};
    }

    inline void removeClient(SOCKET socket) {
        std::lock_guard<std::mutex> lock(registryMutex);
        clients.erase(socket); // no-op if already removed — safe to call twice
    }

    inline void setUsername(SOCKET socket, const std::string& username) {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = clients.find(socket);
        if (it != clients.end()) it->second.username = username;
    }

    inline size_t count() {
        std::lock_guard<std::mutex> lock(registryMutex);
        return clients.size();
    }

    inline std::vector<ClientInfo> snapshot() {
        std::lock_guard<std::mutex> lock(registryMutex);
        std::vector<ClientInfo> result;
        result.reserve(clients.size());
        for (const auto& [socket, info] : clients) result.push_back(info);
        return result;
    }
}
```
```cpp
void handleClient(SOCKET clientSock){
        ClientSession session;
        session.socket = clientSock;
        session.currentDir = g_pathResolver.root();
        int peerLength = sizeof(session.controlPeerAddr);
        getpeername(clientSock,
                    reinterpret_cast<sockaddr*>(&session.controlPeerAddr),
                    &peerLength);

        // NEW: register this session in the shared connected-clients table
        char peerIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &session.controlPeerAddr.sin_addr, peerIp, sizeof(peerIp));
        std::string peerAddress = std::string(peerIp) + ":" +
            std::to_string(ntohs(session.controlPeerAddr.sin_port));
        ClientRegistry::addClient(clientSock, peerAddress);

        // RAII: guarantees removal on EVERY exit path below — normal loop
        // end, QUIT's early return, or an abrupt disconnect — without
        // needing to remember to call removeClient() at each return site.
        struct RegistryGuard {
            SOCKET socket;
            ~RegistryGuard() { ClientRegistry::removeClient(socket); }
        } registryGuard{clientSock};

        replyWithCode(clientSock, ReplyCode::ServiceReady, "Service ready.");

        // ... rest of the function is unchanged ...
}
```

**Refinement / problem-solving:** The ClientRegistry function was implemented cleanly without any conflicts with existing functionalities, 
and logging for client connection/disconnection was implemented based on ClientRegistry.