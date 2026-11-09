#pragma once
#include <string>
#include <vector>

#include "../voice/VoiceExtractor.hpp"

namespace vr {

struct CliOptions {
    std::vector<std::string> inputs;
    std::string outDir;
    bool haveOutDir = false;
    bool quiet = false;
    bool writeMp3 = false;
    bool noAutoClose = false;
    bool force = false;
    int autoCloseSeconds = 3;
    ExtractOptions extract;
};

void PrintBanner();
void PrintUsage();

#ifdef _WIN32
// On Windows, main()'s argv is encoded in the current ANSI codepage, not
// UTF-8 -- so a demo path or output directory containing non-ASCII
// characters (Cyrillic folder names, etc.) passed on the command line
// would already be corrupted before any of our own path handling even
// runs. Fetching the wide command line and converting each arg to UTF-8
// ourselves sidesteps that entirely; everything downstream then
// consistently works in UTF-8 until PathFromUtf8 converts back to wide
// right before touching the filesystem.
std::vector<std::string> GetUtf8Argv();
#endif

// Parses argv (already UTF-8 on all platforms, see GetUtf8Argv on
// Windows) into a CliOptions. Returns false if parsing should stop the
// program immediately (e.g. -h/--help was handled, or an invalid option
// was given); `exitCode` is set to what main() should return in that
// case. Prompts interactively via stdin for a drag&dropped path if no
// positional inputs were given at all.
bool ParseCliArgs(int argc, const std::vector<std::string>& utf8Args, CliOptions& out, int& exitCode);

} // namespace vr
