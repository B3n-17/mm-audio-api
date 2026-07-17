#include <core/debug.h>

#include <recomp/modding.h>
#include <recomp/recompconfig.h>
#include <recomp/recomputils.h>
#include <core/init.h>
#include <core/sequence_functions.h>
#include <audio/soundfont.h>
#include <audio_api/soundfont.h>
#include <audio/effects.h>

u8 gAudioApiDebugHttpEnabled = 0;
u8 gAudioApiDebugVerbose = 0;
static s8 sPrevSeqIo[SEQ_PLAYER_MAX][8];
static u8 sPrevSeqIoInitialized = 0;

// ============================================================
// Sample Patch imports
// ============================================================

// Layout of the 88-byte header written by AudioApiNative_DebugQuerySamplePatch.
typedef struct DebugSamplePatchHeader {
    /* 0x00 */ s32 fontId;
    /* 0x04 */ s32 instId;
    /* 0x08 */ s32 drumId;
    /* 0x0C */ s32 sfxId;
    /* 0x10 */ u8  pitchRegion;
    /* 0x11 */ u8  pad[3];
    /* 0x14 */ f32 tuning;
    /* 0x18 */ u32 numSamples;
    /* 0x1C */ u32 sampleDataSize;
    /* 0x20 */ u32 codec;
    /* 0x24 */ s32 bookOrder;
    /* 0x28 */ s32 bookNumPredictors;
    /* 0x2C */ u32 bookCoeffCount;
    /* 0x30 */ u32 loopStart;
    /* 0x34 */ u32 loopEnd;
    /* 0x38 */ s32 loopCount;
    /* 0x3C */ s16 loopPredictorState[16];
    // Total: 0x5C = 92 bytes
} DebugSamplePatchHeader;

RECOMP_IMPORT(".", bool AudioApiNative_DebugQuerySamplePatch(DebugSamplePatchHeader* out));
RECOMP_IMPORT(".", bool AudioApiNative_DebugTakeSamplePatch(s16* bookCoeffs, u8* adpcmData));
// arr layout:
//   [0]                       = SEQ_PLAYER_MAX (= 5 for MM)
//   [1 .. SEQ_PLAYER_MAX]     = defaultFont per player (0xFF = inactive)
//   [SEQ_PLAYER_MAX+1]        = numFonts
//   Then for each font, a variable-length block:
//     [0] numInstruments
//     [1] numDrums
//     [2] numSfx
//     [3 .. 3+numInstruments-1] per-instrument region bitmask:
//           bit 0 = has lo sample, bit 1 = has mid sample, bit 2 = has hi sample
RECOMP_IMPORT(".", bool AudioApiNative_DebugPushSoundFontInfos(s32* arr, u32 totalInts));
RECOMP_IMPORT(".", u32 AudioApiNative_DebugGetFontPushEnabled(void));

// ============================================================
// Font info push (on change)
// ============================================================

#define AUDIOAPI_DEBUG_TUNING_SCALE 1000000.0f

// The full font-table walk runs on the audio thread; pushing it on a fixed
// 1s timer caused a periodic underrun click on large font tables. The push is
// off by default (enabled from the Sample Patcher UI) and fires only when the
// data could have changed: first push, font count or a player's default font
// changing, or a sample patch applied from the debug UI.
static bool sFontInfoDirty = true;
static u32 sFontInfoPrevNumFonts = 0;
static u8 sFontInfoPrevDefaultFonts[SEQ_PLAYER_MAX];

static s32 AudioApi_DebugEncodeTuning(f32 tuning) {
    if (tuning >= 0.0f) {
        return (s32)(tuning * AUDIOAPI_DEBUG_TUNING_SCALE + 0.5f);
    }
    return (s32)(tuning * AUDIOAPI_DEBUG_TUNING_SCALE - 0.5f);
}

