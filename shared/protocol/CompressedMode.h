#pragma once

#include "TransferMode.h"

#include <cstdint>
#include <vector>

class CompressedEncoder {
public:
    static constexpr std::uint8_t EndOfRecord = 0x80;
    static constexpr std::uint8_t EndOfFile = 0x40;
    static constexpr std::uint8_t SuspectedError = 0x20;

    explicit CompressedEncoder(TransferType type);

    bool appendData(const std::vector<char>& input,
                    std::vector<char>& encoded) const;
    bool appendEndOfFile(std::vector<char>& encoded) const;

private:
    char filler_;
};

class CompressedDecoder {
public:
    explicit CompressedDecoder(TransferType type);

    bool appendPayload(const std::vector<char>& payload);
    bool nextData(std::vector<char>& decoded, bool& endOfFile,
                  bool& endOfRecord, bool& suspectedError);

    bool isValid() const noexcept;
    bool sawEndOfFile() const noexcept;
    bool hasBufferedData() const noexcept;
    bool finish();

private:
    std::vector<char> buffer_;
    char filler_;
    bool valid_ = true;
    bool endOfFileSeen_ = false;
    bool descriptorPending_ = false;
    bool pendingEndOfRecord_ = false;
    bool pendingSuspectedError_ = false;
};
