#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
#include <vector>
#include <random>

#include "helper.h"

namespace fs = std::filesystem;

// ------ChuckedRead------
size_t chunkedCopy(const fs::path& srcPath, const fs::path& dstPath) {
    ChunkedFileReader reader(srcPath);
    assert(reader.isOpen() && "Reader failed to open source file");

    std::ofstream out(dstPath, std::ios::binary);
    assert(out && "Failed to open destination file for writing");

    std::vector<char> chunk;
    size_t totalBytes = 0;
    while (reader.nextChunk(chunk)) {
        assert(chunk.size() > 0 && "nextChunk returned true with an empty chunk");
        assert(chunk.size() <= ChunkedFileReader::CHUNK_SIZE && "chunk exceeded CHUNK_SIZE");

        out.write(chunk.data(), chunk.size());  // chunk.size(), never CHUNK_SIZE
        totalBytes += chunk.size();
    }
    return totalBytes;
}

void test_normal_binary_file() {
    // 2500 bytes = 2 full 1024-byte chunks + 1 partial 452-byte chunk
    fs::path src = "test_normal.bin";
    fs::path dst = "test_normal_copy.bin";
    createTestFile(src, 2500);

    size_t totalRead = chunkedCopy(src, dst);
    assert(totalRead == 2500);
    assert(filesAreIdentical(src, dst));

    std::cout << "[PASS] test_normal_binary_file (2500 bytes, partial last chunk)\n";
}

void test_exact_multiple_of_chunk_size() {
    // 1024 * 3 = exactly 3 full chunks, no partial chunk at all.
    fs::path src = "test_exact.bin";
    fs::path dst = "test_exact_copy.bin";
    createTestFile(src, ChunkedFileReader::CHUNK_SIZE * 3);

    size_t totalRead = chunkedCopy(src, dst);
    assert(totalRead == ChunkedFileReader::CHUNK_SIZE * 3);
    assert(filesAreIdentical(src, dst));

    std::cout << "[PASS] test_exact_multiple_of_chunk_size\n";
}

void test_empty_file() {
    // 0 bytes
    fs::path src = "test_empty.bin";
    fs::path dst = "test_empty_copy.bin";
    createTestFile(src, 0);

    size_t totalRead = chunkedCopy(src, dst);
    assert(totalRead == 0);
    assert(fs::file_size(dst) == 0);
    assert(filesAreIdentical(src, dst));

    std::cout << "[PASS] test_empty_file\n";
}

void test_nonexistent_file_fails_cleanly() {
    // No file created
    ChunkedFileReader reader("this_file_does_not_exist_12345.bin");
    assert(!reader.isOpen());

    std::cout << "[PASS] test_nonexistent_file_fails_cleanly\n";
}

void test_totalSize_matches_actual_bytes_read() {
    // Independently verifies totalSize() (computed once via fs::file_size in
    // the constructor) agrees with the sum of bytes actually retrieved via
    // repeated nextChunk() calls.
    fs::path src = "test_totalsize.bin";
    fs::path dst = "test_totalsize_copy.bin";
    createTestFile(src, 3700);

    ChunkedFileReader reader(src);
    assert(reader.isOpen());
    uintmax_t reportedSize = reader.getTotalSize();

    size_t totalRead = chunkedCopy(src, dst);
    assert(reportedSize == totalRead);

    std::cout << "[PASS] test_totalSize_matches_actual_bytes_read\n";
}

// ------ChuckedWrite------
// Chunks arrive in the correct order
void test_writer_in_order_chunks() {
    fs::path dst = "test_writer_inorder.bin";
    createTestFile("test_writer_source.bin", 2500);

    ChunkedFileReader reader("test_writer_source.bin");
    ChunkedFileWriter writer(dst);

    std::vector<char> chunk;
    uint32_t seq = 0, count = 0;
    while (reader.nextChunk(chunk)) {
        assert(writer.appendChunk(seq, chunk));
        seq++; count++;
    }

    assert(writer.commit());
    assert(filesAreIdentical("test_writer_source.bin", dst));
    std::cout << "[PASS] test_writer_in_order_chunks\n";
}

