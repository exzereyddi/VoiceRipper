#include "DemoFile.hpp"

#include <cstring>
#include <fstream>

namespace vr {

namespace {

constexpr int kHeaderSize = 544;
constexpr int kDirEntrySize = 92;

int32_t GetI32(const std::vector<uint8_t>& b, size_t off) {
    int32_t v;
    std::memcpy(&v, &b[off], 4);
    return v;
}

float GetF32(const std::vector<uint8_t>& b, size_t off) {
    float v;
    std::memcpy(&v, &b[off], 4);
    return v;
}

// Sizes of the fixed, non-variable-length frame types. NetMsg(0/1) and
// Sound(8)/DemoBuffer(9) carry a length-prefixed payload and are handled
// separately in the scan loop below.
size_t FixedFrameSize(FrameType t) {
    switch (t) {
        case FrameType::DemoStart:   return 9;
        case FrameType::Console:     return 9 + 64;
        case FrameType::ClientData:  return 9 + 32;
        case FrameType::NextSection: return 9;
        case FrameType::Event:       return 9 + 88;
        case FrameType::WeaponAnim:  return 9 + 8;
        default: return 0; // variable-length or unknown
    }
}

} // namespace

bool DemoFile::ParseHeader(std::string& error) {
    if (m_buf.size() < static_cast<size_t>(kHeaderSize) + 8 ||
        std::memcmp(m_buf.data(), "HLDEMO", 6) != 0) {
        error = "not a GoldSrc demo file (missing HLDEMO magic)";
        return false;
    }

    int32_t demoProtocol = GetI32(m_buf, 8);
    if (demoProtocol != 5) {
        error = "unsupported demo protocol (" + std::to_string(demoProtocol) + "), expected 5";
        return false;
    }
    m_demoProtocol = demoProtocol;

    // Header layout (544 bytes total): magic[8], demoProtocol(4), netProtocol(4),
    // mapName[260], gameDir[260], mapCrc(4), directoryOffset(4).
    m_netProtocol = GetI32(m_buf, 12);

    m_directoryOffset = GetI32(m_buf, 540);
    return true;
}

bool DemoFile::ParseDirectory() {
    bool ok = m_directoryOffset > 0 &&
              static_cast<size_t>(m_directoryOffset) + 4 <= m_buf.size();
    if (!ok) return false;

    int32_t count = GetI32(m_buf, m_directoryOffset);
    ok = count > 0 && count <= 1024 &&
         static_cast<size_t>(m_directoryOffset) + 4 +
                 static_cast<size_t>(count) * kDirEntrySize <= m_buf.size();
    if (!ok) return false;

    m_entries.resize(count);
    size_t pos = static_cast<size_t>(m_directoryOffset) + 4;
    for (int i = 0; i < count; i++) {
        DirectoryEntry& e = m_entries[i];
        e.type = GetI32(m_buf, pos);
        e.flags = GetI32(m_buf, pos + 68);
        e.cdTrack = GetI32(m_buf, pos + 72);
        e.trackTime = GetF32(m_buf, pos + 76);
        e.frameCount = GetI32(m_buf, pos + 80);
        e.offset = GetI32(m_buf, pos + 84);
        e.length = GetI32(m_buf, pos + 88);
        pos += kDirEntrySize;
    }

    for (const auto& e : m_entries) {
        if (e.offset <= 0 || e.length < 0 ||
            static_cast<size_t>(e.offset) + static_cast<size_t>(e.length) > m_buf.size()) {
            m_entries.clear();
            return false;
        }
    }
    return true;
}

void DemoFile::RebuildDirectoryHeuristically() {
    // Some tools (and some corrupted / hand-edited demos) leave the
    // directory offset pointing at garbage. In that case we just treat
    // everything after the header as one big NetMsg section and let the
    // frame-by-frame scanner figure out real boundaries while decoding.
    m_entries.clear();
    DirectoryEntry e;
    e.type = 1;
    e.offset = kHeaderSize;
    e.length = static_cast<int32_t>(m_buf.size()) - kHeaderSize;
    e.frameCount = 0;
    m_entries.push_back(e);
}

bool DemoFile::Load(const std::filesystem::path& path, std::string& error) {
    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open()) {
        error = "cannot open input file: " + path.string();
        return false;
    }

    m_buf.assign(std::istreambuf_iterator<char>(fin), std::istreambuf_iterator<char>());
    fin.close();

    if (!ParseHeader(error)) return false;

    if (!ParseDirectory()) {
        RebuildDirectoryHeuristically();
    }

    return true;
}

void DemoFile::ForEachFrame(const std::function<void(const RawFrame&)>& cb) const {
    for (const auto& e : m_entries) {
        if (e.type != 1 && e.type != 0) continue; // only sections carrying playback frames

        size_t pos = static_cast<size_t>(e.offset);
        size_t end = pos + static_cast<size_t>(e.length);

        while (pos + 9 <= end) {
            uint8_t rawType = m_buf[pos];
            if (rawType > 9) break; // not a frame boundary we understand, stop this section

            FrameType type = static_cast<FrameType>(rawType);
            float time = GetF32(m_buf, pos + 1);
            int32_t frameIndex = GetI32(m_buf, pos + 5);
            size_t payloadOff = pos + 9;
            size_t frameEnd = 0;

            if (type == FrameType::NetMsgStart || type == FrameType::NetMsg) {
                // Layout: 9-byte common header, then a 468-byte fixed
                // usercmd/clientdata block, then a 4-byte message length,
                // then that many bytes of raw network message data.
                if (payloadOff + 468 + 4 > end) break;
                int32_t msgLen = GetI32(m_buf, payloadOff + 464);
                if (msgLen < 0 || payloadOff + 468 + static_cast<size_t>(msgLen) > end) break;
                frameEnd = payloadOff + 468 + static_cast<size_t>(msgLen);
            } else if (type == FrameType::Sound) {
                if (payloadOff + 4 > end) break;
                int32_t sampleLen = GetI32(m_buf, pos + 13);
                if (sampleLen < 0 || pos + 9 + 24 + static_cast<size_t>(sampleLen) > end) break;
                frameEnd = pos + 9 + 8 + sampleLen + 16;
            } else if (type == FrameType::DemoBuffer) {
                if (payloadOff + 4 > end) break;
                int32_t bufLen = GetI32(m_buf, payloadOff);
                if (bufLen < 0 || payloadOff + 4 + static_cast<size_t>(bufLen) > end) break;
                frameEnd = payloadOff + 4 + static_cast<size_t>(bufLen);
            } else {
                size_t sz = FixedFrameSize(type);
                if (sz == 0 || pos + sz > end) break;
                frameEnd = pos + sz;
            }

            RawFrame rf;
            rf.type = type;
            rf.time = time;
            rf.frameIndex = frameIndex;
            rf.payloadOffset = payloadOff;
            rf.payloadSize = frameEnd - payloadOff;
            cb(rf);

            pos = frameEnd;
        }
    }
}

} // namespace vr
