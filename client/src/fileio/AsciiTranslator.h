#pragma once
#include <vector>

namespace hybridftp::client {

class AsciiTranslator {
public:
    std::vector<char> encode(const std::vector<char>& raw);
    std::vector<char> decode(const std::vector<char>& wire);

private:
    std::vector<char> normalizeToCRLF(const std::vector<char>& in, bool& carryPrevCR);
    bool encodeCarryCR = false;
    bool decodeCarryCR = false;
};

}
