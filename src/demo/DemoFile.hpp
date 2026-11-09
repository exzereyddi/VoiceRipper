#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vr {

// GoldSrc demo frame types (demo protocol 5, used by HL1/CS1.6/ReHLDS).
enum class FrameType : uint8_t {
    NetMsgStart = 0, // "demo start" netmsg frame (before game actually begins)
    NetMsg      = 1, // regular network message frame (contains svc_* messages)
    DemoStart   = 2, // marks the point the engine should start reading frames from
    Console     = 3, // console command frame
    ClientData  = 4, // client view/weapon state snapshot
    NextSection = 5, // end of a directory section
    Event       = 6,
    WeaponAnim  = 7,
    Sound       = 8,
    DemoBuffer  = 9,
};

struct DirectoryEntry {
    int32_t type = 0;
    int32_t flags = 0;
    int32_t cdTrack = 0;
    float   trackTime = 0.0f;
    int32_t frameCount = 0;
    int32_t offset = 0;
    int32_t length = 0;
};

// One raw frame as it sits in the file, with the enclosing time/frame index
// header already parsed and offsets into the owning buffer resolved.
struct RawFrame {
    FrameType type = FrameType::NetMsg;
    float     time = 0.0f;
    int32_t   frameIndex = 0;
    size_t    payloadOffset = 0; // offset of the frame's type-specific payload
    size_t    payloadSize = 0;
};

// A single svc_voicedata chunk extracted from a NetMsg frame's message
// bytes, tagged with the demo timestamp it occurred at.
struct VoiceChunk {
    float    time = 0.0f;
    int32_t  clientSlot = -1;    // 0-based player slot the voice came from
    uint64_t steamId64 = 0;      // when known from svc_voiceinit tracking, else 0
    std::vector<uint8_t> data;   // raw compressed voice payload (codec-specific)
};

// Minimal GoldSrc .dem reader: validates header, resolves the directory
// (rebuilding one heuristically if missing/corrupt, same idea UDF2 uses),
// and walks every NetMsg section frame-by-frame so callers can inspect the
// embedded network message bytes.
class DemoFile {
public:
    bool Load(const std::filesystem::path& path, std::string& error);

    const std::vector<uint8_t>& Bytes() const { return m_buf; }
    const std::vector<DirectoryEntry>& Entries() const { return m_entries; }
    int32_t DemoProtocol() const { return m_demoProtocol; }
    int32_t NetProtocol() const { return m_netProtocol; }

    // Invokes `cb` for every frame found in every directory section, in
    // file order. Returning false from the callback does not stop parsing.
    void ForEachFrame(const std::function<void(const RawFrame&)>& cb) const;

private:
    std::vector<uint8_t> m_buf;
    std::vector<DirectoryEntry> m_entries;
    int32_t m_directoryOffset = 0;
    int32_t m_demoProtocol = 5;
    int32_t m_netProtocol = 0;

    bool ParseHeader(std::string& error);
    bool ParseDirectory();
    void RebuildDirectoryHeuristically();
};

} // namespace vr
