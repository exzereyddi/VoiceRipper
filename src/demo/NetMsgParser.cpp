#include "NetMsgParser.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace vr {

namespace {

bool g_trace = []() {
    const char* e = std::getenv("VR_TRACE");
    return e && e[0] == '1';
}();

// Extremely verbose per-message trace (every single svc_* message, every
// entity delta record) — set VR_TRACE_VERBOSE=1 in addition to VR_TRACE=1
// only when debugging a specific parsing issue; this is far too noisy for
// normal diagnostic use.
bool g_traceVerbose = []() {
    const char* e = std::getenv("VR_TRACE_VERBOSE");
    return e && e[0] == '1';
}();

// File-backed log so trace output survives a process being killed mid-hang
// (stderr through a pty can be lost/buffered right up until the process
// actually exits, which is useless when debugging an infinite loop).
std::ofstream& NetMsgLog() {
    thread_local std::ofstream log;
    thread_local bool initDone = false;
    if (!initDone) {
        initDone = true;
        if (g_trace || g_traceVerbose) {
            log.open("voiceripper_netmsg.log", std::ios::app);
        }
    }
    return log;
}
#define VR_LOG(x) do { if (NetMsgLog().is_open()) { NetMsgLog() << x << std::endl; } } while (0)

// --- Generic delta-compression decode, modeled on ReHLDS's DELTA_ParseDelta
// (engine/delta.cpp). We only need to consume the exact number of bits a
// delta record occupies (to stay in sync with the stream); the decoded
// field VALUES themselves aren't needed for voice extraction, so most are
// discarded immediately after reading.
void SkipDeltaRecord(BitReader& r, const DeltaStruct& st) {
    int nbytes = static_cast<int>(r.ReadBits(3));
    uint64_t mask = 0;
    for (int i = 0; i < nbytes && i < 8; i++) {
        mask |= static_cast<uint64_t>(r.ReadBits(8)) << (i * 8);
    }
    if (g_traceVerbose) {
        NetMsgLog() << "      delta nbytes=" << nbytes << " mask=0x" << std::hex << mask << std::dec
                   << " fieldCount=" << st.fields.size() << "\n";
    }

    for (size_t i = 0; i < st.fields.size() && i < 56; i++) {
        if (!((mask >> i) & 1ULL)) continue; // field not sent, nothing to read

        const DeltaField& f = st.fields[i];
        int32_t type = f.fieldType & ~DT_SIGNED;
        bool signedField = (f.fieldType & DT_SIGNED) != 0;

        switch (type) {
            case DT_BYTE:
            case DT_SHORT:
            case DT_INTEGER:
            case DT_FLOAT:
                if (signedField) r.ReadSBits(f.significantBits);
                else             r.ReadBits(f.significantBits);
                break;
            case DT_ANGLE:
                r.ReadBits(f.significantBits);
                break;
            case DT_TIMEWINDOW_8:
                r.ReadSBits(8);
                break;
            case DT_TIMEWINDOW_BIG:
                r.ReadSBits(f.significantBits);
                break;
            case DT_STRING:
                r.ReadCString();
                break;
            default:
                // Unknown field type encoded in the schema itself -- bail
                // out of this record; caller will detect via Overflowed()
                // or just produce garbage for the rest of this message,
                // which aborts the current frame (see ParseNetMsgFrame).
                return;
        }
    }
}

// Decodes one svc_deltadescription field entry, itself delta-encoded
// against ReHLDS's fixed g_MetaDescription schema (delta.cpp). Unlike
// user-data deltas, meta-fields are always present in a real description
// write (DELTA_WriteDelta against an all-zero baseline marks everything
// non-default as "changed"), but we still honor the presence bitmask
// exactly like any other delta record for correctness.
DeltaField ReadMetaDeltaField(BitReader& r) {
    DeltaField f;
    // Meta schema field order (delta.cpp g_MetaDescription): fieldType(32,
    // unsigned int), fieldName(string), fieldOffset(16, unsigned int),
    // fieldSize(8, unsigned int), significantBits(8, unsigned int),
    // premultiply(32, unsigned float /4000), postmultiply(32, unsigned float /4000).
    int nbytes = static_cast<int>(r.ReadBits(3));
    uint64_t mask = 0;
    for (int i = 0; i < nbytes && i < 8; i++) {
        mask |= static_cast<uint64_t>(r.ReadBits(8)) << (i * 8);
    }

    if (mask & (1ULL << 0)) f.fieldType = static_cast<int32_t>(r.ReadBits(32));
    if (mask & (1ULL << 1)) f.fieldName = r.ReadCString();
    if (mask & (1ULL << 2)) f.fieldOffset = static_cast<int32_t>(r.ReadBits(16));
    if (mask & (1ULL << 3)) f.fieldSize = static_cast<int32_t>(r.ReadBits(8));
    if (mask & (1ULL << 4)) f.significantBits = static_cast<int32_t>(r.ReadBits(8));
    if (mask & (1ULL << 5)) f.premultiply = static_cast<float>(r.ReadBits(32)) / 4000.0f;
    if (mask & (1ULL << 6)) f.postmultiply = static_cast<float>(r.ReadBits(32)) / 4000.0f;
    return f;
}

void HandleDeltaDescription(BitReader& r, NetMsgState& state) {
    std::string name = r.ReadCString();
    uint32_t nEntries = r.ReadBits(16);

    DeltaStruct st;
    st.fields.reserve(nEntries);
    for (uint32_t i = 0; i < nEntries && !r.Overflowed(); i++) {
        st.fields.push_back(ReadMetaDeltaField(r));
    }
    if (g_trace) {
        NetMsgLog() << "[deltadesc] " << name << " nEntries=" << nEntries
                  << " parsedFields=" << st.fields.size()
                  << " overflow=" << r.Overflowed() << "\n";
        for (size_t i = 0; i < st.fields.size() && i < 5; i++) {
            const auto& f = st.fields[i];
            NetMsgLog() << "    [" << i << "] name=" << f.fieldName
                      << " type=" << (f.fieldType & ~DT_SIGNED)
                      << " signed=" << ((f.fieldType & DT_SIGNED) != 0)
                      << " bits=" << f.significantBits
                      << " premul=" << f.premultiply
                      << " postmul=" << f.postmultiply << "\n";
        }
    }
    state.deltaStructs[name] = std::move(st);
}

const DeltaStruct* FindStruct(NetMsgState& state, const char* name) {
    auto it = state.deltaStructs.find(name);
    return it != state.deltaStructs.end() ? &it->second : nullptr;
}

// MSG_ReadBitCoord (rehlds/engine/common.cpp): [has_int:1][has_fract:1]
// then if either set: [sign:1][int:12 if has_int][fract:3 if has_fract].
void SkipBitCoord(BitReader& r) {
    int hasInt = r.ReadOneBit();
    int hasFract = r.ReadOneBit();
    if (hasInt || hasFract) {
        r.ReadOneBit(); // sign
        if (hasInt) r.ReadBits(12);
        if (hasFract) r.ReadBits(3);
    }
}

void SkipBitVec3Coord(BitReader& r) {
    int hasX = r.ReadOneBit();
    int hasY = r.ReadOneBit();
    int hasZ = r.ReadOneBit();
    if (hasX) SkipBitCoord(r);
    if (hasY) SkipBitCoord(r);
    if (hasZ) SkipBitCoord(r);
}

// svc_tempentity (23) sub-type byte-skip table.
bool SkipTempEntity(BitReader& r) {
    uint8_t type = r.ReadByteAligned();
    auto seekBytes = [&](int n) { r.SkipBits(static_cast<size_t>(n) * 8); };

    switch (type) {
        case 16: case 26: break; // obsolete, 0 bytes
        case 2: case 4: case 9: case 10: case 11: seekBytes(6); break;
        case 0: seekBytes(24); break;
        case 1: seekBytes(20); break;
        case 3: seekBytes(11); break;
        case 5: seekBytes(10); break;
        case 6: seekBytes(12); break;
        case 7: seekBytes(17); break;
        case 8: seekBytes(16); break;
        case 12: seekBytes(8); break;
        case 13: {
            seekBytes(8);
            uint16_t entIdx = r.ReadU16Aligned();
            if (entIdx != 0) seekBytes(2);
            break;
        }
        case 14: seekBytes(9); break;
        case 15: seekBytes(19); break;
        case 17: seekBytes(10); break;
        case 18: seekBytes(16); break;
        case 19: case 20: case 21: seekBytes(24); break;
        case 22: seekBytes(10); break;
        case 23: seekBytes(11); break;
        case 24: seekBytes(16); break;
        case 25: seekBytes(19); break;
        case 27: seekBytes(12); break;
        case 28: seekBytes(16); break;
        case 29: {
            seekBytes(5);
            uint8_t effect = r.ReadByteAligned();
            seekBytes(14);
            if (effect == 2) seekBytes(2);
            r.ReadCString();
            break;
        }
        case 30: case 31: seekBytes(17); break;
        case 99: seekBytes(2); break;
        case 100: seekBytes(10); break;
        case 101: seekBytes(14); break;
        case 102: seekBytes(12); break;
        case 103: seekBytes(14); break;
        case 104: seekBytes(9); break;
        case 105: seekBytes(5); break;
        case 106: seekBytes(17); break;
        case 107: seekBytes(13); break;
        case 108: seekBytes(24); break;
        case 109: seekBytes(9); break;
        case 110: seekBytes(17); break;
        case 111: seekBytes(7); break;
        case 112: seekBytes(10); break;
        case 113: case 114: seekBytes(19); break;
        case 115: seekBytes(12); break;
        case 116: case 117: seekBytes(7); break;
        case 118: seekBytes(9); break;
        case 119: seekBytes(16); break;
        case 120: seekBytes(18); break;
        case 121: seekBytes(5); break;
        case 122: seekBytes(10); break;
        case 123: seekBytes(2 * 5 + 3); break; // 5 shorts + 3 bytes
        case 124: seekBytes(7); break;
        case 125: seekBytes(1); break;
        case 126: seekBytes(18); break;
        case 127: seekBytes(15); break;
        default:
            return false; // unknown tempentity type, can't safely continue
    }
    return true;
}

// Entity baseline / packet-entities share the same per-entity delta
// decode shape; `isDeltaVariant` selects the svc_deltapacketentities (41)
// field order, otherwise svc_packetentities (40) order is used. Mirrors
// UnrealDemoScanner.cs MessagePacketEntities / MessageDeltaPacketEntities
// exactly (verified against decompiled source).
bool SkipEntityDeltaLoop(BitReader& r, NetMsgState& state, bool isDeltaVariant, int32_t netProtocol) {
    uint32_t entityNumber = 0;

    while (true) {
        if (r.Overflowed()) return false;

        size_t savedPos = r.BitPos();
        uint16_t footer = r.ReadU16Aligned();
        if (footer == 0) {
            return true; // end-of-list sentinel (16-bit zero), same for both variants
        }
        r.SeekBits(savedPos);

        bool removeEntity = false;
        if (isDeltaVariant) {
            removeEntity = r.ReadOneBit() != 0;
        }

        bool entityNumberIncrement = !isDeltaVariant && r.ReadOneBit() != 0;
        if (isDeltaVariant || !entityNumberIncrement) {
            bool absolute = r.ReadOneBit() != 0;
            if (absolute) {
                entityNumber = r.ReadBits(11);
            } else {
                entityNumber += r.ReadBits(6);
            }
        } else {
            entityNumber++;
        }

        bool isPlayer = entityNumber > 0 && entityNumber <= static_cast<uint32_t>(state.maxClients);
        if (g_traceVerbose) {
            NetMsgLog() << "    ent#" << entityNumber << " isPlayer=" << isPlayer
                      << " bitAfterNum=" << r.BitPos() << "\n";
        }

        if (!isDeltaVariant) {
            // svc_packetentities (40): tfc-bit, custom, useBaseline(+6 bits
            // baseline index if set), THEN classify and decode.
            if (state.gameDir == "tfc") r.ReadOneBit();
            bool custom = r.ReadOneBit() != 0;
            bool useBaseline = r.ReadOneBit() != 0;
            if (useBaseline) r.SkipBits(6);

            const char* structName = isPlayer ? "entity_state_player_t"
                                    : custom   ? "custom_entity_state_t"
                                               : "entity_state_t";
            const DeltaStruct* st = FindStruct(state, structName);
            if (!st) return false;
            SkipDeltaRecord(r, *st);
        } else {
            // svc_deltapacketentities (41): if removeEntity, nothing more
            // for this entity. Otherwise tfc-bit, custom, (netProto<=43:
            // +1 unknown bit), THEN classify and decode.
            if (!removeEntity) {
                if (state.gameDir == "tfc") r.ReadOneBit();
                bool custom = r.ReadOneBit() != 0;
                if (netProtocol <= 43) r.SkipBits(1);

                const char* structName = isPlayer ? "entity_state_player_t"
                                        : custom   ? "custom_entity_state_t"
                                                   : "entity_state_t";
                const DeltaStruct* st = FindStruct(state, structName);
                if (!st) return false;
                SkipDeltaRecord(r, *st);
            }
        }

        if (r.Overflowed()) return false;
    }
}

// svc_spawnbaseline (22): distinct loop shape from packetentities — each
// entry is `entityIndex:11 bits`, terminated by all-1s (2047), followed by
// `entityType:2 bits` where bit0 set means player-or-regular (by index),
// unset means custom. Mirrors UnrealDemoScanner.cs MessageSpawnBaseline.
bool SkipSpawnBaseline(BitReader& r, NetMsgState& state) {
    while (true) {
        if (r.Overflowed()) return false;

        uint32_t entityIndex = r.ReadBits(11);
        if (entityIndex == 2047) break; // all-1s sentinel

        uint32_t entityType = r.ReadBits(2);
        bool isPlayer = entityIndex > 0 && entityIndex <= static_cast<uint32_t>(state.maxClients);

        const char* structName;
        if (entityType & 1) {
            structName = isPlayer ? "entity_state_player_t" : "entity_state_t";
        } else {
            structName = "custom_entity_state_t";
        }

        const DeltaStruct* st = FindStruct(state, structName);
        if (!st) return false;
        SkipDeltaRecord(r, *st);
        if (r.Overflowed()) return false;
    }

    uint32_t footer = r.ReadBits(5);
    if (footer != 31) return false; // "Bad svc_spawnbaseline footer"

    uint32_t nExtra = r.ReadBits(6);
    const DeltaStruct* st = FindStruct(state, "entity_state_t");
    for (uint32_t i = 0; i < nExtra; i++) {
        if (!st) return false;
        SkipDeltaRecord(r, *st);
        if (r.Overflowed()) return false;
    }
    return true;
}

} // namespace

