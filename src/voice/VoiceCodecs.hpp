#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "VoiceExtractor.hpp"

namespace vr {

// Decodes one speaker's raw codec chunk list (time, compressed bytes) into
// a continuous 48kHz mono PCM track, auto-detecting Speex/Silk/Opus the
// same way ReVoiceDecoder's decode_all_codec() does. Implemented in
// VoiceCodecs.cpp against the vendored codec libs under third_party/.
// `outSpeakingSeconds` receives the actual decoded voice audio duration
// (sum of real codec output samples), excluding inter-chunk silence
// padding, i.e. how long this player actually spoke.
//
// This is the "compact" per-speaker track: real gaps between utterances
// (anything >= 0.75s) are replaced with a short fixed pause rather than
// reproducing the real (possibly minutes-long) in-game silence, so a
// player's own file stays a reasonable length regardless of how spread
// out their lines were across the full demo.
std::vector<int16_t> DecodeSpeakerChunks(
    const std::vector<std::pair<float, std::vector<uint8_t>>>& chunks,
    const ExtractOptions& opts, float& outFirstTime, float& outLastTime,
    float& outSpeakingSeconds);

// Decodes one speaker's chunks and writes the result directly into `dest`
// at its true absolute position (round(time * sampleRate)), mixing
// (summing, clamped) with whatever is already there. Used to build the
// full-demo-length "everyone mixed together" track, matching the original
// DemRead.exe-style tool's behaviour of keeping real silence rather than
// compacting it -- `dest` is expected to already be sized to cover the
// whole demo (e.g. ceil(demoTrackTime * sampleRate) samples).
void MixSpeakerChunksAt(
    const std::vector<std::pair<float, std::vector<uint8_t>>>& chunks,
    const ExtractOptions& opts, int sampleRate, std::vector<int32_t>& dest);

} // namespace vr
