#include <core/soundfont.h>
#include <global.h>
#include <recomp/modding.h>
#include <recomp/recompdata.h>
#include <recomp/recomputils.h>
#include <utils/misc.h>
#include <utils/queue.h>
#include <core/heap.h>
#include <core/init.h>
#include <core/load.h>
#include <core/load_status.h>

/**
 * SoundFont Public API - Create, modify, and manage N64 audio soundfont data for recomp mods.
 *
 * ARCHITECTURE:
 *   Vanilla soundfonts live in ROM and are loaded on-demand. This API wraps them with a
 *   CustomSoundFont struct (heap-allocated) that replaces the ROM entry's romAddr pointer.
 *
 * LIFECYCLE (3-phase init via gAudioApiInitPhase):
 *   1. AUDIOAPI_INIT_NOT_READY  - API calls rejected (return -1 / early return)
 *   2. AUDIOAPI_INIT_QUEUEING   - Mod registration phase; commands queue to soundFontInitQueue
 *   3. AUDIOAPI_INIT_READY       - Commands apply immediately if font is CustomSoundFont in RAM
 *                                  (IS_KSEG0 + type==SOUNDFONT_CUSTOM), else queue to soundFontLoadQueue
 *                                  for deferred apply when ROM font is actually loaded.
 *
 * QUEUE SYSTEM (two queues):
 *   soundFontInitQueue  - Collects commands during QUEUEING phase, drained at READY transition.
 *   soundFontLoadQueue  - Holds commands for ROM-based fonts; applied in AudioLoad_RelocateFont()
 *                         when the engine loads font data from ROM into RAM.
 *
 * FONT DATA LAYOUT (vanilla ROM format, read as uintptr_t*):
 *   [0] = offset to drums array,  [1] = offset to SFX array,
 *   [2..2+numInstruments-1] = offsets to instruments (SOUNDFONT_INSTRUMENT_OFFSET = 2)
 *   All offsets are relative to font base; RELOC_TO_RAM converts offset -> absolute pointer.
 *
 * MEMORY OWNERSHIP:
 *   - All Add/Replace API calls deep-copy inputs (AudioApi_Copy*) so caller retains ownership.
 *   - Sample copies are deduplicated via FNV-32a hash -> sampleHashmap (ADPCM codecs only).
 *   - Samples are ref-counted; AudioApi_FreeSample only frees when refcount hits 0.
 *   - Dynamic arrays (instruments/drums/sfx) grow by doubling capacity when full.
 *   - Global soundfont tables also double (soundFontTable, soundFontList, loadStatus).
 *   - IS_RECOMP_ALLOC guards prevent freeing pointers not owned by recomp_alloc.
 *   - IS_AUDIO_HEAP_MEMORY guards trigger copy-out from transient audio heap during relocation.
 *
 * KEY PATCHED FUNCTION:
 *   AudioLoad_RelocateFont (RECOMP_PATCH) - Intercepts vanilla font loading. For SOUNDFONT_VANILLA
 *   type, imports into CustomSoundFont, applies queued changes, then relocates all pointers
 *   (drums/sfx/instruments/samples/envelopes/loops/books). Replaces the table entry's romAddr
 *   with the permanent CustomSoundFont pointer. Fires AudioApi_SoundFontLoaded event.
 *
 *   AudioLoad_RelocateSample (RECOMP_PATCH) - Relocates sample pointers (loop/book/sampleAddr)
 *   and resolves sample bank base addresses. Handles DMA callback device addresses.
 *
 * INSTRUMENT NOTE RANGES:
 *   Instruments have 3 pitch regions: low (normalRangeLo != 0), normal, high (normalRangeHi != 0x7F).
 *   Each has its own TunedSample. Copy/reloc only processes low/high if range bounds indicate use.
 */

/* Limits & defaults */
#define NA_SOUNDFONT_MAX 0x28               // 40 vanilla soundfonts in MM
#define SOUNDFONT_DEFAULT_SAMPLEBANK_1 1    // Primary sample bank default
#define SOUNDFONT_DEFAULT_SAMPLEBANK_2 255  // Secondary sample bank default (0xFF = none)
#define SOUNDFONT_MAX_INSTRUMENTS 126       // Hard cap per font
#define SOUNDFONT_MAX_DRUMS 256             // Hard cap per font
#define SOUNDFONT_MAX_SFX 2048              // Hard cap per font
#define SOUNDFONT_DEFAULT_INSTRUMENT_CAPACITY 16 // Initial alloc, doubles on overflow
#define SOUNDFONT_DEFAULT_DRUM_CAPACITY 16
#define SOUNDFONT_DEFAULT_SFX_CAPACITY 8
#define SOUNDFONT_INSTRUMENT_OFFSET 2       // In vanilla font data, instruments start at word[2]

// Convert ROM-relative offset to absolute RAM pointer. No-op if already a KSEG0 address.
#define RELOC_TO_RAM(x, base) (void*)((uintptr_t)(x) + (uintptr_t)(IS_KSEG0(x) ? 0 : base))

/* Passed to AudioLoad_RelocateSample; maps sample bank indices 0/1 to resolved base addresses */
typedef struct {
    s32 sampleBankId1;
    s32 sampleBankId2;
    uintptr_t baseAddr1;  // Resolved addr for primary sample bank
    uintptr_t baseAddr2;  // Resolved addr for secondary sample bank
    u32 medium1;          // Storage medium (RAM/cart/disk) for bank 1
    u32 medium2;          // Storage medium for bank 2
} SampleBankRelocInfo;

/* Opcodes for deferred soundfont modifications (stored in init/load queues) */
typedef enum {
    AUDIOAPI_CMD_OP_REPLACE_SOUNDFONT,   // Swap entire font entry (data=AudioTableEntry*)
    AUDIOAPI_CMD_OP_SET_SAMPLEBANK,      // arg1=bankNum(1|2), data=bankId
    AUDIOAPI_CMD_OP_ADD_DRUM,            // data=Drum*
    AUDIOAPI_CMD_OP_REPLACE_DRUM,        // arg1=drumId, data=Drum*
    AUDIOAPI_CMD_OP_ADD_SOUNDEFFECT,     // data=SoundEffect*
    AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT, // arg1=sfxId, data=SoundEffect*
    AUDIOAPI_CMD_OP_ADD_INSTRUMENT,      // data=Instrument*
    AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT,  // arg1=instId, data=Instrument*
} AudioApiSoundFontQueueOp;

RecompQueue* soundFontInitQueue;   // Drained once at QUEUEING->READY transition
RecompQueue* soundFontLoadQueue;   // Drained per-font in AudioLoad_RelocateFont
U32ValueHashmapHandle sampleHashmap; // FNV32 hash -> Sample* for ADPCM dedup
u16 soundFontTableCapacity = NA_SOUNDFONT_MAX; // Current alloc size, doubles on overflow

