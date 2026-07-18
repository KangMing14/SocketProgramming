# Hybrid FTP Project — Architecture Blueprint & 3-Week Execution Plan
**Target stack:** C++17, Winsock2 (`ws2_32.lib`), raw Berkeley-style sockets (no MFC)

---

## PART 1 — ARCHITECTURAL BLUEPRINT

### 1.1 What we're keeping / discarding from the demo
| From `Demo_CSocket_thread` | Decision |
|---|---|
| Thread-per-client model (`CreateThread`) | **Keep the concept**, reimplement with `std::thread` + raw `SOCKET` (no `CSocket`, no Attach/Detach dance — that trick only existed because of MFC's limitations, which we're removing) |
| `afxsock.h` / MFC init | **Discard entirely** — switch to `WSAStartup(MAKEWORD(2,2), &wsaData)` |
| Calculator "protocol" | **Discard** — replaced by real FTP command parsing over TCP |
| Everything else (UDP, RDT, hashing, dirs, PORT/PASV) | **Build from zero** |

### 1.2 System overview
```
CLIENT                                   SERVER
  |------ TCP:4567 (control) -----------> |  accept() -> spawn session thread
  |  USER/PASS/CWD/LIST/PORT/PASV/RETR..  |
  |  <---- 3-digit reply codes ---------- |
  |                                       |
  |------ UDP (data, port negotiated) --->|  per-session UDP socket/port
  |  custom RDT-framed file chunks        |
  |  <---- ACKs --------------------------|
```
- **One TCP control socket per client session**, handled by a dedicated thread (or thread-pool worker) holding that client's session state (cwd, logged-in user, transfer type, mode).
- **One ephemeral UDP socket per active data transfer**, bound either by the server (PASV: server picks a random high port, tells client) or by the client (PORT: client picks a port, tells server, server connects out to it).
- TCP and UDP are **fully decoupled**: the control thread never blocks on data-channel I/O except to wait for a "transfer complete" signal from the data-transfer worker (use a `std::future`/condition variable, not busy-waiting).

### 1.3 Custom UDP Header (RDT layer) — exact byte layout
Fixed 16-byte header, network byte order, followed by up to `MAX_PAYLOAD` (e.g. 1024) bytes of file data:

| Field | Size | Notes |
|---|---|---|
| `seq_num` | 4 bytes (`uint32_t`) | Sequence number of this packet |
| `ack_num` | 4 bytes (`uint32_t`) | Cumulative or selective ACK number (0 if this is a data packet, not an ACK) |
| `flags` | 1 byte | bit0=SYN, bit1=ACK, bit2=FIN, bit3=DATA, bit4=NAK/dup-request |
| `window_size` | 2 bytes (`uint16_t`) | Receiver's advertised window, in packets |
| `payload_len` | 2 bytes (`uint16_t`) | Bytes of payload actually used in this packet |
| `checksum` | 2 bytes (`uint16_t`) | Internet checksum (RFC 1071 style) over header+payload with checksum field zeroed |
| `reserved` | 1 byte | Padding to 16 bytes / future use |

```cpp
#pragma pack(push, 1)
struct RdtHeader {
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t payload_len;
    uint16_t checksum;
    uint8_t  reserved;
};
#pragma pack(pop)
static_assert(sizeof(RdtHeader) == 16, "header must be 16 bytes on the wire");

enum RdtFlags : uint8_t {
    FLAG_SYN = 1 << 0,
    FLAG_ACK = 1 << 1,
    FLAG_FIN = 1 << 2,
    FLAG_DATA = 1 << 3,
    FLAG_NAK = 1 << 4,
};

// Pack: host -> network byte order before send()
void serializeHeader(const RdtHeader& h, char* buf) {
    uint32_t seq = htonl(h.seq_num);
    uint32_t ack = htonl(h.ack_num);
    uint16_t win = htons(h.window_size);
    uint16_t plen = htons(h.payload_len);
    uint16_t csum = htons(h.checksum);
    memcpy(buf + 0, &seq, 4);
    memcpy(buf + 4, &ack, 4);
    buf[8] = h.flags;
    memcpy(buf + 9, &win, 2);
    memcpy(buf + 11, &plen, 2);
    memcpy(buf + 13, &csum, 2);
    buf[15] = 0; // reserved
}

// Unpack: network -> host byte order after recvfrom()
RdtHeader deserializeHeader(const char* buf) {
    RdtHeader h{};
    uint32_t seq, ack; uint16_t win, plen, csum;
    memcpy(&seq, buf + 0, 4);  h.seq_num = ntohl(seq);
    memcpy(&ack, buf + 4, 4);  h.ack_num = ntohl(ack);
    h.flags = buf[8];
    memcpy(&win, buf + 9, 2);  h.window_size = ntohs(win);
    memcpy(&plen, buf + 11, 2); h.payload_len = ntohs(plen);
    memcpy(&csum, buf + 13, 2); h.checksum = ntohs(csum);
    return h;
}

uint16_t internetChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (data[i] << 8) | data[i + 1];
    if (len % 2) sum += data[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}
```
**Why this layout:** fixed-size header with `memcpy` field-by-field avoids struct-padding/alignment bugs that bite people who just `send((char*)&header, sizeof(header))` — compiler padding differs across build settings, and that's a classic "works on my machine" bug the examiner will probe for. Always serialize field-by-field.

