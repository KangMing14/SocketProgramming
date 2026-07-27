#pragma once
#include <string>
#include <sstream>
#include <vector>

struct ParsedCommand {
    std::string rawCommand, type;
    std::vector<std::string> args;
};

namespace CommandParser{
    std::string trim(const std::string& str);
    ParsedCommand parseCommand(std::string command);
}