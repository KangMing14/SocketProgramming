#include "CommandParser.h"

namespace CommandParser{
    // Helper to remove leading and trailing whitespace
    std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    ParsedCommand parseCommand(std::string command){
        ParsedCommand cmd;
        std::string trimmedCmd = trim(command);
        if (trimmedCmd.empty()) return cmd;

        std::stringstream ss(trimmedCmd);
        std::string tmp;
        ss >> cmd.type;
        while (ss >> tmp) {
            cmd.args.push_back(tmp);
        }

        return cmd;
    }
}