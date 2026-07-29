#include "RdtHeader.h"
#include <winsock2.h>
#include <cstring>

void serializeHeader(const RdtHeader &h, char *buf)
{
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
    buf[15] = h.reserved;
}

RdtHeader deserializeHeader(const char *buf)
{
    RdtHeader h{};
    uint32_t seq, ack;
    uint16_t win, plen, csum;

    memcpy(&seq, buf + 0, 4);
    memcpy(&ack, buf + 4, 4);
    h.flags = buf[8];
    memcpy(&win, buf + 9, 2);
    memcpy(&plen, buf + 11, 2);
    memcpy(&csum, buf + 13, 2);
    h.reserved = buf[15];

    h.seq_num = ntohl(seq);
    h.ack_num = ntohl(ack);
    h.window_size = ntohs(win);
    h.payload_len = ntohs(plen);
    h.checksum = ntohs(csum);

    return h;
}