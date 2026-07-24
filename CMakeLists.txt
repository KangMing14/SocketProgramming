cmake_minimum_required(VERSION 3.20)
project(HybridFTP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Shared library: everything under server/src except main.cpp ----
add_library(ftp_core STATIC
    server/src/filesystem/PathResolver.cpp
    server/src/filesystem/DirectoryService.cpp
    server/src/filesystem/ChunkedFileReader.cpp
    server/src/filesystem/ChunkedFileWriter.cpp
    server/src/crypto/Sha256Hasher.cpp
)

target_include_directories(ftp_core PUBLIC
    server/src
    server/src/filesystem
    server/src/crypto
    #server/src/datachannel
    #server/src/control
    #server/src/rdt
    server/src/common
)

# Windows-only libs: Winsock + CNG
if(WIN32)
    target_link_libraries(ftp_core PUBLIC ws2_32 bcrypt)
endif()

# ---- Server executable ----
#add_executable(ftp_server server/src/main.cpp)
#target_link_libraries(ftp_server PRIVATE ftp_core)

# ---- Client executable ----
#add_executable(ftp_client client/src/main.cpp)
#target_link_libraries(ftp_client PRIVATE ftp_core)

# ---- Tests ----
enable_testing()
add_executable(test_path_safety tests/test_path_safety.cpp)
target_link_libraries(test_path_safety PRIVATE ftp_core)
add_test(NAME PathSafety COMMAND test_path_safety)

add_executable(test_chunking tests/test_chunking.cpp)
target_link_libraries(test_chunking PRIVATE ftp_core)
add_test(NAME Chunking COMMAND test_chunking)

add_executable(test_hashing tests/test_hashing.cpp)
target_link_libraries(test_hashing PRIVATE ftp_core)
add_test(NAME Hashing COMMAND test_hashing)

#add_executable(test_data_channel tests/test_data_channel.cpp)
#target_link_libraries(test_data_channel PRIVATE ftp_core)
#add_test(NAME DataChannel COMMAND test_data_channel)