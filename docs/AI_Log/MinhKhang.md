### GenAI Log Entry #01
Date: 19/07/2026
Team member: Do Minh Khang - Member C
Module: Get permissions string for LIST

**Prompt used (verbatim):**
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

**Critical refinement performed:**
- AI seperate the fuction into 2 smaller functions. We merge them into one because the fuction only called once at `LIST` command
- AI using too many `else if` for checking file type. Take advance of the `file_status`, we switch from `if else` to `switch(status.type())`

**Verification:** 
- How the team confirmed correctness after refinement (test run, edge case, code review by 
  a teammate who didn't write it)