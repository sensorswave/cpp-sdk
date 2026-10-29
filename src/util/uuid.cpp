#include "util/uuid.h"

#include <cstring>

namespace sensorswave {
namespace {

const char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// 16 bytes → 8-4-4-4-12 lowercase hexadecimal
std::string format(const unsigned char bytes[16]) {
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += kHexDigits[static_cast<unsigned>(bytes[i]) >> 4U];
        out += kHexDigits[static_cast<unsigned>(bytes[i]) & 0x0FU];
    }
    return out;
}

}  // namespace

Uuid::Uuid(IRandom* random) : random_(random) {}

void Uuid::fillRandom(unsigned char* buffer, std::size_t size) {
    if (random_ == nullptr) {
        // Defensive: the caller guarantees non-null; zeroing keeps
        // uninitialized memory out of the output
        std::memset(buffer, 0, size);
        return;
    }
    random_->fill(buffer, size);
}

std::string Uuid::v4() {
    unsigned char bytes[16];
    fillRandom(bytes, sizeof(bytes));
    bytes[6] = static_cast<unsigned char>((static_cast<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((static_cast<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return format(bytes);
}

std::string Uuid::v7(int64_t unixMs) {
    unsigned char bytes[16];
    fillRandom(bytes, sizeof(bytes));
    // Top 48 bits = big-endian unixMs (RFC 9562: milliseconds beyond 48 bits
    // truncate the high ones). Converting to uint64 before shifting keeps the
    // truncation well defined for negative milliseconds (pathological inputs
    // such as clock rollback) and avoids signed left-shift UB
    const uint64_t unixMsU = static_cast<uint64_t>(unixMs);
    for (int i = 0; i < 6; ++i) {
        bytes[i] =
            static_cast<unsigned char>((unixMsU >> (8U * static_cast<unsigned>(5 - i))) & 0xFFU);
    }
    bytes[6] = static_cast<unsigned char>((static_cast<unsigned>(bytes[6]) & 0x0FU) | 0x70U);
    bytes[8] = static_cast<unsigned char>((static_cast<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return format(bytes);
}

int64_t Uuid::v7TimestampMs(const std::string& uuid) {
    // The 48-bit millisecond timestamp = the first 12 hex digits (the formatted
    // string has a hyphen at position 8, which must be skipped)
    int64_t ms = 0;
    int collected = 0;
    for (std::size_t i = 0; i < uuid.size() && collected < 12; ++i) {
        if (uuid[i] == '-') {
            continue;
        }
        const int value = hexValue(uuid[i]);
        if (value < 0) {
            return 0;
        }
        ms = static_cast<int64_t>((static_cast<uint64_t>(ms) << 4U) | static_cast<uint64_t>(value));
        ++collected;
    }
    return collected == 12 ? ms : 0;
}

}  // namespace sensorswave
