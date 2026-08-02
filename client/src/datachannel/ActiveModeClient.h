#pragma once
#include <winsock2.h>
#include <cstdint>
#include <string>

namespace hybridftp::client {

bool getLocalIPv4ForServer(const std::string& serverIp, uint32_t& outIpv4);

bool openActiveListenPort(SOCKET& outDataSock, unsigned short& outPort);

std::string formatPortCommand(uint32_t ipv4Address, unsigned short port);

}
