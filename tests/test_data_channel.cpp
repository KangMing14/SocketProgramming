#include "../server/src/datachannel/DataChannelSession.h"

#include <cassert>
#include <iostream>

#include "helper.h"


class FakeRdtTransport : public IRdtTransport {
public:
    std::vector<std::pair<uint32_t, std::vector<char>>> sentChunks;

    bool sendChunk(uint32_t seq, const char* data, size_t len) override {
        sentChunks.emplace_back(seq, std::vector<char>(data, data + len));
        return true; // no real network, always "succeeds"
    }

    bool receiveNext(uint32_t& seq, std::vector<char>& data, bool& isFinal) override {
        static size_t i = 0;
        if (i >= sentChunks.size()) return false;
        seq = sentChunks[i].first;
        data = sentChunks[i].second;
        isFinal = (i == sentChunks.size() - 1);
        i++;
        return true;
    }
};

void test_send_then_receive_round_trip() {
    // Create a real binary test file, feed it through sendFile using the fake
    // transport, then feed the SAME fake transport's recorded chunks back
    // through receiveFile, and confirm the round trip reproduces the file exactly.
    createTestFile("dc_source.bin", 2600); // reusing the fixture helper from earlier

    FakeRdtTransport transport;
    sockaddr_in dummyAddr{};
    DataChannelSession sender(INVALID_SOCKET, dummyAddr, transport);

    assert(sender.sendFile("dc_source.bin") == true);
    assert(!transport.sentChunks.empty());

    DataChannelSession receiver(INVALID_SOCKET, dummyAddr, transport);
    assert(receiver.receiveFile("dc_result.bin") == true);

    assert(filesAreIdentical("dc_source.bin", "dc_result.bin"));
    std::cout << "[PASS] test_send_then_receive_round_trip\n";
}

int main() {
    test_send_then_receive_round_trip();

    for (const auto& name : { "dc_source.bin", "dc_result.bin" }) {
        std::error_code ec;
        fs::remove(name, ec);
    }
}