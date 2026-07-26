#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>

namespace Session {
    void replyWithCode(SOCKET, int, std::string);
    SOCKET initializeSession();
    void handleClient(SOCKET);
    void runSession();
}