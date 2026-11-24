#include "TextMessages.hpp"
#include "NetMsgParser.hpp"

#include <cstdio>
#include <fstream>

namespace vr {

namespace {

// UDS: ReadString / ReadStringMaxLen(256) -- NUL-terminated, capped at 256
// bytes, position advances past the terminator. Operates on a bounded byte
// span so a malformed message can never run off the end of the frame.
std::string ReadCString(const uint8_t* p, size_t n, size_t& pos, size_t maxLen = 256) {
    std::string s;
    while (pos < n) {
        uint8_t c = p[pos++];
        if (c == 0) break;
        s += static_cast<char>(c);
        if (s.size() >= maxLen) break;
    }
    return s;
}

// Counts UTF-8 code points (non-continuation bytes), so Pad() lines up
// Cyrillic text the same way .NET's string.PadRight(20) does.
size_t Utf8Len(const std::string& s) {
    size_t len = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) len++;
    }
    return len;
}

std::string Pad(const std::string& s, size_t width) {
    size_t len = Utf8Len(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}

// UDS: DemoScanner_AddTextMessage replaces control/color bytes before
// writing (\n/\r -> ^n, 0x01..0x04 -> ^1..^4).
std::string Sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r') {
            out += "^n";
        } else if (c >= 0x01 && c <= 0x04) {
            out += '^';
            out += static_cast<char>('0' + c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

const char* TextMsgTypeName(uint8_t target) {
    static const char* kNames[] = {"NONE", "PRINTNOTIFY", "PRINTCONSOLE",
                                  "PRINTTALK", "PRINTCENTER", "PRINTRADIO"};
    return target <= 5 ? kNames[target] : "UNKNOWN";
}

// "1:17" style clock string, matching UDS's LastKnowTimeString.
std::string ClockString(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    int total = static_cast<int>(seconds + 0.5f);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

std::string TimeString(float seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    return buf;
}

} // namespace

void TextMessageCollector::SetFrame(int32_t frameId, float time) {
    m_frameId = frameId;
    m_time = time;
}

void TextMessageCollector::Add(const std::string& type, const std::string& msg) {
    std::string frameId = std::to_string(m_frameId);
    if (frameId.size() < 4) frameId.insert(0, 4 - frameId.size(), '0');

    const std::string at = " at (" + TimeString(m_time) + ") " + ClockString(m_time);

    if (msg.empty()) {
        m_lines.push_back("[" + type + "]  [FRAME NUMBER: " + frameId +
                          "] : EMPTY MESSAGE" + at);
        return;
    }

    std::string clean = Sanitize(msg);
    if (clean.empty()) return;

    // UDS: type.PadRight(20), msg.PadRight(25), then
    // "[type]  [FRAME NUMBER: NNNN] : [msg] at (time) timestring"
    m_lines.push_back("[" + Pad(type, 20) + "]  [FRAME NUMBER: " + frameId +
                      "] : [" + Pad(clean, 25) + "]" + at);
}

void TextMessageCollector::OnUserInfo(uint8_t slot, int32_t userId, const std::string& info) {
    // Classic HL infokey string: "\key1\value1\key2\value2\..."
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : info) {
        if (c == '\\') {
            tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    tokens.push_back(cur);

    std::string name;
    size_t start = (!tokens.empty() && tokens[0].empty()) ? 1 : 0;
    for (size_t t = start; t + 1 < tokens.size(); t += 2) {
        if (tokens[t] == "name") {
            name = tokens[t + 1];
            break;
        }
    }
    if (name.empty()) return;

    auto it = m_names.find(slot);
    std::string old = (it == m_names.end()) ? std::string() : it->second;
    if (!old.empty() && old != name) {
        Add("PLAYERINFO", "Player " + std::to_string(userId) +
                         " changes nickname from [" + old + "] to [" + name + "] at " +
                         ClockString(m_time));
    }
    m_names[slot] = name;
    m_archiveNames[slot] = name;
}

std::string TextMessageCollector::NameFor(uint8_t clientIdx) const {
    if (clientIdx == 0) return "SERVER";
    int32_t slot = static_cast<int32_t>(clientIdx) - 1;
    auto it = m_names.find(slot);
    if (it != m_names.end() && !it->second.empty()) return it->second;
    auto arc = m_archiveNames.find(slot);
    if (arc != m_archiveNames.end() && !arc->second.empty()) return arc->second;
    return "UNK_CLIENT_ID_" + std::to_string(static_cast<int>(clientIdx));
}

void TextMessageCollector::ParseSayText(const uint8_t* p, size_t n) {
    // [len:byte][clientIdx:byte][arg1:C-string][optional 2nd block]
    if (n < 2) return;
    size_t pos = 0;
    pos++; // declared length byte (UDS uses it only to seek back)
    uint8_t clientIdx = p[pos++];
    std::string arg1 = ReadCString(p, n, pos);

    // UDS: if arg1 == "%s" or starts with '#', drop one byte and append
    // the following string as " = ...".
    if (arg1 == "%s" || (!arg1.empty() && arg1[0] == '#')) {
        if (pos < n) pos++;
        arg1 += " = " + ReadCString(p, n, pos);
    }

    Add("SayText", "[" + NameFor(clientIdx) + "]:" + arg1);
}

void TextMessageCollector::ParseTextMsg(const uint8_t* p, size_t n) {
    // [len][target:byte][PRINTRADIO: text_idx][arg1][arg2]
    if (n < 2) return;
    size_t pos = 0;
    pos++; // declared length byte
    uint8_t target = p[pos++];

    std::string textIdx;
    if (target == 5 /* TEXT_PRINTRADIO */) {
        textIdx = ReadCString(p, n, pos);
    }

    std::string arg1 = ReadCString(p, n, pos);
    bool needArg2 = (arg1.rfind("%s", 0) == 0) ||
                    ((!arg1.empty() && arg1[0] == '#') &&
                     arg1.find(' ') == std::string::npos &&
                     target != 4 /* PRINTCENTER */ && target != 2 /* PRINTCONSOLE */);
    std::string arg2;
    if (needArg2) arg2 = ReadCString(p, n, pos);
    if (!arg2.empty()) arg1 = textIdx + "|" + arg1 + "|" + arg2;

    Add(TextMsgTypeName(target), arg1);
}

void TextMessageCollector::OnUserMessage(const std::string& name, const uint8_t* payload, size_t len) {
    if (name == "SayText") {
        ParseSayText(payload, len);
    } else if (name == "TextMsg") {
        ParseTextMsg(payload, len);
    }
    // UDS's "HLTV" user message only carries 2 bytes (dest, flags) and
    // produces no text; everything else is skipped by length.
}

bool TextMessageCollector::Save(const std::filesystem::path& path, std::string& error) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open " + path.u8string();
        return false;
    }
    for (const std::string& line : m_lines) {
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.write("\r\n", 2);
    }
    if (!out) {
        error = "failed to write " + path.u8string();
        return false;
    }
    return true;
}

TextMessageCollector ExtractTextMessages(const DemoFile& demo) {
    TextMessageCollector collector;

    const auto& buf = demo.Bytes();
    const int32_t netProtocol = demo.NetProtocol();
    NetMsgState netState;
    NetMsgCallbacks cb;

    cb.onUserInfo = [&](uint8_t slot, int32_t userId, const std::string& info) {
        collector.OnUserInfo(slot, userId, info);
    };
    cb.onText = [&](const std::string& type, const std::string& text) {
        collector.Add(type, text);
    };
    cb.onUserMessage = [&](const std::string& name, const uint8_t* payload, size_t len) {
        collector.OnUserMessage(name, payload, len);
    };

    demo.ForEachFrame([&](const RawFrame& rf) {
        if (rf.type != FrameType::NetMsg && rf.type != FrameType::NetMsgStart) return;
        constexpr size_t kFixedBlock = 468;
        if (rf.payloadSize < kFixedBlock) return;

        collector.SetFrame(rf.frameIndex, rf.time);
        ParseNetMsgFrame(buf.data() + rf.payloadOffset + kFixedBlock,
                         rf.payloadSize - kFixedBlock, netProtocol, netState, cb);
    });

    return collector;
}

} // namespace vr
