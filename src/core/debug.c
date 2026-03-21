#include <core/debug.h>

#include <core/init.h>
#include <core/sequence_functions.h>
#include <recomp/modding.h>
#include <recomp/recompconfig.h>

u8 gAudioApiDebugHttpEnabled = 0;
u8 gAudioApiDebugVerbose = 0;
static s8 sPrevSeqIo[SEQ_PLAYER_MAX][8];
static u8 sPrevSeqIoInitialized = 0;

static s32 AudioApi_DebugNormalizePan(s32 pan) {
    if (pan < 0) {
        return pan;
    }

    if (pan > 127) {
        return pan >> 7;
    }

    return pan;
}

RECOMP_IMPORT(".", bool AudioApiNative_DebugSetEnabled(u32 enabled));
RECOMP_IMPORT(".", bool AudioApiNative_DebugSetKhzMode(u32 enabled));
RECOMP_IMPORT(".", bool AudioApiNative_DebugSetSnapshot(u32 initPhase, s32 mainSeqId, s32 subSeqId,
                                                         s32* snapshotTail));
RECOMP_IMPORT(".", bool AudioApiNative_DebugSetSeqPlayer(u32 playerIndex, s32 seqId, s32* playerState));
RECOMP_IMPORT(".", bool AudioApiNative_DebugEvent(u32 tag, s32 a, s32 b, s32* extra));

// Returns mix override for the given player/channel.
// out[0] = pan (0-127, or -1 = no override), out[1] = vol_milli (0-1000, or -1 = no override),
// out[2] = muted (0 or 1). Returns false when the debug server is not enabled.
RECOMP_IMPORT(".", bool AudioApiNative_DebugGetMixOverride(u32 playerIndex, u32 channelIndex, s32* out));

void AudioApi_DebugInitFromConfig(void) {
    gAudioApiDebugHttpEnabled = recomp_get_config_u32("audio_debug_http") != 0;
    gAudioApiDebugVerbose = recomp_get_config_u32("audio_debug_verbose") != 0;
    AudioApiNative_DebugSetEnabled(gAudioApiDebugHttpEnabled);
    AudioApiNative_DebugSetKhzMode((u32)gAudioApi48kHzEnabled);
}

