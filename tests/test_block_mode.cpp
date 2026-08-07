#include "BlockMode.h"
#include "DataChannelSession.h"
#include "ProtocolConstants.h"
#include "Sha256Hasher.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<char> bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

std::string asString(const std::vector<char>& value) {
    return {value.begin(), value.end()};
}

std::size_t recordLength(const std::vector<char>& record) {
    require(record.size() >= BlockEncoder::HeaderSize, "Record header missing");
    return (static_cast<std::size_t>(static_cast<std::uint8_t>(record[1])) << 8) |
           static_cast<std::uint8_t>(record[2]);
}

class MemoryTransport : public IRdtTransport {
public:
    std::vector<std::vector<char>> payloads;
    std::vector<bool> finalFlags;
    std::size_t receiveIndex = 0;
    bool* abortFlag = nullptr;
    bool abortAfterReceive = false;

    bool sendChunk(std::uint32_t, const char* data, std::size_t size,
                   bool isFinal = false) override {
        payloads.emplace_back(size == 0 ? std::vector<char>{}
                                        : std::vector<char>(data, data + size));
        finalFlags.push_back(isFinal);
        return true;
    }

    bool receiveNext(std::uint32_t& sequence, std::vector<char>& data,
                     bool& isFinal) override {
        if (receiveIndex >= payloads.size()) return false;
        sequence = static_cast<std::uint32_t>(receiveIndex);
        data = payloads[receiveIndex];
        isFinal = finalFlags[receiveIndex];
        ++receiveIndex;
        if (abortAfterReceive && abortFlag) *abortFlag = true;
        return true;
    }
};

void writeFile(const fs::path& path, const std::vector<char>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Could not create test file");
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    require(output.good(), "Could not write test file");
}

std::vector<char> readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not read test file");
    return {std::istreambuf_iterator<char>(input), {}};
}

void testEncoderAndDecoder() {
    BlockEncoder encoder;
    std::vector<std::vector<char>> records;
    require(encoder.appendData(bytes("abc"), records), "Encoding failed");
    require(records.size() == 1 && records[0][0] == 0,
            "Normal block descriptor is incorrect");
    require(recordLength(records[0]) == 3 && records[0].size() == 6,
            "Normal block length is incorrect");
    require(static_cast<std::uint8_t>(records[0][1]) == 0 &&
                static_cast<std::uint8_t>(records[0][2]) == 3,
            "Block length is not big-endian");

    require(encoder.appendEndOfFile(records), "EOF encoding failed");
    require(records.size() == 2 &&
                static_cast<std::uint8_t>(records[1][0]) ==
                    BlockEncoder::EndOfFile &&
                recordLength(records[1]) == 0,
            "EOF record is incorrect");

    std::vector<char> wire = records[0];
    wire.insert(wire.end(), records[1].begin(), records[1].end());
    BlockDecoder decoder;
    require(decoder.appendPayload(
                std::vector<char>(wire.begin(), wire.begin() + 1)),
            "Partial header was rejected");
    std::vector<char> decoded;
    bool eof = false;
    bool eor = false;
    require(!decoder.nextRecord(decoded, eof, eor),
            "Partial header produced a record");
    require(decoder.appendPayload(
                std::vector<char>(wire.begin() + 1, wire.end())),
            "Remaining records were rejected");
    require(decoder.nextRecord(decoded, eof, eor) &&
                asString(decoded) == "abc" && !eof,
            "Data record did not decode");
    require(decoder.nextRecord(decoded, eof, eor) && decoded.empty() && eof,
            "EOF record did not decode");
    require(decoder.finish(), "Complete block stream was rejected");
}

void testMaximumLengthSplitting() {
    BlockEncoder encoder;
    std::vector<char> large(BlockEncoder::MaximumDataSize + 4465, 'x');
    std::vector<std::vector<char>> records;
    require(encoder.appendData(large, records), "Large input encoding failed");
    require(records.size() == 2,
            "Large input was not split into two block records");
    require(recordLength(records[0]) == BlockEncoder::MaximumDataSize &&
                recordLength(records[1]) == 4465,
            "Large block split lengths are incorrect");
}

void testMalformedStreams() {
    std::vector<char> data;
    bool eof = false;
    bool eor = false;

    BlockDecoder reservedBits;
    require(reservedBits.appendPayload(
                {static_cast<char>(0x01), 0, 0}),
            "Appending reserved-bit fixture failed");
    require(!reservedBits.nextRecord(data, eof, eor) &&
                !reservedBits.isValid(),
            "Reserved descriptor bits were accepted");

    BlockDecoder suspectedError;
    require(suspectedError.appendPayload(
                {static_cast<char>(BlockEncoder::SuspectedError), 0, 3,
                 'a', 'b', 'c'}),
            "Appending suspected-error fixture failed");
    require(suspectedError.nextRecord(data, eof, eor) &&
                asString(data) == "abc" && suspectedError.isValid(),
            "Suspected-error data block was rejected");

    BlockDecoder truncatedHeader;
    require(truncatedHeader.appendPayload({0, 0}),
            "Appending truncated header failed");
    require(!truncatedHeader.finish(), "Truncated header was accepted");

    BlockDecoder truncatedPayload;
    require(truncatedPayload.appendPayload({0, 0, 5, 'x'}),
            "Appending truncated payload failed");
    require(!truncatedPayload.finish(), "Truncated payload was accepted");
}

