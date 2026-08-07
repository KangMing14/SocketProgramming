#include "CompressedMode.h"

#include <algorithm>
#include <cstddef>

namespace {
constexpr std::uint8_t LiteralMask = 0x7f;
constexpr std::uint8_t ReplicatedPrefix = 0x80;
constexpr std::uint8_t FillerPrefix = 0xc0;
constexpr std::uint8_t CountMask = 0x3f;
constexpr std::uint8_t SupportedDescriptorMask =
    CompressedEncoder::EndOfRecord | CompressedEncoder::EndOfFile |
    CompressedEncoder::SuspectedError;

std::size_t repeatedLength(const std::vector<char>& input,
                           std::size_t offset) {
    std::size_t length = 1;
    while (offset + length < input.size() &&
           input[offset + length] == input[offset]) {
        ++length;
    }
    return length;
}
}

CompressedEncoder::CompressedEncoder(TransferType type)
    : filler_(type == TransferType::ASCII ? ' ' : '\0') {}

bool CompressedEncoder::appendData(const std::vector<char>& input,
                                   std::vector<char>& encoded) const {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t run = repeatedLength(input, offset);
        if (input[offset] == filler_) {
            std::size_t remaining = run;
            while (remaining != 0) {
                const std::size_t count = std::min<std::size_t>(remaining, 63);
                encoded.push_back(static_cast<char>(
                    FillerPrefix | static_cast<std::uint8_t>(count)));
                remaining -= count;
            }
            offset += run;
            continue;
        }

        if (run >= 2) {
            std::size_t remaining = run;
            while (remaining != 0) {
                const std::size_t count = std::min<std::size_t>(remaining, 63);
                encoded.push_back(static_cast<char>(
                    ReplicatedPrefix | static_cast<std::uint8_t>(count)));
                encoded.push_back(input[offset]);
                remaining -= count;
            }
            offset += run;
            continue;
        }

        const std::size_t literalStart = offset++;
        while (offset < input.size() &&
               offset - literalStart < LiteralMask) {
            if (input[offset] == filler_ || repeatedLength(input, offset) >= 2) {
                break;
            }
            ++offset;
        }
        const std::size_t count = offset - literalStart;
        encoded.push_back(static_cast<char>(count));
        encoded.insert(encoded.end(), input.begin() + literalStart,
                       input.begin() + offset);
    }
    return true;
}

bool CompressedEncoder::appendEndOfFile(std::vector<char>& encoded) const {
    encoded.push_back(0);
    encoded.push_back(static_cast<char>(EndOfFile));
    return true;
}

CompressedDecoder::CompressedDecoder(TransferType type)
    : filler_(type == TransferType::ASCII ? ' ' : '\0') {}

bool CompressedDecoder::appendPayload(const std::vector<char>& payload) {
    if (!valid_ || (endOfFileSeen_ && !payload.empty())) {
        valid_ = false;
        return false;
    }
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
    return true;
}

bool CompressedDecoder::nextData(std::vector<char>& decoded, bool& endOfFile,
                                 bool& endOfRecord,
                                 bool& suspectedError) {
    decoded.clear();
    endOfFile = false;
    endOfRecord = false;
    suspectedError = false;
    if (!valid_ || buffer_.empty()) return false;

    auto header = static_cast<std::uint8_t>(buffer_[0]);
    if (!descriptorPending_ && header == 0) {
        if (buffer_.size() < 2) return false;
        const auto descriptor = static_cast<std::uint8_t>(buffer_[1]);
        if (descriptor == 0 ||
            (descriptor & ~SupportedDescriptorMask) != 0) {
            valid_ = false;
            return false;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
        endOfFile = (descriptor & CompressedEncoder::EndOfFile) != 0;
        if (endOfFile) {
            if (endOfFileSeen_ || !buffer_.empty()) {
                valid_ = false;
                return false;
            }
            endOfFileSeen_ = true;
            endOfRecord =
                (descriptor & CompressedEncoder::EndOfRecord) != 0;
            suspectedError =
                (descriptor & CompressedEncoder::SuspectedError) != 0;
            return true;
        }
        descriptorPending_ = true;
        pendingEndOfRecord_ =
            (descriptor & CompressedEncoder::EndOfRecord) != 0;
        pendingSuspectedError_ =
            (descriptor & CompressedEncoder::SuspectedError) != 0;
        if (buffer_.empty()) return false;
        header = static_cast<std::uint8_t>(buffer_[0]);
    }

    if (header == 0) {
        valid_ = false;
        return false;
    }

    if ((header & ReplicatedPrefix) == 0) {
        const std::size_t count = header & LiteralMask;
        if (buffer_.size() < count + 1) return false;
        decoded.assign(buffer_.begin() + 1, buffer_.begin() + count + 1);
        buffer_.erase(buffer_.begin(), buffer_.begin() + count + 1);
        endOfRecord = pendingEndOfRecord_;
        suspectedError = pendingSuspectedError_;
        descriptorPending_ = false;
        pendingEndOfRecord_ = false;
        pendingSuspectedError_ = false;
        return true;
    }

    const std::size_t count = header & CountMask;
    if (count == 0) {
        valid_ = false;
        return false;
    }
    if ((header & FillerPrefix) == FillerPrefix) {
        decoded.assign(count, filler_);
        buffer_.erase(buffer_.begin());
        endOfRecord = pendingEndOfRecord_;
        suspectedError = pendingSuspectedError_;
        descriptorPending_ = false;
        pendingEndOfRecord_ = false;
        pendingSuspectedError_ = false;
        return true;
    }

    if (buffer_.size() < 2) return false;
    decoded.assign(count, buffer_[1]);
    buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
    endOfRecord = pendingEndOfRecord_;
    suspectedError = pendingSuspectedError_;
    descriptorPending_ = false;
    pendingEndOfRecord_ = false;
    pendingSuspectedError_ = false;
    return true;
}

bool CompressedDecoder::isValid() const noexcept { return valid_; }

bool CompressedDecoder::sawEndOfFile() const noexcept {
    return endOfFileSeen_;
}

bool CompressedDecoder::hasBufferedData() const noexcept {
    return !buffer_.empty();
}

bool CompressedDecoder::finish() {
    if (!valid_ || !endOfFileSeen_ || !buffer_.empty() ||
        descriptorPending_) valid_ = false;
    return valid_;
}
