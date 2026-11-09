#include "VoiceExtractor.hpp"
#include "VoiceCodecs.hpp"
#include "../demo/NetMsgParser.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace vr {

namespace {

constexpr int kSpeakerSampleRate = 48000;

std::ofstream& ProgLog() {
    thread_local std::ofstream log;
    thread_local bool initDone = false;
    if (!initDone) {
        initDone = true;
        if (std::getenv("VR_TRACE_FRAMETIME") || std::getenv("VR_TRACE_CHUNKS")) {
            log.open("voiceripper_frameprog.log", std::ios::app);
        }
    }
    return log;
}

uint64_t ParseSteamId64(const std::string& value) {
    try {
        return std::stoull(value);
    } catch (...) {
        return 0;
    }
}

struct SpeakerState {
    int32_t slot = -1;
    uint64_t steamId64 = 0;
    std::string name;
    std::vector<std::pair<float, std::vector<uint8_t>>> chunks; // (time, raw codec payload)
};

void ApplyUserInfo(SpeakerState& s, const std::string& info) {
    // Classic HL infokey string format: "\key1\value1\key2\value2\..."
    // (leading backslash). Splitting naively on '\\' produces an empty
    // first token before that leading separator, which would shift every
    // key/value pair by one if not skipped.
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : info) {
        if (c == '\\') {
            tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    tokens.push_back(cur);

    size_t start = (!tokens.empty() && tokens[0].empty()) ? 1 : 0;
    for (size_t t = start; t + 1 < tokens.size(); t += 2) {
        const std::string& key = tokens[t];
        const std::string& value = tokens[t + 1];
        if (key == "name") {
            s.name = value;
        } else if (key == "*sid") {
            s.steamId64 = ParseSteamId64(value);
        }
    }
}

} // namespace

ExtractResult ExtractVoice(const DemoFile& demo, const ExtractOptions& opts) {
    ExtractResult result;

    std::vector<SpeakerState> speakerStates;
    std::unordered_map<int32_t, size_t> slotToIndex;

    // svc_updateuserinfo for a slot can arrive before OR after that slot's
    // first svc_voicedata (most commonly before, e.g. right after the
    // player connects, long before they ever press push-to-talk). Track
    // the latest userinfo per slot independently so it's available
    // whenever a speaker record for that slot gets created, regardless of
    // message order.
    std::unordered_map<int32_t, std::string> latestUserInfoBySlot;

    const auto& buf = demo.Bytes();
    int32_t netProtocol = demo.NetProtocol();

    NetMsgState netState;
    NetMsgCallbacks cb;
    float currentFrameTime = 0.0f;
    size_t netMsgFrameCount = 0;

    cb.onVoiceData = [&](uint8_t slot, const uint8_t* data, size_t len) {
        auto it = slotToIndex.find(slot);
        size_t idx;
        if (it == slotToIndex.end()) {
            SpeakerState s;
            s.slot = slot;
            auto infoIt = latestUserInfoBySlot.find(slot);
            if (infoIt != latestUserInfoBySlot.end()) {
                ApplyUserInfo(s, infoIt->second);
            }
            idx = speakerStates.size();
            speakerStates.push_back(std::move(s));
            slotToIndex[slot] = idx;
        } else {
            idx = it->second;
        }
        speakerStates[idx].chunks.emplace_back(currentFrameTime,
                                                std::vector<uint8_t>(data, data + len));
        if (std::getenv("VR_TRACE_CHUNKS")) {
            std::cerr << "[chunk] slot=" << static_cast<int>(slot) << " t=" << currentFrameTime
                      << " len=" << len << " netMsgFrame#=" << netMsgFrameCount << "\n";
        }
    };

    cb.onUserInfo = [&](uint8_t slot, int32_t /*userId*/, const std::string& info) {
        latestUserInfoBySlot[slot] = info;
        auto it = slotToIndex.find(slot);
        if (it != slotToIndex.end()) {
            ApplyUserInfo(speakerStates[it->second], info);
        }
    };

    demo.ForEachFrame([&](const RawFrame& rf) {
        if (rf.type != FrameType::NetMsg && rf.type != FrameType::NetMsgStart) return;

        constexpr size_t kFixedBlock = 468;
        if (rf.payloadSize < kFixedBlock) return;

        const uint8_t* msgData = buf.data() + rf.payloadOffset + kFixedBlock;
        size_t msgSize = rf.payloadSize - kFixedBlock;

        currentFrameTime = rf.time;
        netMsgFrameCount++;
        if (ProgLog().is_open()) {
            ProgLog() << "[frame] #" << netMsgFrameCount << " time=" << rf.time
                      << " frameIndex=" << rf.frameIndex << std::endl;
        }
        ParseNetMsgFrame(msgData, msgSize, netProtocol, netState, cb, &result.desyncFrames);
    });

    result.totalFrames = netMsgFrameCount;

    if (std::getenv("VR_TRACE")) {
        std::cerr << "[extractor] netMsgFrames=" << netMsgFrameCount
                  << " speakers=" << speakerStates.size() << "\n";
        for (auto& s : speakerStates) {
            std::cerr << "  slot=" << s.slot << " chunks=" << s.chunks.size() << "\n";
        }
    }

    result.speakers.reserve(speakerStates.size());

    // Build the full-demo-length "everyone mixed together" track first,
    // from the raw per-speaker chunks at their true absolute timestamps
    // (same idea as the original DemRead.exe-style tool: real silence
    // stays real silence here, the file is just as long as the demo
    // itself, no compacting). Per-speaker compact tracks (with the short
    // fixed pause instead of real gaps) are built separately below.
    std::vector<int32_t> mixAccumulator;
    for (auto& s : speakerStates) {
        if (s.chunks.empty()) continue;
        MixSpeakerChunksAt(s.chunks, opts, kSpeakerSampleRate, mixAccumulator);
    }
    if (!mixAccumulator.empty()) {
        result.mixdown.resize(mixAccumulator.size());
        for (size_t i = 0; i < mixAccumulator.size(); i++) {
            int32_t v = mixAccumulator[i];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            result.mixdown[i] = static_cast<int16_t>(v);
        }
    }

    for (auto& s : speakerStates) {
        if (s.chunks.empty()) continue;

        SpeakerTrack track;
        track.slot = s.slot;
        track.steamId64 = s.steamId64;
        track.name = s.name;

        track.samples = DecodeSpeakerChunks(s.chunks, opts, track.firstTime, track.lastTime,
                                             track.speakingSeconds);
        if (!track.samples.empty()) {
            result.speakers.push_back(std::move(track));
        }
    }

    result.ok = true;
    return result;
}

} // namespace vr
