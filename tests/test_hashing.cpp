#include "Sha256Hasher.h"
#include <fstream>
#include <cassert>
#include <iostream>

void test_known_vector_empty_file() {
    // SHA-256 of an empty input is a well-known, publicly documented constant —
    // this is the single strongest test available: it doesn't just check
    // "hashing twice gives the same result," it checks your implementation
    // against the ACTUAL correct SHA-256 algorithm output.
    std::ofstream("empty_test.bin", std::ios::binary).close();
    std::string hash = Sha256Hasher::hashFile("empty_test.bin");
    assert(hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::cout << "[PASS] known SHA-256 vector (empty file)\n";
}

void test_deterministic() {
    // Hashing the same unchanged file twice must give the identical result.
    std::string h1 = Sha256Hasher::hashFile("empty_test.bin");
    std::string h2 = Sha256Hasher::hashFile("empty_test.bin");
    assert(h1 == h2);
    std::cout << "[PASS] deterministic hashing\n";
}

void test_avalanche_effect() {
    std::ofstream out("avalanche_test.bin", std::ios::binary);
    out.put(0x00);
    out.close();
    std::string hashBefore = Sha256Hasher::hashFile("avalanche_test.bin");

    // Flip a single bit (0x00 -> 0x01) and rehash
    std::ofstream out2("avalanche_test.bin", std::ios::binary);
    out2.put(0x01);
    out2.close();
    std::string hashAfter = Sha256Hasher::hashFile("avalanche_test.bin");

    assert(hashBefore != hashAfter);
    // Not just "different" — count differing hex characters to demonstrate
    // the change is substantial, not localized to one part of the digest.
    int differingChars = 0;
    for (size_t i = 0; i < hashBefore.size(); i++)
        if (hashBefore[i] != hashAfter[i]) differingChars++;
    assert(differingChars > 20); // a real avalanche affects roughly half the output
    std::cout << "[PASS] avalanche effect (single-bit change, "
        << differingChars << "/64 hex chars differ)\n";
}

void test_nonexistent_file_returns_empty() {
    std::string hash = Sha256Hasher::hashFile("this_file_does_not_exist.bin");
    assert(hash.empty());
    std::cout << "[PASS] nonexistent file returns empty string\n";
}

int main() {
    test_known_vector_empty_file();
    test_deterministic();
    test_avalanche_effect();
    test_nonexistent_file_returns_empty();

    std::cout << "\n---All Hashing tests passed---\n";

}