**Reliability algorithm — recommend Selective Repeat over Go-Back-N.** SR only retransmits the actual lost packet (not the whole window), which is easier to defend in the "mathematical precision" viva questions and performs better under the packet-loss injection tests. Go-Back-N is acceptable and simpler to implement first if time is short — pick one and be able to justify the tradeoff (SR = more receiver-side buffering + selective ACKs; GBN = simpler receiver, wastes bandwidth on loss).

**Flow/congestion control:** implement a sliding window sender (`send_base`, `next_seq_num`, fixed `WINDOW_SIZE` e.g. 8) plus a simple additive-increase/multiplicative-decrease adjustment: increment window by 1 on N consecutive successful ACKs, halve it on a timeout — enough to demonstrate "basic congestion control" without needing full TCP Reno.

### 1.4 Concurrency & Active/Passive routing model
- **Server control-thread model:** `accept()` loop on main thread → spawn `std::thread` per client, detach or track in a `std::vector<std::thread>` + join on shutdown. Each thread owns a `Session` struct (username, cwd `std::filesystem::path`, logged_in bool, data mode PORT/PASV, current UDP socket).
- **Concurrency safety:** the only *shared* mutable state across sessions is the filesystem itself and (if you keep one) a global connected-clients table for logging. Guard the clients table with a `std::mutex`; per-client session state needs no locking since only that client's thread touches it — this is your answer to "how do you prevent race conditions" in the viva.
- **PASV:** server does `socket(AF_INET, SOCK_DGRAM,...)`, `bind()` to port 0 (OS picks a free port), `getsockname()` to read back the assigned port, replies `227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)`. Client then sends its RDT SYN to that address.
- **PORT:** client sends `PORT h1,h2,h3,h4,p1,p2`; server parses IP/port from the command, opens its own UDP socket, and initiates the RDT handshake to the client's address.
- Each transfer's UDP socket is scoped to that one file transfer (create it fresh per `RETR`/`STOR`, close after `FIN`/ACK) — do **not** share one UDP socket across concurrent clients, or you'll have to demux by address yourself and it gets messy fast.

---

## PART 2 — 3-WEEK PARALLELIZED ROADMAP

**Assumptions:** 3 people, ~8–10 hrs/week each, Git repo with `main` + feature branches, daily 15-min standup (async on Discord/Zalo is fine), merge at the 3 checkpoints below.

### Week 1 — Foundations & Demo Integration
| | Member A (Control/Concurrency) | Member B (RDT Protocol) | Member C (Data/Filesystem) |
|---|---|---|---|
| Day 1-2 | Strip MFC from demo; raw Winsock2 TCP echo server/client; `WSAStartup`/`WSACleanup` lifecycle | Design & unit-test `RdtHeader` (de)serialize + checksum in isolation (no networking yet) | `std::filesystem` wrapper: list dir, cwd tracking, path-traversal safety (block `../` escapes!) |
| Day 3-4 | Command parser: tokenize `VERB arg1 arg2`, reply-code formatter (`"%d %s\r\n"`) | Basic UDP send/recv loop with the header attached, no reliability yet | Binary file read/write in fixed-size chunks (`ifstream::read` in binary mode) |
| Day 5-7 | Thread-per-client server core; session struct; USER/PASS/QUIT/NOOP/PWD working end-to-end | Single Stop-and-Wait ACK loop (no window yet) over loopback | SIZE/MDTM commands; integrate chunked reads with A's TCP command dispatch for STOR/RETR stubs |
| **Merge point (end Week 1):** | A's TCP server can accept a client, log in, and dispatch to stub handlers that B and C's modules can be plugged into. Test: `USER`→`331`, `PASS`→`230`, `PWD`→`257`. |

