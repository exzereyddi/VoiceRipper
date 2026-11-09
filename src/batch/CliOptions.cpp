#include "CliOptions.hpp"

#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace vr {

void PrintBanner() {
    std::cout << "\n  VoiceRipper - GoldSrc demo voice chat extractor\n";
    std::cout << "  -------------------------------------------------\n\n";
}

void PrintUsage() {
    std::cout <<
        "Usage:\n"
        "  voiceripper <demo.dem | folder | archive.zip> [more inputs...] [options]\n\n"
        "  A folder is scanned recursively for .dem files; a .zip archive has\n"
        "  every .dem entry inside it extracted and processed. You can also pass\n"
        "  (or drag & drop) multiple files/folders/archives at once -- everything\n"
        "  found across all of them is processed together in one batch run, with\n"
        "  one combined summary printed at the end.\n\n"
        "Options:\n"
        "  -o, --out <dir>      output directory (default: a folder named after\n"
        "                       voiceripper.exe's own location, NOT the input's)\n"
        "      --no-speex       disable Speex decoding\n"
        "      --no-silk        disable Silk decoding\n"
        "      --no-opus        disable Opus decoding\n"
        "      --no-normalize   skip loudness normalization\n"
        "      --trim-silence   collapse long silent gaps\n"
#ifdef VR_WITH_MP3
        "      --mp3            also export .mp3 (in addition to .wav)\n"
#endif
        "      --no-autoclose   keep console open after finishing (wait for Enter)\n"
        "      --force          re-process demos even if already done before\n"
        "                       (by default, identical file contents seen in a\n"
        "                       previous run are skipped -- see voiceripper_done.txt\n"
        "                       next to the exe)\n"
        "  -q, --quiet          minimal output\n"
        "  -h, --help           this help\n\n";
}

#ifdef _WIN32
std::vector<std::string> GetUtf8Argv() {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> out;
    if (!wargv) return out;
    out.reserve(wargc);
    for (int i = 0; i < wargc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s;
        if (len > 0) {
            s.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        }
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    return out;
}
#endif

bool ParseCliArgs(int argc, const std::vector<std::string>& utf8Args, CliOptions& o, int& exitCode) {
    for (int i = 1; i < argc; i++) {
        const std::string& a = utf8Args[static_cast<size_t>(i)];
        if (a == "-h" || a == "--help") {
            PrintBanner();
            PrintUsage();
            exitCode = 0;
            return false;
        } else if (a == "-o" || a == "--out") {
            if (i + 1 >= argc) {
                std::cout << "missing value for " << a << "\n";
                exitCode = 1;
                return false;
            }
            o.outDir = utf8Args[static_cast<size_t>(++i)];
            o.haveOutDir = true;
        } else if (a == "--no-speex") {
            o.extract.useSpeex = false;
        } else if (a == "--no-silk") {
            o.extract.useSilk = false;
        } else if (a == "--no-opus") {
            o.extract.useOpus = false;
        } else if (a == "--no-normalize") {
            o.extract.normalize = false;
        } else if (a == "--trim-silence") {
            o.extract.removeSilenceGaps = true;
        } else if (a == "--mp3") {
            o.writeMp3 = true;
        } else if (a == "--no-autoclose") {
            o.noAutoClose = true;
        } else if (a == "--force") {
            o.force = true;
        } else if (a == "-q" || a == "--quiet") {
            o.quiet = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cout << "unknown option: " << a << "\n";
            PrintUsage();
            exitCode = 1;
            return false;
        } else {
            // Every non-flag argument is a separate input (demo file,
            // folder, or zip) -- this is what makes dragging multiple
            // files/folders onto the .exe at once work: Windows passes
            // each dropped item as its own argv entry, not just the
            // first one.
            o.inputs.push_back(a);
        }
    }

    if (o.inputs.empty()) {
        PrintBanner();
        std::cout << "  drag & drop one or more .dem files / folders / .zip archives here:\n  > ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            exitCode = 0;
            return false;
        }
        while (!line.empty() && (line.front() == '"' || line.front() == '\'')) line.erase(0, 1);
        while (!line.empty() && (line.back() == '"' || line.back() == '\'')) line.pop_back();
        if (!line.empty()) o.inputs.push_back(line);
        if (o.inputs.empty()) {
            std::cout << "  no input.\n";
            exitCode = 1;
            return false;
        }
    }

    return true;
}

} // namespace vr
