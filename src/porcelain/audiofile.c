/*
 * audiofile.c — Converts decoded audio files (wav/flac/mp3/vorbis/opus) into playable
 *   N64-style sequences by programmatically generating sequence data + soundfont.
 *
 * Core idea: The N64 audio engine only understands sequences (MIDI-like) + soundfonts +
 *   sample banks. To play a streamed audio file, we synthesize a minimal sequence that
 *   simply plays the audio samples as instrument notes at the correct pitch/duration.
 *
 * CreateStreamedSequence(info) — The engine of this file:
 *   1. Creates an empty soundfont and adds one Instrument per audio track, each pointing
 *      to a DMA-backed Sample at the file's native sample rate (tuned via sampleRate/32000).
 *   2. Computes sequence length in tatums (48 per beat @ 1 BPM tempo).
 *   3. Builds a CSeq (compiled sequence) with:
 *      - One channel per mono track, or one channel per stereo pair (L=layer0 pan0, R=layer1 pan127)
 *      - Each channel plays one note (C4) for the full duration at max velocity
 *      - Loop: infinite (loopCount=-1) → jump back to label; finite → play once and end
 *   4. Compiles CSeq to binary, registers it as a new sequence, binds the soundfont.
 *
 * CreateStreamedBgm(info, dir, filename) — High-level: loads audio file from FS,
 *   auto-detects mono/stereo from track count, sets loopCount=-1 (infinite), calls above.
 *
 * CreateStreamedFanfare(info, dir, filename) — Same but loopCount=0 (play once),
 *   sets SEQ_FLAG_FANFARE so the engine treats it as a one-shot jingle that ducks BGM.
 *
 * Channel layout:
 *   MONO:   trackCount tracks → trackCount channels, 1 layer each (centered)
 *   STEREO: trackCount tracks → trackCount/2 channels, 2 layers each (L+R panned)
 */
#include <global.h>
#include <recomp/modding.h>
#include <recomp/recomputils.h>
#include <libc64/fixed_point.h>

#include <audio_api/types.h>
#include <audio_api/sequence.h>
#include <audio_api/soundfont.h>
#include <audio_api/cseq.h>

/* Imported from resource.c porcelain layer (same mod) */
RECOMP_IMPORT(".", s32 AudioApi_AddAudioFileFromFs(AudioApiFileInfo* info, char* dir, char* filename));
RECOMP_IMPORT(".", uintptr_t AudioApi_GetResourceDevAddr(u32 resourceId, u32 arg1, u32 arg2));

/* Builds a sequence + soundfont from a previously-loaded audio file described by info.
 * Returns seqId on success, -1 on failure. Caller must have already loaded the audio resource. */
