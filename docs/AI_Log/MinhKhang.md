# GenAI Usage & Code Refinement Log — Member C (Data & Filesystem Lead)

This log documents instances where I identified a gap, bug, or risk in the AI-assisted output or in our project setup, and directed the AI to produce a fix or enhancement. Verification/testing evidence for each fix is recorded separately in the Application Demo Evidence section.

---

## Entry 1 — Missing Solution File on Clean Clone

**Issue identified:** After cloning the shared GitHub repository, there was no `.sln`/`.vcxproj` file, since the repo only contained loose source folders. I could not set a startup project or run any test file in Visual Studio 2022.

**Prompt (verbatim):**
> "the problem is since we clone a github project, we don't have any .sln, the folders and files just stand alone so I can't run any startup."

**Raw AI output (summary):** AI proposed creating a new VS2022 solution manually, adding the cloned files via "Add → Existing Item," and creating one console-app project per test file within that solution.

**Refinement / problem-solving:** I recognized this approach was fragile for team/submission use — a manually assembled `.sln` depends on paths and settings specific to whichever machine created it, which does not guarantee it will still work when the teacher clones the repo onto a different machine. This concern led directly to Entry 2.

---

## Entry 2 — Build System Not Guaranteed to Work on the Teacher's Machine

**Issue identified:** The manually created `.sln` from Entry 1 would not reliably reproduce on a clean clone (absolute paths, machine-specific VS metadata, no guarantee every source file was registered). I identified this as a submission-correctness risk and proposed a build-system-level fix.

**Prompt (verbatim):**
> "When we submit our code, we have to ensure everything work when cloned to the teacher's device. Is there anything we could do, like making CMake or something."

**Raw AI output (summary):** AI proposed migrating the project to CMake, provided a root `CMakeLists.txt` defining a shared `ftp_core` static library, `ftp_server`/`ftp_client` executables, and per-file test executables registered via `add_test()`, plus `.gitignore` guidance for build artifacts.

**Refinement / problem-solving:** I directed that the setup be verified by having a teammate perform a completely fresh clone and "Open Folder" in VS2022 without any local project state, confirming the CMake-based build reproduces correctly with zero manual setup — directly addressing the original risk I had flagged.

---

## Entry 3 — Missing permissions string for LIST

**Issue identified:** The `listDir()` missing the permissions (required by the project instruction), but the permission string is in Unix ls -l long-format, which is not supported by default.

**Prompt (verbatim):**
> "Create a function to change std::filesystem::status().permissions() to Unix ls -l long-format listing for FTP LIST"

**Raw AI output (fullcode):**
```cpp
#include <iostream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

std::string get_unix_permission_string(fs::perms p) {
    std::string s = "---------";
    
    if ((p & fs::perms::owner_read)   != fs::perms::none) s[0] = 'r';
    if ((p & fs::perms::owner_write)  != fs::perms::none) s[1] = 'w';
    if ((p & fs::perms::owner_exec)   != fs::perms::none) s[2] = 'x';
    
    if ((p & fs::perms::group_read)   != fs::perms::none) s[3] = 'r';
    if ((p & fs::perms::group_write)  != fs::perms::none) s[4] = 'w';
    if ((p & fs::perms::group_exec)   != fs::perms::none) s[5] = 'x';
    
    if ((p & fs::perms::others_read)  != fs::perms::none) s[6] = 'r';
    if ((p & fs::perms::others_write) != fs::perms::none) s[7] = 'w';
    if ((p & fs::perms::others_exec)  != fs::perms::none) s[8] = 'x';
    
    return s;
}

std::string get_ftp_list_format(const fs::path& path) {
    std::error_code ec;
    fs::file_status status = fs::status(path, ec);
    
    if (ec) {
        return "d---------";
    }

    char type_char = '-';
    if (fs::is_directory(status))      type_char = 'd';
    else if (fs::is_symlink(status))   type_char = 'l';
    else if (fs::is_character_file(status)) type_char = 'c';
    else if (fs::is_block_file(status))     type_char = 'b';
    else if (fs::is_fifo(status))       type_char = 'p';
    else if (fs::is_socket(status))     type_char = 's';

    return type_char + get_unix_permission_string(status.permissions());
}
```

**Refinement / problem-solving:** I merged two functions into one, which in charge of formatting and switch from using long `else if` contitional chain to using `switch` instead.

---

## Entry 4 — Incomplete Command Coverage in `DirectoryService`

**Issue identified:** After reviewing the actual FTP command table in the project instructions PDF, I noticed `DirectoryService` (as designed) only covered PWD/CWD/CDUP/MKD/RMD/LIST, and was missing NLST and STAT, which are also directory/filesystem-related commands. I also questioned two specific design gaps in the LIST output: the "permissions" field and whether "type" needed to reflect file format (e.g. `.pdf`, `.docx`).

**Prompt (verbatim):**
> "How many command do DirectoryService in charge of in the FTP Commands Table in the Project Instruction, shouldn't it need to take care of NLST ans STAT also. I also notice that the LIST command also provide permissions, what is that. And about the type in LIST, didn't it have to specify exactly which type of file is this (.doc, .pdf, etc.) instead of just is directory or not, or do we going to have another handler for files?"