static void AudioApi_DebugRestartPatchedNote(Note* note, SequenceLayer* layer, TunedSample* tunedSample, f32 freqScale) {
    if (note == NULL || layer == NULL || layer == NO_LAYER || tunedSample == NULL || tunedSample->sample == NULL) {
        return;
    }

    layer->tunedSample = tunedSample;
    layer->freqScale = freqScale;
    layer->notePropertiesNeedInit = true;

    note->sampleState.tunedSample = tunedSample;
    note->sampleState.bitField0.needsInit = true;
    note->sampleState.bitField0.finished = false;
    note->playbackState.startSamplePos = tunedSample->sample->loop->header.start;
}

static void AudioApi_DebugRefreshInstrumentNotes(s32 fontId, s32 instId) {
    Instrument* instrument = AudioPlayback_GetInstrumentInner(fontId, instId);
    s32 targetInstOrWave = instId + 2;
    s32 i;

    if (instrument == NULL) {
        return;
    }

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        Note* note = &gAudioCtx.notes[i];
        SequenceLayer* layer = note->playbackState.parentLayer;
        SequenceChannel* channel;
        s32 instOrWave;
        TunedSample* tunedSample;

        if (!note->sampleState.bitField0.enabled || note->sampleState.bitField1.isSyntheticWave) {
            continue;
        }
        if (note->playbackState.fontId != fontId || layer == NULL || layer == NO_LAYER) {
            continue;
        }

        channel = layer->channel;
        if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
            continue;
        }

        instOrWave = (layer->instOrWave == 0xFF) ? channel->instOrWave : layer->instOrWave;
        if (instOrWave != targetInstOrWave) {
            continue;
        }

        if (layer->instOrWave == 0xFF) {
            channel->instrument = instrument;
        } else {
            layer->instrument = instrument;
        }

        tunedSample = AudioPlayback_GetInstrumentTunedSample(instrument, layer->semitone);
        if (tunedSample == NULL || tunedSample->sample == NULL) {
            continue;
        }

        AudioApi_DebugRestartPatchedNote(note, layer, tunedSample,
                                         gPitchFrequencies[layer->semitone] * tunedSample->tuning * layer->bend);
    }
}

static void AudioApi_DebugRefreshDrumNotes(s32 fontId, s32 drumId) {
    Drum* drum = AudioPlayback_GetDrum(fontId, drumId);
    s32 i;

    if (drum == NULL || drum->tunedSample.sample == NULL) {
        return;
    }

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        Note* note = &gAudioCtx.notes[i];
        SequenceLayer* layer = note->playbackState.parentLayer;
        SequenceChannel* channel;
        s32 instOrWave;

        if (!note->sampleState.bitField0.enabled || note->sampleState.bitField1.isSyntheticWave) {
            continue;
        }
        if (note->playbackState.fontId != fontId || layer == NULL || layer == NO_LAYER) {
            continue;
        }

        channel = layer->channel;
        if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
            continue;
        }

        instOrWave = (layer->instOrWave == 0xFF) ? channel->instOrWave : layer->instOrWave;
        if (instOrWave != 0 || layer->semitone != drumId) {
            continue;
        }

        layer->adsr.envelope = drum->envelope;
        layer->adsr.decayIndex = drum->adsrDecayIndex;
        if (!layer->ignoreDrumPan) {
            layer->pan = drum->pan;
        }

        AudioApi_DebugRestartPatchedNote(note, layer, &drum->tunedSample, drum->tunedSample.tuning * layer->bend);
    }
}

static void AudioApi_DebugRefreshSfxNotes(s32 fontId, s32 sfxId) {
    SoundEffect* soundEffect = AudioPlayback_GetSoundEffect(fontId, sfxId);
    s32 i;

    if (soundEffect == NULL || soundEffect->tunedSample.sample == NULL) {
        return;
    }

    for (i = 0; i < gAudioCtx.numNotes; i++) {
        Note* note = &gAudioCtx.notes[i];
        SequenceLayer* layer = note->playbackState.parentLayer;
        SequenceChannel* channel;
        s32 instOrWave;
        s32 activeSfxId;

        if (!note->sampleState.bitField0.enabled || note->sampleState.bitField1.isSyntheticWave) {
            continue;
        }
        if (note->playbackState.fontId != fontId || layer == NULL || layer == NO_LAYER) {
            continue;
        }

        channel = layer->channel;
        if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
            continue;
        }

        instOrWave = (layer->instOrWave == 0xFF) ? channel->instOrWave : layer->instOrWave;
        activeSfxId = ((s32)layer->transposition << 6) + layer->semitone;
        if (instOrWave != 1 || activeSfxId != sfxId) {
            continue;
        }

        AudioApi_DebugRestartPatchedNote(note, layer, &soundEffect->tunedSample,
                                         soundEffect->tunedSample.tuning * layer->bend);
    }
}

