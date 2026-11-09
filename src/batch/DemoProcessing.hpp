#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.hpp"

namespace vr {

struct WrittenFile {
    std::string displayName;
    std::string steamId;
    float speakingSeconds = 0.0f;
    float firstTime = 0.0f;
    std::string fileName;
    bool wavOk = false;
};

struct DemoSummary {
    std::string demoDisplayName;
    bool ok = false;
    std::string error;
    std::string warning; // non-fatal issue (e.g. high parser desync rate) -- demo still processed
    std::vector<WrittenFile> written;
    std::string mixFileName;
};

// Converts a 64-bit community SteamID (as sent in the "*sid" userinfo key)
// to the classic STEAM_0:X:Y text form used throughout GoldSrc tooling
// (and by the original DemRead.exe-style extractors), e.g.
// 76561197800961942 -> STEAM_0:0:19848107. Returns empty for steamId64==0.
std::string FormatSteamId(uint64_t steamId64);

// Plain UTF-8-safe path component helpers, operating directly on bytes.
// Safe because ASCII delimiters ('/', '\\', '.') never appear as part of a
// multi-byte UTF-8 sequence (continuation bytes are always >= 0x80), so a
// byte-wise search for them can't accidentally split a Cyrillic character.
// This sidesteps std::filesystem::path entirely for name construction --
// on Windows, path's char-based constructors/appends go through the
// current ANSI codepage, which would corrupt non-ASCII text the same way
// raw std::ofstream(string) does. We only ever convert to a real
// std::filesystem::path (via PathFromUtf8) at the point a file is
// actually opened.
std::string Utf8FileName(const std::string& s);
std::string Utf8Stem(const std::string& s);

// Processes a single resolved .dem file (from disk, or extracted from a
// zip) and writes its output into `outDirUtf8`. Returns a summary used
// both for the batch's final report and for per-demo console output. The
// output folder is only actually created if at least one speaker was
// found -- a demo with zero voice data leaves no trace on disk.
DemoSummary ProcessOneDemo(const std::string& demoPathUtf8, const std::string& demoDisplayName,
                           const std::string& outDirUtf8, const CliOptions& o);

} // namespace vr
