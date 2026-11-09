#pragma once
#include <string>

#include "DemoProcessing.hpp"

namespace vr {

std::string FormatDuration(float seconds);

// Number of Unicode codepoints in a UTF-8 string (continuation bytes
// 10xxxxxx don't count), so table columns line up even with non-ASCII
// player names (Cyrillic, etc). Not a true display-width calculation
// (doesn't handle wide CJK glyphs or combining marks), but good enough
// for the Latin/Cyrillic names GoldSrc servers typically see.
size_t Utf8Length(const std::string& s);
std::string PadUtf8(const std::string& s, size_t width);

// Prints one demo's result to stdout: an error line if it failed to load
// or decode, a one-line "no voice data" note if nothing was found, a
// compact "N speaker(s)" line in --quiet mode, or the full speaker table
// + file list otherwise. Any non-fatal parsing warning is always shown
// regardless of quiet mode, just more tersely.
void PrintDemoSummary(const DemoSummary& s, const std::string& outDirUtf8, bool quiet);

} // namespace vr