**Chaos checklist (W1):** none yet — this week is correctness-first. Just verify multiple `telnet`/raw-socket clients can connect concurrently without the server blocking.

**Examiner trap questions (W1):**
1. *"Walk me through what happens in the OS kernel when your server calls `accept()`."* → Answer: the listening socket's queue holds completed TCP handshakes; `accept()` dequeues one, kernel creates a new socket descriptor bound to the same local port but the specific remote 4-tuple, blocking (or returning `WSAEWOULDBLOCK` if non-blocking) until one is available.
2. *"Why TCP for control and not UDP?"* → Command sequencing must be guaranteed and ordered (e.g., `RNFR` must precede `RNTO`); TCP's reliable, ordered byte stream removes the need to build our own reliability for control messages, unlike the data channel where we want fine control over reliability behavior (hence rolling our own over UDP).
3. *"What happens to your thread if the client disconnects mid-command?"* → `recv()`/`send()` return 0 or `SOCKET_ERROR`; handler must catch that, clean up the session struct, close the socket, and let the thread exit — otherwise it's a resource leak / zombie thread.

### Week 2 — RDT State Machines & Concurrency Hardening
| | Member A | Member B | Member C |
|---|---|---|---|
| Day 8-9 | PORT/PASV command handling; open/bind ephemeral UDP sockets; hand socket to C's transfer worker | Sliding window sender (`send_base`/`next_seq`), per-packet retransmit timer (`std::chrono` + `select()`/timeout on `recvfrom`) | Wire STOR/RETR to actually stream file bytes through B's RDT sender/receiver |
| Day 10-11 | CWD/CDUP/MKD/RMD/LIST/NLST with real filesystem backing | Selective Repeat receiver: out-of-order buffering, cumulative+selective ACKs | Directory tree edge cases: nested paths, permission errors → correct 4xx/5xx codes |
| Day 12-14 | Multi-session stress test harness (spawn N fake clients) | Congestion window growth/backoff logic; NAK/duplicate detection | TYPE A/I handling (binary vs ASCII mode read paths) |
| **Merge point (end Week 2):** | Full RETR/STOR works client↔server over UDP with simulated loss and still finishes correctly. |

**Chaos checklist (W2):**
- Kill/drop 15% of UDP packets artificially (wrap `sendto` with `rand() < 0.15 ? skip : send`) — verify retransmit recovers the file byte-for-byte.
- Inject 200-400ms artificial latency (`Sleep()`/`std::this_thread::sleep_for` before actual send) — verify your retransmit timeout is tuned above this or you'll get spurious retransmits.
- Bit-flip corruption: flip one random bit in ~5% of payloads before send — verify checksum mismatch triggers a NAK/drop, not silent corruption.

**Examiner trap questions (W2):**
1. *"Why did you choose that specific window size / timeout value?"* → Be ready with a real number and reasoning: e.g., "window=8 packets balances throughput vs. the 4KB buffer we allocate per session; timeout=250ms is ~2x observed loopback RTT with our injected 100ms latency, avoiding spurious retransmits while still recovering quickly."
2. *"How do your mutex locks prevent race conditions here?"* → Point to the *specific* shared resource (e.g., the connected-clients table) and explain what would break without the lock (torn reads/writes, iterator invalidation) — don't answer generically.
3. *"What happens if two ACKs arrive out of order?"* → With cumulative ACKs, a lower-numbered ACK arriving after a higher one is simply stale and ignored (compare against `send_base`); with selective ACKs, track a bitmap of acknowledged sequence numbers within the window.

