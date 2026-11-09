#include "WavWriter.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>

#ifdef VR_WITH_MP3
#include <lame/lame.h>
#endif

namespace vr {

namespace {

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize = 0;
    char     wave[4] = {'W', 'A', 'V', 'E'};
    char     fmt[4] = {'f', 'm', 't', ' '};
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 1;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 16;
    char     dataId[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

} // namespace

bool WriteWav(const std::filesystem::path& path, const std::vector<int16_t>& samples, int sampleRate) {
    // std::ofstream(const std::filesystem::path&) uses path's native
    // string representation (wchar_t* on Windows, correctly Unicode;
    // plain UTF-8 bytes on POSIX) -- unlike passing a std::string, which
    // Windows would reinterpret through the current ANSI codepage.
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    WavHeader hdr;
    hdr.sampleRate = static_cast<uint32_t>(sampleRate);
    hdr.blockAlign = hdr.numChannels * hdr.bitsPerSample / 8;
    hdr.byteRate = hdr.sampleRate * hdr.blockAlign;
    hdr.dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    hdr.chunkSize = 36 + hdr.dataSize;

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!samples.empty()) {
        out.write(reinterpret_cast<const char*>(samples.data()),
                   static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
    }
    return out.good();
}

#ifdef VR_WITH_MP3

bool WriteMp3(const std::filesystem::path& path, const std::vector<int16_t>& samples, int sampleRate,
              const std::string& title, const std::string& artist) {
    lame_t lame = lame_init();
    if (!lame) return false;

    lame_set_num_channels(lame, 1);
    lame_set_in_samplerate(lame, sampleRate);
    lame_set_brate(lame, 128);
    lame_set_mode(lame, MONO);
    lame_set_quality(lame, 2);

    // LAME's id3tag API takes plain char* with no documented encoding
    // guarantee; it stores bytes as-is into ID3v1/v2 frames. UTF-8 bytes
    // round-trip fine for ID3v2 (most modern players assume UTF-8/Latin-1
    // there) even though this isn't a fully spec-correct ID3v2 unicode
    // frame -- acceptable for a convenience tag, not load-bearing like the
    // actual filename on disk.
    if (!title.empty()) id3tag_set_title(lame, title.c_str());
    if (!artist.empty()) id3tag_set_artist(lame, artist.c_str());

    if (lame_init_params(lame) < 0) {
        lame_close(lame);
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        lame_close(lame);
        return false;
    }

    std::vector<unsigned char> mp3Buf(samples.size() * 5 / 4 + 7200);

    int written = lame_encode_buffer(
        lame,
        samples.data(), nullptr, // mono: left channel only
        static_cast<int>(samples.size()),
        mp3Buf.data(), static_cast<int>(mp3Buf.size()));

    if (written > 0) {
        out.write(reinterpret_cast<const char*>(mp3Buf.data()), written);
    }

    int flushWritten = lame_encode_flush(lame, mp3Buf.data(), static_cast<int>(mp3Buf.size()));
    if (flushWritten > 0) {
        out.write(reinterpret_cast<const char*>(mp3Buf.data()), flushWritten);
    }

    lame_close(lame);
    return out.good();
}

#else

bool WriteMp3(const std::filesystem::path&, const std::vector<int16_t>&, int, const std::string&, const std::string&) {
    return false; // MP3 support not compiled in (VR_WITH_MP3 not defined)
}

#endif

} // namespace vr
