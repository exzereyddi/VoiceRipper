#include "Dedup.hpp"
#include "PathUtf8.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

namespace vr {

uint64_t HashFileContents(const std::string& pathUtf8) {
    std::ifstream in(PathFromUtf8(pathUtf8), std::ios::binary);
    if (!in.is_open()) return 0;

    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t hash = kFnvOffsetBasis;

    std::vector<char> buf(1 << 20); // 1MB chunks
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; i++) {
            hash ^= static_cast<uint8_t>(buf[static_cast<size_t>(i)]);
            hash *= kFnvPrime;
        }
    }
    return hash;
}

ProcessedDemoSet::ProcessedDemoSet(std::string manifestPathUtf8)
    : m_manifestPath(std::move(manifestPathUtf8)) {
    std::ifstream in(PathFromUtf8(m_manifestPath));
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        // Format: "<16 hex digits> <v|e>"; older builds wrote just the
        // bare hash (treated as HadVoice -- the caller still verifies
        // the output folder exists for those, so a stale entry can never
        // hide a deleted result).
        uint64_t h = 0;
        char flag = 'v';
        if (std::sscanf(line.c_str(), "%llx %c",
                        reinterpret_cast<unsigned long long*>(&h), &flag) >= 1) {
            m_hashes[h] = (flag == 'e') ? DemoOutcome::Empty : DemoOutcome::HadVoice;
        }
    }
}

bool ProcessedDemoSet::Contains(uint64_t hash, DemoOutcome* outcome) const {
    auto it = m_hashes.find(hash);
    if (it == m_hashes.end()) return false;
    if (outcome) *outcome = it->second;
    return true;
}

void ProcessedDemoSet::Add(uint64_t hash, DemoOutcome outcome) {
    m_hashes[hash] = outcome;
}

void ProcessedDemoSet::Save() const {
    std::ofstream out(PathFromUtf8(m_manifestPath), std::ios::trunc);
    if (!out.is_open()) return;
    for (const auto& [h, outcome] : m_hashes) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx %c",
                      static_cast<unsigned long long>(h),
                      outcome == DemoOutcome::Empty ? 'e' : 'v');
        out << buf << "\n";
    }
}

} // namespace vr
