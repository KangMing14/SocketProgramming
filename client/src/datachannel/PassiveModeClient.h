#pragma once
#include <winsock2.h>
#include <string>

bool parsePasvReply(const std::string& replyLine, sockaddr_in& outAddr);

bool connectToPassiveDataPort(const sockaddr_in& serverAddr, SOCKET& outDataSock);