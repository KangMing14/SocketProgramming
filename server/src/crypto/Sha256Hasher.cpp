#include "Sha256Hasher.h"

#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace Sha256Hasher {
	std::string hashFile(const fs::path& filePath) {
		BCRYPT_ALG_HANDLE hAlg = nullptr;
		NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (status < 0) return "";

		DWORD hashObjSize = 0, dataSize = 0;
		BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &dataSize, 0);
		std::vector<UCHAR> hashObj(hashObjSize);

		DWORD hashLen = 0;
		BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(DWORD), &dataSize, 0);
		std::vector<UCHAR> hashResult(hashLen);

		BCRYPT_HASH_HANDLE hHash = nullptr;
		BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0);

		std::ifstream file(filePath, std::ios::binary);
		if (!file) return "";

		constexpr size_t CHUNK_SIZE = 1024;
		std::vector<char> chunk(CHUNK_SIZE);
		while (file.read(chunk.data(), CHUNK_SIZE) || file.gcount() > 0)
			BCryptHashData(hHash, reinterpret_cast<PUCHAR>(chunk.data()), static_cast<ULONG>(file.gcount()), 0);
		
		std::vector<char> finalHex;
		BCryptFinishHash(hHash, hashResult.data(), hashLen, 0);

		static const char hexDigits[] = "0123456789abcdef";
		std::string hex;
		hex.reserve(hashLen * 2);
		for (DWORD i = 0; i < hashLen; i++) {
			hex.push_back(hexDigits[(hashResult[i] >> 4) & 0xF]);
			hex.push_back(hexDigits[hashResult[i] & 0xF]);
		}

		BCryptDestroyHash(hHash);
		BCryptCloseAlgorithmProvider(hAlg, 0);

		return hex;
	}
}