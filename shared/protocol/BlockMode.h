#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class BlockEncoder {
public:
    static constexpr std::uint8_t EndOfRecord = 0x80;
    static constexpr std::uint8_t EndOfFile = 0x40;
    static constexpr std::uint8_t SuspectedError = 0x20;
    static constexpr std::size_t HeaderSize = 3;
    static constexpr std::size_t MaximumDataSize = 65535;

    bool appendData(const std::vector<char>& input,
                    std::vector<std::vector<char>>& records) const;
    bool appendEndOfFile(std::vector<std::vector<char>>& records) const;

private:
    static std::vector<char> makeRecord(std::uint8_t descriptor,
                                        const char* data,
                                        std::size_t size);
};

class BlockDecoder {
public:
    bool appendPayload(const std::vector<char>& payload);
    bool nextRecord(std::vector<char>& data, bool& endOfFile,
                    bool& endOfRecord);

    bool isValid() const noexcept;
    bool sawEndOfFile() const noexcept;
    bool hasBufferedData() const noexcept;
    bool finish();

private:
    std::vector<char> buffer_;
    bool valid_ = true;
    bool endOfFileSeen_ = false;
};
