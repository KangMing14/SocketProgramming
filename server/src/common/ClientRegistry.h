#pragma once

#include <winsock2.h>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ClientRegistry {
    struct ClientInfo {
        std::string address;
        std::string username;
    };

    void addClient(SOCKET socket, const std::string& address);
    void removeClient(SOCKET socket);
    void setUsername(SOCKET socket, const std::string& username);
    std::optional<ClientInfo> findClient(SOCKET socket);
    std::size_t count();
    std::vector<ClientInfo> snapshot();
}
