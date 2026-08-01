#pragma once

namespace ReplyCode {
    // 1xx - Positive Preliminary
    constexpr int FileStatusOkay = 150;

    // 2xx - Positive 
    constexpr int SystemStatus = 211;
    constexpr int FileStatus = 213;
    constexpr int HelpMessage = 214;
    constexpr int ServiceReady = 220;
    constexpr int Goodbye = 221;
    constexpr int LoggedIn = 230;           // PASS success
    constexpr int ActionCompleted = 250;
    constexpr int PathnameCreated = 257;    // PWD, MKD
    constexpr int TransferComplete = 226;

    // 3xx - Positive Intermediate
    constexpr int AuthNeedPass = 331;       // USER success, awaiting PASS
    constexpr int PendingRNTO = 350;

    // 4xx - Transient Negative
    constexpr int CantOpenDataConnection = 425;

    // 5xx - Permanent Negative
    constexpr int SyntaxError = 500;
    constexpr int ActionNotTaken = 550;
    constexpr int FilenameNotAllowed = 553;
}