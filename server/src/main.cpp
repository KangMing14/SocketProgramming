#include "control/Session.h"
#include "filesystem/PathResolver.h"
#include "filesystem/DirectoryService.h"
#include <filesystem>

// Global resolver & service
PathResolver g_pathResolver(std::filesystem::absolute("server_root"));
DirectoryService g_dirService(g_pathResolver);

int main(){
    std::filesystem::create_directories(g_pathResolver.root());
    Session::runSession();
    return 0;
}