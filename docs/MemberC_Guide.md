# Member C Survival Guide — Data & Filesystem Lead
**Scope:** Binary File I/O Chunking, Cryptographic Hashing (Windows CNG/bcrypt), Directory Traversal/Management, Active PORT / Passive PASV Socket Routing

---

## 1. What to Learn First (Prerequisite Checklist)

**1. Text mode vs binary mode file I/O — the silent corruption trap**
C++ streams can open files in "text mode" or "binary mode." In text mode on Windows, the runtime silently translates byte sequences (like turning `\n` into `\r\n` or treating certain byte values specially) — which is fine for `.txt` files but **destroys** images, videos, and archives, because those formats use arbitrary byte values including ones that look like line-ending characters but aren't. The fix is simple but easy to forget: always open with `std::ios::binary` for file transfers, no exceptions. Think of text mode as a translator who "helpfully" rewrites your postcard's contents without asking — fine for casual notes, ruinous for a photograph.

**2. `std::filesystem` — a supervisor for paths, not just strings**
Treating file paths as plain strings you concatenate (`cwd + "/" + userInput`) is how path-traversal bugs happen. `std::filesystem::path` understands what a path segment, parent directory (`..`), and root actually mean, and gives you safe operations like `canonical()` (resolve `..`/`.` and symlinks into an absolute, normalized path) and comparisons to check "is this path still inside the directory I expect?" Treat it as a border-control officer for your filesystem, not a formatting convenience.

**3. Fixed-size chunking — why 1024 bytes at a time, and why the last chunk is different**
Reading a whole file into memory works for small demos but breaks for large files and doesn't match how you'll actually send it over UDP (Member B's packets have a max payload size). You read the file in a loop, a fixed chunk size at a time, and the **very last chunk is almost always smaller than the chunk size** — your loop must handle "partial last read" correctly (check the actual bytes returned, don't assume every chunk is full).

**4. What a cryptographic hash actually proves**
A hash function (MD5/SHA-256) takes an entire file's bytes and produces a short fixed-length fingerprint. The property you care about: if even a single bit differs anywhere in the file, the fingerprint comes out completely different. You compute the hash **before** sending (on the original file) and **after** receiving (on the reassembled file) — if they match, you have strong evidence the file arrived byte-for-byte intact, even after a lossy UDP channel and retransmissions. It doesn't prevent corruption; it detects it after the fact, which is exactly the guarantee your rubric's HASH command needs to provide.

**5. Windows CNG (`bcrypt.h`) — using an OS-provided crypto library, not writing SHA-256 by hand**
Windows ships a "Cryptography API: Next Generation" that any program can call into (`bcrypt.lib`) — this is part of the OS, not a "third-party library" in the sense your project spec bans (that ban targets things like OpenSSL, libcurl, or ready-made FTP frameworks you'd otherwise have to explain you didn't write). Using `BCryptHashData` means you get a correct, fast, well-tested hash implementation, but you must still be able to explain the *algorithm* it — sequence of hash-then-compare — not just the API calls, in the viva. **Confirm this interpretation with your instructor before relying on it**, since "no third-party libraries" wording can be read strictly by some graders.

**6. Active (PORT) vs Passive (PASV) — who calls whom**
In Active mode, the client says "here's *my* IP and port, you (server) connect to me" — the server initiates the data connection outward. In Passive mode, the client says nothing about itself; instead the *server* opens a port and says "here's where to reach *me*," and the client connects to the server. The practical reason both exist in real FTP: firewalls/NAT often block unsolicited inbound connections to a client, so Passive mode (client always initiates) is more NAT-friendly — a genuinely useful thing to say in the viva, not just a memorized definition.

---

## 2. Step-by-Step Implementation Playbook

### Step 0 — Binary file read/write in exact chunks, tested standalone (no networking yet)
```cpp
#include <fstream>
#include <vector>
#include <cstdio>

constexpr size_t CHUNK_SIZE = 1024;

// Returns true while there is more data; fills 'chunk' and 'bytesRead' each call.
bool readNextChunk(std::ifstream& file, std::vector<char>& chunk, size_t& bytesRead) {
    chunk.resize(CHUNK_SIZE);
    file.read(chunk.data(), CHUNK_SIZE);
    bytesRead = static_cast<size_t>(file.gcount()); // gcount() = actual bytes read, NOT always CHUNK_SIZE
    return bytesRead > 0;
}

void copyFileInChunks(const std::string& srcPath, const std::string& dstPath) {
    std::ifstream in(srcPath, std::ios::binary);   // std::ios::binary is non-negotiable
    std::ofstream out(dstPath, std::ios::binary);
    if (!in || !out) { printf("Failed to open files\n"); return; }

    std::vector<char> chunk;
    size_t bytesRead;
    size_t totalBytes = 0;
    while (readNextChunk(in, chunk, bytesRead)) {
        out.write(chunk.data(), bytesRead);        // write EXACTLY bytesRead, not chunk.size()
        totalBytes += bytesRead;
    }
    printf("Copied %zu bytes\n", totalBytes);
}
```
**Test this on a real image or .zip file first, then diff the original and copy byte-for-byte (e.g. `fc /b` on Windows) before touching sockets at all.** If this step has a bug, everything built on top of it will produce corrupted files that look like a networking problem but aren't.