void AudioApi_SoundFontQueueDrain(RecompQueueCmd* cmd);
void AudioLoad_RelocateSample(TunedSample* tunedSample, void* fontData, SampleBankRelocInfo* sampleBankReloc);
bool AudioApi_GrowSoundFontTables();
bool AudioApi_GrowInstrumentList(CustomSoundFont* soundFont);
bool AudioApi_GrowDrumList(CustomSoundFont* soundFont);
bool AudioApi_GrowSoundEffectList(CustomSoundFont* soundFont);
Drum* AudioApi_CopyDrum(Drum* src);
SoundEffect* AudioApi_CopySoundEffect(SoundEffect* src);
Instrument* AudioApi_CopyInstrument(Instrument* src);
Sample* AudioApi_CopySample(Sample* src);
void AudioApi_FreeSoundFont(CustomSoundFont* soundFont);
void AudioApi_FreeDrum(Drum* drum);
void AudioApi_FreeSoundEffect(SoundEffect* sfx);
void AudioApi_FreeInstrument(Instrument* instrument);
void AudioApi_FreeSample(Sample* sample);

extern void AudioLoad_InitSoundFont(s32 fontId);
extern u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 id);

RECOMP_DECLARE_EVENT(AudioApi_SoundFontLoaded(s32 fontId, u8* ramAddr));


/* Called during AudioApi_InitInternal callback. Sets up queues, hashmap, and vanilla font list. */
RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_SoundFontInit() {
    soundFontInitQueue = RecompQueue_Create();  // Holds mod cmds during QUEUEING phase
    soundFontLoadQueue = RecompQueue_Create();  // Holds cmds for not-yet-loaded ROM fonts
    sampleHashmap = recomputil_create_u32_value_hashmap(); // ADPCM sample dedup cache

    gAudioCtx.soundFontList = recomp_alloc(soundFontTableCapacity * sizeof(SoundFont));
    for (u32 i = 0; i < soundFontTableCapacity; i++) {
        AudioLoad_InitSoundFont(i); // Populate runtime SoundFont metadata from table entries
    }
}

/* Called at QUEUEING->READY transition. Flushes all queued init-phase commands. */
RECOMP_CALLBACK(".", AudioApi_ReadyInternal) void AudioApi_SoundFontReady() {
    RecompQueue_Drain(soundFontInitQueue, AudioApi_SoundFontQueueDrain);
    RecompQueue_Destroy(soundFontInitQueue);
}

/* Register a new soundfont entry. Returns fontId or -1 on failure. Grows table if needed.
 * If entry->romAddr is a CustomSoundFont (IS_KSEG0 + SOUNDFONT_CUSTOM), packs metadata into
 * shortData1/2/3 fields (sampleBanks, instrument/drum counts, sfx count). */
RECOMP_EXPORT s32 AudioApi_AddSoundFont(AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }

    // Find the next available font ID
    s32 newFontId = gAudioCtx.soundFontTable->header.numEntries;
    if (newFontId >= soundFontTableCapacity) {
        if (!AudioApi_GrowSoundFontTables()) {
            return -1;
        }
    }

    gAudioCtx.soundFontTable->header.numEntries++;
    gAudioCtx.soundFontTable->entries[newFontId] = *entry;

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        entry->shortData1 = (soundFont->sampleBank1 << 8) | soundFont->sampleBank2;
        entry->shortData2 = (soundFont->numInstruments << 8) | soundFont->numDrums;
        entry->shortData3 = soundFont->numSfx;
    }

    AudioLoad_InitSoundFont(newFontId);

    return newFontId;
}

/* Replace an existing font's table entry. Queues during QUEUEING phase (copies entry). */
RECOMP_EXPORT void AudioApi_ReplaceSoundFont(s32 fontId, AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        AudioTableEntry* copy = recomp_alloc(sizeof(AudioTableEntry));
        if (!copy) {
            return;
        }
        *copy = *entry;
        RecompQueue_PushIfNotQueued(soundFontInitQueue, AUDIOAPI_CMD_OP_REPLACE_SOUNDFONT,
                                    fontId, 0, (void**)&copy);
        return;
    }
    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }

    gAudioCtx.soundFontTable->entries[fontId] = *entry;

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        entry->shortData1 = (soundFont->sampleBank1 << 8) | soundFont->sampleBank2;
        entry->shortData2 = (soundFont->numInstruments << 8) | soundFont->numDrums;
        entry->shortData3 = soundFont->numSfx;
    }

    AudioLoad_InitSoundFont(fontId);
}

/* Restore a font entry to its original vanilla ROM state from gSoundFontTable backup. */
RECOMP_EXPORT void AudioApi_RestoreSoundFont(s32 fontId) {
    if (gAudioApiInitPhase < AUDIOAPI_INIT_READY) {
        return;
    }
    if (fontId >= gSoundFontTable.header.numEntries) {
        return;
    }

    gAudioCtx.soundFontTable->entries[fontId] = gSoundFontTable.entries[fontId];
    AudioLoad_InitSoundFont(fontId);
}

/* Alloc a zeroed CustomSoundFont with default capacities and sample banks. Returns NULL on OOM. */
CustomSoundFont* AudioApi_CreateEmptySoundFontInternal() {
    CustomSoundFont* soundFont = NULL;
    size_t size;

    size = sizeof(CustomSoundFont);
    soundFont = recomp_alloc(size);
    if (!soundFont) goto cleanup;
    Lib_MemSet(soundFont, 0, size);

    soundFont->type = SOUNDFONT_CUSTOM;
    soundFont->sampleBank1 = SOUNDFONT_DEFAULT_SAMPLEBANK_1;
    soundFont->sampleBank2 = SOUNDFONT_DEFAULT_SAMPLEBANK_2;
    soundFont->numInstruments = 0;
    soundFont->numDrums = 0;
    soundFont->numSfx = 0;
    soundFont->instrumentsCapacity = SOUNDFONT_DEFAULT_INSTRUMENT_CAPACITY;
    soundFont->drumsCapacity = SOUNDFONT_DEFAULT_DRUM_CAPACITY;
    soundFont->sfxCapacity = SOUNDFONT_DEFAULT_SFX_CAPACITY;

    size = soundFont->instrumentsCapacity * sizeof(uintptr_t);
    soundFont->instruments = recomp_alloc(size);
    if (!soundFont->instruments) goto cleanup;
    Lib_MemSet(soundFont->instruments, 0, size);

    size = soundFont->drumsCapacity * sizeof(uintptr_t);
    soundFont->drums = recomp_alloc(size);
    if (!soundFont->drums) goto cleanup;
    Lib_MemSet(soundFont->drums, 0, size);

    size = soundFont->sfxCapacity * sizeof(SoundEffect);
    soundFont->soundEffects = recomp_alloc(size);
    if (!soundFont->soundEffects) goto cleanup;
    Lib_MemSet(soundFont->soundEffects, 0, size);

    return soundFont;

 cleanup:
    AudioApi_FreeSoundFont(soundFont);
    return NULL;
}

/* Public: Create and register an empty soundfont. Returns fontId or -1. */
RECOMP_EXPORT s32 AudioApi_CreateEmptySoundFont() {
    CustomSoundFont* soundFont = AudioApi_CreateEmptySoundFontInternal();

    if (soundFont == NULL) {
        return -1;
    }

    AudioTableEntry entry = {
        (uintptr_t) soundFont,
        sizeof(CustomSoundFont),
        MEDIUM_CART,
        CACHE_EITHER,
        0, 0, 0,
    };

    s32 fontId = AudioApi_AddSoundFont(&entry);

    if (fontId == -1) {
        AudioApi_FreeSoundFont(soundFont);
        return -1;
    }

    return fontId;
}

