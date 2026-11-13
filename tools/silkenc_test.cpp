// Codec probe #2: SKP_Silk encode-reference test. Encodes synthetic
// speech-like PCM with the vendored SKP_Silk SDK at various CBR/bitrate
// settings and dumps the frame bytes + their first-byte profile, so we
// can compare against what a demo's voice chunks actually look like.
//
// Input: same bin format as codecprobe ([u8 quality][u32 n][u32 len][bytes])
// Output: prints encoding stats + hex of the first produced frames.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>

extern "C" {
#include "SKP_Silk_SDK_API.h"
#include "SKP_Silk_control.h"
#include "SKP_Silk_typedef.h"
}

static uint32_t ReadU32(const std::vector<uint8_t>& b, size_t& off) {
    uint32_t v; memcpy(&v, &b[off], 4); off += 4; return v;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: silkenc_test <input.bin>\n");
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t off = 0;
    int quality = buf[off++];
    uint32_t count = ReadU32(buf, off);
    std::vector<std::vector<uint8_t>> chunks;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = ReadU32(buf, off);
        if (off + len > buf.size()) break;
        chunks.emplace_back(buf.begin() + off, buf.begin() + off + len);
        off += len;
    }
    printf("loaded %zu chunks (quality=%d)\n", chunks.size(), quality);

    // ---- 1) try DECODE per-38-byte blocks with fs forced through the
    // control struct, printing per-block result codes so we can see the
    // success pattern INSIDE chunks (not just the first block).
    {
        SKP_int32 decSize = 0;
        SKP_Silk_SDK_Get_Decoder_Size(&decSize);
        std::vector<uint8_t> decMem((size_t)decSize);
        SKP_Silk_SDK_InitDecoder(decMem.data());
        int blockIdx = 0;
        for (size_t ci = 0; ci < chunks.size() && ci < 6; ci++) {
            auto& c = chunks[ci];
            printf("chunk %zu (%zu bytes): blocks=", ci, c.size());
            for (size_t o = 0; o + 38 <= c.size(); o += 38, blockIdx++) {
                SKP_SILK_SDK_DecControlStruct ctrl = {};
                ctrl.API_sampleRate = 24000;
                SKP_int16 pcm[480];
                SKP_int16 n = 0;
                SKP_int ret = SKP_Silk_SDK_Decode(decMem.data(), &ctrl, 0,
                                                  c.data() + o, 38, pcm, &n);
                printf("%d ", ret);
            }
            printf("\n");
        }
    }

    // ---- 2) ENCODE synthetic speech at 16 kHz / 20 ms, CBR sweep over the
    // standard SILK bitrates, print produced frame sizes and the first
    // byte of each produced frame ("profile"), so we can compare with the
    // demo chunks' first-byte statistics (0x28..0x2F cluster, 0x2B mode).
    {
        static const int kBitrates[] = { 12000, 13300, 15200, 16000, 18000, 20000, 24000 };
        for (size_t bi = 0; bi < sizeof(kBitrates) / sizeof(kBitrates[0]); bi++) {
            int rate = kBitrates[bi];
            SKP_int32 encSize = 0;
            if (SKP_Silk_SDK_Get_Encoder_Size(&encSize) != 0) continue;
            std::vector<uint8_t> encMem((size_t)encSize);
            SKP_SILK_SDK_EncControlStruct initStatus = {};
            if (SKP_Silk_SDK_InitEncoder(encMem.data(), &initStatus) != 0) continue;

            SKP_SILK_SDK_EncControlStruct ctrl = {};
            ctrl.API_sampleRate = 16000;
            ctrl.maxInternalSampleRate = 16000;
            ctrl.packetSize = 320;          // 20 ms @ 16 kHz
            ctrl.bitRate = rate;
            ctrl.packetLossPercentage = 0;
            ctrl.complexity = 2;
            ctrl.useInBandFEC = 0;
            ctrl.useDTX = 0;

            // speech-ish: 220 Hz + 880 Hz + noise, 1 sec
            static SKP_int16 pcm[320 * 50];
            for (int i = 0; i < 320 * 50; i++) {
                double t = (double)i / 16000.0;
                double v = 600.0 * sin(2 * 3.14159265 * 220.0 * t)
                         + 300.0 * sin(2 * 3.14159265 * 880.0 * t)
                         + 150.0 * sin(2 * 3.14159265 * 3300.0 * t);
                pcm[i] = (SKP_int16)(v * (0.6 + 0.4 * sin(2 * 3.14159265 * 3.0 * t)));
            }

            const SKP_int16* src = pcm;
            int framesLeft = 50;
            printf("encode @%d bps: sizes=", rate);
            int printed = 0;
            uint8_t payload[1280];
            while (framesLeft-- > 0) {
                SKP_int16 nBytes = sizeof(payload);
                SKP_int ret = SKP_Silk_SDK_Encode(encMem.data(), &ctrl,
                                                   src, 320,
                                                   payload, &nBytes);
                if (ret != 0) { printf("E(ret=%d) ", ret); break; }
                src += 320;
                printf("%d ", nBytes);
                if (printed < 3 && nBytes > 0) {
                    printf("[");
                    for (int b = 0; b < nBytes && b < 8; b++) printf("%02X ", payload[b]);
                    printf("%s] ", nBytes > 8 ? "..." : "");
                    printed++;
                }
            }
            printf("\n");
        }
    }

    return 0;
}