### Step 1 — Directory operations with `std::filesystem`, including the traversal-safety check
```cpp
#include <filesystem>
namespace fs = std::filesystem;

const fs::path SERVER_ROOT = fs::absolute("server_root");

// Resolves a client-supplied path against the session's current dir, and REJECTS
// anything that would escape SERVER_ROOT (the ../../ attack).
bool resolveSafePath(const fs::path& currentDir, const std::string& userInput, fs::path& outPath) {
    fs::path candidate = currentDir / userInput;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(candidate, ec); // normalizes .. and . without requiring existence
    if (ec) return false;

    // mismatch() finds the first differing element between the two path element sequences;
    // if SERVER_ROOT's elements are ALL a prefix of resolved's elements, resolved is safely inside.
    auto rootEnd = SERVER_ROOT.end();
    auto [rootIt, resolvedIt] = std::mismatch(SERVER_ROOT.begin(), rootEnd, resolved.begin(), resolved.end());
    if (rootIt != rootEnd) return false; // resolved escaped SERVER_ROOT

    outPath = resolved;
    return true;
}

void listDirectory(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        auto size = entry.is_regular_file() ? fs::file_size(entry) : 0;
        printf("%s\t%s\t%llu bytes\n",
               entry.is_directory() ? "DIR" : "FILE",
               entry.path().filename().string().c_str(),
               (unsigned long long)size);
    }
}
```
**Test the traversal check with deliberately hostile input** (`"../../../../windows/system32"`, `"..\\..\\secret"`) before you ever wire this to a live CWD/LIST command — this is a real vulnerability, not just an academic checkbox, and it's the single most common thing examiners try live.

### Step 2 — Hashing with Windows CNG
```cpp
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

// Computes SHA-256 of a file, returns hex string. Error handling omitted for brevity —
// add real checks on every BCrypt* return value before this goes in your final code.
std::string sha256File(const std::string& path) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);

    DWORD hashObjSize = 0, dataSize = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjSize, sizeof(DWORD), &dataSize, 0);
    std::vector<UCHAR> hashObj(hashObjSize);

    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(DWORD), &dataSize, 0);
    std::vector<UCHAR> hashResult(hashLen);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0);

    std::ifstream file(path, std::ios::binary);
    std::vector<char> chunk(CHUNK_SIZE);
    while (file.read(chunk.data(), CHUNK_SIZE) || file.gcount() > 0) {
        BCryptHashData(hHash, (PUCHAR)chunk.data(), (ULONG)file.gcount(), 0);
    }
    BCryptFinishHash(hHash, hashResult.data(), hashLen, 0);

    char hex[65] = {0};
    for (DWORD i = 0; i < hashLen; i++) sprintf_s(hex + i * 2, 3, "%02x", hashResult[i]);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return std::string(hex);
}
```
**Test:** hash a known file, change one byte with a hex editor, hash again — confirm the output is completely different, not just slightly different (that "avalanche effect" is the property worth explaining in the viva).

### Step 3 — PASV: server opens an ephemeral UDP port
```cpp
// Called when server receives a PASV command from a client
bool openPassiveDataPort(SOCKET& outDataSock, unsigned short& outPort) {
    outDataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // port 0 = "OS, please pick any free port for me"

    if (bind(outDataSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;

    sockaddr_in actual{};
    int len = sizeof(actual);
    getsockname(outDataSock, (sockaddr*)&actual, &len); // read back which port the OS actually chose
    outPort = ntohs(actual.sin_port);
    return true;
}
// Then format the 227 reply as: h1,h2,h3,h4,p1,p2 where p1 = port>>8, p2 = port & 0xFF
```

