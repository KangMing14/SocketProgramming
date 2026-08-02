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

## Entry 7 — Cross-Module Constant Mismatch Between `MAX_PAYLOAD` and `CHUNK_SIZE`
 
**Issue identified:** While reviewing Member B's RDT header implementation, I noticed the packet checksum field used only `uint16_t` and questioned whether this was suitable compared to SHA-256, and separately noticed `MAX_PAYLOAD` was defined as 1000 while my own `CHUNK_SIZE` was 1024, and suspected this discrepancy could be a real problem worth checking across the codebase.
 
**Prompt (verbatim):**
> "I just found out the rdt header only used uint16_t for checksum, which I think is not suitable for SHA256 isn't it? And the MAX_PAYLOAD also only 1000, isn't it suppose to be 1024 or is it use for different purpose? If those are really a problems, provide all needed fixes (across the codebase if necessary). Also find out any other existed potential problem due to the mis-match between members."
 
**Raw AI output (summary):** AI confirmed the 16-bit checksum was an intentional, separate per-packet transport check (not a substitute for SHA-256), but confirmed the `MAX_PAYLOAD`/`CHUNK_SIZE` mismatch was a real, severe bug — a stack buffer overflow in `RdtSender::sendChunk` plus silent truncation of every full-size chunk — and provided a centralized-constant fix (`ProtocolConstants.h`) plus a second, independently-found bug: a PASV-mode socket double-bind conflict between `PassiveModeHandler` and `RdtReceiver`.
 
