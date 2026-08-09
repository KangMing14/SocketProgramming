#include "ClientRegistry.h"

#include <mutex>
#include <unordered_map>

namespace {
std::mutex registryMutex;
std::unordered_map<SOCKET, ClientRegistry::ClientInfo> clients;
}

namespace ClientRegistry {

void addClient(SOCKET socket, const std::string& address) {
    std::lock_guard<std::mutex> lock(registryMutex);
    clients[socket] = ClientInfo{address, ""};
}

void removeClient(SOCKET socket) {
    std::lock_guard<std::mutex> lock(registryMutex);
    clients.erase(socket);
}

void setUsername(SOCKET socket, const std::string& username) {
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto it = clients.find(socket);
    if (it != clients.end()) it->second.username = username;
}

std::optional<ClientInfo> findClient(SOCKET socket) {
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto it = clients.find(socket);
    if (it == clients.end()) return std::nullopt;
    return it->second;
}

std::size_t count() {
    std::lock_guard<std::mutex> lock(registryMutex);
    return clients.size();
}

std::vector<ClientInfo> snapshot() {
    std::lock_guard<std::mutex> lock(registryMutex);
    std::vector<ClientInfo> result;
    result.reserve(clients.size());
    for (const auto& entry : clients) result.push_back(entry.second);
    return result;
}

}
