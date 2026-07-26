#include "CommandDispatcher.h"

namespace CommandDispatcher{
    std::map<std::string, std::function<void(SOCKET&, const std::vector<std::string>&)>> commandMap = {
        { "QUIT", [](SOCKET& s, const std::vector<std::string>& args) { quitSession(s, args); } }
    };
    
    void executeCommand(SOCKET& s, const ParsedCommand cmd){
        auto it = commandMap.find(cmd.type);
        if (it != commandMap.end()) {
            it->second(s, cmd.args);
        }
        else {
            Session::replyWithCode(s, ReplyCode::SyntaxError, "Command not found: " + cmd.type);
        }
    }

    void quitSession(SOCKET& s, const std::vector<std::string>& args){
        Session::replyWithCode(s, ReplyCode::Goodbye, "Service closing control connection.");
        shutdown(s, SD_SEND);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}