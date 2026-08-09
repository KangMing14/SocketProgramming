#pragma once
#include <winsock2.h>
#include <cstdint>
#include <string>

namespace hybridftp::client {

bool getLocalIPv4ForServer(const std::string& serverIp, uint32_t& outIpv4);

bool openActiveListenPort(SOCKET& outDataSock, unsigned short& outPort);

bool parsePortCommand(const std::string& argument, sockaddr_in& outAddress);

bool openActiveListenPort(const sockaddr_in& localAddress,
                          SOCKET& outDataSock);

std::string formatPortCommand(uint32_t ipv4Address, unsigned short port);

}