static void AudioApi_DebugPushFontInfos(void) {
    u32 numFonts;
    u32 numPlayers;
    u32 totalInts;
    s32* arr;
    u32 i;
    u32 j;
    u32 idx;

    if (!gAudioApiDebugHttpEnabled) {
        return;
    }

    if (gAudioCtx.soundFontTable == NULL || gAudioCtx.soundFontList == NULL) {
        return;
    }

    if (!AudioApiNative_DebugGetFontPushEnabled()) {
        return;
    }

    numFonts = gAudioCtx.soundFontTable->header.numEntries;
    numPlayers = SEQ_PLAYER_MAX;

    if (numFonts != sFontInfoPrevNumFonts) {
        sFontInfoDirty = true;
        sFontInfoPrevNumFonts = numFonts;
    }
    for (i = 0; i < numPlayers; i++) {
        if (gAudioCtx.seqPlayers[i].defaultFont != sFontInfoPrevDefaultFonts[i]) {
            sFontInfoDirty = true;
            sFontInfoPrevDefaultFonts[i] = gAudioCtx.seqPlayers[i].defaultFont;
        }
    }

    if (!sFontInfoDirty) {
        return;
    }
    sFontInfoDirty = false;

    // Header: 1 (numPlayers) + numPlayers + 1 (numFonts).
    // Per font: 3 + numInst * 4 (mask + lo/mid/hi tuning) + numDrums + numSfx.
    totalInts = 1 + numPlayers + 1;
    for (i = 0; i < numFonts; i++) {
        totalInts += 3 + (gAudioCtx.soundFontList[i].numInstruments * 4) +
                     gAudioCtx.soundFontList[i].numDrums + gAudioCtx.soundFontList[i].numSfx;
    }

    arr = recomp_alloc(sizeof(s32) * totalInts);
    if (arr == NULL) {
        return;
    }

    // Header.
    idx = 0;
    arr[idx++] = (s32)numPlayers;
    for (i = 0; i < numPlayers; i++) {
        arr[idx++] = (s32)gAudioCtx.seqPlayers[i].defaultFont;
    }
    arr[idx++] = (s32)numFonts;

    // Per-font blocks.
    for (i = 0; i < numFonts; i++) {
        u32 numInst = gAudioCtx.soundFontList[i].numInstruments;
        arr[idx++] = (s32)numInst;
        arr[idx++] = (s32)gAudioCtx.soundFontList[i].numDrums;
        arr[idx++] = (s32)gAudioCtx.soundFontList[i].numSfx;

        for (j = 0; j < numInst; j++) {
            s32 mask = 0;
            s32 loTuning = 0;
            s32 midTuning = 0;
            s32 hiTuning = 0;
            if (gAudioCtx.soundFontList[i].instruments != NULL) {
                Instrument* inst = gAudioCtx.soundFontList[i].instruments[j];
                if (inst != NULL) {
                    if (inst->lowPitchTunedSample.sample != NULL) {
                        mask |= 1;
                        loTuning = AudioApi_DebugEncodeTuning(inst->lowPitchTunedSample.tuning);
                    }
                    if (inst->normalPitchTunedSample.sample != NULL) {
                        mask |= 2;
                        midTuning = AudioApi_DebugEncodeTuning(inst->normalPitchTunedSample.tuning);
                    }
                    if (inst->highPitchTunedSample.sample != NULL) {
                        mask |= 4;
                        hiTuning = AudioApi_DebugEncodeTuning(inst->highPitchTunedSample.tuning);
                    }
                }
            }
            arr[idx++] = mask;
            arr[idx++] = loTuning;
            arr[idx++] = midTuning;
            arr[idx++] = hiTuning;
        }

        for (j = 0; j < gAudioCtx.soundFontList[i].numDrums; j++) {
            s32 tuning = 0;
            if (gAudioCtx.soundFontList[i].drums != NULL && gAudioCtx.soundFontList[i].drums[j] != NULL &&
                gAudioCtx.soundFontList[i].drums[j]->tunedSample.sample != NULL) {
                tuning = AudioApi_DebugEncodeTuning(gAudioCtx.soundFontList[i].drums[j]->tunedSample.tuning);
            }
            arr[idx++] = tuning;
        }

        for (j = 0; j < gAudioCtx.soundFontList[i].numSfx; j++) {
            s32 tuning = 0;
            if (gAudioCtx.soundFontList[i].soundEffects != NULL &&
                gAudioCtx.soundFontList[i].soundEffects[j].tunedSample.sample != NULL) {
                tuning = AudioApi_DebugEncodeTuning(gAudioCtx.soundFontList[i].soundEffects[j].tunedSample.tuning);
            }
            arr[idx++] = tuning;
        }
    }

    AudioApiNative_DebugPushSoundFontInfos(arr, totalInts);
    recomp_free(arr);
}

