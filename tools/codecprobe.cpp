// Codec probe: feeds captured svc_voicedata payloads (from
// voiceripper_chunkprog.log dumps) to every codec hypothesis we can
// think of, to identify what format a given demo's voice actually is.
//
// Input file format: [u8 quality][u32 chunkCount] then per chunk
// [u32 len][len bytes].
//
// Build: linked against the same vendored codec libs as voiceripper.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "header.h" // VoiceEncoder_*, SteamP2PCodec

extern "C" {
// vendored standalone SKP_Silk (old engine-era reference codec)
#include "SKP_Silk_SDK_API.h"
// vendored opus
#include "opus.h"
// vendored speex
#include "speex.h"
}

static int ReadU8(const std::vector<uint8_t>& b, size_t& off) {
    int v = b[off]; off++; return v;
}
static uint32_t ReadU32(const std::vector<uint8_t>& b, size_t& off) {
    uint32_t v; memcpy(&v, &b[off], 4); off += 4; return v;
}

static void DumpPcm16(const char* path, const std::vector<int16_t>& pcm, int rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    uint8_t hdr[44] = {};
    memcpy(hdr, "RIFF", 4);
    uint32_t sz = 36 + pcm.size() * 2;
    memcpy(hdr + 4, &sz, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt16 = 16; memcpy(hdr + 16, &fmt16, 4);
    uint16_t pcm1 = 1; memcpy(hdr + 20, &pcm1, 2);
    uint16_t ch = 1; memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    uint32_t byterate = rate * 2; memcpy(hdr + 28, &byterate, 4);
    uint16_t align = 2; memcpy(hdr + 32, &align, 2);
    uint16_t bits = 16; memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    uint32_t dsz = (uint32_t)(pcm.size() * 2); memcpy(hdr + 40, &dsz, 4);
    f.write((char*)hdr, 44);
    f.write((char*)pcm.data(), pcm.size() * 2);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: codecprobe <input.bin> <outprefix>\n");
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t off = 0;
    int quality = ReadU8(buf, off);
    uint32_t count = ReadU32(buf, off);

    std::vector<std::vector<uint8_t>> chunks;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = ReadU32(buf, off);
        if (off + len > buf.size()) break;
        chunks.emplace_back(buf.begin() + off, buf.begin() + off + len);
        off += len;
    }
    printf("quality=%d chunks=%zu\n", quality, chunks.size());

    std::vector<int16_t> pcmOpus24k, pcmOpus48k, pcmOpus16k, pcmP2pOpus, pcmP2pSilk, pcmSpeex;
    int opus24Ok = 0, opus48Ok = 0, opus16Ok = 0, p2pOpusOk = 0, p2pSilkOk = 0, speexOk = 0;

    { // raw opus decoders, various rates
        int err = 0;
        OpusDecoder* d24 = opus_decoder_create(24000, 1, &err);
        OpusDecoder* d48 = opus_decoder_create(48000, 1, &err);
        OpusDecoder* d16 = opus_decoder_create(16000, 1, &err);
        static int16_t out[5760 * 8];
        for (auto& c : chunks) {
            int n = opus_decode(d24, c.data(), (int)c.size(), out, 5760 * 8, 0);
            if (n > 0) { opus24Ok++; pcmOpus24k.insert(pcmOpus24k.end(), out, out + n); }
            n = opus_decode(d48, c.data(), (int)c.size(), out, 5760 * 8, 0);
            if (n > 0) { opus48Ok++; pcmOpus48k.insert(pcmOpus48k.end(), out, out + n); }
            n = opus_decode(d16, c.data(), (int)c.size(), out, 5760 * 8, 0);
            if (n > 0) { opus16Ok++; pcmOpus16k.insert(pcmOpus16k.end(), out, out + n); }
        }
        opus_decoder_destroy(d24);
        opus_decoder_destroy(d48);
        opus_decoder_destroy(d16);
    }

    { // standalone SKP_Silk raw at each API rate
        static const int kRates[4] = { 8000, 12000, 16000, 24000 };
        for (int r = 0; r < 4; r++) {
            std::vector<int16_t> pcm;
            int ok = 0;
            SKP_int32 decSize = 0;
            if (SKP_Silk_SDK_Get_Decoder_Size(&decSize) != 0 || decSize <= 0) {
                printf("SKP_Silk Get_Decoder_Size failed\n");
                break;
            }
            std::vector<uint8_t> decMem((size_t)decSize);
            if (SKP_Silk_SDK_InitDecoder(decMem.data()) != 0) {
                printf("SKP_Silk InitDecoder failed\n");
                continue;
            }
            static int16_t out[5760];
            int shown = 0;
            for (auto& c : chunks) {
                SKP_SILK_SDK_DecControlStruct ctrl = {};
                ctrl.API_sampleRate = kRates[r];
                SKP_int16 outLen = 0;
                SKP_int ret = SKP_Silk_SDK_Decode(decMem.data(), &ctrl, 0,
                                                  c.data(), (SKP_int)c.size(),
                                                  out, &outLen);
                // For the first few chunks dump what the SDK itself
                // thinks this packet is: frames per packet + internal
                // continuation flag.
                if (r == 3 && shown < 8) {
                    printf("  inspect: chunkSize=%d ret=%d outLen=%d framesPerPacket=%d moreInternal=%d\n",
                           (int)c.size(), ret, outLen, ctrl.framesPerPacket, ctrl.moreInternalDecoderFrames);
                    shown++;
                }
                if (ret == 0 && outLen > 0) { ok++; pcm.insert(pcm.end(), out, out + outLen); }
            }
            printf("SKP_Silk raw @%dHz: ok=%d/%zu samples=%zu\n", kRates[r], ok, chunks.size(), pcm.size());
            if (ok > 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s_silk_%d.wav", argv[2], kRates[r]);
                DumpPcm16(path, pcm, kRates[r]);
            }
        }
    }
    printf("opus@24k: ok=%d/%zu samples=%zu\n", opus24Ok, chunks.size(), pcmOpus24k.size());
    printf("opus@48k: ok=%d/%zu samples=%zu\n", opus48Ok, chunks.size(), pcmOpus48k.size());
    printf("opus@16k: ok=%d/%zu samples=%zu\n", opus16Ok, chunks.size(), pcmOpus16k.size());
    if (opus24Ok > 0) { char p[512]; snprintf(p, sizeof(p), "%s_opus24k.wav", argv[2]); DumpPcm16(p, pcmOpus24k, 24000); }
    if (opus48Ok > 0) { char p[512]; snprintf(p, sizeof(p), "%s_opus48k.wav", argv[2]); DumpPcm16(p, pcmOpus48k, 48000); }
    if (opus16Ok > 0) { char p[512]; snprintf(p, sizeof(p), "%s_opus16k.wav", argv[2]); DumpPcm16(p, pcmOpus16k, 16000); }

    // P2P-wrapped ladder (same as the app) + speex, for comparison
    {
        IVoiceCodec* p2pOpus = new CSteamP2PCodec(new VoiceEncoder_Opus());
        IVoiceCodec* p2pSilk = new CSteamP2PCodec(new VoiceEncoder_Silk());
        IVoiceCodec* speex = new VoiceCodec_Frame(new VoiceEncoder_Speex());
        p2pOpus->Init(quality);
        p2pSilk->Init(quality);
        speex->Init(quality);
        static std::vector<char> decBuf(0x100000);
        for (auto& c : chunks) {
            int n = p2pOpus->Decompress((const char*)c.data(), (int)c.size(), decBuf.data(), (int)decBuf.size());
            if (n > 4) { p2pOpusOk++; int16_t* s = (int16_t*)decBuf.data(); pcmP2pOpus.insert(pcmP2pOpus.end(), s, s + n); }
            n = p2pSilk->Decompress((const char*)c.data(), (int)c.size(), decBuf.data(), (int)decBuf.size());
            if (n > 4) { p2pSilkOk++; int16_t* s = (int16_t*)decBuf.data(); pcmP2pSilk.insert(pcmP2pSilk.end(), s, s + n); }
            n = speex->Decompress((const char*)c.data(), (int)c.size(), decBuf.data(), (int)decBuf.size());
            if (n > 4) { speexOk++; int16_t* s = (int16_t*)decBuf.data(); pcmSpeex.insert(pcmSpeex.end(), s, s + n); }
        }
        p2pOpus->Release(); p2pSilk->Release(); speex->Release();
    }
    printf("P2P-opus: ok=%d/%zu samples=%zu\n", p2pOpusOk, chunks.size(), pcmP2pOpus.size());
    printf("P2P-silk: ok=%d/%zu samples=%zu\n", p2pSilkOk, chunks.size(), pcmP2pSilk.size());
    printf("speex(frame): ok=%d/%zu samples=%zu\n", speexOk, chunks.size(), pcmSpeex.size());

    // direct micro-debug of the speex frame path on the first chunk: whole
    // chunk vs single 38-byte block, so we can see where the ladder's
    // speex(codec) result diverges from the raw per-block sweep above.
    if (!chunks.empty()) {
        IVoiceCodec* t = new VoiceCodec_Frame(new VoiceEncoder_Speex());
        bool initOk = t->Init(5);
        static std::vector<char> dbgBuf(0x100000);
        int nFull = t->Decompress((const char*)chunks[0].data(), (int)chunks[0].size(), dbgBuf.data(), (int)dbgBuf.size());
        int nBlock = chunks[0].size() >= 38
            ? t->Decompress((const char*)chunks[0].data(), 38, dbgBuf.data(), (int)dbgBuf.size())
            : -1;
        printf("speex(frame) micro: init=%d chunk=%zuB -> n=%d; single38 -> n=%d\n",
               (int)initOk, chunks[0].size(), nFull, nBlock);
        t->Release();
    }

    { // per-38-byte-block codec sweep -- the HLTV family chunks are strict
      // CBR 38-byte blocks whose first bytes cluster in 0x29-0x2F; verify
      // which codec family actually understands them. Speex NB rejects
      // garbage (its decode returns nonzero on corrupt syntax), so a
      // positive result here is meaningful; wideband/ultra-wideband were
      // never possible through the app's ladder because the vendored glue
      // only ever initialized speex_nb_mode.
        struct SpecexMode { const char* name; SpeexMode* mode; int outSamples; int rate; };
        static const SpecexMode speexModes[] = {
            { "nb  8k",  &speex_nb_mode,  160,  8000 },
            { "wb 16k",  &speex_wb_mode,  320, 16000 },
            { "uwb 32k", &speex_uwb_mode, 640, 32000 },
        };
        int blockTotal = 0;
        std::vector<int16_t> bestPcm;
        int bestMode = -1;
        for (int m = 0; m < 3; m++) {
            void* st = speex_decoder_init(speexModes[m].mode);
            SpeexBits bits;
            speex_bits_init(&bits);
            int enh = 1;
            speex_decoder_ctl(st, SPEEX_SET_ENH, &enh);
            int okBlocks = 0;
            int triedBlocks = 0;
            std::vector<int16_t> pcm;
            static float fbuf[2000];
            for (auto& c : chunks) {
                for (size_t o = 0; o + 38 <= c.size(); o += 38) {
                    triedBlocks++;
                    speex_bits_read_from(&bits, (char*)(c.data() + o), 38);
                    int ret = speex_decode(st, &bits, fbuf);
                    if (ret == 0) {
                        okBlocks++;
                        for (int s = 0; s < speexModes[m].outSamples; s++) {
                            pcm.push_back((int16_t)fbuf[s]);
                        }
                    }
                }
            }
            blockTotal = triedBlocks;
            printf("speex %s: okBlocks=%d of %d blocks\n",
                   speexModes[m].name, okBlocks, triedBlocks);
            if (triedBlocks > 0 && okBlocks * 2 > triedBlocks && bestMode < 0) {
                bestMode = m;
                bestPcm = pcm;
            }
            speex_bits_destroy(&bits);
            speex_decoder_destroy(st);
        }
        if (bestMode >= 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s_speex_%d.wav", argv[2], speexModes[bestMode].rate);
            DumpPcm16(path, bestPcm, speexModes[bestMode].rate);
        }
    }

    // replicate the vendored VoiceEncoder_Speex init EXACTLY (the ladder
    // path returns 0 while the plain sweep decodes 134/134 blocks) to see
    // which init ctl is the killer.
    {
        struct Ctl { const char* name; bool doIt; };
        static const Ctl ctls[3] = {
            { "enh", true },
            { "rate48000", true },
            { "quality8", true },
        };
        for (int variant = 0; variant < 8; variant++) {
            void* st = speex_decoder_init(&speex_nb_mode);
            SpeexBits bits;
            speex_bits_init(&bits);
            if (variant & 1) { int enh = 1; speex_decoder_ctl(st, SPEEX_SET_ENH, &enh); }
            if (variant & 2) { int rate = 48000; speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &rate); }
            if (variant & 4) { int q = 8; speex_decoder_ctl(st, SPEEX_SET_QUALITY, &q); }
            int ok = 0, tried = 0;
            static float fbuf2[2000];
            for (auto& c : chunks) {
                for (size_t o = 0; o + 38 <= c.size(); o += 38) {
                    tried++;
                    speex_bits_read_from(&bits, (char*)(c.data() + o), 38);
                    if (speex_decode(st, &bits, fbuf2) == 0) ok++;
                }
            }
            printf("glue-init variant enh=%d rate48k=%d q8=%d: ok=%d/%d\n",
                   variant & 1, (variant >> 1) & 1, (variant >> 2) & 1, ok, tried);
            speex_bits_destroy(&bits);
            speex_decoder_destroy(st);
        }
    }

    return 0;
}
