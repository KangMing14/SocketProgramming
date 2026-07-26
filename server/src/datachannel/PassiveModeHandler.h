#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>

bool openPassiveDataPort(SOCKET& outDataSock, unsigned short& outPort);

std::string formatPasvReply(uint32_t ipv4Address, unsigned short port);