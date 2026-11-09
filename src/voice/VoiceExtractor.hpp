#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../demo/DemoFile.hpp"

namespace vr {

// One speaker's extracted audio, as raw 16-bit mono PCM samples at the
// codec's native output rate (voice_fix.cpp / ReVoice both use 48kHz for
// Speex and the Steam P2P wrapped codecs resample to the rate carried in
// the stream, so we normalize everything to 48kHz on the way out).
struct SpeakerTrack {
    int32_t slot = -1;
    uint64_t steamId64 = 0;
    std::string name;             // from svc_voiceinit, if present
    std::vector<int16_t> samples; // 48kHz mono PCM, sparse silence included
    float firstTime = 0.0f;
    float lastTime = 0.0f;
    float speakingSeconds = 0.0f; // actual decoded voice audio, excludes silence padding
};

struct ExtractResult {
    bool ok = false;
    std::string error;
    std::vector<SpeakerTrack> speakers;
    std::vector<int16_t> mixdown; // all speakers summed into one track

    // Parsing quality indicators: how many NetMsg frames were walked in
    // total, and how many of those hit a desync (unrecognized message id,
    // or a bit-packed message referencing a schema that was never
    // registered) partway through and had their remaining messages in
    // that one frame skipped. A high desync ratio means some voice data
    // in this demo may have been missed -- surfaced as a warning in the
    // CLI report rather than silently hidden.
    size_t totalFrames = 0;
    size_t desyncFrames = 0;
};

struct ExtractOptions {
    bool useSpeex = true;
    bool useSilk = true;
    bool useOpus = true;
    bool normalize = true;     // apply loudness normalization per speaker
    bool removeSilenceGaps = false;
};

// Scans a parsed DemoFile for svc_voiceinit / svc_voicedata messages inside
// NetMsg frames, decodes each chunk with whichever codec actually produces
// valid output (same auto-detect strategy ReVoiceDecoder uses: try Opus,
// fall back to Silk, fall back to Speex), and assembles one continuous PCM
// track per speaking client slot, time-aligned against the demo clock so
// multiple speakers mixed together stay in sync.
ExtractResult ExtractVoice(const DemoFile& demo, const ExtractOptions& opts);

} // namespace vr