**Refinement / problem-solving:** I directed that the fix apply across every affected file (not just the constant itself), and later verified across two follow-up branch checks that both the constant-unification fix and the socket-ownership fix were correctly applied, catching that the second fix had only landed partially (the command-dispatch wiring wasn't updated to use it) before it was fully resolved.
 
---
 
## Entry 8 — Circular Header Dependency Between `ProtocolConstants.h` and `RdtHeader.h`
 
**Issue identified:** While deriving `HEADER_SIZE` from `sizeof(RdtHeader)` in the shared constants header, I found that `ProtocolConstants.h` needed to include `RdtHeader.h` for the struct size, while `RdtHeader.h` needed to include `ProtocolConstants.h` for `MAX_PAYLOAD` — a circular include that caused a compile error. I applied a temporary hardcoded value as a workaround and asked for a proper solution.
 
**Prompt (verbatim):**
> "There's also a problem where at RdtHeader.h, I need to include ProtocolConstants.h for the MAX_PAYLOAD, but in ProtocolConstants.h, I also need to include RdtHeader.h for sizeof(RdtHeader) which lead to error so I define it at 16 as a temporary solution, provide some proper way to handle this."
 
**Raw AI output (summary):** AI explained why the intuitive fixes (forward declaration, `#pragma once` alone) don't resolve a true circular dependency, and proposed the correct structural fix: recognizing `HEADER_SIZE` as a value derived from `RdtHeader` itself, and therefore defining it directly inside `RdtHeader.h` immediately after the struct, rather than in the separate constants file — eliminating the cycle entirely rather than papering over it with a hardcoded literal.
 
**Refinement / problem-solving:** I had already identified the root cause (the circular include) and applied a working, if temporary, fix myself before asking for the permanent solution, then removed the hardcoded `16` in favor of the compiler-derived `sizeof(RdtHeader)` value.
 
---

## Entry 9 — Silent Hang in the Chaos Integrity Test
 
**Issue identified:** After implementing the end-to-end chaos test on my own branch, I noticed the test ran normally, printing repeated "[Receiver] ACK sent for seq=X" messages, then stopped producing any console output at all — while the process itself remained running rather than crashing or exiting.
 
**Prompt (verbatim):**
> "Regarding the chaos test, I just implemented the test in this branch https://github.com/KangMing14/SocketProgramming/tree/feat/add-client-side-file-handler but the test run normally until this massage:  [Receiver] ACK sent for seq=<some number>. This massage did also appear before in the console but until 1 time it just stop to print anything to console (the exe still run tho). Could you check what happen."
 
**Raw AI output (summary):** AI pulled the actual branch, traced the Selective Repeat sender/receiver logic, and identified two real bugs: `MAX_RETRIES` was defined but never enforced in the sender's retry loop (allowing infinite silent retries), and the receiver's `recvfrom` timeout loop looped forever on timeout with no bound and no log output — together explaining the exact "still running, nothing printing" symptom. Provided a fix enforcing retry limits with explicit failure reporting, plus a bounded, logged receiver timeout.
 
**Refinement / problem-solving:** I provided the precise, reproducible symptom (which log line appeared last, that the process kept running) rather than just reporting "it hangs," which let the diagnosis proceed directly from code tracing rather than guesswork.
 
---
 
## Entry 10 — Shared `ftp_core` CMake Target Causing Symbol Redefinition Between Client and Server
 
**Issue identified:** As client-side files accumulated with the same class names as their server-side counterparts (by design, per the team's duplication decision), I recognized that the single shared `ftp_core` CMake library target would cause redefinition conflicts once both sides were linked together.
 
**Prompt (verbatim):**
> "Right now, the CMake create a single ftp_core for both Client and Server, which lead to many re-definition and conflict, how can I resolve it?"
 
**Raw AI output (summary):** AI confirmed the root cause (one library compiling both `server/src/control/Session.cpp` and `client/src/control/Session.cpp`, both defining a class named `Session`, plus ambiguous flat include paths) and restructured the build into two independent static libraries (`server_core`/`client_core`), each scoped strictly to its own source tree, with a shared header-only `common/` directory for genuinely non-conflicting constants.
 
**Refinement / problem-solving:** I identified this as a forward-looking structural risk before it caused an actual build failure, based on recognizing the pattern from the team's own file-duplication convention, rather than waiting to hit the error directly.
 
---
 
## Entry 11 — Teammate's Independently-Built Client `DataChannelSession` Conflicting With My Own
 
**Issue identified:** A teammate performed a substantial rework on his own branch and, working independently, wrote his own client-side `DataChannelSession` rather than using the one I had built, creating two competing implementations.
 
**Prompt (verbatim):**
> "My teammate just have a grand rework for a codebase, because he do in his own branch so he did not use my client datachannelsession but create his own. https://github.com/KangMing14/SocketProgramming/tree/hwng-PTC-Engineer is the branch he just worked on, explain what changes in the latest commit and their impact, and compare it agaist what have we done so far, especially about the changes related to my work."
 
**Raw AI output (summary):** AI pulled the branch and did a direct line-by-line comparison, finding the teammate's version correctly handled two real gaps mine did not (explicit `isFinal` marking via lookahead, and an empty-file edge case my version would have hung on), tied both to a `sendChunk` interface change I hadn't caught up to, and recommended adopting his version after verifying the specific gaps with a test.
 
**Refinement / problem-solving:** I directed the comparison specifically at "changes related to my work" rather than a generic branch review, and followed through by having the empty-file and `isFinal` gaps verified with a real test before deciding to adopt the teammate's implementation, rather than assuming the comparison alone was sufficient grounds to switch.
 
---
 
## Entry 12 — Unnoticed Changes to My Own Server-Side Files
 
**Issue identified:** While reviewing the teammate's rework, I noticed my own server-side files had also been modified without my direct involvement, and needed to know exactly what changed and whether any of it was critical before proceeding.
 
**Prompt (verbatim):**
> "I am about to change to his code to work entirely, I also notice that even my server side code have some changes, what are those and is it critical?"
 
**Raw AI output (summary):** AI checked commit history per-file across every file under my ownership, confirming most were untouched, but that `DataChannelSession.cpp` (server) had been substantially rewritten by the same teammate to apply the identical `isFinal`/empty-file fix found on the client side — assessed as a necessary, correctly-implemented change, but flagged as code I now needed to personally review for viva readiness since I hadn't authored the current version.
 
**Refinement / problem-solving:** I treated "did something change" and "is it safe" as two separate questions worth asking explicitly, rather than assuming a passing build meant no review was needed.
 
---
 
*End of log excerpt — additional entries to be appended as development continues.*
 