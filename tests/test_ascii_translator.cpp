#include "helper.h"
#include <cassert>
#include <vector>
#include <winsock2.h>
#include <iostream>
#include "AsciiChunkedReader.h"
#include "AsciiTranslator.h"
#include "DataChannelSession.h"

void test_bare_lf_promoted_to_crlf() {
    AsciiTranslator t;
    std::vector<char> in = { 'a', '\n', 'b' };
    std::vector<char> out = t.encode(in);
    std::vector<char> expected = { 'a', '\r', '\n', 'b' };
    assert(out == expected);
    std::cout << "[PASS] test_bare_lf_promoted_to_crlf\n";
}

void test_existing_crlf_not_doubled() {
    AsciiTranslator t;
    std::vector<char> in = { 'a', '\r', '\n', 'b' };
    std::vector<char> out = t.encode(in);
    assert(out == in);
    std::cout << "[PASS] test_existing_crlf_not_doubled\n";
}

void test_crlf_split_across_chunk_boundary() {
    AsciiTranslator t;
    std::vector<char> chunk1 = { 'a', '\r' };
    std::vector<char> chunk2 = { '\n', 'b' };

    std::vector<char> out1 = t.encode(chunk1);
    std::vector<char> out2 = t.encode(chunk2);

    std::vector<char> combined = out1;
    combined.insert(combined.end(), out2.begin(), out2.end());
    std::vector<char> expected = { 'a', '\r', '\n', 'b' };
    assert(combined == expected);
    std::cout << "[PASS] test_crlf_split_across_chunk_boundary\n";
}

void test_lone_cr_not_followed_by_lf_passed_through() {
    AsciiTranslator t;
    std::vector<char> in = { 'a', '\r', 'b' };
    std::vector<char> out = t.encode(in);
    assert(out == in);
    std::cout << "[PASS] test_lone_cr_not_followed_by_lf_passed_through\n";
}

void test_reader_never_exceeds_chunk_size_under_pathological_growth() {
    createTestFile("all_newlines.txt", ChunkedFileReader::CHUNK_SIZE);
    {
        std::ofstream out("all_newlines.txt", std::ios::binary);
        for (size_t i = 0; i < ChunkedFileReader::CHUNK_SIZE; i++) out.put('\n');
    }
    AsciiChunkedReader reader("all_newlines.txt");
    assert(reader.isOpen());
    std::vector<char> chunk;
    while (reader.nextChunk(chunk)) {
        assert(chunk.size() <= ChunkedFileReader::CHUNK_SIZE);
    }
    std::cout << "[PASS] test_reader_never_exceeds_chunk_size_under_pathological_growth\n";
}

void test_full_pipeline_ascii_round_trip() {
    {
        std::ofstream out("mixed_endings.txt", std::ios::binary);
        out << "line one\n" << "line two\r\n" << "line three\n";
    }

    FakeRdtTransport transport;
    sockaddr_in dummy{};
    DataChannelSession sender(INVALID_SOCKET, dummy, transport);
    assert(sender.sendFile("mixed_endings.txt", TransferMode::ASCII) == true);

    DataChannelSession receiver(INVALID_SOCKET, dummy, transport);
    assert(receiver.receiveFile("mixed_endings_result.txt", TransferMode::ASCII) == true);

    std::ifstream result("mixed_endings_result.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    assert(content == "line one\r\nline two\r\nline three\r\n");
    std::cout << "[PASS] test_full_pipeline_ascii_round_trip\n";
}

int main() {
    test_bare_lf_promoted_to_crlf();
    test_existing_crlf_not_doubled();
    test_crlf_split_across_chunk_boundary();
    test_lone_cr_not_followed_by_lf_passed_through();
    test_reader_never_exceeds_chunk_size_under_pathological_growth();
    test_full_pipeline_ascii_round_trip();
    std::cout << "\n---All ASCII tests passed---\n";

    for (const auto& name : { "all_newlines.txt", "mixed_endings.txt",
                              "mixed_endings_result.txt"}) {
        std::error_code ec;
        fs::remove(name, ec);
    }
}