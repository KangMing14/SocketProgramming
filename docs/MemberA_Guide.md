# Member A Survival Guide — Control & Concurrency Lead
**Scope:** TCP Control Channel, Session State Machine, Authentication, Multi-threaded Server Core, Thread Safety

---

## 1. What to Learn First (Prerequisite Checklist)

You don't need to know all of networking or all of C++. You need these specific pieces, in this order.

**1. What a socket actually is**
Think of a socket as a phone handset. `socket()` picks up a new handset. `bind()` writes your own phone number on it. `listen()` puts it in "ready to receive calls" mode. `accept()` is the act of physically answering a specific incoming call — and crucially, it hands you back a **brand new handset** (a new socket) dedicated to that one caller, while your original handset stays free to answer the next call. Beginners often think `accept()` reuses the listening socket — it doesn't. That distinction is the whole reason multi-client servers work at all.

**2. Blocking vs non-blocking, and why your thread "freezes"**
By default, `recv()` and `accept()` **block** — your program stops on that line and waits, like standing at a door until someone knocks. That's fine *inside a thread dedicated to one client*, because you want that thread to just wait for that client. It's only a problem on your *main* thread, where blocking on one client would freeze the whole server. This is exactly why you need one thread per client.

**3. `std::thread` — a worker you hire, not a function you call**
A normal function call says "do this, and I'll wait." `std::thread t(myFunction, args)` says "go hire someone to do this while I keep working." The thread runs independently until it finishes. You must either `t.join()` (wait for it to finish before continuing) or `t.detach()` (let it run off on its own, you never check on it again). For a server accepting unknown numbers of clients, `detach()` is simplest to start with — each client thread cleans up after itself when the client disconnects.

**4. Mutex — a single bathroom key**
If two threads both write to the same shared data (e.g. a "list of connected clients" for your log) at the same time, you get corrupted data — not a crash necessarily, just silently wrong data, which is worse. A `std::mutex` is like a bathroom with one key: `mutex.lock()` = take the key (wait if someone else has it), `mutex.unlock()` = return the key. Anyone without the key must wait outside. `std::lock_guard<std::mutex> lock(myMutex);` takes the key automatically and returns it automatically when the current `{ }` block ends — even if an exception is thrown. Always prefer this over manual `lock()`/`unlock()`.

**5. `std::string` parsing basics**
FTP commands arrive as text like `"USER bob\r\n"`. You need `std::string::find()`, `substr()`, and stream-based splitting (`std::istringstream`) to pull out the verb (`USER`) and argument (`bob`). Nothing fancy — this is the same string-splitting you've done in intro programming, just applied to network text.

