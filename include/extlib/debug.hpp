#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
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
    std::array<uint32_t, 16> chEnabled;
    std::array<int32_t, 16> chPan;
    std::array<int32_t, 16> chVolMilli;
    std::array<int32_t, 16> chReverbVol;
    std::array<uint32_t, 16> layerEnabledMask;
    std::array<int32_t, 16> layerPan;
    std::array<int32_t, 16> layerNotePan;
    std::array<int32_t, 16> layerNoteAttrPan;
    std::array<int32_t, 16> layerNoteTargetL;
    std::array<int32_t, 16> layerNoteTargetR;
    std::array<int32_t, 16> layerNoteCurL;
    std::array<int32_t, 16> layerNoteCurR;
    std::array<int32_t, 16> layerSampleMediumCodec;
    std::array<int32_t, 16> chFontId;
    std::array<int32_t, 16> chInstOrWave;
};

struct Snapshot {
    uint64_t tsMs;
    uint32_t initPhase;
    bool khzMode;
    int32_t mainSeqId;
    int32_t subSeqId;
    int32_t fanfareSeqId;
    int32_t ambienceSeqId;
    std::array<PlayerState, MAX_PLAYERS> players;
};

struct StreamState {
    uint32_t resourceId;
    uint32_t trackNo;
    int32_t  pos;           // current samplePosInt (decode offset)
    uint32_t sampleCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    int32_t  loopCount;     // -1 = infinite
    uint64_t tsMs;
};

struct MixOverride {
    bool active      = false;
    int32_t pan      = -1;   // 0–127, -1 = passthrough
    int32_t volMilli = -1;   // 0–1000 (volume * 1000), -1 = passthrough
    int32_t reverb   = -1;   // 0–127 targetReverbVol, -1 = passthrough
    bool muted       = false;
};

void setEnabled(bool enabled);
bool isEnabled();

void pushEvent(uint32_t tag, int32_t a, int32_t b, int32_t c, int32_t d);
void registerTag(uint32_t tag, const char* name);
void setKhzMode(bool enabled);
void setSnapshot(uint32_t initPhase, int32_t mainSeqId, int32_t subSeqId, int32_t fanfareSeqId, int32_t ambienceSeqId);
void setSeqPlayerPacked(uint32_t playerIndex, int32_t seqId, const int32_t* packed);

// DJ mixer overrides — set by the debug UI, consumed by the audio thread.
// setMixerOpen(false) also clears all active overrides.
void setMixerOpen(bool open);
bool isMixerOpen();
void setMixOverride(uint32_t playerIndex, uint32_t channelIndex, int32_t pan, int32_t volMilli, int32_t reverb, bool muted);
void clearMixOverride(uint32_t playerIndex, uint32_t channelIndex);
MixOverride getMixOverride(uint32_t playerIndex, uint32_t channelIndex);

void setStreamState(uint32_t resourceId, uint32_t trackNo, int32_t pos,
                    uint32_t sampleCount, uint32_t loopStart, uint32_t loopEnd, int32_t loopCount);

Snapshot getSnapshot();
std::vector<Event> getEventsSince(uint64_t sinceId, size_t limit);
uint64_t droppedEventCount();
uint64_t lastEventId();
std::string getTagsJson();
std::string getStreamsJson();

void startHttpServer();
std::string buildAudioDebugHtml();

// Sample patch: encoded VADPCM data ready for the game to apply.
struct SamplePatchData {
    int32_t  fontId       = -1;
    int32_t  instId       = -1;   // -1 if not targeting an instrument
    int32_t  drumId       = -1;   // -1 if not targeting a drum
    int32_t  sfxId        = -1;   // -1 if not targeting a sfx
    uint8_t  pitchRegion  = 1;    // 0=low, 1=normal, 2=high (instrument only)
    float    tuning       = 1.0f;
    uint32_t codec        = 0;

    uint32_t numSamples   = 0;
    std::vector<uint8_t>  adpcmData;

    int32_t  bookOrder          = 2;
    int32_t  bookNumPredictors  = 4;
    std::vector<int16_t> bookCoeffs; // length = 8 * order * numPredictors

    uint32_t loopStart    = 0;
    uint32_t loopEnd      = 0;
    int32_t  loopCount    = 0;
    std::array<int16_t, 16> loopPredictorState = {};
};

bool hasSamplePatch();
std::optional<SamplePatchData> peekSamplePatch();  // read without consuming
std::optional<SamplePatchData> takeSamplePatch();  // read and consume

// Called from the native DLL func to push font info for GET /soundfonts.
// json is a pre-formatted {"fonts":[...]} string.
void updateSoundFontInfos(const std::string& json);

} // namespace Debug
