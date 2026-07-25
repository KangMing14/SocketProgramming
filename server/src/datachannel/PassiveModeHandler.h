#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>

// Opens a new UDP socket bound to an OS-assigned ephemeral port, for use as
// this session's PASV data channel. Returns false if the socket could not
// be created or bound. On success, outDataSock is a valid, bound (but not yet
// connected) UDP socket, and outPort holds the port the OS actually assigned.
bool openPassiveDataPort(SOCKET& outDataSock, unsigned short& outPort);

// Formats the PASV 227 reply body, e.g. "192,168,1,10,195,80", from a given
// local IPv4 address and port — the caller (CommandDispatcher) wraps this in
// the full "227 Entering Passive Mode (...)." reply text.
std::string formatPasvReply(uint32_t ipv4Address, unsigned short port);