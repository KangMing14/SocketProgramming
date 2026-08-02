#pragma once
#include "../common/Users.h"
#include "../common/ReplyCodes.h"
#include "CommandParser.h"
#include "Session.h"

#include <map>
#include <string>
#include <functional>
#include <vector>

namespace CommandDispatcher{
    void executeCommand(ClientSession&, const ParsedCommand);
}