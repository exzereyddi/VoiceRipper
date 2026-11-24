#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "DemoFile.hpp"

namespace vr {

// Collects every text-bearing message from a demo the same way
// UnrealDemoScanner does (WTFPL): there is no separate "chat parser" there,
// all text sources funnel through one Add() with one fixed output format.
// The result is written to "<demo>_msglist.txt".
class TextMessageCollector {
public:
    void SetFrame(int32_t frameId, float time);
    void Add(const std::string& type, const std::string& msg);

    // svc_updateuserinfo: keeps slot -> name (needed to turn SayText's
    // clientIdx into a real nickname) and emits PLAYERINFO on name changes.
    void OnUserInfo(uint8_t slot, int32_t userId, const std::string& info);

    // Dynamic user messages (id >= 64). Only SayText / TextMsg carry text;
    // everything else is ignored. `payload` starts at the length byte for
    // variable-length (-1) registrations, same as UDS's BitBuffer.
    void OnUserMessage(const std::string& name, const uint8_t* payload, size_t len);

    size_t Count() const { return m_lines.size(); }
    bool Save(const std::filesystem::path& path, std::string& error) const;

private:
    std::string NameFor(uint8_t clientIdx) const;
    void ParseSayText(const uint8_t* payload, size_t len);
    void ParseTextMsg(const uint8_t* payload, size_t len);

    std::vector<std::string> m_lines;
    std::unordered_map<int32_t, std::string> m_names;        // current players
    std::unordered_map<int32_t, std::string> m_archiveNames; // everyone ever seen
    int32_t m_frameId = 0;
    float m_time = 0.0f;
};

// Independent second pass over the demo's NetMsg frames. Kept separate from
// ExtractVoice so the two use their own NetMsgState and neither can disturb
// the other's bit/delta state.
TextMessageCollector ExtractTextMessages(const DemoFile& demo);

} // namespace vr