// ============================================================
// Sample patch application
// ============================================================

static void AudioApi_DebugApplySamplePatch(void) {
    DebugSamplePatchHeader hdr;
    AdpcmBook* book;
    AdpcmLoop* loop;
    Sample* sample;
    s16* bookCoeffs;
    u8* adpcmData;
    size_t bookSize;
    size_t loopSize;
    bool hasLoop;
    Instrument* inst;
    Drum* drum;
    SoundEffect sfx;

    if (!gAudioApiDebugHttpEnabled) {
        return;
    }

    if (gAudioCtx.soundFontTable == NULL || gAudioCtx.soundFontList == NULL) {
        return;
    }

    if (!AudioApiNative_DebugQuerySamplePatch(&hdr)) {
        return;
    }

    // Validate.
    if (hdr.fontId < 0 || (u32)hdr.fontId >= (u32)gAudioCtx.soundFontTable->header.numEntries) {
        // Consume and discard.
        AudioApiNative_DebugTakeSamplePatch(NULL, NULL);
        return;
    }
    if (hdr.sampleDataSize == 0) {
        AudioApiNative_DebugTakeSamplePatch(NULL, NULL);
        return;
    }

    if (hdr.codec == CODEC_ADPCM && hdr.bookCoeffCount == 0) {
        AudioApiNative_DebugTakeSamplePatch(NULL, NULL);
        return;
    }

    bookCoeffs = NULL;
    if (hdr.bookCoeffCount != 0) {
        bookCoeffs = recomp_alloc(sizeof(s16) * hdr.bookCoeffCount);
        if (bookCoeffs == NULL) {
            AudioApiNative_DebugTakeSamplePatch(NULL, NULL);
            return;
        }
    }

    // Allocate ADPCM data buffer (must be 16-byte aligned for RSP DMA).
    // recomp_alloc returns suitably aligned memory.
    adpcmData = recomp_alloc(hdr.sampleDataSize);
    if (adpcmData == NULL) {
        if (bookCoeffs != NULL) {
            recomp_free(bookCoeffs);
        }
        AudioApiNative_DebugTakeSamplePatch(NULL, NULL);
        return;
    }

    // Consume the patch, filling both buffers.
    if (!AudioApiNative_DebugTakeSamplePatch(bookCoeffs, adpcmData)) {
        if (bookCoeffs != NULL) {
            recomp_free(bookCoeffs);
        }
        recomp_free(adpcmData);
        return;
    }

    // Patched tunings must reach the patcher UI's post-apply refresh.
    sFontInfoDirty = true;

    book = NULL;
    if (hdr.codec == CODEC_ADPCM) {
        bookSize = sizeof(AdpcmBookHeader) + sizeof(s16) * hdr.bookCoeffCount;
        book = recomp_alloc(bookSize);
        if (book == NULL) {
            recomp_free(bookCoeffs);
            recomp_free(adpcmData);
            return;
        }
        book->header.order         = hdr.bookOrder;
        book->header.numPredictors = hdr.bookNumPredictors;
        Lib_MemCpy(book->codeBook, bookCoeffs, sizeof(s16) * hdr.bookCoeffCount);
    }
    if (bookCoeffs != NULL) {
        recomp_free(bookCoeffs);
    }

    // Build AdpcmLoop.
    hasLoop = (hdr.loopCount != 0) && (hdr.loopEnd > hdr.loopStart);
    loopSize = hasLoop ? sizeof(AdpcmLoop) : sizeof(AdpcmLoopHeader);
    loop = recomp_alloc(loopSize);
    if (loop == NULL) {
        recomp_free(book);
        recomp_free(adpcmData);
        return;
    }
    loop->header.start     = hdr.loopStart;
    loop->header.loopEnd   = hdr.loopEnd;
    loop->header.count     = (u32)hdr.loopCount;
    loop->header.sampleEnd = hdr.numSamples;
    if (hasLoop) {
        Lib_MemCpy(loop->predictorState, hdr.loopPredictorState, sizeof(s16) * 16);
    }

    // Build Sample.
    sample = recomp_alloc(sizeof(Sample));
    if (sample == NULL) {
        recomp_free(loop);
        recomp_free(book);
        recomp_free(adpcmData);
        return;
    }
    sample->unk_0       = 0;
    sample->codec       = hdr.codec;
    sample->medium      = MEDIUM_CART;
    sample->unk_bit26   = 0;
    sample->isRelocated = true;
    sample->size        = hdr.sampleDataSize;
    sample->sampleAddr  = adpcmData;
    sample->loop        = loop;
    sample->book        = book;

    recomp_printf("[SamplePatch] received: font=%d inst=%d drum=%d sfx=%d region=%d codec=%u dataSize=%u\n",
                  hdr.fontId, hdr.instId, hdr.drumId, hdr.sfxId, hdr.pitchRegion, hdr.codec, hdr.sampleDataSize);

    // Apply to the appropriate slot.
    if (hdr.instId >= 0) {
        s32 numInst = (s32)gAudioCtx.soundFontList[hdr.fontId].numInstruments;
        recomp_printf("[SamplePatch] inst path: numInst=%d existing=%p\n",
                      numInst, hdr.instId < numInst ? (void*)gAudioCtx.soundFontList[hdr.fontId].instruments[hdr.instId] : NULL);
        if (hdr.instId >= numInst) {
            goto cleanup;
        }

        // Clone the existing instrument to preserve envelope + other pitch regions.
        Instrument* existing = gAudioCtx.soundFontList[hdr.fontId].instruments[hdr.instId];
        if (existing == NULL) {
            // Create a new minimal instrument.
            inst = recomp_alloc(sizeof(Instrument));
            if (inst == NULL) goto cleanup;
            Lib_MemSet(inst, 0, sizeof(Instrument));
            inst->isRelocated    = true;
            inst->normalRangeLo  = 0;
            inst->normalRangeHi  = 0x7F;
            inst->adsrDecayIndex = 0;
            inst->envelope       = DefaultEnvelopePoint;
        } else {
            inst = recomp_alloc(sizeof(Instrument));
            if (inst == NULL) goto cleanup;
            Lib_MemCpy(inst, existing, sizeof(Instrument));
            inst->isRelocated = true;
        }

        // Build the TunedSample for the target pitch region.
        switch (hdr.pitchRegion) {
            case 0:
                inst->lowPitchTunedSample.sample  = sample;
                inst->lowPitchTunedSample.tuning  = hdr.tuning;
                break;
            case 2:
                inst->highPitchTunedSample.sample = sample;
                inst->highPitchTunedSample.tuning = hdr.tuning;
                break;
            default: // 1 = normal
                inst->normalPitchTunedSample.sample = sample;
                inst->normalPitchTunedSample.tuning = hdr.tuning;
                break;
        }

        AudioApi_ReplaceInstrument(hdr.fontId, hdr.instId, inst);
        AudioApi_DebugRefreshInstrumentNotes(hdr.fontId, hdr.instId);
        // AudioApi_CopySample (called internally) deep-copies loop/book but keeps sampleAddr ptr.
        // So: free the Sample struct + loop + book (all deep-copied); keep adpcmData (not copied).
        recomp_free(inst);
        recomp_free(sample);
        recomp_free(loop);
        recomp_free(book);
        // adpcmData is now owned by the internal copy via sampleAddr — do NOT free.
        return;

    } else if (hdr.drumId >= 0) {
        s32 numDrum = (s32)gAudioCtx.soundFontList[hdr.fontId].numDrums;
        if (hdr.drumId >= numDrum) goto cleanup;

        Drum* existing = gAudioCtx.soundFontList[hdr.fontId].drums[hdr.drumId];
        drum = recomp_alloc(sizeof(Drum));
        if (drum == NULL) goto cleanup;

        if (existing != NULL) {
            Lib_MemCpy(drum, existing, sizeof(Drum));
        } else {
            Lib_MemSet(drum, 0, sizeof(Drum));
            drum->adsrDecayIndex = 0;
            drum->pan            = 64;
            drum->envelope       = DefaultEnvelopePoint;
        }

        drum->isRelocated            = true;
        drum->tunedSample.sample     = sample;
        drum->tunedSample.tuning     = hdr.tuning;

        AudioApi_ReplaceDrum(hdr.fontId, hdr.drumId, drum);
        AudioApi_DebugRefreshDrumNotes(hdr.fontId, hdr.drumId);
        recomp_free(drum);
        recomp_free(sample);
        recomp_free(loop);
        recomp_free(book);
        // adpcmData kept alive via sampleAddr in the internal copy.
        return;

    } else if (hdr.sfxId >= 0) {
        s32 numSfx = (s32)gAudioCtx.soundFontList[hdr.fontId].numSfx;
        if (hdr.sfxId >= numSfx) goto cleanup;

        sfx.tunedSample.sample = sample;
        sfx.tunedSample.tuning = hdr.tuning;
        AudioApi_ReplaceSoundEffect(hdr.fontId, hdr.sfxId, &sfx);
        AudioApi_DebugRefreshSfxNotes(hdr.fontId, hdr.sfxId);
        recomp_free(sample);
        recomp_free(loop);
        recomp_free(book);
        // adpcmData kept alive via sampleAddr.
        return;
    }

cleanup:
    recomp_free(sample);
    recomp_free(loop);
    recomp_free(book);
    recomp_free(adpcmData);
}

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
    AudioApi_DebugApplySamplePatch();
    AudioApi_DebugPushFontInfos();
    u32 i;
    u32 j;
    u32 pi;
    u32 ci;
    s32 mixOut[4];
    s32 snapshotTail[2];
    s32 playerState[260];
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

        // ch fields: [20..35] chEnabled, [36..51] chPan, [52..67] chVolMilli, [68..83] chReverbVol,
        // [228..243] chFontId, [244..259] chInstOrWave
        for (j = 0; j < SEQ_NUM_CHANNELS; j++) {
            channel = seqPlayer->channels[j];
            if (channel == NULL || !IS_SEQUENCE_CHANNEL_VALID(channel)) {
                playerState[20 + j] = 0;
                playerState[36 + j] = -1;
                playerState[52 + j] = 0;
                playerState[68 + j] = 0;
                playerState[228 + j] = -1;
                playerState[244 + j] = -1;
                continue;
            }

            playerState[20 + j] = channel->enabled;
            playerState[36 + j] = AudioApi_DebugNormalizePan(channel->pan);
            playerState[52 + j] = channel->volume * 1000.0f;
            playerState[68 + j] = channel->targetReverbVol;
            playerState[228 + j] = channel->fontId;
            playerState[244 + j] = channel->instOrWave;
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
