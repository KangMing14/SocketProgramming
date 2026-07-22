#pragma once

namespace ReplyCode {
    // 2xx - Positive Completion
    constexpr int ActionCompleted = 250;
    constexpr int PathnameCreated = 257;    // Pathname created/reported (e.g. PWD, MKD)

    // 5xx - Permanent Negative
    constexpr int ActionNotTaken = 550;
}