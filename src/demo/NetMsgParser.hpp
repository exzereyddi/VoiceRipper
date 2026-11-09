#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "BitReader.hpp"
#include "NetMsgTypes.hpp"

namespace vr {

struct NetMsgCallbacks {
    std::function<void(const std::string& codecName)> onVoiceInit;
    std::function<void(uint8_t slot, const uint8_t* data, size_t len)> onVoiceData;
    std::function<void(uint8_t slot, int32_t userId, const std::string& infoString)> onUserInfo;
};

// Walks one NetMsg frame's raw engine-message bytes (the "msg" bytes that
// follow the fixed 468-byte usercmd/clientdata block in a GoldSrc demo
// NetMsg frame), dispatching svc_* messages one after another exactly like
// the real engine's message loop does, so byte/bit alignment is never
// lost partway through. svc_deltadescription messages update `state` with
// field schemas so later delta-compressed messages (clientdata, entities,
// events) can be skipped correctly; svc_voiceinit/svc_voicedata/
// svc_updateuserinfo invoke the matching callback with real data.
//
// If an unrecognized message id or a bit-packed message referencing an
// unregistered delta schema is hit, parsing of the REST of this frame is
// aborted (same as the real engine would disconnect on desync) — but
// since outer demo frame boundaries are already known from the .dem
// directory/frame-length fields, this only drops the remainder of one
// frame's messages, not the rest of the file. When `outDesyncCount` is
// non-null, it's incremented once per such abort so callers can report a
// "quality" indicator for the demo without needing to enable full
// VR_TRACE logging (see ExtractResult::desyncCount).
void ParseNetMsgFrame(const uint8_t* data, size_t size, int32_t netProtocol,
                      NetMsgState& state, const NetMsgCallbacks& cb,
                      size_t* outDesyncCount = nullptr);

} // namespace vr
