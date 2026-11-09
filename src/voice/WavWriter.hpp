#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vr {

// Writes 16-bit mono PCM as a standard RIFF/WAVE file. `path` must already
// be a proper std::filesystem::path (see PathUtf8.hpp::PathFromUtf8 for
// building one from a UTF-8 std::string without Windows ANSI-codepage
// mangling of non-ASCII filenames).
bool WriteWav(const std::filesystem::path& path, const std::vector<int16_t>& samples, int sampleRate);

// Encodes 16-bit mono PCM to MP3 via libmp3lame, if VR_WITH_MP3 is enabled
// at build time. Returns false (no-op) if MP3 support wasn't compiled in.
// `title`/`artist` are plain UTF-8 text embedded as ID3 tags.
bool WriteMp3(const std::filesystem::path& path, const std::vector<int16_t>& samples, int sampleRate,
              const std::string& title, const std::string& artist);

} // namespace vr
