# Member B Survival Guide — Protocol Engineer (Custom UDP RDT Layer)
**Scope:** Bit/Byte Header Framing, Checksum Math, Sequence Numbers, Timeout & Retransmission, Sliding Window Flow Control

---

## 1. What to Learn First (Prerequisite Checklist)

This is the hardest role on the team conceptually, but it breaks down into a short list of ideas — master these before writing any RDT code.

**1. UDP is a postcard, not a phone call**
TCP (Member A's world) is a phone call: connected, ordered, and the network guarantees delivery. UDP is a postcard: you write an address on it and drop it in the mailbox (`sendto()`), and the network makes *no promises* — it might arrive, arrive twice, arrive out of order, arrive corrupted, or never arrive. Everything you build this sprint exists to fake TCP's guarantees on top of postcards. Understanding *why* each guarantee is missing is why you need each piece of RDT — ordering needs sequence numbers, loss needs ACKs+timeouts, corruption needs a checksum.

**2. Byte order (endianness) — the "which end of the egg" problem**
A `uint32_t` like `sequence number = 1` is stored in your computer's memory as 4 bytes, but different CPU architectures disagree on whether the biggest or smallest byte comes first. If you just `memcpy` a raw `int` into your packet and send it, a different machine (or even the same machine misreading it) can interpret `1` as `16777216`. The fix: **always** convert to "network byte order" (big-endian, biggest byte first) with `htonl()`/`htons()` before you put a number in the wire buffer, and convert back with `ntohl()`/`ntohs()` after you read it out. Think of it as translating your number into a universal language before mailing it, and translating it back after you receive it — every RDT implementation on earth does this, it's not project-specific ceremony.

**3. Struct padding — why you can't just `send((char*)&myStruct, sizeof(myStruct))`**
Compilers are allowed to insert invisible padding bytes between struct fields for CPU alignment efficiency, and the amount of padding can differ between compilers/settings. If you send a raw struct and the receiver's compiler pads differently, your fields land at the wrong offsets — this is a classic bug that "compiles and works on your machine" until it silently breaks. The fix (used throughout this guide): serialize fields **one at a time** into a flat `char` buffer at known fixed offsets, never trust the compiler's struct layout on the wire.

**4. Checksums — a tamper-evident seal, not encryption**
A checksum is a small number computed from all the bytes in a packet such that if even one bit flips in transit, the checksum computed on receipt won't match the one sent. It doesn't stop tampering, it just detects accidental corruption. You'll implement the "Internet checksum" (RFC 1071 style — same idea used in real IP/TCP/UDP headers): sum up all 16-bit words, fold any overflow back in, then take the one's complement.

**5. Sequence numbers, ACKs, and "the window"**
A sequence number labels each packet ("this is packet #5") so the receiver can detect loss (a gap in numbers) and duplicates (a number seen before) and reorder anything that arrived out of order. An ACK is the receiver's postcard back saying "I got everything up through #5." A sliding window is simply: "don't send more than N un-acknowledged packets at once" — it exists so a fast sender doesn't flood a slow receiver or network (that's your flow/congestion control requirement). Picture a window as a physical frame sliding along a numbered list of packets — only packets currently inside the frame are allowed to be "in flight" unacknowledged.

**6. Timers and timeouts**
Since UDP never tells you a packet was lost, you infer loss by *absence of an ACK within a deadline*. You need a way to say "if I haven't heard back about packet #5 within X milliseconds, assume it's lost and resend it." `setsockopt` with `SO_RCVTIMEO` is the simplest beginner tool for this — it makes `recvfrom()` give up and return an error after a timeout instead of blocking forever.

---

## 2. Step-by-Step Implementation Playbook

### Step 0 — Prove you can serialize/deserialize the header correctly, with NO networking yet
Do this as a standalone console test before touching sockets at all — get this 100% correct first, because every later bug you'll have traces back to header framing if you skip this.
```cpp
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <winsock2.h> // for htons/htonl/ntohs/ntohl only, at this stage

struct RdtHeader {
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t payload_len;
    uint16_t checksum;
};
constexpr int HEADER_SIZE = 15; // sum of field sizes — do NOT use sizeof(RdtHeader), padding lies

void serializeHeader(const RdtHeader& h, unsigned char* buf) {
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
}

RdtHeader deserializeHeader(const unsigned char* buf) {
    RdtHeader h{};
    uint32_t seq, ack; uint16_t win, plen, csum;
    memcpy(&seq, buf + 0, 4);   h.seq_num = ntohl(seq);
    memcpy(&ack, buf + 4, 4);   h.ack_num = ntohl(ack);
    h.flags = buf[8];
    memcpy(&win, buf + 9, 2);  h.window_size = ntohs(win);
    memcpy(&plen, buf + 11, 2); h.payload_len = ntohs(plen);
    memcpy(&csum, buf + 13, 2); h.checksum = ntohs(csum);
    return h;
}

int main() {
    RdtHeader original{ 42, 0, 0b00001000, 8, 1024, 0 };
    unsigned char buf[HEADER_SIZE];
    serializeHeader(original, buf);
    RdtHeader roundTrip = deserializeHeader(buf);

    printf("seq: %u -> %u (%s)\n", original.seq_num, roundTrip.seq_num,
           original.seq_num == roundTrip.seq_num ? "OK" : "MISMATCH");
    printf("payload_len: %u -> %u (%s)\n", original.payload_len, roundTrip.payload_len,
           original.payload_len == roundTrip.payload_len ? "OK" : "MISMATCH");
    return 0;
}
```
**Do not proceed until every field round-trips correctly.** This is the single most valuable 30 minutes you'll spend all sprint.

### Step 1 — Checksum function, tested in isolation
```cpp
uint16_t internetChecksum(const unsigned char* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (data[i] << 8) | data[i + 1];
    if (len % 2)                       // odd byte left over
        sum += data[len - 1] << 8;
    while (sum >> 16)                  // fold carry bits back in
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}
// Test: compute checksum over a buffer, flip one bit, recompute — must differ.
```

### Step 2 — Basic UDP send/receive with a timeout (no reliability logic yet)
```cpp
SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

// This is the beginner-friendly way to implement "wait for a reply, but not forever."
DWORD timeoutMs = 500; // 500ms — tune this based on your measured RTT
setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

sockaddr_in destAddr{};
destAddr.sin_family = AF_INET;
destAddr.sin_port = htons(9999);
inet_pton(AF_INET, "127.0.0.1", &destAddr.sin_addr);

unsigned char sendBuf[HEADER_SIZE + 1024];
// ... fill sendBuf with a serialized header + payload ...
sendto(udpSock, (char*)sendBuf, HEADER_SIZE + payloadLen, 0, (sockaddr*)&destAddr, sizeof(destAddr));

unsigned char recvBuf[HEADER_SIZE + 1024];
sockaddr_in fromAddr{};
int fromLen = sizeof(fromAddr);
int n = recvfrom(udpSock, (char*)recvBuf, sizeof(recvBuf), 0, (sockaddr*)&fromAddr, &fromLen);
if (n == SOCKET_ERROR) {
    int err = WSAGetLastError();
    if (err == WSAETIMEDOUT) {
        printf("No ACK within timeout — treat as packet loss, retransmit.\n");
    }
}
```

### Step 3 — Stop-and-Wait first (get this fully correct before attempting a sliding window)
Send packet #N, wait for ACK #N, only then send #N+1. This is deliberately slow but is the simplest correct baseline — implement and test this against simulated loss before adding a window at all:
```cpp
uint32_t seq = 0;
while (moreDataToSend) {
    bool acked = false;
    int attempts = 0;
    while (!acked && attempts < MAX_RETRIES) {
        sendPacket(seq, currentChunk);           // your send + serialize
        RdtHeader ackHeader;
        if (waitForAck(ackHeader, /*timeoutMs=*/500)) {  // returns false on SO_RCVTIMEO
            if (ackHeader.ack_num == seq) acked = true;  // else: stale ACK, ignore, keep waiting
        }
        attempts++;
    }
    if (!acked) { /* give up, report transfer failure */ break; }
    seq++;
}
```

### Step 4 — Upgrade to a sliding window (Selective Repeat recommended)
Keep a small struct per in-flight packet: `{seq_num, data, sent_time, acked}`. On each loop iteration: (a) send new packets while `next_seq - send_base < WINDOW_SIZE`, (b) check for incoming ACKs non-blockingly and mark matching packets acked, advance `send_base` while the oldest packet is acked, (c) check each in-flight packet's `sent_time` against the timeout and resend individually if expired (this "resend individually" step, not the whole window, is what makes it Selective Repeat rather than Go-Back-N).

### Weekly sequence
- **Week 1:** Steps 0-2, entirely offline/loopback, no reliability logic yet.
- **Week 2:** Step 3 (Stop-and-Wait) fully working with simulated loss/corruption, then Step 4 (sliding window) if time allows. **A working Stop-and-Wait beats a broken Selective Repeat — don't skip the baseline.**
- **Week 3:** Congestion window growth/backoff (increase window by 1 after N clean ACKs, halve on timeout), tune timeout value against measured RTT, integration with Member C's file chunking.

---

## 3. Beginner Pitfalls & How to Avoid Them

**Pitfall 1: `sizeof(RdtHeader)` doesn't equal your intended wire size, because of struct padding.**
If you ever write `send(sock, (char*)&header, sizeof(header), 0)` instead of manually serializing field-by-field, you'll get a size that's larger than the sum of your fields (compiler-inserted padding), and it'll differ across build configurations. Always use a `constexpr int HEADER_SIZE` equal to the manually-summed field sizes, and always serialize/deserialize field-by-field as shown in Step 0. Debug symptom: header fields read back garbled on the receiving end even on localhost — check your buffer offsets first.

**Pitfall 2: Retransmit timer set too aggressively short, causing "false loss" storms.**
If your timeout is shorter than the actual round-trip time (especially once you add artificial latency for chaos testing), you'll retransmit packets that were never actually lost, flooding the network and making things worse, not better. Symptom: transfer "works" on pure loopback but falls apart the moment latency is added. Fix: measure actual RTT first (timestamp a ping-style exchange), set your timeout to roughly 2x observed RTT, and make it a named constant you can tune, not a magic number buried in code.

**Pitfall 3: Forgetting that UDP can deliver duplicates, and double-counting or double-writing data as a result.**
Because you retransmit on suspected loss, sometimes the "lost" packet actually arrives late, AND your retransmission also arrives — the receiver gets the same sequence number twice. If your receiver doesn't check "have I already accepted this sequence number?" before writing it to the file, you'll get duplicated bytes in the reassembled file. Fix: receiver keeps a record (e.g., `std::set<uint32_t>` or a bitmap) of sequence numbers already accepted, and silently ACKs-and-discards (does not re-write) duplicates.

---

## 4. Oral Defense (Viva Voce) Mastery Kit

**Script Template 1 — "Explain your header design"**
> "My header is a fixed 15-byte layout I serialize field-by-field into a flat buffer — I deliberately avoid sending the raw struct because compiler padding isn't guaranteed consistent across platforms. Every multi-byte field is converted to network byte order with `htonl`/`htons` before sending and back with `ntohl`/`ntohs` on receipt, so the same code works correctly regardless of the host's native endianness. The checksum covers the header and payload together, computed with the standard Internet checksum algorithm, so any single-bit corruption in transit is detected before the packet is trusted."

**Script Template 2 — "Explain how you detect and recover from loss"**
> "Every packet gets a monotonically increasing sequence number. The sender keeps a table of in-flight, unacknowledged packets with their send timestamps. If an ACK for a given sequence number doesn't arrive within my timeout window — which I tuned to roughly twice the measured round-trip time — I assume that specific packet was lost and retransmit only that packet, which is what makes this Selective Repeat rather than Go-Back-N. The receiver tracks which sequence numbers it's already accepted so a late-arriving duplicate — for instance if my 'lost' assumption was wrong and both the original and retransmission arrive — gets ACKed again but not re-written to the output."

**Script Template 3 — "Explain your flow/congestion control"**
> "I maintain a sliding window of size N representing the maximum number of unacknowledged packets allowed in flight at once, which prevents overwhelming the receiver's buffer or the network path. After N consecutive clean ACKs I grow the window by one, and on any timeout I halve it — a simple additive-increase/multiplicative-decrease policy, the same underlying idea as TCP congestion control, just simplified for our scope."

**Whiteboard Diagram Concept:** Draw a **timeline with two horizontal lines, Sender above and Receiver below**, with time flowing left to right. Draw arrows for packets #3, #4, #5 going sender→receiver, show packet #4's arrow as a dashed line disappearing partway (simulating loss), show ACK arrows going receiver→sender only for #3 and #5 (out of order, since #4 never arrived), then draw a small clock icon with "timeout" next to a second arrow for #4 being resent, followed by its ACK. Label the window boundary as a bracket around whichever packets are "in flight" at each moment. This single diagram — packet loss, out-of-order ACK, timeout-triggered retransmission, and the sliding window boundary all in one picture — covers essentially every "explain your RDT mechanism" question the professor can ask.