// RDT must reorder chunks before handing them to the streaming writer.
void test_writer_rejects_out_of_order_chunks() {
    fs::path dst = "test_writer_outoforder.bin";
    createTestFile(dst, 17);
    const std::vector<char> chunk{'x'};

    ChunkedFileWriter writer(dst);
    assert(!writer.appendChunk(1, chunk));
    writer.abort();
    assert(fs::file_size(dst) == 17);
    std::cout << "[PASS] test_writer_rejects_out_of_order_chunks\n";
}

void test_writer_rejects_duplicate_chunks() {
    fs::path dst = "test_writer_dup.bin";
    const std::vector<char> chunk{'a', 'b', 'c'};
    ChunkedFileWriter writer(dst);
    assert(writer.appendChunk(0, chunk));
    assert(!writer.appendChunk(0, chunk));
    writer.abort();
    assert(!fs::exists(dst));
    std::cout << "[PASS] test_writer_rejects_duplicate_chunks\n";
}

void test_writer_abort_preserves_existing_destination() {
    fs::path dst = "test_writer_missing.bin";
    fs::path expected = "test_writer_existing_expected.bin";
    createTestFile(dst, 333);
    fs::copy_file(dst, expected, fs::copy_options::overwrite_existing);
    ChunkedFileWriter writer(dst);
    assert(writer.appendChunk(0, std::vector<char>(1024, 'z')));
    writer.abort();
    assert(filesAreIdentical(dst, expected));
    std::cout << "[PASS] test_writer_abort_preserves_existing_destination\n";
}

void test_writer_replaces_existing_destination() {
    fs::path source = "test_writer_source4.bin";
    fs::path dst = "test_writer_replace.bin";
    createTestFile(source, 4097);
    createTestFile(dst, 19);

    ChunkedFileReader reader(source);
    ChunkedFileWriter writer(dst);
    std::vector<char> chunk;
    uint32_t sequence = 0;
    while (reader.nextChunk(chunk)) {
        assert(writer.appendChunk(sequence++, chunk));
    }
    assert(writer.commit());
    assert(filesAreIdentical(source, dst));
    std::cout << "[PASS] test_writer_replaces_existing_destination\n";
}

void test_writer_streams_large_file() {
    fs::path dst = "test_writer_large.bin";
    ChunkedFileWriter writer(dst);
    assert(writer.isValid());
    const std::vector<char> chunk(ChunkedFileReader::CHUNK_SIZE, 'L');
    constexpr uint32_t chunkCount = 16 * 1024;
    for (uint32_t sequence = 0; sequence < chunkCount; ++sequence) {
        assert(writer.appendChunk(sequence, chunk));
    }
    assert(writer.commit());
    assert(fs::file_size(dst) ==
           static_cast<uintmax_t>(chunk.size()) * chunkCount);
    std::cout << "[PASS] test_writer_streams_large_file\n";
}


int main() {
    std::cout << "ChunkedFileReader tests:\n";
    test_normal_binary_file();
    test_exact_multiple_of_chunk_size();
    test_empty_file();
    test_nonexistent_file_fails_cleanly();
    test_totalSize_matches_actual_bytes_read();

    std::cout << "\nChunkedFileWriter tests:\n";
    test_writer_in_order_chunks();
    test_writer_rejects_out_of_order_chunks();
    test_writer_rejects_duplicate_chunks();
    test_writer_abort_preserves_existing_destination();
    test_writer_replaces_existing_destination();
    test_writer_streams_large_file();

    std::cout << "\n---All Chunking tests passed---\n";

    // Clean up generated fixture files
    for (const auto& name : { "test_normal.bin", "test_normal_copy.bin",
                              "test_exact.bin", "test_exact_copy.bin",
                              "test_empty.bin", "test_empty_copy.bin",
                              "test_totalsize.bin", "test_totalsize_copy.bin",
                              "test_writer_inorder.bin", "test_writer_outoforder.bin",
                              "test_writer_dup.bin", "test_writer_missing.bin",
                              "test_writer_existing_expected.bin",
                              "test_writer_replace.bin",
                              "test_writer_large.bin",
                              "test_writer_source.bin", "test_writer_source2.bin",
                              "test_writer_source3.bin", "test_writer_source4.bin" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }

    return 0;
}
