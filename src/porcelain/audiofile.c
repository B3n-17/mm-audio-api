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
 *   2. Computes sequence length in tatums (48 per beat @ 25 BPM = 20 tatums/sec = 1 per frame).
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

#define CREDITS_PART1_TOTAL_TATUMS 2537
#define CREDITS_PART2_TOTAL_TATUMS 5053

/* Imported from resource.c porcelain layer (same mod) */
RECOMP_IMPORT(".", s32 AudioApi_AddAudioFileFromFs(AudioApiFileInfo* info, char* dir, char* filename));
RECOMP_IMPORT(".", uintptr_t AudioApi_GetResourceDevAddr(u32 resourceId, u32 arg1, u32 arg2));
RECOMP_IMPORT(".", void AudioApi_SetWindfishReplacementSeqId(s32 seqId));

/* Builds a sequence + soundfont from a previously-loaded audio file described by info.
 * seqIO selects optional IO channel behavior (e.g. AUDIOAPI_SEQ_IO_BREMEN for march sync).
 * Returns seqId on success, -1 on failure. Caller must have already loaded the audio resource. */
RECOMP_EXPORT s32 AudioApi_CreateStreamedSequence(AudioApiFileInfo* info, AudioApiSequenceIO seqIO) {
    u32 channelCount, trackCount;
    u32 channelNo, trackNo, i;
    s32 seqId, fontId;
    u16 length;
    u16 initChanMask, freeChanMask;
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
    CSeqSection* ioLabel;

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

    /* Step 2: Compute note duration in tatums.
     * At 25 BPM with 48 tatums/beat: 20 tatums/sec (= 1 tatum per game frame at 20 FPS).
     * Infinite loop → max length (0x7FFF). Finite → ceil(duration_sec * tatums_per_sec). */
    if (info->loopCount == -1) {
        length = 0x7FFF;
    } else {
        length = lceilf((info->loopCount + 1) * ((f32)info->sampleCount / info->sampleRate) *
                        (TATUMS_PER_BEAT * 25.0f / 60.0f));
        length = CLAMP(length, 0, 0x7FFF);

        if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_1) {
            length = MAX(length, CREDITS_PART1_TOTAL_TATUMS + 1);
        } else if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_2) {
            length = MAX(length, CREDITS_PART2_TOTAL_TATUMS + 1);
        }
    }

    /* Step 3: Build CSeq binary — sequence header, channel commands, layer note data. */
    root = cseq_create();
    seq = cseq_sequence_create(root);

    initChanMask = (1 << channelCount) - 1;
    freeChanMask = initChanMask;

    if ((seqIO == AUDIOAPI_SEQ_IO_BREMEN ||
         seqIO == AUDIOAPI_SEQ_IO_CREDITS_1 ||
         seqIO == AUDIOAPI_SEQ_IO_CREDITS_2 ||
         seqIO == AUDIOAPI_SEQ_IO_FROG) && channelCount < 16) {
        initChanMask |= (1 << 15);
        freeChanMask |= (1 << 15);
    }

    cseq_mutebhv(seq, 0x20);   /* mute behavior: stop notes */
    cseq_mutescale(seq, 0x32); /* mute scale: 50 */
    cseq_vol(seq, 0x7F);      /* max volume */
    cseq_initchan(seq, initChanMask);

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

    /* Channel 15: IO port writer for game engine synchronization.
     * Mode depends on seqIO:
     *   BREMEN:    Tight loop writing 0x00 to IO_PORT_0 every tatum (march actor sync).
     *   CREDITS_1: 8 timed cue pulses for credits part 1 scene transitions.
     *   CREDITS_2: 12 timed cue pulses for credits part 2 scene transitions.
     *   FROG:      5 beat pulses for frog conducting timing checks.
     * Credits delay values are pre-converted from the original variable-tempo sequences
     * to fixed 25 BPM (20 tatums/sec). See vanillaSequenceBehavior.md for derivation. */
    if ((seqIO == AUDIOAPI_SEQ_IO_BREMEN ||
         seqIO == AUDIOAPI_SEQ_IO_CREDITS_1 ||
         seqIO == AUDIOAPI_SEQ_IO_CREDITS_2 ||
         seqIO == AUDIOAPI_SEQ_IO_FROG) && channelCount < 16) {
        chan = cseq_channel_create(root);
        cseq_ldchan(seq, 15, chan);
        cseq_vol(chan, 0);

        if (seqIO == AUDIOAPI_SEQ_IO_BREMEN) {
            ioLabel = cseq_label_create(chan);

            cseq_setval(chan, 0x00);
            cseq_stio(chan, 0);
            cseq_delay1(chan, 1);

            cseq_jump(chan, ioLabel);

        } else if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_1) {
            /* seq_116 replacement mode: emit part 1 cues only.
             * After part 1, the sequence-level runseq hands off to seq_127 (credits part 2),
             * matching vanilla seq_116 behavior. */
            static const u16 credits1Delays[] = { 414, 566, 300, 300, 300, 300, 349, 8 };

            for (i = 0; i < ARRAY_COUNT(credits1Delays); i++) {
                cseq_delay(chan, credits1Delays[i]);
                cseq_setval(chan, 0x00);
                cseq_stio(chan, 0);
            }

            cseq_section_end(chan);

        } else if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_2) {
            /* seq_127: 12 cue writes to IO_PORT_0 = 0 at converted timings */
            static const u16 credits2Delays[] = { 258, 300, 300, 300, 279, 300, 300, 300, 309, 929, 411, 1067 };

            for (i = 0; i < ARRAY_COUNT(credits2Delays); i++) {
                cseq_delay(chan, credits2Delays[i]);
                cseq_setval(chan, 0x00);
                cseq_stio(chan, 0);
            }
            cseq_section_end(chan);

        } else if (seqIO == AUDIOAPI_SEQ_IO_FROG) {
            /* seq_90: initialize IO_PORT_0 = 0, then 5 beat pulses every 177 ticks with 15-tick spacing. */
            cseq_setval(chan, 0x00);
            cseq_stio(chan, 0);

            for (i = 0; i < 5; i++) {
                cseq_delay(chan, 177);
                cseq_setval(chan, i + 1);
                cseq_stio(chan, 0);
                cseq_delay(chan, 15);
            }

            cseq_section_end(chan);
        }
    }

    cseq_tempo(seq, 25);         /* 25 BPM → 20 tatums/sec → 1 tatum per game frame at 20 FPS */
    cseq_delay(seq, length - 1); /* wait for playback to finish */

    if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_1) {
        /* After part 1 finishes, hand off to seq_127 (credits part 2) on the same player.
         * 0xFF = self (same player index), matching vanilla seq_116's runseq behavior. */
        cseq_runseq(seq, 0xFF, NA_BGM_END_CREDITS_SECOND_HALF);
    }

    if (info->loopCount == -1) {
        cseq_jump(seq, label);   /* infinite loop: jump back to start */
    }

    cseq_freechan(seq, freeChanMask);
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

