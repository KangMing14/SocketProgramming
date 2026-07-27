#pragma once

#include <iostream>
#include <string>
#include <mutex>

namespace Logger {
    inline std::mutex logMutex;
    inline void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMutex);
        std::cout << "[LOG] " << msg << std::endl;
    }
}