RECOMP_EXPORT s32 AudioApi_CreateStreamedSequence(AudioApiFileInfo* info) {
    u32 channelCount, trackCount;
    u32 channelNo, trackNo;
    s32 seqId, fontId;
    u16 length;
    uintptr_t sampleAddr;
    AdpcmLoop sampleLoop;
    Sample sample;
    Instrument inst;
    size_t seqSize;
    u8* seqData;
    AudioTableEntry entry;
    CSeqContainer* root;
    CSeqSection* seq;
    CSeqSection* chan;
    CSeqSection* layer;
    CSeqSection* label;

    if (info == NULL) {
        return -1;
    }

    /* Mono: 1 track = 1 channel (max 16). Stereo: 2 tracks = 1 channel (max 32 tracks / 16 ch). */
    if (info->channelType == AUDIOAPI_CHANNEL_TYPE_MONO) {
        trackCount = MIN(info->trackCount, 16);
        channelCount = trackCount;
    } else if (info->channelType == AUDIOAPI_CHANNEL_TYPE_STEREO) {
        trackCount = MIN(info->trackCount, 32);
        channelCount = trackCount / 2;
    } else {
        return -1;
    }

    /* Step 1: Build soundfont — one instrument per track, each referencing DMA-backed samples. */
    fontId = AudioApi_CreateEmptySoundFont();

    for (trackNo = 0; trackNo < trackCount; trackNo++) {

        sampleAddr = AudioApi_GetResourceDevAddr(info->resourceId, trackNo, 0);

        sampleLoop = (AdpcmLoop){
            { info->loopStart, info->loopEnd, info->loopCount, info->sampleCount }, {}
        };

        sample = (Sample){
            0, CODEC_S16, MEDIUM_CART, false, false,
            info->sampleCount * 2, /* size in bytes (16-bit PCM = 2 bytes/sample) */
            (void*)sampleAddr,
            &sampleLoop,
            NULL
        };

        inst = (Instrument){
            false,
            INSTR_SAMPLE_LO_NONE,
            INSTR_SAMPLE_HI_NONE,
            251,                              /* release rate */
            DefaultEnvelopePoint,
            INSTR_SAMPLE_NONE,                /* lo-range sample (unused) */
            { &sample, info->sampleRate / 32000.0f }, /* normal sample, tuning = native/32kHz */
            INSTR_SAMPLE_NONE,                /* hi-range sample (unused) */
        };

        AudioApi_AddInstrument(fontId, &inst);
    }

    /* Step 2: Compute note duration in tatums (48/beat @ 1 BPM → 48 tatums/sec @ tempo=1).
     * Infinite loop → max length (0x7FFF). Finite → ceil(duration_sec * tatums_per_sec). */
    if (info->loopCount == -1) {
        length = 0x7FFF;
    } else {
        length = lceilf((info->loopCount + 1) * ((f32)info->sampleCount / info->sampleRate) *
                        (TATUMS_PER_BEAT / 60.0f));
        length = CLAMP(length, 0, 0x7FFF);
    }

    /* Step 3: Build CSeq binary — sequence header, channel commands, layer note data. */
    root = cseq_create();
    seq = cseq_sequence_create(root);

    cseq_mutebhv(seq, 0x20);   /* mute behavior: stop notes */
    cseq_mutescale(seq, 0x32); /* mute scale: 50 */
    cseq_vol(seq, 0x7F);      /* max volume */
    cseq_initchan(seq, (1 << channelCount) - 1); /* enable all channels */

    label = cseq_label_create(seq);

    for (channelNo = 0; channelNo < channelCount; channelNo++) {
        chan = cseq_channel_create(root);
        cseq_ldchan(seq, channelNo, chan);
        cseq_noshort(chan);
        cseq_panweight(chan, 0);
        cseq_notepri(chan, 1);
        cseq_vol(chan, 0x7F);

        if (info->channelType == AUDIOAPI_CHANNEL_TYPE_MONO) {
            layer = cseq_layer_create(root);
            cseq_ldlayer(chan, 0, layer);
            cseq_instr(layer, channelNo);
            cseq_notepan(layer, 0);
            cseq_notedv(layer, PITCH_C4, length, 127);
            cseq_section_end(layer);

        } else if (info->channelType == AUDIOAPI_CHANNEL_TYPE_STEREO) {
            layer = cseq_layer_create(root);
            cseq_ldlayer(chan, 0, layer);
            cseq_instr(layer, channelNo * 2);
            cseq_notepan(layer, 0);
            cseq_notedv(layer, PITCH_C4, length, 127);
            cseq_section_end(layer);

            layer = cseq_layer_create(root);
            cseq_ldlayer(chan, 1, layer);
            cseq_instr(layer, channelNo * 2 + 1);
            cseq_notepan(layer, 127);
            cseq_notedv(layer, PITCH_C4, length, 127);
            cseq_section_end(layer);
        }

        cseq_delay(chan, length);
        cseq_section_end(chan);
    }

    cseq_tempo(seq, 0x01);       /* 1 BPM — timing controlled by note length, not tempo */
    cseq_delay(seq, length - 1); /* wait for playback to finish */

    if (info->loopCount == -1) {
        cseq_jump(seq, label);   /* infinite loop: jump back to start */
    }

    cseq_freechan(seq, (1 << channelCount) - 1);
    cseq_section_end(seq);

    /* Step 4: Compile CSeq to binary, copy to persistent allocation, register as sequence. */
    cseq_compile(root, 0);

    seqSize = root->buffer->size;
    seqData = recomp_alloc(seqSize);
    Lib_MemCpy(seqData, root->buffer->data, seqSize);

    cseq_destroy(root);

    entry = (AudioTableEntry){
        (uintptr_t)seqData,
        seqSize,
        MEDIUM_CART,
        CACHE_EITHER,
        0, 0, 0,
    };

    seqId = AudioApi_AddSequence(&entry);
    AudioApi_AddSequenceFont(seqId, fontId);

    return seqId;
}

/* High-level: load audio file → create infinite-loop sequence for background music.
 * Caller may set info->loopCount before calling; if left at 0 (default), loops infinitely. */
RECOMP_EXPORT s32 AudioApi_CreateStreamedBgm(AudioApiFileInfo* info, char* dir, char* filename) {
    AudioApiFileInfo defaultInfo = {0};

    if (info == NULL) {
        info = &defaultInfo;
    }

    if (!AudioApi_AddAudioFileFromFs(info, dir, filename)) {
        return -1;
    }

    if (info->channelType == AUDIOAPI_CHANNEL_TYPE_DEFAULT) {
        info->channelType = info->trackCount & 1
            ? AUDIOAPI_CHANNEL_TYPE_MONO
            : AUDIOAPI_CHANNEL_TYPE_STEREO;
    }

    if (info->loopCount == 0) {
        info->loopCount = -1;
    }

    return AudioApi_CreateStreamedSequence(info);
}

/* High-level: load audio file → create one-shot sequence flagged as fanfare (ducks BGM).
 * Caller may set info->loopCount before calling; if left at 0 (default), plays once.
 * Set loopCount=-1 for looping fanfares (e.g. Bremen March). */
RECOMP_EXPORT s32 AudioApi_CreateStreamedFanfare(AudioApiFileInfo* info, char* dir, char* filename) {
    AudioApiFileInfo defaultInfo = {0};
    s32 seqId;

    if (info == NULL) {
        info = &defaultInfo;
    }

    if (!AudioApi_AddAudioFileFromFs(info, dir, filename)) {
        return -1;
    }

    if (info->channelType == AUDIOAPI_CHANNEL_TYPE_DEFAULT) {
        info->channelType = info->trackCount & 1
            ? AUDIOAPI_CHANNEL_TYPE_MONO
            : AUDIOAPI_CHANNEL_TYPE_STEREO;
    }

    seqId = AudioApi_CreateStreamedSequence(info);
    if (seqId == -1) {
        return -1;
    }

    AudioApi_SetSequenceFlags(seqId, SEQ_FLAG_FANFARE);

    return seqId;
}