/* High-level: load audio file → create streamed sequence for background music.
 * Policy:
 *   - loop markers present: always loop infinitely
 *   - no loop markers: play once
 * This avoids requiring mod authors to set finite loop counts correctly in files. */
RECOMP_EXPORT s32 AudioApi_CreateStreamedBgm(AudioApiFileInfo* info, char* dir, char* filename,
                                              AudioApiSequenceIO seqIO) {
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

    if (seqIO == AUDIOAPI_SEQ_IO_CREDITS_1 || seqIO == AUDIOAPI_SEQ_IO_CREDITS_2) {
        info->loopCount = 0;
    } else {
        info->loopCount = (info->loopCount != 0) ? -1 : 0;
    }

    return AudioApi_CreateStreamedSequence(info, seqIO);
}

/* High-level: load audio file → create one-shot sequence flagged as fanfare (ducks BGM).
 * Loop metadata from the file is ignored — fanfares always play once.
 * Exception: AUDIOAPI_SEQ_IO_BREMEN forces infinite loop (march needs continuous playback).
 * seqIO selects optional IO channel behavior (e.g. AUDIOAPI_SEQ_IO_BREMEN for march sync). */
RECOMP_EXPORT s32 AudioApi_CreateStreamedFanfare(AudioApiFileInfo* info, char* dir, char* filename,
                                                  AudioApiSequenceIO seqIO) {
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

    /* Fanfares play once by default — ignore loop metadata from the file.
     * Only loop if seqIO requires it (e.g. Bremen March needs infinite playback). */
    if (seqIO == AUDIOAPI_SEQ_IO_BREMEN) {
        info->loopCount = -1;
    } else {
        info->loopCount = 0;
    }

    seqId = AudioApi_CreateStreamedSequence(info, seqIO);
    if (seqId == -1) {
        return -1;
    }

    if (seqIO == AUDIOAPI_SEQ_IO_WINDFISH) {
        AudioApi_SetWindfishReplacementSeqId(seqId);
    }

    AudioApi_SetSequenceFlags(seqId, SEQ_FLAG_FANFARE);

    return seqId;
}
