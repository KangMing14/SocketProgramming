#pragma once
#include <filesystem>
#include <string>
#include <chrono>

inline std::string formatMdtmTimestamp(std::filesystem::file_time_type ft) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm);
    return std::string(buf);
}