### Week 3 — Advanced Features, Stress Testing, Viva Prep
| | Member A | Member B | Member C |
|---|---|---|---|
| Day 15-16 | ABOR, RNFR/RNTO, DELE, APPE, STOU; full 3xx/4xx/5xx coverage audit against spec table | Tune RDT for large files (multi-MB); fragment/reassembly correctness at scale | SHA-256/MD5 hashing (roll your own or use `<bcrypt.h>`/Windows CNG API — NOT a banned third-party lib since it's OS-bundled crypto, but confirm with your instructor) + HASH command |
| Day 17-18 | Logging: connected-client table, command audit log to file | Congestion control demo mode (deliberately throttle to show window shrink/grow live) | End-to-end HASH verification wired into RETR/STOR completion |
| Day 19-20 | Full integration testing all three modules together; fix cross-module bugs | Same | Same — all three do joint bug-bash |
| Day 21 | Oral defense dry-run: each member explains only their own module, cold, no notes | Same | Same |

**Chaos checklist (W3):**
- Combine loss + latency + corruption simultaneously at realistic-but-harsh rates (10% loss, 150ms latency, 2% corruption) on a multi-MB binary file transfer; verify final SHA-256 matches source.
- Kill the server process mid-transfer; verify client times out gracefully with a 4xx reply rather than hanging forever.
- Two clients uploading different files to the same directory simultaneously; verify no data interleaving/corruption on disk.

**Examiner trap questions (W3):**
1. *"Prove your hash check actually catches corruption — show me live."* → Have a demo path ready where you intentionally flip a bit *after* your RDT layer confirms delivery (e.g., corrupt the file on disk) so the HASH mismatch is visibly triggered — distinguishes "we have integrity checking" from "we have integrity checking that we can prove works."
2. *"What's the theoretical max throughput of your RDT layer given your window size and RTT?"* → Be ready to compute `throughput = window_size * payload_len / RTT` and compare it to what you actually measured — mismatches are a great, honest place to discuss real overhead (ACK processing time, thread scheduling).
3. *"If I set packet loss to 50%, does your congestion control still make forward progress or does it stall?"* → Know the actual behavior of your backoff (does it ever hit a minimum window > 0? does it retry indefinitely?) — test this before the exam, don't guess live.

---

## PART 3 — GENAI DOCUMENTATION TEMPLATE (Section 4.3)

Your rubric (4.2/4.3) explicitly **permits** GenAI use — the zero-tolerance clause is about being unable to explain code you imported, not about having used AI at all. The appendix just needs to show honest, traceable, critical use. Use this per-entry template in your report, one entry per meaningful AI interaction:

```
### GenAI Log Entry #N
Date: 
Team member: 
Module: (e.g., RDT sliding window sender)

**Prompt used (verbatim):**
"..."

**Raw AI output (verbatim, or summarized with a note if long):**
"..."
[If code: paste as-is in a code block, unedited]

**Critical refinement performed:**
- What was wrong / suboptimal / non-compliant (e.g., "AI suggested std::mutex per-packet, 
  causing lock contention; we moved the lock to per-session granularity")
- What was manually changed, and why
- Any banned library/pattern caught and removed (e.g., AI initially suggested Boost.Asio — 
  removed, replaced with raw Winsock2 per project rules)

**Verification:** 
- How the team confirmed correctness after refinement (test run, edge case, code review by 
  a teammate who didn't write it)
```

**Workflow to keep this low-overhead during a 3-week sprint:**
1. Each person keeps a running plain-text/markdown scratch file *while working* (not reconstructed after the fact — reconstructed logs are obviously thin and get flagged).
2. At each of the 3 weekly merge points, do a 20-minute team review pass: read teammates' log entries, ask "could you explain this cold in the viva?" If no, mark it for refactor before merge, not after.
3. Final report pass: convert scratch logs into the numbered template above, keep prompts/outputs verbatim (don't paraphrase them — that undermines the "unedited" requirement), and make sure the "critical refinement" section is never empty — an entry with no refinement reads as unverified copy-paste.
4. Cross-check every AI-suggested pattern against the banned-library list before it goes in the log as "accepted."

This structure directly maps to the "Industry-grade documentation... GenAI appendix shows deep critical auditing" Excellent-tier bar, because the refinement/verification fields are exactly what an examiner will spot-check against your live code.
