#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace vr {

// Fast (non-cryptographic) 64-bit FNV-1a hash of a file's full contents,
// used purely for duplicate-detection -- if the exact same bytes were
// already processed before (same recording re-dropped, or the same demo
// appearing both loose and inside a .zip), there's no point decoding it
// all over again. Collisions are astronomically unlikely for this use
// case and even if one happened, the worst outcome is skipping a demo
// that should've been processed -- not corrupting anything.
uint64_t HashFileContents(const std::string& pathUtf8);

// Outcome flag stored next to each processed hash in the manifest.
enum class DemoOutcome : uint8_t {
    HadVoice = 0, // a "<demoname>_voice" folder was written for it
    Empty    = 1, // parsed fine, but nobody talked in it (no folder)
};

// Persistent "already processed" set, backed by a small text manifest
// file next to the exe: one "<hex hash> <v|e>" entry per line. The flag
// matters: a demo that produced voice is only considered "done" while
// its output folder still exists (the user may well have deleted the
// files -- e.g. to redo them after installing a newer VoiceRipper build
// -- and then re-dropping MUST process it again); an empty demo leaves
// nothing on disk by design, so its hash alone is the only marker and
// it stays skipped permanently.
//
// Manifest lines written by older builds carry just the bare hash; those
// are read as HadVoice (the folder check makes them safe either way).
class ProcessedDemoSet {
public:
    // `manifestPathUtf8` is the file to load from / save to.
    explicit ProcessedDemoSet(std::string manifestPathUtf8);

    // Returns true if the hash is in the manifest and fills `outcome`.
    bool Contains(uint64_t hash, DemoOutcome* outcome = nullptr) const;
    void Add(uint64_t hash, DemoOutcome outcome);
    void Save() const;

private:
    std::string m_manifestPath;
    std::unordered_map<uint64_t, DemoOutcome> m_hashes;
};

} // namespace vr
