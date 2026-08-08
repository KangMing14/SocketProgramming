#include "Sha256Hasher.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <vector>

namespace {
bool failed(NTSTATUS status) {
    return status < 0;
}

class AlgorithmHandle {
public:
    ~AlgorithmHandle() {
        if (value != nullptr) BCryptCloseAlgorithmProvider(value, 0);
    }

    BCRYPT_ALG_HANDLE value = nullptr;
};

class HashHandle {
public:
    ~HashHandle() {
        if (value != nullptr) BCryptDestroyHash(value);
    }

    BCRYPT_HASH_HANDLE value = nullptr;
};
}

namespace Sha256Hasher {

std::string hashFile(const fs::path& filePath) {
    AlgorithmHandle algorithm;
    if (failed(BCryptOpenAlgorithmProvider(
            &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }

    DWORD resultSize = 0;
    DWORD hashObjectSize = 0;
    if (failed(BCryptGetProperty(
            algorithm.value, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjectSize), sizeof(hashObjectSize),
            &resultSize, 0)) ||
        resultSize != sizeof(hashObjectSize) || hashObjectSize == 0) {
        return {};
    }

    DWORD hashLength = 0;
    if (failed(BCryptGetProperty(
            algorithm.value, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
            &resultSize, 0)) ||
        resultSize != sizeof(hashLength) || hashLength == 0) {
        return {};
    }

    std::vector<UCHAR> hashObject(hashObjectSize);
    std::vector<UCHAR> digest(hashLength);
    HashHandle hash;
    if (failed(BCryptCreateHash(
            algorithm.value, &hash.value, hashObject.data(), hashObjectSize,
            nullptr, 0, 0))) {
        return {};
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input) return {};

    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const auto bytesRead = static_cast<ULONG>(input.gcount());
        if (failed(BCryptHashData(
                hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
                bytesRead, 0))) {
            return {};
        }
    }
    if (input.bad()) return {};

    if (failed(BCryptFinishHash(
            hash.value, digest.data(), hashLength, 0))) {
        return {};
    }

    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const UCHAR byte : digest) {
        result.push_back(HEX_DIGITS[(byte >> 4) & 0x0f]);
        result.push_back(HEX_DIGITS[byte & 0x0f]);
    }
    return result;
}

}
