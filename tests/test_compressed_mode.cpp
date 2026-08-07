#include "CompressedMode.h"
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

void writeFile(const fs::path& path, const std::vector<char>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Could not create test file");
    if (!data.empty()) {
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    require(output.good(), "Could not write test file");
}

std::vector<char> readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not read test file");
    return {std::istreambuf_iterator<char>(input), {}};
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

std::vector<char> decodeAll(const std::vector<std::vector<char>>& payloads,
                            TransferType type) {
    CompressedDecoder decoder(type);
    std::vector<char> result;
    for (const auto& payload : payloads) {
        require(decoder.appendPayload(payload), "Compressed payload rejected");
        std::vector<char> data;
        bool eof = false;
        bool eor = false;
        bool suspect = false;
        while (decoder.nextData(data, eof, eor, suspect)) {
            result.insert(result.end(), data.begin(), data.end());
            if (eof) break;
        }
        require(decoder.isValid(), "Compressed stream became invalid");
    }
    require(decoder.finish(), "Compressed stream did not finish");
    return result;
}

void testKnownEncodings() {
    CompressedEncoder binary(TransferType::Binary);
    std::vector<char> encoded;
    require(binary.appendData(bytes("ABC"), encoded), "Literal encoding failed");
    require(encoded == std::vector<char>({3, 'A', 'B', 'C'}),
            "Literal token is incorrect");

    encoded.clear();
    require(binary.appendData(bytes("AAAA"), encoded), "Run encoding failed");
    require(encoded == std::vector<char>({static_cast<char>(0x84), 'A'}),
            "Replicated-byte token is incorrect");

    encoded.clear();
    require(binary.appendData(std::vector<char>(5, '\0'), encoded),
            "Binary filler encoding failed");
    require(encoded == std::vector<char>({static_cast<char>(0xc5)}),
            "Binary filler token is incorrect");

    CompressedEncoder ascii(TransferType::ASCII);
    encoded.clear();
    require(ascii.appendData(std::vector<char>(5, ' '), encoded),
            "ASCII filler encoding failed");
    require(encoded == std::vector<char>({static_cast<char>(0xc5)}),
            "ASCII filler token is incorrect");

    encoded.clear();
    require(binary.appendEndOfFile(encoded), "EOF encoding failed");
    require(encoded == std::vector<char>({0, static_cast<char>(0x40)}),
            "Compressed EOF token is incorrect");
}

void testCountBoundaries() {
    CompressedEncoder encoder(TransferType::Binary);
    std::vector<char> encoded;
    require(encoder.appendData(std::vector<char>(64, 'R'), encoded),
            "64-byte run encoding failed");
    require(encoded == std::vector<char>({static_cast<char>(0xbf), 'R',
                                          static_cast<char>(0x81), 'R'}),
            "64-byte run was not split 63+1");

    std::vector<char> literals(128);
    for (std::size_t index = 0; index < literals.size(); ++index) {
        literals[index] = static_cast<char>('!' + (index % 80));
    }
    encoded.clear();
    require(encoder.appendData(literals, encoded),
            "128-byte literal encoding failed");
    require(static_cast<std::uint8_t>(encoded[0]) == 127 &&
                static_cast<std::uint8_t>(encoded[128]) == 1 &&
                encoded.size() == 130,
            "128-byte literal was not split 127+1");
}

void testFragmentedRoundTripAndDescriptors() {
    const std::vector<char> original = {
        'a', 'b', 'c', 'X', 'X', 'X', '\0', '\0', 'z'
    };
    CompressedEncoder encoder(TransferType::Binary);
    std::vector<char> wire;
    require(encoder.appendData(original, wire) &&
                encoder.appendEndOfFile(wire),
            "Mixed stream encoding failed");

    CompressedDecoder decoder(TransferType::Binary);
    std::vector<char> decoded;
    for (char value : wire) {
        require(decoder.appendPayload({value}),
                "Byte-fragmented payload was rejected");
        std::vector<char> data;
        bool eof = false;
        bool eor = false;
        bool suspect = false;
        while (decoder.nextData(data, eof, eor, suspect)) {
            decoded.insert(decoded.end(), data.begin(), data.end());
        }
        require(decoder.isValid(), "Byte-fragmented stream became invalid");
    }
    require(decoder.finish() && decoded == original,
            "Byte-fragmented compressed round trip failed");

    CompressedDecoder descriptors(TransferType::Binary);
    require(descriptors.appendPayload(
                {0, static_cast<char>(CompressedEncoder::EndOfRecord |
                                      CompressedEncoder::SuspectedError),
                 1, 'x'}),
            "Descriptor payload rejected");
    std::vector<char> data;
    bool eof = false;
    bool eor = false;
    bool suspect = false;
    require(descriptors.nextData(data, eof, eor, suspect) &&
                data == std::vector<char>({'x'}) && !eof && eor && suspect,
            "EOR/suspected-error descriptor was not applied to its data");
}

void testFillerSelection() {
    const std::vector<std::vector<char>> binaryWire = {
        {static_cast<char>(0xc3)},
        {0, static_cast<char>(CompressedEncoder::EndOfFile)}
    };
    require(decodeAll(binaryWire, TransferType::Binary) ==
                std::vector<char>(3, '\0'),
            "Binary filler decoded incorrectly");
    require(decodeAll(binaryWire, TransferType::ASCII) ==
                std::vector<char>(3, ' '),
            "ASCII filler decoded incorrectly");
}

void requireMalformed(const std::vector<char>& wire,
                      const std::string& message) {
    CompressedDecoder decoder(TransferType::Binary);
    require(decoder.appendPayload(wire), "Could not append malformed fixture");
    std::vector<char> data;
    bool eof = false;
    bool eor = false;
    bool suspect = false;
    while (decoder.nextData(data, eof, eor, suspect)) {}
    require(!decoder.finish(), message);
}

void testMalformedStreams() {
    requireMalformed({static_cast<char>(0x80)},
                     "Zero-count replicated token was accepted");
    requireMalformed({static_cast<char>(0xc0)},
                     "Zero-count filler token was accepted");
    requireMalformed({3, 'a'}, "Truncated literal token was accepted");
    requireMalformed({static_cast<char>(0x82)},
                     "Truncated replicated token was accepted");
    requireMalformed({0}, "Truncated control token was accepted");
    requireMalformed({0, 1}, "Reserved descriptor bits were accepted");
    requireMalformed({0, 0x10}, "Restart marker was accepted");
    requireMalformed({1, 'x'}, "Stream without EOF was accepted");

    CompressedDecoder trailing(TransferType::Binary);
    require(trailing.appendPayload(
                {0, static_cast<char>(CompressedEncoder::EndOfFile), 1, 'x'}),
            "Could not append trailing-data fixture");
    std::vector<char> data;
    bool eof = false;
    bool eor = false;
    bool suspect = false;
    require(!trailing.nextData(data, eof, eor, suspect) &&
                !trailing.isValid(),
            "Data after EOF was accepted");

    CompressedDecoder duplicate(TransferType::Binary);
    require(duplicate.appendPayload(
                {0, static_cast<char>(CompressedEncoder::EndOfFile),
                 0, static_cast<char>(CompressedEncoder::EndOfFile)}),
            "Could not append duplicate-EOF fixture");
    require(!duplicate.nextData(data, eof, eor, suspect) &&
                !duplicate.isValid(),
            "Duplicate EOF was accepted");
}

void testDataChannelRoundTrip(const fs::path& directory) {
    std::vector<char> fixture;
    fixture.insert(fixture.end(), 1500, 'A');
    fixture.insert(fixture.end(), 700, '\0');
    for (std::size_t index = 0; index < MAX_PAYLOAD * 2 + 31; ++index) {
        fixture.push_back(static_cast<char>((index * 73U + 19U) & 0xffU));
    }
    const fs::path source = directory / "compressed-source.bin";
    const fs::path destination = directory / "compressed-result.bin";
    writeFile(source, fixture);

    MemoryTransport transport;
    DataChannelSession sender(transport);
    const SendResult result = sender.sendFile(
        source, TransferType::Binary, TransferMode::Compressed);
    require(result.success, "Compressed data-channel sender failed");
    require(result.sha256 == Sha256Hasher::hashFile(source),
            "Compression changed the logical SHA-256 hash");
    require(!transport.payloads.empty() && transport.finalFlags.back(),
            "Compressed stream did not terminate with RDT FIN");
    require(std::count(transport.finalFlags.begin(), transport.finalFlags.end(),
                       true) == 1,
            "Compressed stream emitted multiple RDT FIN flags");
    for (const auto& payload : transport.payloads) {
        require(payload.size() <= MAX_PAYLOAD,
                "Compressed output exceeded the RDT payload limit");
    }

    DataChannelSession receiver(transport);
    require(receiver.receiveFile(destination, TransferType::Binary,
                                 TransferMode::Compressed),
            "Compressed data-channel receiver failed");
    require(readFile(destination) == fixture,
            "Compressed data-channel round trip changed contents");
}

void testAsciiOrderingAndEmptyFile(const fs::path& directory) {
    const fs::path source = directory / "compressed-source.txt";
    writeFile(source, bytes("one\ntwo\r\n   "));
    MemoryTransport asciiTransport;
    DataChannelSession asciiSender(asciiTransport);
    require(asciiSender.sendFile(source, TransferType::ASCII,
                                 TransferMode::Compressed),
            "ASCII compressed sender failed");
    require(asString(decodeAll(asciiTransport.payloads, TransferType::ASCII)) ==
                "one\r\ntwo\r\n   ",
            "TYPE A conversion did not occur before compression");

    const fs::path empty = directory / "empty.bin";
    const fs::path emptyResult = directory / "empty-result.bin";
    writeFile(empty, {});
    MemoryTransport emptyTransport;
    DataChannelSession emptySender(emptyTransport);
    require(emptySender.sendFile(empty, TransferType::Binary,
                                 TransferMode::Compressed),
            "Empty compressed sender failed");
    DataChannelSession emptyReceiver(emptyTransport);
    require(emptyReceiver.receiveFile(emptyResult, TransferType::Binary,
                                      TransferMode::Compressed) &&
                readFile(emptyResult).empty(),
            "Empty compressed round trip failed");
}

void testFailureAndAbortPreserveDestination(const fs::path& directory) {
    const fs::path destination = directory / "preserved.bin";
    writeFile(destination, bytes("original"));

    MemoryTransport malformed;
    malformed.payloads.push_back({3, 'x'});
    malformed.finalFlags.push_back(true);
    DataChannelSession malformedReceiver(malformed);
    require(!malformedReceiver.receiveFile(
                destination, TransferType::Binary, TransferMode::Compressed),
            "Malformed compressed transfer unexpectedly succeeded");
    require(asString(readFile(destination)) == "original",
            "Malformed compressed transfer replaced the destination");

    CompressedEncoder encoder(TransferType::Binary);
    std::vector<char> data;
    encoder.appendData(bytes("replacement"), data);
    std::vector<char> eof;
    encoder.appendEndOfFile(eof);
    MemoryTransport cancelled;
    cancelled.payloads = {data, eof};
    cancelled.finalFlags = {false, true};
    bool abort = false;
    cancelled.abortFlag = &abort;
    cancelled.abortAfterReceive = true;
    DataChannelSession cancelledReceiver(cancelled, [&] { return abort; });
    require(!cancelledReceiver.receiveFile(
                destination, TransferType::Binary, TransferMode::Compressed),
            "Cancelled compressed transfer unexpectedly succeeded");
    require(asString(readFile(destination)) == "original",
            "Cancelled compressed transfer replaced the destination");
}
}

int main() {
    fs::path directory;
    try {
        directory = fs::temp_directory_path() /
            ("hybridftp_compressed_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory);

        testKnownEncodings();
        testCountBoundaries();
        testFragmentedRoundTripAndDescriptors();
        testFillerSelection();
        testMalformedStreams();
        testDataChannelRoundTrip(directory);
        testAsciiOrderingAndEmptyFile(directory);
        testFailureAndAbortPreserveDestination(directory);

        fs::remove_all(directory);
        std::cout << "[PASS] FTP MODE C codec and data-channel tests\n";
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
