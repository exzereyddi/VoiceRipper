#pragma once
#include <cstdint>
#include <string>

namespace vr {

// Bit-level reader matching GoldSrc engine's MSG_ReadBits/MSG_ReadOneBit
// semantics (see rehlds/engine/common.cpp): bits are packed LSB-first
// within each byte, bytes consumed in stream order, and up to 32 bits can
// be read at once forming a little-endian-within-bits value.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t sizeBytes)
        : m_data(data), m_sizeBits(sizeBytes * 8) {}

    bool Overflowed() const { return m_overflowed; }
    size_t BitPos() const { return m_bitPos; }
    size_t SizeBits() const { return m_sizeBits; }
    void SeekBits(size_t pos) { m_bitPos = pos; m_overflowed = (pos > m_sizeBits); }

    int ReadOneBit() {
        // Returning 0 (not 1) once the stream is exhausted is deliberate:
        // several callers use `while (r.ReadOneBit())` as a "more items
        // follow" loop condition (svc_clientdata's weapon list,
        // svc_pings, svc_resourcelist's consistency list). If this
        // returned a fixed 1 on overflow, every such loop would spin
        // forever once the input ran out -- genuinely hanging the whole
        // program at 100% CPU on any demo containing garbage/desynced
        // bits feeding one of those loops (this was a real bug: see
        // NetMsgParser.cpp's svc_clientdata weapon loop). Returning 0
        // instead makes every "while (ReadOneBit())" loop terminate
        // immediately and safely once Overflowed() is set, same as every
        // other read primitive here degrading to a safe default past
        // end-of-buffer.
        if (m_bitPos >= m_sizeBits) { m_overflowed = true; return 0; }
        size_t bytePos = m_bitPos >> 3;
        int bitIdx = static_cast<int>(m_bitPos & 7);
        int bit = (m_data[bytePos] >> bitIdx) & 1;
        m_bitPos++;
        return bit;
    }

    // Reads up to 32 bits, LSB of the result corresponds to the first bit
    // read (matches MSG_ReadBits's bit ordering).
    uint32_t ReadBits(int n) {
        if (n <= 0) return 0;
        if (n > 32) n = 32;
        uint32_t result = 0;
        for (int i = 0; i < n; i++) {
            result |= static_cast<uint32_t>(ReadOneBit()) << i;
        }
        return result;
    }

    // Matches MSG_ReadSBits: sign bit read FIRST, then (n-1) magnitude bits.
    int32_t ReadSBits(int n) {
        if (n <= 0) return 0;
        int sign = ReadOneBit();
        uint32_t mag = ReadBits(n - 1);
        return sign ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
    }

    void SkipBits(size_t n) { m_bitPos += n; if (m_bitPos > m_sizeBits) m_overflowed = true; }
    void AlignToByte() { m_bitPos = (m_bitPos + 7) & ~static_cast<size_t>(7); }
    bool IsByteAligned() const { return (m_bitPos & 7) == 0; }

    // Byte-aligned raw byte read (used for the top-level message ID byte
    // and other definitely-byte-aligned spots like svc_updateuserinfo's
    // leading fields, which GoldSrc always sends outside the bit-packed
    // sections).
    // Reads a value as N 8-bit groups via ReadBits(8) each — works
    // correctly regardless of current bit alignment (NOT a byte-boundary
    // seek; "Aligned" here just means "whole bytes", matching engine
    // naming for these call sites which read plain bytes/shorts embedded
    // in an otherwise bit-packed stream without ever byte-aligning first).
    uint8_t ReadByteAligned() {
        return static_cast<uint8_t>(ReadBits(8));
    }

    uint16_t ReadU16Aligned() {
        uint16_t lo = ReadByteAligned();
        uint16_t hi = ReadByteAligned();
        return static_cast<uint16_t>(lo | (hi << 8));
    }

    int32_t ReadI32Aligned() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ReadByteAligned()) << (i * 8);
        return static_cast<int32_t>(v);
    }

    // Null-terminated C-string, read byte-by-byte (works whether or not
    // the reader happens to be at a byte boundary, same as the engine's
    // bit-packed MSG_ReadBitString loop of ReadBits(8)).
    std::string ReadCString(size_t maxLen = 8192) {
        std::string s;
        while (true) {
            if (m_bitPos + 8 > m_sizeBits) { m_overflowed = true; break; }
            uint8_t c = static_cast<uint8_t>(ReadBits(8));
            if (c == 0) break;
            s += static_cast<char>(c);
            if (s.size() >= maxLen) { m_overflowed = true; break; }
        }
        return s;
    }

private:
    const uint8_t* m_data;
    size_t m_sizeBits;
    size_t m_bitPos = 0;
    bool m_overflowed = false;
};

} // namespace vr
