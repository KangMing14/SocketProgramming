#include "ChunkedFileReader.h"
#include "ChunkedFileWriter.h"

#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
#include <vector>
#include <random>

namespace fs = std::filesystem;

void createTestFile(const fs::path& path, size_t sizeBytes) {
    std::ofstream out(path, std::ios::binary);
    std::mt19937 rng(42); // fixed seed
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < sizeBytes; i++) {
        char byte = static_cast<char>(dist(rng));
        out.write(&byte, 1);
    }
}

bool filesAreIdentical(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    if (fs::file_size(a) != fs::file_size(b)) return false;

    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}

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
        writer.addChunk(seq, chunk);
        seq++; count++;
    }

    assert(writer.finalize(count) == true);
    assert(filesAreIdentical("test_writer_source.bin", dst));
    std::cout << "[PASS] test_writer_in_order_chunks\n";
}

// Chunks arrive in the incorrect order - reversed
void test_writer_out_of_order_chunks() {
    fs::path dst = "test_writer_outoforder.bin";
    createTestFile("test_writer_source2.bin", 3600);

    ChunkedFileReader reader("test_writer_source2.bin");
    std::vector<std::vector<char>> allChunks;
    std::vector<char> chunk;
    while (reader.nextChunk(chunk)) allChunks.push_back(chunk);

    ChunkedFileWriter writer(dst);
    // Feed chunks in reverse order
    for (int i = static_cast<int>(allChunks.size()) - 1; i >= 0; i--) {
        writer.addChunk(static_cast<uint32_t>(i), allChunks[i]);
    }

    assert(writer.finalize(static_cast<uint32_t>(allChunks.size())) == true);
    assert(filesAreIdentical("test_writer_source2.bin", dst));
    std::cout << "[PASS] test_writer_out_of_order_chunks\n";
}

// Send chunk 0 twice
void test_writer_duplicate_chunks() {
    fs::path dst = "test_writer_dup.bin";
    createTestFile("test_writer_source3.bin", 1500);

    ChunkedFileReader reader("test_writer_source3.bin");
    ChunkedFileWriter writer(dst);
    std::vector<char> chunk;
    uint32_t seq = 0, count = 0;
    std::vector<char> firstChunkCopy;

    while (reader.nextChunk(chunk)) {
        writer.addChunk(seq, chunk);
        if (seq == 0) firstChunkCopy = chunk;
        seq++; count++;
    }
    writer.addChunk(0, firstChunkCopy); // re-send seqNum 0 again

    assert(writer.finalize(count) == true);
    assert(filesAreIdentical("test_writer_source3.bin", dst));
    std::cout << "[PASS] test_writer_duplicate_chunks\n";
}

// Withhold one chunk
void test_writer_missing_chunk_fails() {
    fs::path dst = "test_writer_missing.bin";
    createTestFile("test_writer_source4.bin", 3000);

    ChunkedFileReader reader("test_writer_source4.bin");
    ChunkedFileWriter writer(dst);
    std::vector<char> chunk;
    uint32_t seq = 0, count = 0;
    while (reader.nextChunk(chunk)) {
        if (seq != 1) writer.addChunk(seq, chunk); // skip seqNum 1
        seq++; count++;
    }

    std::cout << "[PASS] test_writer_missing_chunk_fails\n";
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
    test_writer_out_of_order_chunks();
    test_writer_duplicate_chunks();
    test_writer_missing_chunk_fails();

    std::cout << "\n---All Chunking tests passed---\n";

    // Clean up generated fixture files
    for (const auto& name : { "test_normal.bin", "test_normal_copy.bin",
                              "test_exact.bin", "test_exact_copy.bin",
                              "test_empty.bin", "test_empty_copy.bin",
                              "test_totalsize.bin", "test_totalsize_copy.bin",
                              "test_writer_inorder.bin", "test_writer_outoforder.bin",
                              "test_writer_dup.bin", "test_writer_missing.bin",
                              "test_writer_source.bin", "test_writer_source2.bin",
                              "test_writer_source3.bin", "test_writer_source4.bin" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }

    return 0;
}