void ParseNetMsgFrame(const uint8_t* data, size_t size, int32_t netProtocol,
                      NetMsgState& state, const NetMsgCallbacks& cb,
                      size_t* outDesyncCount) {
    BitReader r(data, size);

    while (r.BitPos() + 8 <= r.SizeBits()) {
        size_t msgStartBit = r.BitPos();
        uint8_t id = r.ReadByteAligned();

        bool ok = true;

        if (g_traceVerbose) {
            NetMsgLog() << "  msg id=" << static_cast<int>(id) << " startBit=" << msgStartBit << "\n";
        }

        switch (id) {
            case 0: // svc_bad / unrecognized -> stop this frame
                return;
            case 1: // svc_nop
                break;
            case 2: r.ReadCString(); break;                    // svc_disconnect
            case 3: {                                          // svc_event
                uint32_t nEvents = r.ReadBits(5);
                for (uint32_t i = 0; i < nEvents && ok; i++) {
                    r.ReadBits(10); // index
                    if (r.ReadOneBit()) {
                        r.ReadSBits(11); // packet index
                        if (r.ReadOneBit()) {
                            const DeltaStruct* st = FindStruct(state, "event_t");
                            if (!st) { ok = false; break; }
                            SkipDeltaRecord(r, *st);
                        }
                    }
                    if (r.ReadOneBit()) r.ReadBits(16); // fire time
                }
                r.AlignToByte();
                break;
            }
            case 4: r.SkipBits(32); break;                     // svc_version
            case 5: r.SkipBits(16); break;                     // svc_setview
            case 6: {                                          // svc_sound
                uint32_t flags = r.ReadBits(9);
                if (flags & 1) r.ReadBits(8);   // volume
                if (flags & 2) r.ReadBits(8);   // atten
                r.ReadBits(3);                  // channel
                r.ReadBits(11);                 // edict num
                if (flags & 4) r.ReadBits(16); else r.ReadBits(8); // sound id
                SkipBitVec3Coord(r);
                if (flags & 8) r.ReadBits(8);    // pitch
                r.AlignToByte();
                break;
            }
            case 7: r.SkipBits(32); break;                     // svc_time (float)
            case 8: r.ReadCString(); break;                    // svc_print
            case 9: r.ReadCString(); break;                    // svc_stufftext
            case 10: r.SkipBits(48); break;                    // svc_setangle (3x int16)
            case 11: {                                         // svc_serverinfo
                r.SkipBits(32 + 32 + 32);  // 2x int32 + mapcrc uint32
                r.SkipBits(16 * 8);        // 16 raw bytes
                state.maxClients = r.ReadByteAligned();
                r.SkipBits(8 * 2);         // player id, deathmatch
                state.gameDir = r.ReadCString();
                if (netProtocol > 43) r.ReadCString(); // server name
                r.ReadCString();           // map name
                if (netProtocol == 45) {
                    size_t savedPos = r.BitPos();
                    uint8_t peek = r.ReadByteAligned();
                    r.SeekBits(savedPos);
                    if (peek != 54 /* svc_sendextrainfo */) {
                        r.ReadCString();
                        uint8_t more = r.ReadByteAligned();
                        if (more > 0) r.SkipBits(36 * 8);
                    }
                } else {
                    r.ReadCString();
                    if (netProtocol > 43) {
                        uint8_t more = r.ReadByteAligned();
                        if (more > 0) r.SkipBits(21 * 8);
                    }
                }
                break;
            }
            case 12: r.SkipBits(8); r.ReadCString(); break;    // svc_lightstyle
            case 13: {                                         // svc_updateuserinfo
                uint8_t slot = r.ReadByteAligned();
                int32_t userId = r.ReadI32Aligned();
                std::string info = r.ReadCString();
                if (netProtocol > 43) r.SkipBits(16 * 8);
                if (g_trace) {
                    NetMsgLog() << "  [userinfo] slot=" << static_cast<int>(slot)
                              << " userId=" << userId << " info=" << info << "\n";
                }
                if (cb.onUserInfo) cb.onUserInfo(slot, userId, info);
                break;
            }
            case 14: HandleDeltaDescription(r, state); r.AlignToByte(); break; // svc_deltadescription
            case 15: {                                         // svc_clientdata
                // demoProtocol==3 legacy branch not supported (pre-Steam
                // ancient demos); assume modern demoProtocol==5 layout.
                bool hasDeltaSeq = r.ReadOneBit() != 0;
                if (hasDeltaSeq) r.ReadBits(8);
                const DeltaStruct* cdSt = FindStruct(state, "clientdata_t");
                if (!cdSt) { ok = false; break; }
                SkipDeltaRecord(r, *cdSt);
                // IMPORTANT: must also check r.Overflowed() here, not just
                // ok/ReadOneBit(). BitReader::ReadOneBit() returns 1 (not
                // 0) once the stream is exhausted (see BitReader.hpp) so
                // that single-bit reads elsewhere fail safe rather than
                // silently returning a wrong "0" -- but that same design
                // means "while (r.ReadOneBit())" on its own NEVER
                // terminates once overflowed: it keeps reading a
                // weapon_data_t delta record forever (each one technically
                // "making progress" in bit position terms, but position is
                // pinned at m_sizeBits with Overflowed() latched true), a
                // real CPU-pegging infinite loop on any desynced/garbage
                // clientdata message. This was the actual cause of
                // VoiceRipper hanging indefinitely on certain demos.
                while (ok && !r.Overflowed() && r.ReadOneBit()) {
                    r.ReadBits(netProtocol < 47 ? 5 : 6); // weapon index
                    const DeltaStruct* wdSt = FindStruct(state, "weapon_data_t");
                    if (!wdSt) { ok = false; break; }
                    SkipDeltaRecord(r, *wdSt);
                }
                r.AlignToByte();
                break;
            }
            case 16: r.SkipBits(16); break;                    // svc_stopsound
            case 17: {                                         // svc_pings
                while (r.ReadOneBit()) {
                    r.ReadBits(5);
                    r.ReadBits(12);
                    r.ReadBits(7);
                }
                r.AlignToByte();
                break;
            }
            case 18: r.SkipBits(11 * 8); break;                // svc_particle
            case 19: break;                                    // svc_damage
            case 20: {                                         // svc_spawnstatic
                r.SkipBits(16);        // model
                r.SkipBits(16 * 8);    // 16 bytes
                uint8_t renderMode = r.ReadByteAligned();
                if (renderMode != 0) r.SkipBits(5 * 8);
                break;
            }
            case 21: {                                         // svc_event_reliable
                r.ReadBits(10);
                const DeltaStruct* st = FindStruct(state, "event_t");
                if (!st) { ok = false; break; }
                SkipDeltaRecord(r, *st);
                if (r.ReadOneBit()) r.ReadBits(16);
                r.AlignToByte();
                break;
            }
            case 22: {                                         // svc_spawnbaseline
                ok = SkipSpawnBaseline(r, state);
                r.AlignToByte();
                break;
            }
            case 23: ok = SkipTempEntity(r); break;            // svc_tempentity
            case 24: r.SkipBits(8); break;                     // svc_setpause
            case 25: r.SkipBits(8); break;                     // svc_signonnum
            case 26: {                                         // svc_centerprint
                std::string s = r.ReadCString();
                if (s.rfind("%s", 0) == 0) r.ReadCString();
                break;
            }
            case 27: break;                                    // svc_killedmonster
            case 28: break;                                    // svc_foundsecret
            case 29: r.SkipBits(14 * 8); break;                // svc_spawnstaticsound
            case 30: break;                                    // svc_intermission
            case 31: r.ReadCString(); break;                   // svc_finale
            case 32: r.SkipBits(2 * 8); break;                 // svc_cdtrack
            case 33: r.ReadCString(); break;                   // svc_restore
            case 34: r.ReadCString(); break;                   // svc_cutscene
            case 35: r.SkipBits(2 * 8); break;                 // svc_weaponanim
            case 36: r.SkipBits(8); r.ReadCString(); break;    // svc_decalname
            case 37: r.SkipBits(16); break;                    // svc_roomtype
            case 38: r.SkipBits(16); break;                    // svc_addangle
            case 39: {                                         // svc_newusermsg
                uint8_t msgId = r.ReadByteAligned();
                int8_t length = static_cast<int8_t>(r.ReadByteAligned());
                // Name is a fixed 16-byte slot on the wire regardless of
                // the actual string length (ReadString(16) in the
                // reference parser: read cstring, then seek to a total of
                // 16 bytes consumed for this field).
                size_t nameStartBit = r.BitPos();
                std::string name = r.ReadCString(16);
                r.SeekBits(nameStartBit + 16 * 8);
                NetMsgState::UserMsgInfo info;
                info.length = length;
                info.name = name;
                state.userMessages[msgId] = info;
                if (g_trace) {
                    NetMsgLog() << "  [newusermsg] id=" << static_cast<int>(msgId)
                              << " length=" << static_cast<int>(length)
                              << " name=" << name << "\n";
                }
                break;
            }
            case 40: r.SkipBits(16); ok = SkipEntityDeltaLoop(r, state, false, netProtocol); r.AlignToByte(); break; // svc_packetentities
            case 41: {                                         // svc_deltapacketentities
                r.SkipBits(16); // num_ents hint
                r.SkipBits(8);  // seq_number
                ok = SkipEntityDeltaLoop(r, state, true, netProtocol);
                r.AlignToByte();
                break;
            }
            case 42: break;                                    // svc_choke
            case 43: {                                         // svc_resourcelist
                uint32_t nEntries = r.ReadBits(12);
                for (uint32_t i = 0; i < nEntries && ok; i++) {
                    r.ReadBits(4);          // type
                    r.ReadCString();        // name
                    r.ReadBits(12);         // index
                    r.ReadBits(24);         // size
                    uint32_t flags = r.ReadBits(3);
                    if (flags & 4) r.SkipBits(16 * 8);
                    if (r.ReadOneBit()) r.SkipBits(32 * 8);
                }
                if (r.ReadOneBit()) {
                    while (r.ReadOneBit()) {
                        int nBits = r.ReadOneBit() ? 5 : 10;
                        r.SkipBits(static_cast<size_t>(nBits));
                    }
                }
                r.AlignToByte();
                break;
            }
            case 44: {                                         // svc_newmovevars
                r.SkipBits(25 * 32); // 25 floats (gravity..skyvec approx)
                r.ReadCString();     // sky name
                break;
            }
            case 45: r.SkipBits(8 * 8); break;                 // svc_resourcerequest
            case 46: {                                         // svc_customization
                r.SkipBits(16);      // 2 bytes
                r.ReadCString();
                r.SkipBits(16);      // uint16
                r.SkipBits(32);      // uint32
                uint8_t flags = r.ReadByteAligned();
                if (flags & 4) r.SkipBits(16 * 8);
                break;
            }
            case 47: r.SkipBits(2 * 8); break;                 // svc_crosshairangle
            case 48: r.SkipBits(4 * 8); break;                 // svc_soundfade
            case 49: r.ReadCString(); break;                   // svc_filetxferfailed
            case 50: {                                         // svc_hltv
                uint8_t sub = r.ReadByteAligned();
                if (sub == 1) r.SkipBits(10 * 8);
                else if (sub == 2) r.ReadCString();
                break;
            }
            case 51: {                                         // svc_director
                uint8_t len = r.ReadByteAligned();
                uint8_t msgType = r.ReadByteAligned();
                if (msgType == 10) {
                    r.ReadCString();
                } else if (msgType == 6) {
                    r.SkipBits(8);
                    r.SkipBits(32);
                    r.SkipBits(5 * 32);
                    r.ReadCString();
                } else if (len > 0) {
                    r.SkipBits(static_cast<size_t>(len - 1) * 8);
                }
                break;
            }
            case 52: {                                         // svc_voiceinit
                std::string codec = r.ReadCString();
                if (netProtocol >= 47 && !codec.empty()) r.ReadByteAligned();
                if (cb.onVoiceInit) cb.onVoiceInit(codec);
                break;
            }
            case 53: {                                         // svc_voicedata
                uint8_t slot = r.ReadByteAligned();
                uint16_t len = r.ReadU16Aligned();
                size_t bytePos = r.BitPos() / 8;
                if (bytePos + len > size) { ok = false; break; }
                if (g_trace) {
                    NetMsgLog() << "  [voicedata] slot=" << static_cast<int>(slot) << " len=" << len << "\n";
                }
                if (cb.onVoiceData) cb.onVoiceData(slot, data + bytePos, len);
                r.SkipBits(static_cast<size_t>(len) * 8);
                break;
            }
            case 54: r.ReadCString(); r.SkipBits(8); break;    // svc_sendextrainfo
            case 55: r.SkipBits(4 * 8); break;                 // svc_timescale
            case 56: r.ReadCString(); break;                   // svc_resourcelocation
            case 57: r.ReadCString(); break;                   // svc_sendcvarvalue
            case 58: r.SkipBits(32); r.ReadCString(); break;   // svc_sendcvarvalue2
            case 59: {                                         // svc_exec
                uint8_t what = r.ReadByteAligned();
                if (what == 1) r.ReadByteAligned();
                break;
            }
            default: {
                if (id >= 64) {
                    auto it = state.userMessages.find(id);
                    if (it != state.userMessages.end() && it->second.length >= 0) {
                        r.SkipBits(static_cast<size_t>(it->second.length) * 8);
                    } else {
                        uint8_t len = r.ReadByteAligned();
                        r.SkipBits(static_cast<size_t>(len) * 8);
                    }
                } else {
                    ok = false; // ids 60-63 unregistered
                }
                break;
            }
        }

        if (!ok || r.Overflowed()) {
            if (outDesyncCount) (*outDesyncCount)++;
            if (g_trace) {
                NetMsgLog() << "[netmsg] desync at msg id=" << static_cast<int>(id)
                          << " bitpos=" << r.BitPos() << "/" << r.SizeBits()
                          << " frameSize=" << size << "\n";
            }
            // Desync: stop parsing the rest of THIS frame. Outer demo
            // frame boundaries are already known from the .dem directory,
            // so this only drops remaining messages in one frame.
            return;
        }
        if (r.BitPos() <= msgStartBit) {
            return; // safety: no forward progress, avoid infinite loop
        }
    }
}

} // namespace vr
