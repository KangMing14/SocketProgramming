cmake_minimum_required(VERSION 3.20)
project(HybridFTP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ftp_protocol STATIC
    shared/protocol/RdtHeader.cpp
    shared/protocol/CheckSum.cpp
)
target_include_directories(ftp_protocol PUBLIC shared/protocol)

add_library(ftp_server_core STATIC
    server/src/filesystem/PathResolver.cpp
    server/src/filesystem/DirectoryService.cpp
    server/src/filesystem/ChunkedFileReader.cpp
    server/src/filesystem/ChunkedFileWriter.cpp
    server/src/filesystem/AsciiTranslator.cpp
    server/src/filesystem/AsciiChunkedReader.cpp
    server/src/crypto/Sha256Hasher.cpp
    server/src/datachannel/DataChannelSession.cpp
    server/src/datachannel/PassiveModeHandler.cpp
    server/src/datachannel/ActiveModeHandler.cpp
    server/src/control/CommandDispatcher.cpp
    server/src/control/CommandParser.cpp
    server/src/control/Session.cpp
    server/src/rdt/RdtSender.cpp
    server/src/rdt/RdtReceiver.cpp
)
target_include_directories(ftp_server_core PUBLIC
    server/src
    server/src/filesystem
    server/src/crypto
    server/src/datachannel
    server/src/control
    server/src/rdt
    server/src/common
)
target_link_libraries(ftp_server_core PUBLIC ftp_protocol)

add_library(ftp_client_core STATIC
    client/src/control/Session.cpp
    client/src/datachannel/ActiveModeClient.cpp
    client/src/datachannel/PassiveModeClient.cpp
    client/src/datachannel/DataChannelSession.cpp
    client/src/fileio/ChunkedFileReader.cpp
    client/src/fileio/ChunkedFileWriter.cpp
    client/src/fileio/AsciiTranslator.cpp
    client/src/fileio/AsciiChunkedReader.cpp
    client/src/crypto/Sha256Hasher.cpp
    client/src/rdt/RdtSender.cpp
    client/src/rdt/RdtReceiver.cpp
)
target_include_directories(ftp_client_core PUBLIC
    client/src
    client/src/control
    client/src/datachannel
    client/src/fileio
    client/src/rdt
    client/src/common
)
target_link_libraries(ftp_client_core PUBLIC ftp_protocol)

option(ENABLE_RDT_DEBUG "Enable RDT debug logging" OFF)
if(ENABLE_RDT_DEBUG)
    target_compile_definitions(ftp_server_core PRIVATE RDT_DEBUG=1)
else()
    target_compile_definitions(ftp_server_core PRIVATE RDT_DEBUG=0)
endif()

option(ENABLE_CHAOS_MODE "Enable artificial RDT network failures" OFF)
if(ENABLE_CHAOS_MODE)
    target_compile_definitions(ftp_server_core PRIVATE CHAOS_MODE=1)
else()
    target_compile_definitions(ftp_server_core PRIVATE CHAOS_MODE=0)
endif()

if(WIN32)
    target_link_libraries(ftp_protocol PUBLIC ws2_32)
    target_link_libraries(ftp_server_core PUBLIC ws2_32 bcrypt)
    target_link_libraries(ftp_client_core PUBLIC ws2_32 bcrypt)
endif()

add_executable(ftp_server server/src/main.cpp)
target_link_libraries(ftp_server PRIVATE ftp_server_core)

add_executable(ftp_client client/src/main.cpp)
target_link_libraries(ftp_client PRIVATE ftp_client_core)

enable_testing()

function(add_server_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ftp_server_core)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_server_test(test_path_safety tests/test_path_safety.cpp)
add_server_test(test_chunking tests/test_chunking.cpp)
add_server_test(test_hashing tests/test_hashing.cpp)
add_server_test(test_rdt tests/test_rdt.cpp)
add_server_test(test_rdt_payload_bounds tests/test_rdt_payload_bounds.cpp)
add_server_test(test_rdt_integration tests/test_rdt_integration.cpp)
add_server_test(test_rdt_congestion tests/test_rdt_congestion.cpp)
add_server_test(test_ascii_translator tests/test_ascii_translator.cpp)
add_server_test(test_temp_transfer tests/test_temp_transfer.cpp)

add_executable(test_transfer_matrix tests/test_transfer_matrix.cpp)
target_link_libraries(test_transfer_matrix PRIVATE ftp_server_core ftp_client_core)
add_test(NAME test_transfer_matrix COMMAND test_transfer_matrix)

add_executable(test_active_stor_control tests/test_active_stor_control.cpp)
target_link_libraries(test_active_stor_control PRIVATE ftp_server_core ftp_client_core)
add_test(NAME test_active_stor_control COMMAND test_active_stor_control)

add_executable(test_remaining_commands tests/test_remaining_commands.cpp)
target_link_libraries(test_remaining_commands PRIVATE ftp_server_core ftp_client_core)
add_test(NAME test_remaining_commands COMMAND test_remaining_commands)

add_executable(test_temp_transfer_client tests/test_temp_transfer_client.cpp)
target_link_libraries(test_temp_transfer_client PRIVATE ftp_client_core)
add_test(NAME test_temp_transfer_client COMMAND test_temp_transfer_client)

if(ENABLE_CHAOS_MODE)
    add_server_test(test_chaos_integrity tests/test_chaos_integrity.cpp)
    set_tests_properties(test_chaos_integrity PROPERTIES LABELS "chaos")
else()
    set_tests_properties(
        test_rdt_integration test_rdt_congestion
        test_transfer_matrix
        test_active_stor_control
        test_remaining_commands
        PROPERTIES LABELS "deterministic"
    )
endif()
