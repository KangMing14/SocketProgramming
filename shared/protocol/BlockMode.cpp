#include "BlockMode.h"

#include <algorithm>
#include <limits>

namespace {
constexpr std::uint8_t SupportedDescriptorMask =
    BlockEncoder::EndOfRecord | BlockEncoder::EndOfFile |
    BlockEncoder::SuspectedError;
}

std::vector<char> BlockEncoder::makeRecord(std::uint8_t descriptor,
                                           const char* data,
                                           std::size_t size) {
    std::vector<char> record;
    record.reserve(HeaderSize + size);
    record.push_back(static_cast<char>(descriptor));
    record.push_back(static_cast<char>((size >> 8) & 0xff));
    record.push_back(static_cast<char>(size & 0xff));
    if (size != 0) record.insert(record.end(), data, data + size);
    return record;
}

bool BlockEncoder::appendData(
    const std::vector<char>& input,
    std::vector<std::vector<char>>& records) const {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t size =
            std::min(MaximumDataSize, input.size() - offset);
        records.push_back(makeRecord(0, input.data() + offset, size));
        offset += size;
    }
    return true;
}

bool BlockEncoder::appendEndOfFile(
    std::vector<std::vector<char>>& records) const {
    records.push_back(makeRecord(EndOfFile, nullptr, 0));
    return true;
}

bool BlockDecoder::appendPayload(const std::vector<char>& payload) {
    if (!valid_ || (endOfFileSeen_ && !payload.empty())) {
        valid_ = false;
        return false;
    }
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
    return true;
}

bool BlockDecoder::nextRecord(std::vector<char>& data, bool& endOfFile,
                              bool& endOfRecord) {
    data.clear();
    endOfFile = false;
    endOfRecord = false;
    if (!valid_ || buffer_.size() < BlockEncoder::HeaderSize) return false;

    const auto descriptor = static_cast<std::uint8_t>(buffer_[0]);
    if ((descriptor & ~SupportedDescriptorMask) != 0) {
        valid_ = false;
        return false;
    }

    const auto high = static_cast<std::uint8_t>(buffer_[1]);
    const auto low = static_cast<std::uint8_t>(buffer_[2]);
    const std::size_t size = (static_cast<std::size_t>(high) << 8) | low;
    const std::size_t recordSize = BlockEncoder::HeaderSize + size;
    if (buffer_.size() < recordSize) return false;

    data.assign(buffer_.begin() + BlockEncoder::HeaderSize,
                buffer_.begin() + recordSize);
    endOfFile = (descriptor & BlockEncoder::EndOfFile) != 0;
    endOfRecord = (descriptor & BlockEncoder::EndOfRecord) != 0;
    buffer_.erase(buffer_.begin(), buffer_.begin() + recordSize);

    if (endOfFile) {
        endOfFileSeen_ = true;
        if (!buffer_.empty()) valid_ = false;
    }
    return valid_;
}

bool BlockDecoder::isValid() const noexcept { return valid_; }

bool BlockDecoder::sawEndOfFile() const noexcept { return endOfFileSeen_; }

bool BlockDecoder::hasBufferedData() const noexcept { return !buffer_.empty(); }

bool BlockDecoder::finish() {
    if (!valid_ || !endOfFileSeen_ || !buffer_.empty()) valid_ = false;
    return valid_;
}
