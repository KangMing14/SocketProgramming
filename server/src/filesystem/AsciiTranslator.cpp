#include "AsciiTranslator.h"

std::vector<char> AsciiTranslator::normalizeToCRLF(const std::vector<char>& in, bool& carryPrevCR) {
    std::vector<char> out;
    out.reserve(in.size() + in.size() / 8);   // headroom guess; grows automatically if needed

    bool prevWasCR = carryPrevCR;
    for (char b : in) {
        if (b == '\n') {
            if (!prevWasCR) out.push_back('\r');  // bare LF -> promote to CRLF
            out.push_back('\n');
            prevWasCR = false;
        }
        else {
            out.push_back(b);
            prevWasCR = (b == '\r');
        }
    }
    carryPrevCR = prevWasCR;   // remember for the NEXT call in this same stream
    return out;
}

std::vector<char> AsciiTranslator::encode(const std::vector<char>& raw) {
    return normalizeToCRLF(raw, encodeCarryCR);
}

std::vector<char> AsciiTranslator::decode(const std::vector<char>& wire) {
    // Windows only, need modify for other OS
    return normalizeToCRLF(wire, decodeCarryCR);
}