/* Convert raw vanilla ROM font data (uintptr_t array) into a heap-allocated CustomSoundFont.
 * Copies instrument/drum/sfx pointer arrays from the ROM layout. Does NOT relocate pointers;
 * the caller must relocate drums[i], instruments[i], envelopes, samples etc. afterwards.
 * Capacities auto-grow (doubling) to fit the actual counts. */
CustomSoundFont* AudioApi_ImportVanillaSoundFontInternal(uintptr_t* fontData, u8 sampleBank1, u8 sampleBank2,
                                                         u8 numInstruments, u8 numDrums, u16 numSfx) {
    CustomSoundFont* soundFont = NULL;
    size_t size;

    size = sizeof(CustomSoundFont);
    soundFont = recomp_alloc(size);
    if (!soundFont) goto cleanup;
    Lib_MemSet(soundFont, 0, size);

    soundFont->type = SOUNDFONT_CUSTOM;
    soundFont->sampleBank1 = sampleBank1;
    soundFont->sampleBank2 = sampleBank2;
    soundFont->numInstruments = numInstruments;
    soundFont->numDrums = numDrums;
    soundFont->numSfx = numSfx;
    soundFont->instrumentsCapacity = SOUNDFONT_DEFAULT_INSTRUMENT_CAPACITY;
    soundFont->drumsCapacity = SOUNDFONT_DEFAULT_DRUM_CAPACITY;
    soundFont->sfxCapacity = SOUNDFONT_DEFAULT_SFX_CAPACITY;

    while (numInstruments > soundFont->instrumentsCapacity) soundFont->instrumentsCapacity <<= 1;
    size = soundFont->instrumentsCapacity * sizeof(uintptr_t);
    soundFont->instruments = recomp_alloc(size);
    if (!soundFont->instruments) goto cleanup;
    Lib_MemSet(soundFont->instruments, 0, size);
    Lib_MemCpy(soundFont->instruments, fontData + SOUNDFONT_INSTRUMENT_OFFSET,
               numInstruments * sizeof(uintptr_t));

    while (numDrums > soundFont->drumsCapacity) soundFont->drumsCapacity <<= 1;
    size = soundFont->drumsCapacity * sizeof(uintptr_t);
    soundFont->drums = recomp_alloc(size);
    if (!soundFont->drums) goto cleanup;
    Lib_MemSet(soundFont->drums, 0, size);
    Lib_MemCpy(soundFont->drums, (void*)RELOC_TO_RAM(fontData[0], fontData),
               numDrums * sizeof(uintptr_t));

    while (numSfx > soundFont->sfxCapacity) soundFont->sfxCapacity <<= 1;
    size = soundFont->sfxCapacity * sizeof(SoundEffect);
    soundFont->soundEffects = recomp_alloc(size);
    if (!soundFont->soundEffects) goto cleanup;
    Lib_MemSet(soundFont->soundEffects, 0, size);
    Lib_MemCpy(soundFont->soundEffects, (void*)RELOC_TO_RAM(fontData[1], fontData),
               numSfx * sizeof(SoundEffect));

    return soundFont;

 cleanup:
    AudioApi_FreeSoundFont(soundFont);
    return NULL;
}

/* Public: Import vanilla font data, relocate all internal pointers (drums->envelope->sample->
 * loop/book, sfx->sample->loop/book, instruments->{low,normal,high}PitchSample->loop/book),
 * then register as new CustomSoundFont. Returns fontId or -1. */
RECOMP_EXPORT s32 AudioApi_ImportVanillaSoundFont(uintptr_t* fontData, u8 sampleBank1, u8 sampleBank2,
                                                  u8 numInstruments, u8 numDrums, u16 numSfx) {
    AudioTableEntry entry;
    CustomSoundFont* soundFont;
    Instrument* inst;
    Drum* drum;
    SoundEffect* soundEffect;
    Sample* sample;
    s32 fontId, i;

    soundFont = AudioApi_ImportVanillaSoundFontInternal(fontData, sampleBank1, sampleBank2,
                                                        numInstruments, numDrums, numSfx);
    if (soundFont == NULL) {
        return -1;
    }

    for (i = 0; i < soundFont->numDrums; i++) {
        drum = soundFont->drums[i];
        if (drum == NULL) {
            continue;
        }

        drum = soundFont->drums[i] = RELOC_TO_RAM(drum, fontData);
        drum->envelope = RELOC_TO_RAM(drum->envelope, fontData);

        sample = drum->tunedSample.sample = RELOC_TO_RAM(drum->tunedSample.sample, fontData);
        if (sample->size != 0) {
            sample->loop = RELOC_TO_RAM(sample->loop, fontData);
            sample->book = RELOC_TO_RAM(sample->book, fontData);
        }
    }

    for (i = 0; i < soundFont->numSfx; i++) {
        soundEffect = &soundFont->soundEffects[i];
        if (soundEffect == NULL || soundEffect->tunedSample.sample == NULL) {
            continue;
        }

        sample = soundEffect->tunedSample.sample = RELOC_TO_RAM(soundEffect->tunedSample.sample, fontData);
        if (sample->size != 0) {
            sample->loop = RELOC_TO_RAM(sample->loop, fontData);
            sample->book = RELOC_TO_RAM(sample->book, fontData);
        }
    }

    for (i = 0; i < MIN(soundFont->numInstruments, SOUNDFONT_MAX_INSTRUMENTS); i++) {
        inst = soundFont->instruments[i];
        if (inst == NULL) {
            continue;
        }

        inst = soundFont->instruments[i] = RELOC_TO_RAM(inst, fontData);
        inst->envelope = RELOC_TO_RAM(inst->envelope, fontData);

        sample = inst->normalPitchTunedSample.sample = RELOC_TO_RAM(inst->normalPitchTunedSample.sample, fontData);
        if (sample->size != 0) {
            sample->loop = RELOC_TO_RAM(sample->loop, fontData);
            sample->book = RELOC_TO_RAM(sample->book, fontData);
        }

        if (inst->normalRangeLo != 0) {
            sample = inst->lowPitchTunedSample.sample = RELOC_TO_RAM(inst->lowPitchTunedSample.sample, fontData);
            if (sample->size != 0) {
                sample->loop = RELOC_TO_RAM(sample->loop, fontData);
                sample->book = RELOC_TO_RAM(sample->book, fontData);
            }
        }
        if (inst->normalRangeHi != 0x7F) {
            sample = inst->highPitchTunedSample.sample = RELOC_TO_RAM(inst->highPitchTunedSample.sample, fontData);
            if (sample->size != 0) {
                sample->loop = RELOC_TO_RAM(sample->loop, fontData);
                sample->book = RELOC_TO_RAM(sample->book, fontData);
            }
        }
    }

    entry = (AudioTableEntry){
        (uintptr_t) soundFont,
        sizeof(CustomSoundFont),
        MEDIUM_CART,
        CACHE_EITHER,
        0, 0, 0,
    };

    fontId = AudioApi_AddSoundFont(&entry);

    if (fontId == -1) {
        AudioApi_FreeSoundFont(soundFont);
        return -1;
    }

    return fontId;
}

