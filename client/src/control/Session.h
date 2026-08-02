#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <filesystem>

namespace Session {
    SOCKET connectToServer(const std::string&, int);
    void runClientSession(SOCKET);
}