### Step 4 — PORT: client tells server where to send, server connects out
```cpp
// Parses "192,168,1,5,200,10" into an address the server will send the data channel to
bool parsePortCommand(const std::string& arg, sockaddr_in& outAddr) {
    int h1, h2, h3, h4, p1, p2;
    if (sscanf_s(arg.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return false;
    char ipStr[16];
    sprintf_s(ipStr, "%d.%d.%d.%d", h1, h2, h3, h4);
    outAddr.sin_family = AF_INET;
    outAddr.sin_port = htons(static_cast<unsigned short>(p1 * 256 + p2));
    inet_pton(AF_INET, ipStr, &outAddr.sin_addr);
    return true;
}
```

### Weekly sequence
- **Week 1:** Steps 0-1, entirely standalone — no sockets touched yet. Prove chunked copy is byte-perfect and traversal rejection works against hostile input.
- **Week 2:** Wire chunked read/write into Member B's RDT sender/receiver for real STOR/RETR; Step 3/4 (PORT/PASV) joint work with Member A.
- **Week 3:** Step 2 (hashing), wired so HASH runs automatically after transfer completion and the result is compared/reported; edge cases (empty files, files exactly divisible by chunk size, deeply nested directories).

---

## 3. Beginner Pitfalls & How to Avoid Them

**Pitfall 1: Opening files without `std::ios::binary`, silently corrupting non-text files.**
Symptom: text files transfer perfectly, but images/zips arrive slightly different size or fail to open at all, and it looks like a networking bug when it's actually a file-open-mode bug. Always grep your own code for every `ifstream`/`ofstream` construction and confirm `std::ios::binary` is present on every single one used for transfer — no exceptions, even "just for testing."

**Pitfall 2: Using `chunk.size()` instead of the actual bytes read/received when writing the last (partial) chunk.**
If you write a fixed `CHUNK_SIZE` on every write regardless of how many bytes were actually valid in that chunk, the final chunk of every file gets padded with garbage/leftover buffer contents, producing a file that's slightly larger than the original and fails the hash check. Always track and use the *actual* byte count (`file.gcount()` on read, `payload_len` from Member B's header on receive) for every write, never the buffer's allocated size.

**Pitfall 3: Path traversal — trusting client-supplied paths without normalizing and boundary-checking them.**
If `CWD ../../../../etc` or a Windows-equivalent hostile path is naively concatenated onto your current directory without resolving `..` and checking the result stays inside your server root, a malicious or even just fat-fingering client can read/write/delete files anywhere the server process has permission to touch. This is the single most commonly graded live "trap" for the Data/Filesystem role — test it explicitly with the hostile inputs shown in Step 1 before the demo, not just normal-looking paths.

---

## 4. Oral Defense (Viva Voce) Mastery Kit

**Script Template 1 — "Explain your binary file handling"**
> "Every file stream used for transfer is opened with `std::ios::binary` specifically to disable the OS's text-mode byte translation, which would otherwise corrupt non-text formats. I read and write in fixed 1024-byte chunks to match the RDT layer's payload size, but I always track the actual byte count returned by the read call rather than assuming a full chunk, because the final chunk of any file is virtually always partial."

**Script Template 2 — "Explain your directory traversal protection"**
> "Every client-supplied path is resolved against the session's current directory using `std::filesystem`'s path-normalization, which collapses any `..` or `.` segments into a canonical absolute path. I then verify that canonical path's element sequence still has the server root as a strict prefix before allowing any filesystem operation — if a client tries to escape the root with `../` sequences, the resolved path fails that prefix check and the command is rejected with an error code rather than touching the filesystem."

**Script Template 3 — "Explain your hash verification"**
> "After a transfer completes — meaning the RDT layer has confirmed every packet was delivered and acknowledged — I compute a SHA-256 hash over the reassembled file using the Windows CNG API, and compare it against the hash computed on the source file before transfer began. Because a cryptographic hash function is designed so any single-bit difference produces a completely different output, a match gives strong end-to-end evidence the file arrived byte-for-byte identical, independent of whatever happened at the RDT layer underneath."

**Whiteboard Diagram Concept:** Draw a **file as a horizontal strip of numbered boxes** (chunk 0, 1, 2, ... last-partial-chunk shaded differently to show it's smaller), with an arrow from each chunk down into "RDT packet #N" boxes (showing the 1:1 mapping between file chunks and Member B's sequence numbers), and off to the side, two small fingerprint icons labeled "hash(original)" and "hash(reassembled)" with an equals-or-not-equals sign between them. Underneath, draw a small tree diagram of `server_root` with a nested subfolder and a red "X" arrow showing a `../../` attempt bouncing off the root boundary. This proves, in one picture, that you understand chunking-to-packet mapping, why the last chunk is special, what the hash actually verifies, and how traversal protection works — the four things most likely to be probed individually in your viva.