void AudioApi_DebugSyncSnapshot(void) {
    u32 i;
    u32 j;
    u32 pi;
    u32 ci;
    s32 mixOut[4];
    s32 snapshotTail[2];
    s32 playerState[228];
    SequencePlayer* seqPlayer;
    SequenceChannel* channel;
    SequenceLayer* layer;
    Note* note;
    s32 noteIndex;
    s32 noteActiveByPlayer[SEQ_PLAYER_MAX];
    s32 noteDecayingByPlayer[SEQ_PLAYER_MAX];
    s32 noteReleasingByPlayer[SEQ_PLAYER_MAX];
    s32 panTotal;
    s32 panCount;
    s32 panNorm;

    for (i = 0; i < SEQ_PLAYER_MAX; i++) {
        noteActiveByPlayer[i] = 0;
        noteDecayingByPlayer[i] = 0;
        noteReleasingByPlayer[i] = 0;
    }

    for (noteIndex = 0; noteIndex < gAudioCtx.numNotes; noteIndex++) {
        s8 ownerPlayerIndex = -1;

        note = &gAudioCtx.notes[noteIndex];

        if (note->playbackState.parentLayer != NULL && note->playbackState.parentLayer != NO_LAYER &&
            note->playbackState.parentLayer->channel != NULL &&
            note->playbackState.parentLayer->channel->seqPlayer != NULL) {
            ownerPlayerIndex = note->playbackState.parentLayer->channel->seqPlayer->playerIndex;
        }

        if (ownerPlayerIndex < 0 || ownerPlayerIndex >= SEQ_PLAYER_MAX) {
            continue;
        }

        if (note->sampleState.bitField0.enabled) {
            noteActiveByPlayer[ownerPlayerIndex]++;
        }

        if (note->playbackState.adsr.action.s.decay) {
            noteDecayingByPlayer[ownerPlayerIndex]++;
        }

        if (note->playbackState.adsr.action.s.release) {
            noteReleasingByPlayer[ownerPlayerIndex]++;
        }
    }

    if (!sPrevSeqIoInitialized) {
        for (i = 0; i < SEQ_PLAYER_MAX; i++) {
            for (j = 0; j < 8; j++) {
                sPrevSeqIo[i][j] = -128;
            }
        }
        sPrevSeqIoInitialized = 1;
    }

    if (!gAudioApiDebugHttpEnabled) {
        return;
    }

    snapshotTail[0] = gExtActiveSeqs[SEQ_PLAYER_FANFARE].seqId;
    snapshotTail[1] = gExtActiveSeqs[SEQ_PLAYER_AMBIENCE].seqId;

    AudioApiNative_DebugSetSnapshot(gAudioApiInitPhase,
                                    gExtActiveSeqs[SEQ_PLAYER_BGM_MAIN].seqId,
                                    gExtActiveSeqs[SEQ_PLAYER_BGM_SUB].seqId,
                                    snapshotTail);

    for (i = 0; i < SEQ_PLAYER_MAX; i++) {
        seqPlayer = &gAudioCtx.seqPlayers[i];

        playerState[0] = seqPlayer->enabled;
        playerState[1] = gActiveSeqs[i].isWaitingForFonts;
        playerState[2] = gExtActiveSeqs[i].setupCmdNum;
        playerState[3] = 0;
        playerState[4] = 0;
        playerState[5] = noteActiveByPlayer[i];
        playerState[6] = noteDecayingByPlayer[i];
        playerState[7] = noteReleasingByPlayer[i];
        playerState[8] = -1;
        playerState[9] = 0;
        playerState[10] = 0;
        playerState[11] = 0;

        panTotal = 0;
        panCount = 0;

        for (j = 0; j < SEQ_NUM_CHANNELS; j++) {
            channel = seqPlayer->channels[j];
            if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel) || !channel->enabled) {
                continue;
            }

            playerState[3]++;
            playerState[4] |= (1 << j);

            panNorm = AudioApi_DebugNormalizePan(channel->pan);
            panTotal += panNorm;
            panCount++;

            if (panNorm < 43) {
                playerState[9]++;
            } else if (panNorm > 85) {
                playerState[11]++;
            } else {
                playerState[10]++;
            }
        }

        if (panCount > 0) {
            playerState[8] = panTotal / panCount;
        }

        for (j = 0; j < 8; j++) {
            playerState[12 + j] = seqPlayer->seqScriptIO[j];

            if (sPrevSeqIo[i][j] != seqPlayer->seqScriptIO[j]) {
                AudioApi_DebugEvent(AUDIOAPI_DEBUG_EVENT_SEQ_IO_CHANGE, i, j,
                                    sPrevSeqIo[i][j], seqPlayer->seqScriptIO[j]);
                sPrevSeqIo[i][j] = seqPlayer->seqScriptIO[j];
            }
        }

        // ch fields: [20..35] chEnabled, [36..51] chPan, [52..67] chVolMilli, [68..83] chReverbVol
        for (j = 0; j < SEQ_NUM_CHANNELS; j++) {
            channel = seqPlayer->channels[j];
            if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
                playerState[20 + j] = 0;
                playerState[36 + j] = -1;
                playerState[52 + j] = 0;
                playerState[68 + j] = 0;
                continue;
            }

            playerState[20 + j] = channel->enabled;
            playerState[36 + j] = AudioApi_DebugNormalizePan(channel->pan);
            playerState[52 + j] = channel->volume * 1000.0f;
            playerState[68 + j] = channel->targetReverbVol;
        }

        // layer fields: [84..99] layerEnabledMask, [100..115] layerPan,
        // [116..131] layerNotePan, [132..147] layerNoteAttrPan,
        // [148..163] layerNoteTargetL, [164..179] layerNoteTargetR,
        // [180..195] layerNoteCurL, [196..211] layerNoteCurR,
        // [212..227] layerSampleMediumCodec
        for (j = 0; j < SEQ_NUM_CHANNELS; j++) {
            playerState[84 + j] = 0;
        }

        for (j = 0; j < 16; j++) {
            playerState[100 + j] = -1;
            playerState[116 + j] = -1;
            playerState[132 + j] = -1;
            playerState[148 + j] = -1;
            playerState[164 + j] = -1;
            playerState[180 + j] = -1;
            playerState[196 + j] = -1;
            playerState[212 + j] = -1;
        }

        // Layer arrays have 16 entries — one slot per channel (first active layer wins).
        for (j = 0; j < SEQ_NUM_CHANNELS; j++) {
            u32 k;

            channel = seqPlayer->channels[j];
            if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel) || !channel->enabled) {
                continue;
            }

            for (k = 0; k < 4; k++) {
                layer = channel->layers[k];
                if (layer == NULL || layer == NO_LAYER) {
                    continue;
                }

                if (layer->enabled) {
                    playerState[84 + j] |= (1 << k);
                }

                // Only write layer detail for the first active layer on this channel.
                if (playerState[100 + j] == -1) {
                    playerState[100 + j] = layer->pan;
                    playerState[116 + j] = layer->notePan;

                    note = layer->note;
                    if (note != NULL) {
                        playerState[132 + j] = note->playbackState.attributes.pan;
                        playerState[148 + j] = note->sampleState.targetVolLeft;
                        playerState[164 + j] = note->sampleState.targetVolRight;
                        playerState[180 + j] = note->synthesisState.curVolLeft;
                        playerState[196 + j] = note->synthesisState.curVolRight;

                        if (!note->sampleState.bitField1.isSyntheticWave &&
                            note->sampleState.tunedSample != NULL &&
                            note->sampleState.tunedSample->sample != NULL) {
                            Sample* sample = note->sampleState.tunedSample->sample;
                            playerState[212 + j] = ((sample->medium & 0xFF) << 8) | (sample->codec & 0xFF);
                        }
                    }
                }
            }
        }

        AudioApiNative_DebugSetSeqPlayer(i, gExtActiveSeqs[i].seqId, playerState);
    }

    // Apply DJ mixer overrides: write pan/volume back to live channel state so the
    // audio engine picks them up on the next synthesis pass.
    if (gAudioApiDebugHttpEnabled) {
        for (pi = 0; pi < (u32)SEQ_PLAYER_MAX; pi++) {
            seqPlayer = &gAudioCtx.seqPlayers[pi];
            for (ci = 0; ci < (u32)SEQ_NUM_CHANNELS; ci++) {
                channel = seqPlayer->channels[ci];
                if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
                    continue;
                }

                if (!AudioApiNative_DebugGetMixOverride(pi, ci, mixOut)) {
                    goto done_mix_overrides; // debug disabled
                }

                // mixOut[0] = pan (-1 = passthrough), mixOut[1] = vol_milli (-1 = passthrough),
                // mixOut[2] = muted, mixOut[3] = reverb (0-127, -1 = passthrough)
                if (mixOut[2]) {
                    // Muted: silence the channel volume
                    channel->volume = 0.0f;
                    channel->changes.s.volume = true;
                } else {
                    if (mixOut[1] >= 0) {
                        channel->volume = (f32)mixOut[1] / 1000.0f;
                        channel->changes.s.volume = true;
                    }
                    if (mixOut[0] >= 0) {
                        channel->newPan = (u8)mixOut[0];
                        channel->changes.s.pan = true;
                    }
                }
                if (mixOut[3] >= 0) {
                    channel->targetReverbVol = (u8)mixOut[3];
                }
            }
        }
        done_mix_overrides:;
    }
}

void AudioApi_DebugEvent(u32 tag, s32 a, s32 b, s32 c, s32 d) {
    s32 extra[2];

    if (!gAudioApiDebugHttpEnabled) {
        return;
    }

    if (!gAudioApiDebugVerbose &&
        (tag == AUDIOAPI_DEBUG_EVENT_PROCESS_SEQ_CMD || tag == AUDIOAPI_DEBUG_EVENT_QUEUE_EXT_CMD)) {
        return;
    }

    extra[0] = c;
    extra[1] = d;
    AudioApiNative_DebugEvent(tag, a, b, extra);
}
