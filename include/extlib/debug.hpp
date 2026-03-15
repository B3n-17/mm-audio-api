#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Debug {

constexpr uint16_t HTTP_PORT = 18480;
constexpr size_t MAX_PLAYERS = 8;

struct Event {
    uint64_t id;
    uint64_t tsMs;
    uint32_t tag;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
};

struct PlayerState {
    int32_t seqId;
    uint32_t enabled;
    uint32_t waitingForFonts;
    uint32_t setupCmdNum;
    uint32_t activeChannelCount;
    uint32_t activeChannelMask;
    int32_t notesActive;
    int32_t notesDecaying;
    int32_t notesReleasing;
    int32_t panAverage;
    uint32_t panLeftCount;
    uint32_t panCenterCount;
    uint32_t panRightCount;
    std::array<int32_t, 8> seqIo;
    std::array<uint32_t, 4> chEnabled;
    std::array<int32_t, 4> chPan;
    std::array<int32_t, 4> chVolMilli;
    std::array<uint32_t, 4> layerEnabledMask;
    std::array<int32_t, 16> layerPan;
    std::array<int32_t, 16> layerNotePan;
    std::array<int32_t, 16> layerNoteAttrPan;
    std::array<int32_t, 16> layerNoteTargetL;
    std::array<int32_t, 16> layerNoteTargetR;
    std::array<int32_t, 16> layerNoteCurL;
    std::array<int32_t, 16> layerNoteCurR;
    std::array<int32_t, 16> layerSampleMediumCodec;
};

struct Snapshot {
    uint64_t tsMs;
    uint32_t initPhase;
    int32_t mainSeqId;
    int32_t subSeqId;
    int32_t fanfareSeqId;
    int32_t ambienceSeqId;
    std::array<PlayerState, MAX_PLAYERS> players;
};

void setEnabled(bool enabled);
bool isEnabled();

void pushEvent(uint32_t tag, int32_t a, int32_t b, int32_t c, int32_t d);
void setSnapshot(uint32_t initPhase, int32_t mainSeqId, int32_t subSeqId, int32_t fanfareSeqId, int32_t ambienceSeqId);
void setSeqPlayerPacked(uint32_t playerIndex, int32_t seqId, const int32_t* packed);

Snapshot getSnapshot();
std::vector<Event> getEventsSince(uint64_t sinceId, size_t limit);
uint64_t droppedEventCount();
uint64_t lastEventId();

void startHttpServer();
std::string buildAudioDebugHtml();

} // namespace Debug
