#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vr {

// Mirrors ReHLDS's delta.h DT_* field type flags (engine/delta.h) — these
// describe how a single delta-compressed field is bit-packed on the wire.
// See rehlds/engine/delta.cpp DELTA_ParseDelta for the authoritative
// decode algorithm this is modeled on.
enum DeltaFieldType : int32_t {
    DT_BYTE          = 1 << 0,
    DT_SHORT         = 1 << 1,
    DT_FLOAT         = 1 << 2,
    DT_INTEGER       = 1 << 3,
    DT_ANGLE         = 1 << 4,
    DT_TIMEWINDOW_8  = 1 << 5,
    DT_TIMEWINDOW_BIG= 1 << 6,
    DT_STRING        = 1 << 7,
    DT_SIGNED        = 1 << 31,
};

struct DeltaField {
    int32_t fieldType = 0;
    std::string fieldName;
    int32_t fieldOffset = 0;
    int32_t fieldSize = 0;
    int32_t significantBits = 0;
    float premultiply = 1.0f;
    float postmultiply = 1.0f;
};

// A named delta structure ("clientdata_t", "entity_state_t", ...) as
// declared by a svc_deltadescription message earlier in the demo. We don't
// need the decoded field VALUES for voice extraction — only enough to
// correctly compute how many bits a given delta-encoded record consumes
// so we can skip over it without losing sync.
struct DeltaStruct {
    std::vector<DeltaField> fields;
};

// Tracks delta structure schemas + dynamic user message registrations
// (svc_newusermsg) encountered while walking a demo, since later messages
// reference them by name/id. One instance should be reused across all
// NetMsg frames of a single demo (schemas are registered once, near the
// start, and apply for the rest of the file).
struct NetMsgState {
    std::unordered_map<std::string, DeltaStruct> deltaStructs;

    struct UserMsgInfo {
        int8_t length = -1; // -1 == variable (length-prefixed on wire)
        std::string name;
    };
    std::unordered_map<uint8_t, UserMsgInfo> userMessages;

    // From svc_serverinfo: needed to classify entities as player slots
    // (entity_state_player_t) vs regular/custom in packetentities,
    // deltapacketentities and spawnbaseline, exactly like the real engine.
    int32_t maxClients = 32;
    std::string gameDir;
};

} // namespace vr
