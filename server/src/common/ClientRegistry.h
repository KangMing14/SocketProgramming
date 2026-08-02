#pragma once

#include <winsock2.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace ClientRegistry {
    struct ClientInfo {
        std::string address;
        std::string username;
    };

    inline std::mutex registryMutex;
    inline std::unordered_map<SOCKET, ClientInfo> clients;

    inline void addClient(SOCKET socket, const std::string& address) {
        std::lock_guard<std::mutex> lock(registryMutex);
        clients[socket] = ClientInfo{address, ""};
    }

    inline void removeClient(SOCKET socket) {
        std::lock_guard<std::mutex> lock(registryMutex);
        clients.erase(socket);
    }

    inline void setUsername(SOCKET socket, const std::string& username) {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = clients.find(socket);
        if (it != clients.end()) it->second.username = username;
    }

    inline size_t count() {
        std::lock_guard<std::mutex> lock(registryMutex);
        return clients.size();
    }

    inline std::vector<ClientInfo> snapshot() {
        std::lock_guard<std::mutex> lock(registryMutex);
        std::vector<ClientInfo> result;
        result.reserve(clients.size());
        for (const auto& [socket, info] : clients) result.push_back(info);
        return result;
    }
}