**6. Byte-ordering basics (you'll use this less than Member B, but need to understand it)**
Different computers can store multi-byte numbers with the most significant byte first or last. Network protocols agree to always use "big-endian" (`htons`/`htonl` convert your machine's format to network format before sending; `ntohs`/`ntohl` convert back after receiving). You mostly won't touch this directly — Member B owns the UDP header — but you must understand it exists, because the professor may ask you to explain the overall data flow.

---

## 2. Step-by-Step Implementation Playbook

### Step 0 — Get a bare Winsock2 TCP echo working (before any FTP logic)
```cpp
// server_bootstrap.cpp — run this FIRST, alone, before building anything else.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsaData;
    // Every Winsock program must call this once before using any socket function.
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;   // listen on all local interfaces
    serverAddr.sin_port = htons(4567);         // control channel port

    bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listenSock, SOMAXCONN);             // start accepting connection attempts

    printf("Server listening on port 4567...\n");

    while (true) {
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSock == INVALID_SOCKET) continue; // one bad accept shouldn't kill the server

        char welcomeMsg[] = "220 Service ready\r\n";
        send(clientSock, welcomeMsg, (int)strlen(welcomeMsg), 0);
        closesocket(clientSock); // for now: close immediately, no threading yet
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}
```
**Get this compiling and connecting via `telnet 127.0.0.1 4567` before moving on.** If you can't see `220 Service ready`, nothing else in this project will work — debug this first, in isolation.

### Step 1 — Add threading (still no real FTP commands yet)
```cpp
#include <thread>

void handleClient(SOCKET clientSock) {
    char welcomeMsg[] = "220 Service ready\r\n";
    send(clientSock, welcomeMsg, (int)strlen(welcomeMsg), 0);

    char buffer[512];
    while (true) {
        int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) break;          // 0 = client closed, <0 = error
        buffer[bytesReceived] = '\0';
        printf("Received: %s", buffer);
        // Echo back for now — real command parsing comes in Step 2
        send(clientSock, "200 OK\r\n", 8, 0);
    }
    closesocket(clientSock);
    printf("Client disconnected.\n");
}

// In your accept loop, replace the direct handling with:
std::thread(handleClient, clientSock).detach();
```
**Test:** open two `telnet` windows at once. Both should get independent `220` welcomes and independent echo responses. If one freezes while you type in the other, threading isn't working — stop and fix this before adding commands.

### Step 2 — Command parsing + reply codes
```cpp
struct ParsedCommand {
    std::string verb;   // e.g. "USER", always uppercase
    std::string arg;    // e.g. "bob", may be empty
};

ParsedCommand parseCommand(const std::string& line) {
    ParsedCommand cmd;
    std::istringstream iss(line);
    iss >> cmd.verb;
    std::getline(iss, cmd.arg);
    // trim leading space and trailing \r\n
    if (!cmd.arg.empty() && cmd.arg[0] == ' ') cmd.arg.erase(0, 1);
    while (!cmd.arg.empty() && (cmd.arg.back() == '\r' || cmd.arg.back() == '\n'))
        cmd.arg.pop_back();
    for (auto& c : cmd.verb) c = toupper(c);
    return cmd;
}
```

### Step 3 — Session state machine
Give every client thread its own `Session` struct (NOT global/shared — this is what makes it thread-safe by construction):
```cpp
struct Session {
    SOCKET controlSocket;
    std::string username;
    bool loggedIn = false;
    std::filesystem::path currentDir = "server_root";
    // later: data mode (PORT/PASV), transfer type (A/I)
};
```
Route based on `cmd.verb` with a simple `if`/`else if` chain (or `std::unordered_map<std::string, HandlerFunc>` once you have >10 commands — cleaner but don't over-engineer Week 1).

### Step 4 — Shared state, guarded by a mutex
Only the connected-clients log table needs a lock, because it's the only thing every thread touches:
```cpp
std::mutex clientsMutex;
std::vector<std::string> connectedClients; // e.g. IP:port strings, for your log

void addClient(const std::string& info) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    connectedClients.push_back(info);
}
void removeClient(const std::string& info) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    connectedClients.erase(std::remove(connectedClients.begin(), connectedClients.end(), info), connectedClients.end());
}
```

### Weekly sequence
- **Week 1:** Steps 0-3 above. End of week: multiple clients can `USER`/`PASS`/`PWD`/`QUIT` concurrently.
- **Week 2:** Wire in PORT/PASV command *parsing* (actual socket opening is joint work with Member C); harden thread lifecycle (what happens on abrupt disconnect mid-transfer — needs a way to signal the data-thread to stop, e.g. an `std::atomic<bool> abort_flag` in the session).
- **Week 3:** Full command coverage, logging, and — importantly — **read every line of your own code out loud to a teammate** before the demo. If you can't explain a line, rewrite it simpler.

---

## 3. Beginner Pitfalls & How to Avoid Them

**Pitfall 1: Forgetting `\r\n` and using `\n` only, or not stripping it, causing string comparisons to silently fail.**
FTP text protocol requires `\r\n` line endings on the wire per the spec you're implementing against, but your parsing must strip both before comparing (`cmd.verb == "USER"` will silently fail forever if `cmd.verb` is actually `"USER\r"`). Debug by printing the string length and each byte (`printf("[%d]", (int)c)`) when a comparison mysteriously fails — you'll immediately spot the stray `\r`.

**Pitfall 2: Deadlock from locking a mutex you already hold.**
If `addClient()` locks `clientsMutex` and, inside that same lock, calls another function that *also* tries to lock `clientsMutex`, your thread waits forever for a key it's already holding. Rule: keep lock scope as small as possible, and never call a function that might lock the same mutex while you're inside a `lock_guard` block for it. In Visual Studio, if the server appears to "hang" with no crash, pause the debugger (Debug → Break All) and check each thread's call stack — a thread stuck inside two nested locks on the same mutex is the signature of this bug.

**Pitfall 3: Detaching threads and not tracking socket cleanup, causing resource leaks that eventually crash the server under the stress test.**
Every `SOCKET` you open must eventually get `closesocket()`'d, even (especially) on error paths. A detached thread that `return`s early on an error without closing its socket leaks a handle; leak enough under your teammates' concurrent stress test and `socket()` starts failing with `WSAENOBUFS`. Fix: wrap client handling in a structure where `closesocket()` happens on **every** exit path — easiest is a small RAII wrapper class whose destructor calls `closesocket()`, so it's automatic no matter how the function returns.

---

## 4. Oral Defense (Viva Voce) Mastery Kit

**Script Template 1 — "Explain your accept loop"**
> "My main thread calls `accept()` in a loop, which blocks until a TCP three-way handshake completes with a new client. Each successful `accept()` returns a *new* socket descriptor scoped to that specific client's 4-tuple — source IP, source port, dest IP, dest port — while the original listening socket stays free to accept the next connection. I immediately hand that new socket to a `std::thread`, so the accept loop never blocks on any individual client's I/O."

**Script Template 2 — "Explain your thread safety"**
> "Each client's session state — username, current directory, login status — lives entirely inside that client's own thread stack, so there's no sharing and no race condition possible there by construction. The only genuinely shared mutable state across threads is [X — e.g. the connected-clients log]. I protect that with a `std::mutex` using RAII `lock_guard`, so the lock is always released even if an exception unwinds the stack, and I keep the locked critical section to just the container mutation — no I/O or blocking calls happen while holding the lock, which is what prevents contention from becoming a bottleneck under concurrent load."

**Script Template 3 — "Explain what happens on a malformed or malicious command"**
> "Every line the client sends is parsed into a verb and argument with bounds-checked string operations before any filesystem or socket action happens. Unknown verbs get a `502 Command not implemented`; verbs used out of session state — like `RETR` before login — get a `530 Not logged in`. For path arguments specifically, I resolve them against the session's current directory using `std::filesystem::canonical` and verify the result is still inside the server root, to reject `../../` traversal attempts before touching disk."

**Whiteboard Diagram Concept:** Draw the **thread lifecycle timeline** for two simultaneous clients side by side: a vertical timeline for "Main Thread" showing repeated `accept()` calls as dots, with arrows branching off each dot into a new horizontal lane labeled "Client A Thread" / "Client B Thread," each lane showing its own sequence of `recv → parse → session state → send` boxes, and one shared box off to the side labeled "clientsMutex-protected log" with dashed lines from both lanes touching it only briefly. This single diagram proves you understand isolation (why races don't happen in session state) and the one place they can (the shared log) — which is precisely the two things the rubric wants to hear in the Theoretical Understanding section.