/* Set primary (bankNum=1) or secondary (bankNum=2) sample bank for a font.
 * Applies immediately to CustomSoundFonts; queues for ROM fonts. */
RECOMP_EXPORT void AudioApi_SetSoundFontSampleBank(s32 fontId, s32 bankNum, s32 bankId) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }

    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(soundFontInitQueue, AUDIOAPI_CMD_OP_SET_SAMPLEBANK,
                                    fontId, bankNum, (void**)&bankId);
        return;
    }

    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }
    if (bankId >= gAudioCtx.sampleBankTable->header.numEntries) {
        return;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];
    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;

    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        if (bankNum == 1) {
            soundFont->sampleBank1 = bankId;
        } else if (bankNum == 2) {
            soundFont->sampleBank2 = bankId;
        }
    } else {
        RecompQueue_Push(soundFontLoadQueue, AUDIOAPI_CMD_OP_SET_SAMPLEBANK, fontId, bankNum, (void**)&bankId);
    }
}

/* Append instrument to CustomSoundFont, growing array if needed. Returns instId or -1. */
s32 AudioApi_AddInstrumentInternal(CustomSoundFont* soundFont, Instrument* instrument) {
    if (soundFont->numInstruments >= soundFont->instrumentsCapacity) {
        if (!AudioApi_GrowInstrumentList(soundFont)) {
            return -1;
        }
    }
    s32 instId = soundFont->numInstruments++;
    soundFont->instruments[instId] = instrument;
    return instId;
}

/* Public: Deep-copy instrument and add to font. Immediate for custom fonts, queued for ROM. */
RECOMP_EXPORT s32 AudioApi_AddInstrument(s32 fontId, Instrument* instrument) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }
    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return -1;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    s32 instId = gAudioCtx.soundFontList[fontId].numInstruments;
    if (instId >= SOUNDFONT_MAX_INSTRUMENTS) {
        return -1;
    }

    Instrument* copy = AudioApi_CopyInstrument(instrument);
    if (!copy) {
        return -1;
    }

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        instId = AudioApi_AddInstrumentInternal(soundFont, copy);
    } else {
        RecompQueue_Push(soundFontLoadQueue, AUDIOAPI_CMD_OP_ADD_INSTRUMENT, fontId, instId, (void**)&copy);
    }
    if (instId == -1) {
        AudioApi_FreeInstrument(copy);
        return -1;
    }

    gAudioCtx.soundFontList[fontId].numInstruments = instId + 1;

    return instId;
}

/* Append drum to CustomSoundFont, growing array if needed. Returns drumId or -1. */
s32 AudioApi_AddDrumInternal(CustomSoundFont* soundFont, Drum* drum) {
    if (soundFont->numDrums >= soundFont->drumsCapacity) {
        if (!AudioApi_GrowDrumList(soundFont)) {
            return -1;
        }
    }
    s32 drumId = soundFont->numDrums++;
    soundFont->drums[drumId] = drum;
    return drumId;
}

/* Public: Deep-copy drum and add to font. Immediate for custom fonts, queued for ROM. */
RECOMP_EXPORT s32 AudioApi_AddDrum(s32 fontId, Drum* drum) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }
    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return -1;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    s32 drumId = gAudioCtx.soundFontList[fontId].numDrums;
    if (drumId >= SOUNDFONT_MAX_DRUMS) {
        return -1;
    }

    Drum* copy = AudioApi_CopyDrum(drum);
    if (!copy) {
        return -1;
    }

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        drumId = AudioApi_AddDrumInternal(soundFont, copy);
    } else {
        RecompQueue_Push(soundFontLoadQueue, AUDIOAPI_CMD_OP_ADD_DRUM, fontId, drumId, (void**)&copy);
    }
    if (drumId == -1) {
        AudioApi_FreeDrum(copy);
        return -1;
    }

    gAudioCtx.soundFontList[fontId].numDrums = drumId + 1;

    return drumId;
}

/* Append SFX to CustomSoundFont (value-copies then frees input). Returns sfxId or -1. */
s32 AudioApi_AddSoundEffectInternal(CustomSoundFont* soundFont, SoundEffect* sfx) {
    if (soundFont->numSfx >= soundFont->sfxCapacity) {
        if (!AudioApi_GrowSoundEffectList(soundFont)) {
            return -1;
        }
    }
    s32 sfxId = soundFont->numSfx++;
    soundFont->soundEffects[sfxId] = *sfx;
    recomp_free(sfx);
    return sfxId;
}

/* Public: Deep-copy SFX and add to font. Immediate for custom fonts, queued for ROM. */
RECOMP_EXPORT s32 AudioApi_AddSoundEffect(s32 fontId, SoundEffect* sfx) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }
    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return -1;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    s32 sfxId = gAudioCtx.soundFontList[fontId].numSfx;
    if (sfxId >= SOUNDFONT_MAX_SFX) {
        return -1;
    }

    SoundEffect* copy = AudioApi_CopySoundEffect(sfx);
    if (!copy) {
        return -1;
    }

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        sfxId = AudioApi_AddSoundEffectInternal(soundFont, copy);
    } else {
        RecompQueue_Push(soundFontLoadQueue, AUDIOAPI_CMD_OP_ADD_SOUNDEFFECT, fontId, sfxId, (void**)&copy);
    }
    if (sfxId == -1) {
        AudioApi_FreeSoundEffect(copy);
        return -1;
    }

    gAudioCtx.soundFontList[fontId].numSfx = sfxId + 1;

    return sfxId;
}

/* Replace drum at drumId in-place. No bounds growth; silently no-ops if drumId >= numDrums. */
void AudioApi_ReplaceDrumInternal(CustomSoundFont* soundFont, s32 drumId, Drum* drum) {
    if (drumId < soundFont->numDrums) {
        soundFont->drums[drumId] = drum;
    }
}

/* Public: Deep-copy and replace drum. Queues during QUEUEING; defers for ROM fonts. */
RECOMP_EXPORT void AudioApi_ReplaceDrum(s32 fontId, s32 drumId, Drum* drum) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    Drum* copy = AudioApi_CopyDrum(drum);
    if (!copy) return;

    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(soundFontInitQueue, AUDIOAPI_CMD_OP_REPLACE_DRUM,
                                    fontId, drumId, (void**)&copy);
        return;
    }

    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];
    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;

    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        AudioApi_ReplaceDrumInternal(soundFont, drumId, copy);
    } else {
        RecompQueue_PushIfNotQueued(soundFontLoadQueue, AUDIOAPI_CMD_OP_REPLACE_DRUM,
                                    fontId, drumId, (void**)&copy);
    }
}

/* Replace SFX at sfxId (value-copy, frees input). No-op if sfxId >= numSfx. */
void AudioApi_ReplaceSoundEffectInternal(CustomSoundFont* soundFont, s32 sfxId, SoundEffect* sfx) {
    if (sfxId < soundFont->numSfx) {
        soundFont->soundEffects[sfxId] = *sfx;
    }
    recomp_free(sfx);
}