void testDataChannelRoundTrip(const fs::path& directory) {
    std::vector<char> fixture(MAX_PAYLOAD * 3 + 19);
    for (std::size_t index = 0; index < fixture.size(); ++index) {
        fixture[index] = static_cast<char>((index * 29U + 7U) & 0xffU);
    }
    const fs::path source = directory / "source.bin";
    const fs::path destination = directory / "destination.bin";
    writeFile(source, fixture);

    MemoryTransport transport;
    DataChannelSession sender(transport);
    const SendResult result = sender.sendFile(
        source, TransferType::Binary, TransferMode::Block);
    require(result.success, "Block-mode sender failed");
    require(result.sha256 == Sha256Hasher::hashFile(source),
            "Block headers changed the logical SHA-256 hash");
    require(!transport.payloads.empty() && transport.finalFlags.back(),
            "Block stream did not terminate with RDT FIN");
    require(std::count(transport.finalFlags.begin(), transport.finalFlags.end(),
                       true) == 1,
            "Block stream emitted multiple RDT FIN flags");
    for (const auto& payload : transport.payloads) {
        require(payload.size() <= MAX_PAYLOAD,
                "FTP block framing exceeded the RDT payload limit");
    }

    DataChannelSession receiver(transport);
    require(receiver.receiveFile(destination, TransferType::Binary,
                                 TransferMode::Block),
            "Block-mode receiver failed");
    require(readFile(destination) == fixture,
            "Block-mode round trip changed binary contents");
}

void testAsciiBeforeBlockEncoding(const fs::path& directory) {
    const fs::path source = directory / "source.txt";
    writeFile(source, bytes("one\ntwo\r\n"));

    MemoryTransport transport;
    DataChannelSession sender(transport);
    require(sender.sendFile(source, TransferType::ASCII,
                            TransferMode::Block),
            "ASCII block-mode sender failed");

    BlockDecoder decoder;
    std::vector<char> logicalWireData;
    for (const auto& payload : transport.payloads) {
        require(decoder.appendPayload(payload), "Encoded ASCII block failed");
        std::vector<char> record;
        bool eof = false;
        bool eor = false;
        while (decoder.nextRecord(record, eof, eor)) {
            logicalWireData.insert(logicalWireData.end(),
                                   record.begin(), record.end());
            if (eof) break;
        }
    }
    require(decoder.finish(), "ASCII block stream was incomplete");
    require(asString(logicalWireData) == "one\r\ntwo\r\n",
            "TYPE A conversion did not occur before block encoding");
}

void testFailureAndAbortPreserveDestination(const fs::path& directory) {
    const fs::path destination = directory / "preserved.bin";
    writeFile(destination, bytes("original"));

    MemoryTransport malformed;
    malformed.payloads.push_back({0, 0, 5, 'x'});
    malformed.finalFlags.push_back(true);
    DataChannelSession malformedReceiver(malformed);
    require(!malformedReceiver.receiveFile(
                destination, TransferType::Binary, TransferMode::Block),
            "Malformed block transfer unexpectedly succeeded");
    require(asString(readFile(destination)) == "original",
            "Malformed block transfer replaced the destination");

    BlockEncoder encoder;
    std::vector<std::vector<char>> records;
    encoder.appendData(bytes("replacement"), records);
    encoder.appendEndOfFile(records);
    MemoryTransport cancelled;
    for (std::size_t index = 0; index < records.size(); ++index) {
        cancelled.payloads.push_back(records[index]);
        cancelled.finalFlags.push_back(index + 1 == records.size());
    }
    bool abort = false;
    cancelled.abortFlag = &abort;
    cancelled.abortAfterReceive = true;
    DataChannelSession cancelledReceiver(cancelled, [&] { return abort; });
    require(!cancelledReceiver.receiveFile(
                destination, TransferType::Binary, TransferMode::Block),
            "Cancelled block transfer unexpectedly succeeded");
    require(asString(readFile(destination)) == "original",
            "Cancelled block transfer replaced the destination");
}

}

int main() {
    fs::path directory;
    try {
        directory = fs::temp_directory_path() /
            ("hybridftp_block_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);

        testEncoderAndDecoder();
        testMaximumLengthSplitting();
        testMalformedStreams();
        testDataChannelRoundTrip(directory);
        testAsciiBeforeBlockEncoding(directory);
        testFailureAndAbortPreserveDestination(directory);

        fs::remove_all(directory);
        std::cout << "[PASS] FTP MODE B framing and data-channel tests\n";
        return 0;
    } catch (const std::exception& error) {
        if (!directory.empty()) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
