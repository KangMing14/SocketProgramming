#pragma once
#include "../common/ReplyCodes.h"
#include "CommandParser.h"
#include "Session.h"

#include <map>
#include <string>
#include <functional>
#include <vector>

namespace CommandDispatcher{
    void executeCommand(SOCKET&, const ParsedCommand);
    void quitSession(SOCKET&, const std::vector<std::string>&);
}