/* Public: Deep-copy and replace SFX. Queues during QUEUEING; defers for ROM fonts. */
RECOMP_EXPORT void AudioApi_ReplaceSoundEffect(s32 fontId, s32 sfxId, SoundEffect* sfx) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    SoundEffect* copy = AudioApi_CopySoundEffect(sfx);
    if (!copy) return;

    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(soundFontInitQueue, AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT,
                                    fontId, sfxId, (void**)&copy);
        return;
    }

    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];
    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;

    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        AudioApi_ReplaceSoundEffectInternal(soundFont, sfxId, copy);
    } else {
        RecompQueue_PushIfNotQueued(soundFontLoadQueue, AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT,
                                    fontId, sfxId, (void**)&copy);
    }
}

/* Replace instrument at instId. No-op if instId >= numInstruments. */
void AudioApi_ReplaceInstrumentInternal(CustomSoundFont* soundFont, s32 instId, Instrument* instrument) {
    if (instId < soundFont->numInstruments)  {
        soundFont->instruments[instId] = instrument;
    }
}

/* Public: Deep-copy and replace instrument. Queues during QUEUEING; defers for ROM fonts. */
RECOMP_EXPORT void AudioApi_ReplaceInstrument(s32 fontId, s32 instId, Instrument* instrument) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    Instrument* copy = AudioApi_CopyInstrument(instrument);
    if (!copy) return;

    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(soundFontInitQueue, AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT,
                                    fontId, instId, (void**)&copy);
        return;
    }

    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }

    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];
    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;

    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        AudioApi_ReplaceInstrumentInternal(soundFont, instId, copy);
    } else {
        RecompQueue_PushIfNotQueued(soundFontLoadQueue, AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT,
                                    fontId, instId, (void**)&copy);
    }
}

/* Init queue drain callback. For each cmd: if font is already a CustomSoundFont in RAM,
 * apply immediately; otherwise forward to soundFontLoadQueue for deferred application.
 * REPLACE_SOUNDFONT is special-cased: re-calls AudioApi_ReplaceSoundFont and frees the copy. */
