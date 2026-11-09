#include "ReportPrinter.hpp"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>

namespace vr {

std::string FormatDuration(float seconds) {
    int total = static_cast<int>(seconds + 0.5f);
    int mins = total / 60;
    int secs = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
    return buf;
}

size_t Utf8Length(const std::string& s) {
    size_t len = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) len++;
    }
    return len;
}

std::string PadUtf8(const std::string& s, size_t width) {
    size_t len = Utf8Length(s);
    if (len >= width) return s;
    return s + std::string(width - len, ' ');
}

void PrintDemoSummary(const DemoSummary& s, const std::string& outDirUtf8, bool quiet) {
    (void)outDirUtf8; // kept in the signature for call-site symmetry/future use

    if (!s.ok) {
        std::cout << "  [ERROR] " << s.demoDisplayName << ": " << s.error << "\n";
        return;
    }
    if (s.written.empty()) {
        if (!quiet) {
            std::cout << "  " << s.demoDisplayName << ": no voice data found.\n";
            if (!s.warning.empty()) {
                std::cout << "  [WARN] " << s.warning << "\n";
            }
        }
        return;
    }

    if (quiet) {
        std::cout << "  " << s.demoDisplayName << ": " << s.written.size() << " speaker(s)";
        if (!s.warning.empty()) std::cout << "  [WARN: " << s.warning << "]";
        std::cout << "\n";
        return;
    }

    std::cout << "\n  == " << s.demoDisplayName << " ==\n";
    if (!s.warning.empty()) {
        std::cout << "  [WARN] " << s.warning << "\n";
    }
    std::cout << "  People found: " << s.written.size() << "\n\n";

    size_t nameWidth = 4;
    for (auto& w : s.written) nameWidth = std::max(nameWidth, Utf8Length(w.displayName));

    std::cout << "  " << PadUtf8("Name", nameWidth)
              << "  " << std::left << std::setw(17) << "SteamID"
              << "  " << std::setw(10) << "Talked"
              << "  First heard\n";
    std::cout << "  " << std::string(nameWidth + 17 + 10 + 20, '-') << "\n";

    for (auto& w : s.written) {
        std::cout << "  " << PadUtf8(w.displayName, nameWidth)
                  << "  " << std::left << std::setw(17) << w.steamId
                  << "  " << std::setw(10) << FormatDuration(w.speakingSeconds)
                  << "  " << FormatDuration(w.firstTime) << "\n";
    }

    std::cout << "\n  Files:\n";
    for (auto& w : s.written) {
        std::cout << "    " << (w.wavOk ? "[ok] " : "[!!] ") << w.fileName << "\n";
    }
    if (!s.mixFileName.empty()) {
        std::cout << "    [ok] " << s.mixFileName << "  (everyone mixed together)\n";
    }
}

} // namespace vr
