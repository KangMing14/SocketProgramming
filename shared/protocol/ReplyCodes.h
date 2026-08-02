#pragma once

namespace ReplyCode {
    constexpr int FileStatusOkay = 150;

    constexpr int CommandOkay = 200;
    constexpr int SystemStatus = 211;
    constexpr int FileStatus = 213;
    constexpr int HelpMessage = 214;
    constexpr int ServiceReady = 220;
    constexpr int Goodbye = 221;
    constexpr int TransferComplete = 226;
    constexpr int EnterPasvMode = 227;
    constexpr int LoggedIn = 230;
    constexpr int ActionCompleted = 250;
    constexpr int PathnameCreated = 257;

    constexpr int AuthNeedPass = 331;
    constexpr int PendingRNTO = 350;

    constexpr int CantOpenDataConnection = 425;
    constexpr int TransferAborted = 426;

    constexpr int SyntaxError = 500;
    constexpr int ActionNotTaken = 550;
    constexpr int FilenameNotAllowed = 553;
}
