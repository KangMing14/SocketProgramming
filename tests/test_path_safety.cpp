#include "PathResolver.h"

#include <filesystem>
#include <cassert>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    fs::create_directories("server_root/subdir"); // make sure test fixture exists
    PathResolver resolver(fs::absolute("server_root"));
    fs::path cwd = resolver.root();
    fs::path out;

    assert(resolver.resolve(cwd, "subdir", out) == true);
    assert(resolver.resolve(cwd, "../../../../windows/system32", out) == false);
    assert(resolver.resolve(cwd, "..\\..\\secret", out) == false);
    assert(resolver.resolve(cwd, "./subdir/../subdir", out) == true);

    std::cout << "All PathResolver tests passed!\n";
    return 0;
}