void AudioApi_SoundFontQueueDrain(RecompQueueCmd* cmd) {
    s32 fontId = cmd->arg0;
    if (fontId >= gAudioCtx.soundFontTable->header.numEntries) {
        return;
    }
    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    if (cmd->op == AUDIOAPI_CMD_OP_REPLACE_SOUNDFONT) {
        AudioApi_ReplaceSoundFont(cmd->arg0, (AudioTableEntry*) cmd->asPtr);
        recomp_free(cmd->asPtr);
        return;
    }

    CustomSoundFont* soundFont = (CustomSoundFont*)entry->romAddr;
    if (IS_KSEG0(entry->romAddr) && soundFont->type == SOUNDFONT_CUSTOM) {
        // If the font is in memory and is a CustomSoundFont, we can call the replace command now
        switch (cmd->op) {
        case AUDIOAPI_CMD_OP_SET_SAMPLEBANK:
            AudioApi_SetSoundFontSampleBank(cmd->arg0, cmd->arg1, cmd->asInt);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_DRUM:
            AudioApi_ReplaceDrumInternal(soundFont, cmd->arg1, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT:
            AudioApi_ReplaceSoundEffectInternal(soundFont, cmd->arg1, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT:
            AudioApi_ReplaceInstrumentInternal(soundFont, cmd->arg1, cmd->asPtr);
            break;
        }
    } else {
        // Otherwise we need to move it to load queue to apply once the font is loaded
        switch (cmd->op) {
        case AUDIOAPI_CMD_OP_SET_SAMPLEBANK:
        case AUDIOAPI_CMD_OP_ADD_DRUM:
        case AUDIOAPI_CMD_OP_REPLACE_DRUM:
        case AUDIOAPI_CMD_OP_ADD_SOUNDEFFECT:
        case AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT:
        case AUDIOAPI_CMD_OP_ADD_INSTRUMENT:
        case AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT:
            RecompQueue_PushIfNotQueued(soundFontLoadQueue, cmd->op, cmd->arg0, cmd->arg1, &cmd->data);
            break;
        }
    }
}

/* Scan soundFontLoadQueue for all entries matching fontId and apply them to the
 * now-loaded CustomSoundFont. Called from AudioLoad_RelocateFont for vanilla fonts. */
void AudioApi_ApplySoundFontChanges(s32 fontId, CustomSoundFont* customSoundFont) {
    RecompQueueCmd* cmd;
    for (s32 i = 0; i < soundFontLoadQueue->numEntries; i++) {
        cmd = &soundFontLoadQueue->entries[i];
        if (cmd->arg0 != (u32)fontId) {
            continue;
        }
        switch (cmd->op) {
        case AUDIOAPI_CMD_OP_ADD_DRUM:
            AudioApi_AddDrumInternal(customSoundFont, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_DRUM:
            AudioApi_ReplaceDrumInternal(customSoundFont, cmd->arg1, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_ADD_SOUNDEFFECT:
            AudioApi_AddSoundEffectInternal(customSoundFont, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_SOUNDEFFECT:
            AudioApi_ReplaceSoundEffectInternal(customSoundFont, cmd->arg1, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_ADD_INSTRUMENT:
            AudioApi_AddInstrumentInternal(customSoundFont, cmd->asPtr);
            break;
        case AUDIOAPI_CMD_OP_REPLACE_INSTRUMENT:
            AudioApi_ReplaceInstrumentInternal(customSoundFont, cmd->arg1, cmd->asPtr);
            break;
        }
    }
}

/**
 * RECOMP_PATCH: Replaces vanilla AudioLoad_RelocateFont.
 *
 * Called by the audio engine when a soundfont finishes loading from ROM.
 * For SOUNDFONT_VANILLA: imports into CustomSoundFont via ImportVanillaSoundFontInternal,
 * then applies any queued load-queue changes (AudioApi_ApplySoundFontChanges).
 * Then for ALL fonts: relocates every drum/sfx/instrument and their nested
 * envelopes + samples (via AudioLoad_RelocateSample). Items in the transient audio heap
 * (IS_AUDIO_HEAP_MEMORY) are deep-copied out to permanent recomp_alloc memory.
 * Finally updates gAudioCtx.soundFontList[fontId] with relocated pointers/counts,
 * frees transient buffers, sets entry->romAddr to the permanent CustomSoundFont,
 * and fires the AudioApi_SoundFontLoaded event.
 */
RECOMP_PATCH void AudioLoad_RelocateFont(s32 fontId, void* fontDataStartAddr, SampleBankRelocInfo* sampleBankReloc) {
    CustomSoundFont* fontData = (CustomSoundFont*)fontDataStartAddr;
    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];
    Instrument* inst;
    Drum* drum;
    SoundEffect* soundEffect;
    s32 i;

    // We've just loaded this font from ROM or callback, so apply any changes from our load queue
    if (fontData->type == SOUNDFONT_VANILLA) {
        u8 sampleBank1 = (entry->shortData1 & 0xFF00) >> 8;
        u8 sampleBank2 = (entry->shortData1 & 0xFF);
        u8 numInstruments = (entry->shortData2 & 0xFF00) >> 8;
        u8 numDrums = (entry->shortData2 & 0xFF);
        u16 numSfx = (entry->shortData3);

        fontData = AudioApi_ImportVanillaSoundFontInternal((uintptr_t*)fontDataStartAddr, sampleBank1, sampleBank2,
                                                           numInstruments, numDrums, numSfx);

        AudioApi_ApplySoundFontChanges(fontId, fontData);
    }

    for (i = 0; i < fontData->numDrums; i++) {
        drum = fontData->drums[i];
        if (drum == NULL) {
            continue;
        }

        drum = fontData->drums[i] = RELOC_TO_RAM(drum, fontDataStartAddr);
        drum->envelope = RELOC_TO_RAM(drum->envelope, fontDataStartAddr);
        drum->isRelocated = true;

        AudioLoad_RelocateSample(&drum->tunedSample, fontDataStartAddr, sampleBankReloc);

        if (IS_AUDIO_HEAP_MEMORY(drum)) {
            fontData->drums[i] = AudioApi_CopyDrum(drum);
        }
    }

    for (i = 0; i < fontData->numSfx; i++) {
        soundEffect = &fontData->soundEffects[i];
        if (soundEffect == NULL || soundEffect->tunedSample.sample == NULL) {
            continue;
        }

        AudioLoad_RelocateSample(&soundEffect->tunedSample, fontDataStartAddr, sampleBankReloc);

        if (IS_AUDIO_HEAP_MEMORY(soundEffect->tunedSample.sample)) {
            soundEffect->tunedSample.sample = AudioApi_CopySample(soundEffect->tunedSample.sample);
        }
    }

    for (i = 0; i < MIN(fontData->numInstruments, SOUNDFONT_MAX_INSTRUMENTS); i++) {
        inst = fontData->instruments[i];
        if (inst == NULL) {
            continue;
        }

        inst = fontData->instruments[i] = RELOC_TO_RAM(inst, fontDataStartAddr);
        inst->envelope = RELOC_TO_RAM(inst->envelope, fontDataStartAddr);
        inst->isRelocated = true;

        AudioLoad_RelocateSample(&inst->normalPitchTunedSample, fontDataStartAddr, sampleBankReloc);

        if (inst->normalRangeLo != 0) {
            AudioLoad_RelocateSample(&inst->lowPitchTunedSample, fontDataStartAddr, sampleBankReloc);
        }
        if (inst->normalRangeHi != 0x7F) {
            AudioLoad_RelocateSample(&inst->highPitchTunedSample, fontDataStartAddr, sampleBankReloc);
        }

        if (IS_AUDIO_HEAP_MEMORY(inst)) {
            fontData->instruments[i] = AudioApi_CopyInstrument(inst);
        }
    }

    // Update counts after applying any queued changes
    gAudioCtx.soundFontList[fontId].numInstruments = fontData->numInstruments;
    gAudioCtx.soundFontList[fontId].numDrums = fontData->numDrums;
    gAudioCtx.soundFontList[fontId].numSfx = fontData->numSfx;

    // Store the relocated pointers
    gAudioCtx.soundFontList[fontId].drums = fontData->drums;
    gAudioCtx.soundFontList[fontId].soundEffects = fontData->soundEffects;
    gAudioCtx.soundFontList[fontId].instruments = fontData->instruments;

    // Free temporary memory
    if (IS_AUDIO_HEAP_MEMORY(fontDataStartAddr)) {
        AudioHeap_LoadBufferFree(FONT_TABLE, fontId);
    } else if (IS_DMA_CALLBACK_DEV_ADDR(entry->romAddr)) {
        // recomp_free(fontDataStartAddr);
    }

    // If this soundfont was loaded from ROM or a callback, update the entry's romAddr to our new permanent memory.
    if (!IS_KSEG0(entry->romAddr)) {
        entry->romAddr = (uintptr_t)fontData;
    }

    // Dispatch loaded event
    AudioApi_SoundFontLoaded(fontId, (u8*)fontData);
}

/**
 * RECOMP_PATCH: Relocate a TunedSample's Sample pointer and its sub-pointers (loop, book).
 * Resolves sample bank base address: medium==0 -> bank1, medium==1 -> bank2.
 * For DMA callback addresses, wraps via AudioApi_AddDmaSubCallback instead of direct reloc.
 * Sets sample->isRelocated=true to prevent double-relocation.
 */
RECOMP_PATCH void AudioLoad_RelocateSample(TunedSample* tunedSample, void* fontData, SampleBankRelocInfo* sampleBankReloc) {
    Sample* sample = tunedSample->sample = RELOC_TO_RAM(tunedSample->sample, fontData);
    uintptr_t baseAddr;

    if ((sample->size != 0) && (sample->isRelocated != true)) {
        sample->loop = RELOC_TO_RAM(sample->loop, fontData);
        sample->book = RELOC_TO_RAM(sample->book, fontData);
        sample->isRelocated = true;

        if (sample->medium == 0) {
            baseAddr = sampleBankReloc->baseAddr1;
            sample->medium = sampleBankReloc->medium1;
        } else if (sample->medium == 1) {
            baseAddr = sampleBankReloc->baseAddr2;
            sample->medium = sampleBankReloc->medium2;
        } else {
            return;
        }

        if (IS_DMA_CALLBACK_DEV_ADDR(baseAddr)) {
            sample->sampleAddr = (u8*)AudioApi_AddDmaSubCallback(baseAddr, (uintptr_t)sample->sampleAddr, 0);
        } else {
            sample->sampleAddr = RELOC_TO_RAM(sample->sampleAddr, baseAddr);
        }
    }
}

// ======== MEMORY MANAGEMENT: Dynamic array growth + deep copy + ref-counted free ========

/* Double the capacity of ALL global soundfont tables (soundFontTable, soundFontList,
 * sExtSoundFontLoadStatus). Copies old data, frees old allocs if recomp-owned. */
bool AudioApi_GrowSoundFontTables() {
    u16 oldCapacity = soundFontTableCapacity;
    u16 newCapacity = soundFontTableCapacity << 1;
    size_t oldSize, newSize;
    AudioTable* newSoundFontTable = NULL;
    SoundFont* newSoundFontList = NULL;
    u8* newSoundFontLoadStatus = NULL;

    // Grow gAudioCtx.soundFontTable
    oldSize = sizeof(AudioTableHeader) + oldCapacity * sizeof(AudioTableEntry);
    newSize = sizeof(AudioTableHeader) + newCapacity * sizeof(AudioTableEntry);
    newSoundFontTable = recomp_alloc(newSize);
    if (!newSoundFontTable) {
        goto cleanup;
    }
    Lib_MemSet(newSoundFontTable, 0, newSize);
    Lib_MemCpy(newSoundFontTable, gAudioCtx.soundFontTable, oldSize);

    // Grow gAudioCtx.soundFontList
    oldSize = sizeof(SoundFont) * oldCapacity;
    newSize = sizeof(SoundFont) * newCapacity;
    newSoundFontList = recomp_alloc(newSize);
    if (!newSoundFontList) {
        goto cleanup;
    }
    Lib_MemSet(newSoundFontList, 0, newSize);
    Lib_MemCpy(newSoundFontList, gAudioCtx.soundFontList, oldSize);

    // Grow sExtSoundFontLoadStatus
    oldSize = sizeof(u8) * oldCapacity;
    newSize = sizeof(u8) * newCapacity;
    newSoundFontLoadStatus = recomp_alloc(newSize);
    if (!newSoundFontLoadStatus) {
        goto cleanup;
    }
    Lib_MemSet(newSoundFontLoadStatus, 0, newSize);
    Lib_MemCpy(newSoundFontLoadStatus, sExtSoundFontLoadStatus, oldSize);

    // Free old tables
    if (IS_RECOMP_ALLOC(gAudioCtx.soundFontTable)) recomp_free(gAudioCtx.soundFontTable);
    if (IS_RECOMP_ALLOC(gAudioCtx.soundFontList)) recomp_free(gAudioCtx.soundFontList);
    if (IS_RECOMP_ALLOC(sExtSoundFontLoadStatus)) recomp_free(sExtSoundFontLoadStatus);

    // Store new tables
    recomp_printf("AudioApi: Resized soundfont tables to %d\n", newCapacity);
    gAudioCtx.soundFontTable = newSoundFontTable;
    gAudioCtx.soundFontList = newSoundFontList;
    sExtSoundFontLoadStatus = newSoundFontLoadStatus;
    soundFontTableCapacity = newCapacity;
    return true;

 cleanup:
    recomp_printf("AudioApi: Error resizing soundfont table to %d\n", newCapacity);
    if (newSoundFontTable != NULL) {
        recomp_free(newSoundFontTable);
    }
    if (newSoundFontList != NULL) {
        recomp_free(newSoundFontList);
    }
    if (newSoundFontLoadStatus != NULL) {
        recomp_free(newSoundFontLoadStatus);
    }
    return false;
}

/* Double a CustomSoundFont's instruments array capacity. */
bool AudioApi_GrowInstrumentList(CustomSoundFont* soundFont) {
    Instrument** newInstList = NULL;
    u16 oldCapacity = soundFont->instrumentsCapacity;
    u16 newCapacity = soundFont->instrumentsCapacity << 1;
    size_t oldSize = sizeof(uintptr_t) * oldCapacity;
    size_t newSize = sizeof(uintptr_t) * newCapacity;

    newInstList = recomp_alloc(newSize);
    if (!newInstList) {
        return false;
    }
    Lib_MemSet(newInstList, 0, newSize);
    Lib_MemCpy(newInstList, soundFont->instruments, oldSize);

    if (IS_RECOMP_ALLOC(soundFont->instruments)) {
        recomp_free(soundFont->instruments);
    }
    soundFont->instrumentsCapacity = newCapacity;
    soundFont->instruments = newInstList;
    return true;
}

/* Double a CustomSoundFont's drums array capacity. */
bool AudioApi_GrowDrumList(CustomSoundFont* soundFont) {
    Drum** newDrumList = NULL;
    u16 oldCapacity = soundFont->drumsCapacity;
    u16 newCapacity = soundFont->drumsCapacity << 1;
    size_t oldSize = sizeof(uintptr_t) * oldCapacity;
    size_t newSize = sizeof(uintptr_t) * newCapacity;

    newDrumList = recomp_alloc(newSize);
    if (!newDrumList) {
        return false;
    }
    Lib_MemSet(newDrumList, 0, newSize);
    Lib_MemCpy(newDrumList, soundFont->drums, oldSize);

    if (IS_RECOMP_ALLOC(soundFont->drums)) {
        recomp_free(soundFont->drums);
    }
    soundFont->drumsCapacity = newCapacity;
    soundFont->drums = newDrumList;
    return true;
}

/* Double a CustomSoundFont's soundEffects array capacity. */
bool AudioApi_GrowSoundEffectList(CustomSoundFont* soundFont) {
    SoundEffect* newSfxList = NULL;
    u16 oldCapacity = soundFont->sfxCapacity;
    u16 newCapacity = soundFont->sfxCapacity << 1;
    size_t oldSize = sizeof(SoundEffect) * oldCapacity;
    size_t newSize = sizeof(SoundEffect) * newCapacity;

    newSfxList = recomp_alloc(newSize);
    if (!newSfxList) {
        return false;
    }
    Lib_MemSet(newSfxList, 0, newSize);
    Lib_MemCpy(newSfxList, soundFont->soundEffects, oldSize);

    if (IS_RECOMP_ALLOC(soundFont->soundEffects)) {
        recomp_free(soundFont->soundEffects);
    }
    soundFont->sfxCapacity = newCapacity;
    soundFont->soundEffects = newSfxList;
    return true;
}

/* Deep-copy a Drum: copies struct, deep-copies sample (via CopySample), and copies
 * envelope array (scans for ADSR_DISABLE/ADSR_HANG terminator to determine length). */
Drum* AudioApi_CopyDrum(Drum* src) {
    if (!src) return NULL;

    Drum* copy = recomp_alloc(sizeof(Drum));
    if (!copy) return NULL;

    Lib_MemCpy(copy, src, sizeof(Drum));
    copy->isRelocated = true;

    if (src->tunedSample.sample) {
        copy->tunedSample.sample = AudioApi_CopySample(src->tunedSample.sample);
        if (!copy->tunedSample.sample) {
            AudioApi_FreeDrum(copy);
            return NULL;
        }
        copy->isRelocated &= copy->tunedSample.sample->isRelocated;
    }

    if (src->envelope) {
        size_t envCount = 0;
        while (src->envelope[envCount].delay != ADSR_DISABLE && src->envelope[envCount].delay != ADSR_HANG) {
            envCount++;
        }
        envCount++;

        copy->envelope = recomp_alloc(sizeof(EnvelopePoint) * envCount);
        if (!copy->envelope) {
            AudioApi_FreeDrum(copy);
            return NULL;
        }
        Lib_MemCpy(copy->envelope, src->envelope, sizeof(EnvelopePoint) * envCount);
    }

    return copy;
}

/* Deep-copy a SoundEffect: copies struct and deep-copies its sample. */
SoundEffect* AudioApi_CopySoundEffect(SoundEffect* src) {
    if (!src) return NULL;

    SoundEffect* copy = recomp_alloc(sizeof(SoundEffect));
    if (!copy) return NULL;

    Lib_MemCpy(copy, src, sizeof(SoundEffect));

    if (src->tunedSample.sample) {
        copy->tunedSample.sample = AudioApi_CopySample(src->tunedSample.sample);
        if (!copy->tunedSample.sample) {
            AudioApi_FreeSoundEffect(copy);
            return NULL;
        }
    }

    return copy;
}

/* Deep-copy an Instrument: copies struct, envelope, and up to 3 TunedSamples
 * (lowPitch, normalPitch, highPitch). isRelocated is AND'd with all sample reloc states. */
Instrument* AudioApi_CopyInstrument(Instrument* src) {
    if (!src) return NULL;

    Instrument* copy = recomp_alloc(sizeof(Instrument));
    if (!copy) return NULL;

    Lib_MemCpy(copy, src, sizeof(Instrument));
    copy->isRelocated = true;

    if (src->envelope) {
        size_t envCount = 0;
        while (src->envelope[envCount].delay != ADSR_DISABLE && src->envelope[envCount].delay != ADSR_HANG) {
            envCount++;
        }
        envCount++;

        copy->envelope = recomp_alloc(sizeof(EnvelopePoint) * envCount);
        if (!copy->envelope) {
            AudioApi_FreeInstrument(copy);
            return NULL;
        }
        Lib_MemCpy(copy->envelope, src->envelope, sizeof(EnvelopePoint) * envCount);
    }

    if (src->lowPitchTunedSample.sample) {
        copy->lowPitchTunedSample.sample = AudioApi_CopySample(src->lowPitchTunedSample.sample);
        if (!copy->lowPitchTunedSample.sample) {
            AudioApi_FreeInstrument(copy);
            return NULL;
        }
        copy->isRelocated &= copy->lowPitchTunedSample.sample->isRelocated;
    }

    if (src->normalPitchTunedSample.sample) {
        copy->normalPitchTunedSample.sample = AudioApi_CopySample(src->normalPitchTunedSample.sample);
        if (!copy->normalPitchTunedSample.sample) {
            AudioApi_FreeInstrument(copy);
            return NULL;
        }
        copy->isRelocated &= copy->normalPitchTunedSample.sample->isRelocated;
    }

    if (src->highPitchTunedSample.sample) {
        copy->highPitchTunedSample.sample = AudioApi_CopySample(src->highPitchTunedSample.sample);
        if (!copy->highPitchTunedSample.sample) {
            AudioApi_FreeInstrument(copy);
            return NULL;
        }
        copy->isRelocated &= copy->highPitchTunedSample.sample->isRelocated;
    }

    return copy;
}

/* Compute FNV-32a hash of a Sample for dedup. Hashes the Sample struct (minus last 2 ptrs),
 * plus the loop data and ADPCM codebook data. Used by CopySample to avoid duplicate heap allocs. */
Fnv32_t AudioApi_HashSample(Sample* sample) {
    if (!sample) return 0;

    Fnv32_t hval = FNV1_32A_INIT;
    hval = fnv_32a_buf(sample, sizeof(Sample) - sizeof(uintptr_t) * 2, hval);

    if (sample->loop) {
        size_t loopSize = (sample->loop->header.count != 0) ? sizeof(AdpcmLoop) : sizeof(AdpcmLoopHeader);
        hval = fnv_32a_buf(sample->loop, loopSize, hval);
    }

    if (sample->book) {
        s32 order = sample->book->header.order;
        s32 numPredictors = sample->book->header.numPredictors;
        size_t bookSize = sizeof(AdpcmBookHeader) + sizeof(s16) * 8 * order * numPredictors;
        hval = fnv_32a_buf(sample->book, bookSize, hval);
    }

    return hval;
}


/* Deep-copy a Sample with ADPCM dedup. For CODEC_ADPCM/CODEC_SMALL_ADPCM, hashes the sample
 * and returns an existing ref-counted copy if found in sampleHashmap. Otherwise allocs new
 * Sample + loop (AdpcmLoop or just header if count==0) + book (header + 8*order*numPredictors
 * s16 coefficients). KSEG0 sampleAddr -> sets medium=MEDIUM_CART, isRelocated=true. */
Sample* AudioApi_CopySample(Sample* src) {
    if (!src) return NULL;

    Fnv32_t hval = 0;
    uintptr_t dupe;

    // ADPCM samples need to have their codeBook and predictorState data moved onto the audio heap.
    // To avoid having duplicated data on the heap, we will hash the sample struct and return an
    // existing instance if available.
    if (src->codec == CODEC_ADPCM || src->codec == CODEC_SMALL_ADPCM) {
        hval = AudioApi_HashSample(src);
        if (recomputil_u32_value_hashmap_get(sampleHashmap, hval, &dupe)) {
            refcounter_inc((void*)dupe);
            return (Sample*)dupe;
        }
    }

    Sample* copy = recomp_alloc(sizeof(Sample));
    if (!copy) return NULL;

    Lib_MemCpy(copy, src, sizeof(Sample));

    if (IS_KSEG0(src->sampleAddr)) {
        // Custom samples are always medium cart, never preloaded, and already relocated
        copy->medium = MEDIUM_CART;
        copy->unk_bit26 = false;
        copy->isRelocated = true;
    } else {
        copy->isRelocated = false;
    }

    if (src->loop) {
        // s16 predictorState[16] only exists if count != 0.
        size_t loopSize = (src->loop->header.count != 0) ? sizeof(AdpcmLoop) : sizeof(AdpcmLoopHeader);

        copy->loop = recomp_alloc(loopSize);
        if (!copy->loop) {
            AudioApi_FreeSample(copy);
            return NULL;
        }
        Lib_MemCpy(copy->loop, src->loop, loopSize);
    }

    if (src->book) {
        s32 order = src->book->header.order;
        s32 numPredictors = src->book->header.numPredictors;
        size_t bookSize = sizeof(AdpcmBookHeader) + sizeof(s16) * 8 * order * numPredictors;

        copy->book = recomp_alloc(bookSize);
        if (!copy->book) {
            AudioApi_FreeSample(copy);
            return NULL;
        }
        Lib_MemCpy(copy->book, src->book, bookSize);
    }

    if (hval) {
        refcounter_inc(copy);
        recomputil_u32_value_hashmap_insert(sampleHashmap, hval, (uintptr_t)copy);
    }

    return copy;
}

/* Free a CustomSoundFont and its arrays (does NOT free individual instruments/drums/samples). */
void AudioApi_FreeSoundFont(CustomSoundFont* soundFont) {
    if (!soundFont) return;
    if (soundFont->instruments) recomp_free(soundFont->instruments);
    if (soundFont->drums) recomp_free(soundFont->drums);
    if (soundFont->soundEffects) recomp_free(soundFont->soundEffects);
    recomp_free(soundFont);
}

/* Free drum, its sample (ref-counted), and its envelope. */
void AudioApi_FreeDrum(Drum* drum) {
    if (!drum) return;
    if (drum->tunedSample.sample) AudioApi_FreeSample(drum->tunedSample.sample);
    if (drum->envelope) recomp_free(drum->envelope);
    recomp_free(drum);
}

/* Free SFX and its sample (ref-counted). */
void AudioApi_FreeSoundEffect(SoundEffect* sfx) {
    if (!sfx) return;
    if (sfx->tunedSample.sample) AudioApi_FreeSample(sfx->tunedSample.sample);
    recomp_free(sfx);
}

/* Free instrument, its envelope, and all 3 pitch region samples (ref-counted). */
void AudioApi_FreeInstrument(Instrument* instrument) {
    if (!instrument) return;
    if (instrument->envelope) recomp_free(instrument->envelope);
    if (instrument->lowPitchTunedSample.sample) AudioApi_FreeSample(instrument->lowPitchTunedSample.sample);
    if (instrument->normalPitchTunedSample.sample) AudioApi_FreeSample(instrument->normalPitchTunedSample.sample);
    if (instrument->highPitchTunedSample.sample) AudioApi_FreeSample(instrument->highPitchTunedSample.sample);
    recomp_free(instrument);
}

/* Ref-counted free: decrements refcount, only frees sample+loop+book when refcount hits 0. */
void AudioApi_FreeSample(Sample* sample) {
    if (!sample) return;
    if (refcounter_dec(sample) > 0) return;
    if (sample->loop) recomp_free(sample->loop);
    if (sample->book) recomp_free(sample->book);
    recomp_free(sample);
}
