cmake_minimum_required(VERSION 3.20)
project(HybridFTP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ftp_core STATIC
    server/src/control/CommandDispatcher.cpp
    server/src/control/CommandParser.cpp
    server/src/control/Session.cpp
)

target_include_directories(ftp_core PUBLIC
    server/src
    server/src/filesystem
    #server/src/crypto
    #server/src/datachannel
    server/src/control
    #server/src/rdt
    server/src/common
)

# Windows-only libs: Winsock + CNG
if(WIN32)
    target_link_libraries(ftp_core PUBLIC ws2_32 bcrypt)
endif()

# ---- Server executable ----
add_executable(ftp_server server/src/main.cpp)
target_link_libraries(ftp_server PRIVATE ftp_core)

# ---- Client executable ----
#add_executable(ftp_client client/src/main.cpp)
#target_link_libraries(ftp_client PRIVATE ftp_core)