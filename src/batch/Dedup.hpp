#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>

namespace vr {

// Fast (non-cryptographic) 64-bit FNV-1a hash of a file's full contents,
// used purely for duplicate-detection -- if the exact same bytes were
// already processed before (same recording re-dropped, or the same demo
// appearing both loose and inside a .zip), there's no point decoding it
// all over again. Collisions are astronomically unlikely for this use
// case and even if one happened, the worst outcome is skipping a demo
// that should've been processed -- not corrupting anything.
uint64_t HashFileContents(const std::string& pathUtf8);

// Persistent "already processed" set, backed by a small text manifest
// file next to the exe (one hex hash per line) so re-running VoiceRipper
// later (even after closing it) still remembers what was already done.
class ProcessedDemoSet {
public:
    // `manifestPathUtf8` is the file to load from / save to.
    explicit ProcessedDemoSet(std::string manifestPathUtf8);

    bool Contains(uint64_t hash) const;
    void Add(uint64_t hash);
    void Save() const;

private:
    std::string m_manifestPath;
    std::unordered_set<uint64_t> m_hashes;
};

} // namespace vr
