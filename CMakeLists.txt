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
    server/src/filesystem/AsciiTranslator.cpp
    server/src/filesystem/AsciiChunkedReader.cpp
    server/src/crypto/Sha256Hasher.cpp
    server/src/datachannel/DataChannelSession.cpp
    server/src/control/CommandDispatcher.cpp
    server/src/control/CommandParser.cpp
    server/src/control/Session.cpp
    server/src/rdt/RdtHeader.cpp
    server/src/rdt/CheckSum.cpp
    server/src/rdt/RdtSender.cpp
    server/src/rdt/RdtReceiver.cpp
    server/src/datachannel/PassiveModeHandler.cpp
    server/src/datachannel/ActiveModeHandler.cpp
    client/src/control/Session.cpp
)

target_include_directories(ftp_core PUBLIC
    server/src
    server/src/filesystem
    server/src/crypto
    server/src/datachannel
    server/src/control
    server/src/rdt
    server/src/common
    client/src
    client/src/control
    client/src/common
    client/scr/crypto
    client/scr/datachannel
    client/scr/fileio
    client/src/rdt
)

option(ENABLE_RDT_DEBUG "Enable RDT debug logging" OFF)
if(ENABLE_RDT_DEBUG)
    target_compile_definitions(ftp_core PRIVATE RDT_DEBUG=1)
else()
    target_compile_definitions(ftp_core PRIVATE RDT_DEBUG=0)
endif()

option(ENABLE_CHAOS_MODE "Enable artificial RDT network failures" OFF)
if(ENABLE_CHAOS_MODE)
    target_compile_definitions(ftp_core PRIVATE CHAOS_MODE=1)
else()
    target_compile_definitions(ftp_core PRIVATE CHAOS_MODE=0)
endif()

# Windows-only libs: Winsock + CNG
if(WIN32)
    target_link_libraries(ftp_core PUBLIC ws2_32 bcrypt)
endif()

# ---- Server executable ----
add_executable(ftp_server server/src/main.cpp)
target_link_libraries(ftp_server PRIVATE ftp_core)

# ---- Client executable ----
add_executable(ftp_client client/src/main.cpp)
target_link_libraries(ftp_client PRIVATE ftp_core)

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

#add_executable(test_ascii_translator tests/test_ascii_translator.cpp)
#target_link_libraries(test_ascii_translator PRIVATE ftp_core ws2_32)
#add_test(NAME ASCIITest COMMAND test_ascii_translator)

add_executable(test_rdt tests/test_rdt.cpp)
target_link_libraries(test_rdt PRIVATE ftp_core ws2_32)
add_test(NAME RdtTest COMMAND test_rdt)

add_executable(test_rdt_payload_bounds tests/test_rdt_payload_bounds.cpp)
target_link_libraries(test_rdt_payload_bounds PRIVATE ftp_core ws2_32)
add_test(NAME RdtPayloadTest COMMAND test_rdt_payload_bounds)

add_executable(test_rdt_integration tests/test_rdt_integration.cpp)
target_link_libraries(test_rdt_integration PRIVATE ftp_core ws2_32)

add_executable(test_rdt_congestion tests/test_rdt_congestion.cpp)
target_link_libraries(test_rdt_congestion PRIVATE ftp_core ws2_32)

if(ENABLE_CHAOS_MODE)
    add_executable(test_chaos_integrity tests/test_chaos_integrity.cpp)
    target_link_libraries(test_chaos_integrity PRIVATE ftp_core ws2_32)
    add_test(NAME ChaosTest COMMAND test_chaos_integrity)
    set_tests_properties(ChaosTest PROPERTIES LABELS "chaos")
else()
    add_test(NAME RdtIntegrationTest COMMAND test_rdt_integration)
    add_test(NAME RdtCongestionTest COMMAND test_rdt_congestion)
    set_tests_properties(
        RdtIntegrationTest RdtCongestionTest
        PROPERTIES LABELS "deterministic"
    )
endif()