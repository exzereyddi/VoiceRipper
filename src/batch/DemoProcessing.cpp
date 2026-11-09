#include "DemoProcessing.hpp"
#include "PathUtf8.hpp"

#include "../demo/DemoFile.hpp"
#include "../voice/Normalize.hpp"
#include "../voice/VoiceExtractor.hpp"
#include "../voice/WavWriter.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace vr {

namespace {

constexpr int kSampleRate = 48000;

std::string SanitizeFileNamePart(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

std::string FormatSteamId(uint64_t steamId64) {
    if (steamId64 == 0) return {};
    uint64_t accountId = steamId64 & 0xFFFFFFFFULL;
    uint64_t authServer = accountId & 1ULL;
    uint64_t accountNum = accountId >> 1;
    return "STEAM_0_" + std::to_string(authServer) + "_" + std::to_string(accountNum);
}

std::string Utf8FileName(const std::string& s) {
    size_t pos = s.find_last_of("/\\");
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string Utf8Stem(const std::string& s) {
    std::string name = Utf8FileName(s);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

DemoSummary ProcessOneDemo(const std::string& demoPathUtf8, const std::string& demoDisplayName,
                           const std::string& outDirUtf8, const CliOptions& o) {
    DemoSummary summary;
    summary.demoDisplayName = demoDisplayName;

    DemoFile demo;
    std::string err;
    if (!demo.Load(PathFromUtf8(demoPathUtf8), err)) {
        summary.error = "failed to load demo: " + err;
        return summary;
    }

    ExtractResult result = ExtractVoice(demo, o.extract);
    if (!result.ok) {
        summary.error = "extraction failed: " + result.error;
        return summary;
    }

    // A desync means the parser hit a network message it couldn't make
    // sense of (unrecognized id, or a bit-packed message whose field
    // schema was never registered) and had to skip the rest of that one
    // frame -- usually harmless (most frames don't carry voice data
    // anyway), but if it's happening on a large fraction of frames, some
    // svc_voicedata in this demo may have been silently missed. Surface
    // that as a warning rather than hiding it, but don't treat it as a
    // hard failure -- the demo is still processed and whatever voice WAS
    // found is still written out.
    if (result.totalFrames > 0) {
        double desyncRatio = static_cast<double>(result.desyncFrames) / static_cast<double>(result.totalFrames);
        if (desyncRatio >= 0.05) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "parser desynced on %.0f%% of frames (%zu/%zu) -- some voice may be missing",
                          desyncRatio * 100.0, result.desyncFrames, result.totalFrames);
            summary.warning = buf;
        }
    }

    if (result.speakers.empty()) {
        summary.ok = true; // not an error, just nothing to write -- and
                            // deliberately no output folder created either
        return summary;
    }

    std::filesystem::create_directories(PathFromUtf8(outDirUtf8));

    std::sort(result.speakers.begin(), result.speakers.end(),
              [](const SpeakerTrack& a, const SpeakerTrack& b) {
                  return a.speakingSeconds > b.speakingSeconds;
              });

    std::string baseName = Utf8Stem(demoDisplayName);

    for (auto& speaker : result.speakers) {
        if (o.extract.normalize) {
            NormalizeLoudness(speaker.samples, kSampleRate);
        }

        std::string steamTag = FormatSteamId(speaker.steamId64);
        std::string displayName = !speaker.name.empty() ? speaker.name
                                 : (!steamTag.empty() ? steamTag : ("slot" + std::to_string(speaker.slot)));

        // File is already inside a "<demoname>_voice" folder, so the demo
        // name would just be redundant noise in every single filename --
        // keep only the bit that's actually unique per speaker: when they
        // started talking, their SteamID, and their name.
        std::string who = SanitizeFileNamePart(displayName);
        std::string fileBase;
        char timeBuf[32];
        std::snprintf(timeBuf, sizeof(timeBuf), "%.3f", speaker.firstTime);
        if (!steamTag.empty() && !speaker.name.empty()) {
            fileBase = "[" + std::string(timeBuf) + "]" + steamTag + "_" + who;
        } else {
            fileBase = "[" + std::string(timeBuf) + "]_" + who;
        }

        std::string wavName = fileBase + ".wav";
        bool wavOk = WriteWav(PathFromUtf8(outDirUtf8 + "/" + wavName), speaker.samples, kSampleRate);

        if (o.writeMp3) {
            std::string mp3Name = fileBase + ".mp3";
            WriteMp3(PathFromUtf8(outDirUtf8 + "/" + mp3Name), speaker.samples, kSampleRate, displayName, baseName);
        }

        WrittenFile wf;
        wf.displayName = displayName;
        wf.steamId = steamTag.empty() ? std::string("-") : steamTag;
        std::replace(wf.steamId.begin(), wf.steamId.end(), '_', ':');
        wf.speakingSeconds = speaker.speakingSeconds;
        wf.firstTime = speaker.firstTime;
        wf.fileName = wavName;
        wf.wavOk = wavOk;
        summary.written.push_back(std::move(wf));
    }

    if (!result.mixdown.empty()) {
        std::vector<int16_t> mix = result.mixdown;
        if (o.extract.normalize) {
            NormalizeLoudness(mix, kSampleRate);
        }
        summary.mixFileName = "all.wav";
        WriteWav(PathFromUtf8(outDirUtf8 + "/" + summary.mixFileName), mix, kSampleRate);
        if (o.writeMp3) {
            std::string mixMp3Name = "all.mp3";
            WriteMp3(PathFromUtf8(outDirUtf8 + "/" + mixMp3Name), mix, kSampleRate, baseName + " (all)", baseName);
        }
    }

    summary.ok = true;
    return summary;
}

} // namespace vr