**Raw AI output (summary):** AI cross-checked the actual command table from the PDF, confirmed NLST/STAT were missing from the current scope, clarified that LIST's "type" refers only to file-vs-directory (not file format/extension), explained the Unix-style permission-string convention, and proposed extending `DirectoryService` with a shared `PathMetadata`/`getMetadata()` design to also cover SIZE, MDTM, DELE, and RNFR/RNTO.

**Refinement / problem-solving:** I confirmed this scope correction should be reflected in the module's actual responsibilities going forward, since the original file organization plan (written before this cross-check) had undercounted the commands genuinely belonging to the filesystem layer.

---

## Entry 5 — Runtime Assertion Failure in `PassiveModeHandler` Test

**Issue identified:** Running `test_open_passive_port_succeeds()` produced `Assertion failed: ok == true`, meaning `openPassiveDataPort` was returning `false` at runtime despite compiling and appearing logically correct.

**Prompt (verbatim):**
> "I run the test and there's an Assertion failed: ok == true in test_open_passive_port_succeeds(). How can I pinpoint what happen and how to tackle it?"

**Raw AI output (summary):** AI identified the most likely root cause as a missing `WSAStartup()` call in the standalone test executable (since it doesn't go through the main server's startup path), and provided instrumentation using `WSAGetLastError()` to confirm the exact failing call and error code before applying a fix.

**Refinement / problem-solving:** I applied the suggested diagnostic logging to confirm the actual failure point before accepting the fix, rather than applying the most-likely fix blindly, and added `WSAStartup`/`WSACleanup` to the test's `main()`.

---

## Entry 6 — Silent Parsing Bug in `parsePortCommand` (Trailing Input Not Rejected)

**Issue identified:** My own test case `parsePortCommand("192,168,1,5,195,80,1", addr) == false` failed — the function incorrectly returned `true` for a PORT string containing a trailing extra field. I diagnosed the likely cause myself before asking for confirmation.

**Prompt (verbatim):**
> "This code line failed: assert(parsePortCommand("192,168,1,5,195,80,1", addr) == false);
I suspect that because the code: int fieldsParsed = sscanf_s(arg.c_str(), d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2); just take 6 fields even if the input contains 7 or more, the rest just got dump out but never reported. Is my guess correct?"

**Raw AI output (summary):** AI confirmed the diagnosis was correct — `sscanf_s` stops once its format string is satisfied and does not report unconsumed trailing input via its return count — and provided two fix options: adding a `%n` consumed-character check, or replacing the parser with manual comma-tokenization plus `std::stoi`'s consumed-position check.

**Refinement / problem-solving:** Since I had already correctly identified the root cause before requesting a fix, I selected and applied the `%n`-based fix directly, and added the failing case plus related trailing-garbage variants as permanent regression tests in the test suite.

---

*End of log excerpt — additional entries to be appended as development continues.*


### GenAI Log Entry #02
Date: 19/07/2026
Team member: Do Minh Khang - Member C
Module: Get permissions string for LIST

**Prompt used:**
"How to change std::filesystem::status().permissions() to Unix ls -l long-format listing for FTP LIST"

**Raw AI output (verbatim, or summarized with a note if long):**
```cpp
#include <iostream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

std::string get_unix_permission_string(fs::perms p) {
    std::string s = "---------";
    
    if ((p & fs::perms::owner_read)   != fs::perms::none) s[0] = 'r';
    if ((p & fs::perms::owner_write)  != fs::perms::none) s[1] = 'w';
    if ((p & fs::perms::owner_exec)   != fs::perms::none) s[2] = 'x';
    
    if ((p & fs::perms::group_read)   != fs::perms::none) s[3] = 'r';
    if ((p & fs::perms::group_write)  != fs::perms::none) s[4] = 'w';
    if ((p & fs::perms::group_exec)   != fs::perms::none) s[5] = 'x';
    
    if ((p & fs::perms::others_read)  != fs::perms::none) s[6] = 'r';
    if ((p & fs::perms::others_write) != fs::perms::none) s[7] = 'w';
    if ((p & fs::perms::others_exec)  != fs::perms::none) s[8] = 'x';
    
    return s;
}

std::string get_ftp_list_format(const fs::path& path) {
    std::error_code ec;
    fs::file_status status = fs::status(path, ec);
    
    if (ec) {
        return "d---------";
    }

    char type_char = '-';
    if (fs::is_directory(status))      type_char = 'd';
    else if (fs::is_symlink(status))   type_char = 'l';
    else if (fs::is_character_file(status)) type_char = 'c';
    else if (fs::is_block_file(status))     type_char = 'b';
    else if (fs::is_fifo(status))       type_char = 'p';
    else if (fs::is_socket(status))     type_char = 's';

    return type_char + get_unix_permission_string(status.permissions());
}
```

**Refinement performed:**
- AI seperate the fuction into 2 smaller functions. We merge them into one because the fuction only called once at `LIST` command
- AI using too many `else if` for checking file type. Take advance of the `file_status`, we switch from `if else` to `switch(status.type())`

