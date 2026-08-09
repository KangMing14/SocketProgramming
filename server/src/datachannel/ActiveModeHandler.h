#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

bool parsePortCommand(const std::string& arg, sockaddr_in& outAddr);

bool openActiveDataConnection(const sockaddr_in& peerAddr, SOCKET& outDataSock);