// Glue between VoiceExtractor's raw per-speaker voice chunks and the
// vendored Speex/Silk/Opus codec wrappers from ReVoiceDecoder. Mirrors the
// auto-detect strategy used by decode_all_codec() in UnrealDemoScanner's
// ReVoiceDecoder/Source.cpp: try Opus first, fall back to Silk, fall back
// to Speex, and once one of them has produced plausible output, stick with
// it for the rest of the stream (codec doesn't change mid-conversation).
#include "VoiceExtractor.hpp"
#include "VoiceCodecs.hpp"

#include "header.h" // pulls in IVoiceCodec.h, VoiceEncoder_*, SteamP2PCodec, voice_codec_frame

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace vr {

namespace {

std::ofstream& ChunkProgLog() {
    thread_local std::ofstream log;
    thread_local bool initDone = false;
    if (!initDone) {
        initDone = true;
        if (std::getenv("VR_TRACE_CHUNKPROG")) {
            log.open("voiceripper_chunkprog.log", std::ios::app);
        }
    }
    return log;
}

constexpr int kVoiceQuality = 5;
// Voice chunks arrive with natural network/frame jitter even during
// continuous, unbroken speech (the demo's frame timestamp only has the
// resolution of the recording's tick rate, it's not a sample-accurate
// audio clock) -- so treating every tiny timing gap as "silence" would
// chop continuous speech into dozens of stuttering micro-pauses. Below
// this threshold we just concatenate chunks with no gap at all (same
// utterance, player briefly released push-to-talk mid-sentence).
constexpr float kMinGapToPadSec = 0.75f;
// Above the threshold, we don't care HOW long the player was actually
// silent in-game (could be seconds or several minutes between separate
// utterances) -- we just insert one fixed short pause so the output file
// reflects "this is where he started talking again" without bloating the
// file with minutes of dead air for every real in-game silence.
constexpr float kPauseBetweenUtterancesSec = 1.0f;
constexpr size_t kMaxDecodeBuf = 0x100000;
constexpr int kMinValidSamples = 4; // matches ReVoiceDecoder's VALID_SAMPLES_TEST

// Which codec family a speaker's chunks turned out to be encoded with.
// Auto-detected on the first chunk that decodes cleanly, then locked for
// the whole track (per-speaker state: one demo can mix speakers whose
// voice went through different server-side codec paths).
enum class ActiveCodec {
    None,     // not detected yet
    Opus,     // Steam P2P wrapper + Opus
    SilkP2p,  // Steam P2P wrapper + Silk
    SilkRaw,  // raw SKP_Silk packets, no wrapper (old engine format)
    Speex,    // raw 38-byte CBR narrowband frames
};

} // namespace

std::vector<int16_t> DecodeSpeakerChunks(
    const std::vector<std::pair<float, std::vector<uint8_t>>>& chunks,
    const ExtractOptions& opts, float& outFirstTime, float& outLastTime,
    float& outSpeakingSeconds) {

    std::vector<int16_t> out;
    outFirstTime = chunks.empty() ? 0.0f : chunks.front().first;
    outLastTime = chunks.empty() ? 0.0f : chunks.back().first;
    outSpeakingSeconds = 0.0f;
    if (chunks.empty()) return out;

    IVoiceCodec* speexCodec = opts.useSpeex ? new VoiceCodec_Frame(new VoiceEncoder_Speex()) : nullptr;
    IVoiceCodec* silkCodec  = opts.useSilk  ? new CSteamP2PCodec(new VoiceEncoder_Silk())    : nullptr;
    IVoiceCodec* opusCodec  = opts.useOpus  ? new CSteamP2PCodec(new VoiceEncoder_Opus())    : nullptr;
    // Raw SKP_Silk frames without any wrapper (the old engine-era
    // "voice_silk" wire format, still what no-steam server scenes record
    // into their HLTV demos). Tried after the P2P-wrapped codecs, before
    // speex.
    IVoiceCodec* silkRawCodec = opts.useSilk ? new VoiceCodec_RawSilk() : nullptr;

    if (speexCodec) speexCodec->Init(kVoiceQuality);
    if (silkCodec)  silkCodec->Init(kVoiceQuality);
    if (opusCodec)  opusCodec->Init(kVoiceQuality);
    if (silkRawCodec) silkRawCodec->Init(kVoiceQuality);

    ActiveCodec active = ActiveCodec::None;

    std::vector<uint8_t> decodeBuf(kMaxDecodeBuf);
    float lastTime = chunks.front().first;
    size_t decodedSampleCount = 0;

    // 48kHz is the sample rate every one of these codecs ultimately
    // produces for GoldSrc/Source voice chat (Speex narrowband frames are
    // upsampled by VoiceEncoder_Speex itself; Silk/Opus report whatever
    // rate was negotiated in the PLT_SamplingRate sub-frame, but in
    // practice this pipeline always ends up at 48kHz mono).
    constexpr float kSampleRate = 48000.0f;

    for (const auto& [time, data] : chunks) {
        if (data.empty()) continue;

        if (ChunkProgLog().is_open()) {
            thread_local size_t chunkCounter = 0;
            ChunkProgLog() << "[chunkprog] #" << chunkCounter++ << " t=" << time << " size=" << data.size() << std::endl;
        }

        // Only pad with silence for gaps big enough to be a real pause in
        // speech, not every bit of inter-packet network jitter (which
        // would otherwise chop continuous talking into audible
        // micro-stutters -- this was the original bug: padding on ANY gap
        // > 20ms made normal speech sound broken up). The pause itself is
        // a fixed short duration regardless of how long the real in-game
        // gap was, so a player who spoke once near the start and once
        // near the end of a 30-minute demo doesn't end up with a
        // half-hour-long output file full of silence.
        float gap = time - lastTime;
        if (gap >= kMinGapToPadSec) {
            size_t silentSamples = static_cast<size_t>(kPauseBetweenUtterancesSec * kSampleRate);
            out.insert(out.end(), silentSamples, 0);
        }
        lastTime = time;

        int samples = 0;
        int opusSamples = -1, silkSamples = -1, silkRawSamples = -1, speexSamples = -1;

        auto tryDecode = [&](IVoiceCodec* codec, int* attempted) {
            *attempted = codec->Decompress(reinterpret_cast<const char*>(data.data()),
                                            static_cast<int>(data.size()),
                                            reinterpret_cast<char*>(decodeBuf.data()),
                                            static_cast<int>(decodeBuf.size()));
            return *attempted > kMinValidSamples;
        };

        // Codec auto-detect ladder, same strategy ReVoiceDecoder uses, with
        // one crucial upgrade over the original nested-if ladder: the
        // first codec that produces valid audio STICKS, and every
        // subsequent chunk decodes through that codec only. The old
        // ladder re-ran the whole cascade in the wrong order once a
        // non-standard codec (raw Silk) had been detected, silently
        // falling back to speex for every chunk after the first and
        // producing nothing.
        if (active == ActiveCodec::Opus) {
            samples = opusCodec->Decompress(reinterpret_cast<const char*>(data.data()),
                                             static_cast<int>(data.size()),
                                             reinterpret_cast<char*>(decodeBuf.data()),
                                             static_cast<int>(decodeBuf.size()));
            opusSamples = samples;
        } else if (active == ActiveCodec::SilkP2p) {
            samples = silkCodec->Decompress(reinterpret_cast<const char*>(data.data()),
                                            static_cast<int>(data.size()),
                                            reinterpret_cast<char*>(decodeBuf.data()),
                                            static_cast<int>(decodeBuf.size()));
            silkSamples = samples;
        } else if (active == ActiveCodec::SilkRaw) {
            samples = silkRawCodec->Decompress(reinterpret_cast<const char*>(data.data()),
                                               static_cast<int>(data.size()),
                                               reinterpret_cast<char*>(decodeBuf.data()),
                                               static_cast<int>(decodeBuf.size()));
            silkRawSamples = samples;
        } else if (active == ActiveCodec::Speex) {
            samples = speexCodec->Decompress(reinterpret_cast<const char*>(data.data()),
                                             static_cast<int>(data.size()),
                                             reinterpret_cast<char*>(decodeBuf.data()),
                                             static_cast<int>(decodeBuf.size()));
            speexSamples = samples;
        } else {
            // Not locked yet. Ladder mirrors what real servers actually
            // send, in order of real-world likelihood: Steam-P2P Opus,
            // Steam-P2P Silk, legacy plain speex (the format
            // svc_voiceinit "voice_speex" advertises -- HLTV demos of
            // no-steam servers carry exactly this), and only then raw
            // SKP_Silk as the most exotic last resort. NOTE: the speex
            // rung used to sit AFTER the raw-silk one and the vendored
            // speex glue had an inverted success contract (see
            // VoiceEncoder_Speex::DecodeFrame), so plain-speex demos
            // used to fall into raw silk, which "decodes" foreign bytes
            // into garbage noise without ever failing -- that produced
            // the notorious robot-noise output on HLTV demos.
            if (opusCodec && tryDecode(opusCodec, &opusSamples)) {
                active = ActiveCodec::Opus;
            } else if (silkCodec && tryDecode(silkCodec, &silkSamples)) {
                active = ActiveCodec::SilkP2p;
            } else if (speexCodec && tryDecode(speexCodec, &speexSamples)) {
                active = ActiveCodec::Speex;
            } else if (silkRawCodec && tryDecode(silkRawCodec, &silkRawSamples)) {
                active = ActiveCodec::SilkRaw;
            }
            // samples from the winning attempt (0 if none won)
            samples = opusSamples > kMinValidSamples ? opusSamples
                   : silkSamples > kMinValidSamples ? silkSamples
                   : speexSamples > kMinValidSamples ? speexSamples
                   : silkRawSamples > kMinValidSamples ? silkRawSamples : 0;
        }

        if (ChunkProgLog().is_open()) {
            char hex[1024 * 3 + 1];
            size_t hx = 0;
            size_t nHex = data.size() < 1024 ? data.size() : 1024;
            for (size_t hb = 0; hb < nHex; hb++) {
                hx += std::snprintf(hex + hx, sizeof(hex) - hx, "%02X ", data[hb]);
            }
            ChunkProgLog() << "[codec] t=" << time << " size=" << data.size()
                           << " opus=" << opusSamples << " silk=" << silkSamples
                           << " silkraw=" << silkRawSamples
                           << " speex=" << speexSamples
                           << " active=" << static_cast<int>(active)
                           << " head=" << hex
                           << std::endl;
        }

        if (samples > 0) {
            const int16_t* samplesArray = reinterpret_cast<const int16_t*>(decodeBuf.data());
            out.insert(out.end(), samplesArray, samplesArray + samples);
            decodedSampleCount += static_cast<size_t>(samples);
        }
    }

    outSpeakingSeconds = static_cast<float>(decodedSampleCount) / kSampleRate;

    if (speexCodec) speexCodec->Release();
    if (silkCodec) silkCodec->Release();
    if (opusCodec) opusCodec->Release();
    if (silkRawCodec) silkRawCodec->Release();

    if (opts.removeSilenceGaps) {
        std::vector<int16_t> trimmed;
        trimmed.reserve(out.size());
        size_t silenceRun = 0;
        constexpr size_t kMaxSilenceSamples = 48000 * 2; // cap gaps at 2s
        for (int16_t s : out) {
            if (s == 0) {
                silenceRun++;
            } else {
                if (silenceRun > 0) {
                    trimmed.insert(trimmed.end(), std::min(silenceRun, kMaxSilenceSamples), 0);
                    silenceRun = 0;
                }
                trimmed.push_back(s);
            }
        }
        out.swap(trimmed);
    }

    return out;
}

void MixSpeakerChunksAt(
    const std::vector<std::pair<float, std::vector<uint8_t>>>& chunks,
    const ExtractOptions& opts, int sampleRate, std::vector<int32_t>& dest) {

    if (chunks.empty()) return;

    IVoiceCodec* speexCodec = opts.useSpeex ? new VoiceCodec_Frame(new VoiceEncoder_Speex()) : nullptr;
    IVoiceCodec* silkCodec  = opts.useSilk  ? new CSteamP2PCodec(new VoiceEncoder_Silk())    : nullptr;
    IVoiceCodec* opusCodec  = opts.useOpus  ? new CSteamP2PCodec(new VoiceEncoder_Opus())    : nullptr;
    IVoiceCodec* silkRawCodec = opts.useSilk ? new VoiceCodec_RawSilk() : nullptr;

    if (speexCodec) speexCodec->Init(kVoiceQuality);
    if (silkCodec)  silkCodec->Init(kVoiceQuality);
    if (opusCodec)  opusCodec->Init(kVoiceQuality);
    if (silkRawCodec) silkRawCodec->Init(kVoiceQuality);

    ActiveCodec active = ActiveCodec::None;
    std::vector<uint8_t> decodeBuf(kMaxDecodeBuf);

    // Voice codecs (Opus/Silk especially) decode each compressed chunk
    // into MORE audio than the real time elapsed since the previous chunk
    // arrived (they carry some lookahead/overlap for smoother synthesis),
    // so consecutive chunks' decoded samples routinely overlap in time by
    // tens of milliseconds. Placing each one at its own
    // round(time*sampleRate) and summing (+=) therefore double-adds
    // overlapping regions, producing audible distortion/stutter exactly
    // where chunks butt up against each other -- this was the actual bug
    // behind "the mixdown sounds broken up/glitchy" while the per-speaker
    // compact files (built by simple sequential concatenation) sound
    // fine. Fix: keep a running write cursor per speaker. Only jump the
    // cursor forward to a chunk's true absolute position when there's a
    // real gap (the cursor has fallen behind, i.e. previous chunk's
    // decoded audio + real silence has already passed); otherwise keep
    // writing sequentially from wherever the cursor already is, so this
    // speaker's own audio never overlaps itself. Real silence between
    // utterances is still preserved faithfully on the shared timeline.
    bool haveCursor = false;
    size_t cursor = 0;

    for (const auto& [time, data] : chunks) {
        if (data.empty()) continue;

        if (ChunkProgLog().is_open()) {
            thread_local size_t mixChunkCounter = 0;
            ChunkProgLog() << "[mixchunkprog] #" << mixChunkCounter++ << " t=" << time << " size=" << data.size() << std::endl;
        }

        int samples = 0;

        auto tryDecodeMix = [&](IVoiceCodec* codec) {
            return codec->Decompress(reinterpret_cast<const char*>(data.data()),
                                     static_cast<int>(data.size()),
                                     reinterpret_cast<char*>(decodeBuf.data()),
                                     static_cast<int>(decodeBuf.size()));
        };

        // Same detect-then-lock ladder as DecodeSpeakerChunks.
        switch (active) {
            case ActiveCodec::Opus:
                samples = tryDecodeMix(opusCodec);
                break;
            case ActiveCodec::SilkP2p:
                samples = tryDecodeMix(silkCodec);
                break;
            case ActiveCodec::SilkRaw:
                samples = tryDecodeMix(silkRawCodec);
                break;
            case ActiveCodec::Speex:
                samples = tryDecodeMix(speexCodec);
                break;
            case ActiveCodec::None:
            default:
                samples = 0;
                if (opusCodec) {
                    samples = tryDecodeMix(opusCodec);
                    if (samples > kMinValidSamples) active = ActiveCodec::Opus;
                }
                if (active == ActiveCodec::None && silkCodec) {
                    samples = tryDecodeMix(silkCodec);
                    if (samples > kMinValidSamples) active = ActiveCodec::SilkP2p;
                }
                if (active == ActiveCodec::None && speexCodec) {
                    samples = tryDecodeMix(speexCodec);
                    if (samples > kMinValidSamples) active = ActiveCodec::Speex;
                }
                if (active == ActiveCodec::None && silkRawCodec) {
                    samples = tryDecodeMix(silkRawCodec);
                    if (samples > kMinValidSamples) active = ActiveCodec::SilkRaw;
                }
                break;
        }

        if (ChunkProgLog().is_open()) {
            ChunkProgLog() << "[mixchunkprog]   -> samples=" << samples << std::endl;
        }

        if (samples <= 0) continue;

        const int16_t* samplesArray = reinterpret_cast<const int16_t*>(decodeBuf.data());

        size_t idealStart = static_cast<size_t>(time * static_cast<float>(sampleRate) + 0.5f);
        size_t startSample = (!haveCursor || idealStart > cursor) ? idealStart : cursor;
        haveCursor = true;

        size_t endSample = startSample + static_cast<size_t>(samples);
        if (endSample > dest.size()) {
            dest.resize(endSample, 0);
        }
        for (int i = 0; i < samples; i++) {
            dest[startSample + static_cast<size_t>(i)] += samplesArray[i];
        }
        cursor = endSample;
    }

    if (speexCodec) speexCodec->Release();
    if (silkCodec) silkCodec->Release();
    if (opusCodec) opusCodec->Release();
    if (silkRawCodec) silkRawCodec->Release();
}

} // namespace vr
