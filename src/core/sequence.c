#include <global.h>
#include <recomp/modding.h>
#include <recomp/recomputils.h>
#include <utils/misc.h>
#include <utils/queue.h>
#include <core/heap.h>
#include <core/init.h>
#include <core/load_status.h>
#include <core/sequence_functions.h>

/**
 * @file sequence.c
 * @brief Public modding API for sequence (BGM/SFX) table management in MM Recomp.
 *
 * == Architecture ==
 * Four parallel arrays are managed, all indexed by seqId and dynamically growable (doubling):
 *   - gAudioCtx.sequenceTable   : AudioTable of AudioTableEntry {romAddr, medium, size, cachePolicy}
 *                                  For mod sequences, romAddr points to recomp_alloc'd mod memory.
 *   - gAudioCtx.sequenceFontTable: Packed lookup table mapping seqId -> font list.
 *                                  Layout: [u16 offsets[capacity] | (u8 numFonts, u8 fontIds[4])* ]
 *                                  Header region is u16 per seqId giving byte offset into data region.
 *                                  Data region is (MAX_FONTS_PER_SEQUENCE+1) bytes per seqId.
 *   - sExtSeqFlags              : u8 per seqId, bitmask (SEQ_FLAG_ENEMY, _FANFARE, _RESTORE, etc.)
 *   - sExtSeqLoadStatus         : u8 per seqId, tracks load state (see load_status.h)
 *
 * == Lifecycle / Init Phases (AudioApiInitPhase) ==
 *   NOT_READY(0) -> QUEUEING(1) -> QUEUED(2) -> READY(3)
 *   - NOT_READY: API calls return early / fail.
 *   - QUEUEING: Mods call API; mutations are enqueued (sequenceQueue) for deferred execution.
 *   - READY:    Queue drained; API calls mutate tables directly. Restore functions need this.
 *
 * == Queue System ==
 *   During QUEUEING phase, ReplaceSequence/ReplaceSequenceFont/SetSequenceFlags push commands
 *   into sequenceQueue (RecompQueue). PushIfNotQueued deduplicates by (op, arg0, arg1).
 *   AudioApi_SequenceReady() drains and destroys the queue on transition to READY.
 *
 * == Sequence ID Allocation ==
 *   AddSequence appends to the table. IDs ending in 0xFE/0xFF are skipped (NA_BGM_DISABLED/UNKNOWN
 *   sentinel values that vanilla code checks via low byte masking).
 *
 * == Sequence Loading / Relocation ==
 *   AudioApi_RelocateSequence (callback on AudioApi_SequenceLoadedInternal):
 *   - If loaded into audio heap temp buffer: copies to persistent recomp_alloc'd memory, frees buffer.
 *   - If loaded from ROM (not KSEG0): updates romAddr to point to permanent RAM copy.
 *   - Fires AudioApi_SequenceLoaded event for mod hooks to post-process sequence data.
 *
 * == Exported API Summary ==
 *   AudioApi_AddSequence(entry)              -> s32 seqId or -1; appends new sequence
 *   AudioApi_ReplaceSequence(seqId, entry)   -> void; overwrites entry (queueable)
 *   AudioApi_RestoreSequence(seqId)          -> void; restores from original ROM table
 *   AudioApi_GetSequenceFont(seqId, fontNum) -> s32 fontId or -1
 *   AudioApi_AddSequenceFont(seqId, fontId)  -> s32 new count or -1; prepends font, shifts rest
 *   AudioApi_ReplaceSequenceFont(seqId, n, fontId) -> void; replaces nth font (queueable)
 *   AudioApi_RestoreSequenceFont(seqId, n)   -> void; restores nth font from ROM table
 *   AudioApi_GetSequenceFlags(seqId)         -> u8 flags
 *   AudioApi_SetSequenceFlags(seqId, flags)  -> void; (queueable)
 *   AudioApi_RestoreSequenceFlags(seqId)     -> void; restores from original sSeqFlags[]
 */

#define MAX_FONTS_PER_SEQUENCE 4

typedef enum {
    AUDIOAPI_CMD_OP_REPLACE_SEQUENCE,
    AUDIOAPI_CMD_OP_REPLACE_SEQUENCE_FONT,
    AUDIOAPI_CMD_OP_SET_SEQUENCE_FLAGS,
} AudioApiSequenceQueueOp;

RecompQueue* sequenceQueue;
u16 sequenceTableCapacity = NA_BGM_MAX;
static bool sLoggedFrogSongReplaceIgnore = false;

void AudioApi_SequenceQueueDrain(RecompQueueCmd* cmd);
bool AudioApi_GrowSequenceTables();
u8* AudioApi_RebuildSequenceFontTable(u16 oldCapacity, u16 newCapacity);

RECOMP_DECLARE_EVENT(AudioApi_SequenceLoaded(s32 seqId, u8* ramAddr));

/* Called during QUEUEING phase. Rebuilds font table into fixed-stride format (4 fonts/seq),
 * then doubles capacity so custom seqIds start at 256 (avoids collisions with vanilla IDs). */
RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_SequenceInit() {
    sequenceQueue = RecompQueue_Create();

    u8* newSeqFontTable = AudioApi_RebuildSequenceFontTable(sequenceTableCapacity, sequenceTableCapacity);
    if (newSeqFontTable != NULL) {
        gAudioCtx.sequenceFontTable = newSeqFontTable;
    } else {
        recomp_printf("AudioApi: Error rebuilding sequence font table\n");
    }

    AudioApi_GrowSequenceTables();
    gAudioCtx.sequenceTable->header.numEntries = gAudioCtx.numSequences = sequenceTableCapacity;
}

/* Called on transition to READY phase. Executes all deferred mutations, then frees the queue. */
RECOMP_CALLBACK(".", AudioApi_ReadyInternal) void AudioApi_SequenceReady() {
    RecompQueue_Drain(sequenceQueue, AudioApi_SequenceQueueDrain);
    RecompQueue_Destroy(sequenceQueue);
}

/* Allocates next seqId, writes entry, auto-grows tables if needed.
 * Skips IDs with low byte 0xFE/0xFF (vanilla sentinel values). Not queueable. */
RECOMP_EXPORT s32 AudioApi_AddSequence(AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }
    s32 newSeqId = gAudioCtx.sequenceTable->header.numEntries;
    while ((newSeqId & 0xFF) == (NA_BGM_DISABLED & 0xFF) || (newSeqId & 0xFF) == (NA_BGM_UNKNOWN & 0xFF)) {
        newSeqId++;
    }
    if (newSeqId >= sequenceTableCapacity) {
        if (!AudioApi_GrowSequenceTables()) {
            return -1;
        }
    }
    gAudioCtx.sequenceTable->entries[newSeqId] = *entry;
    gAudioCtx.numSequences = gAudioCtx.sequenceTable->header.numEntries = newSeqId + 1;
    return newSeqId;
}

/* Overwrites an existing sequence table entry. During QUEUEING: heap-copies entry and enqueues. */
RECOMP_EXPORT void AudioApi_ReplaceSequence(s32 seqId, AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    if (seqId == NA_BGM_FROG_SONG) {
        if (!sLoggedFrogSongReplaceIgnore) {
            recomp_printf("AudioApi: Ignoring replacement for NA_BGM_FROG_SONG (seq_90)\n");
            sLoggedFrogSongReplaceIgnore = true;
        }
        return;
    }
    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        AudioTableEntry* copy = recomp_alloc(sizeof(AudioTableEntry));
        if (!copy) {
            return;
        }
        *copy = *entry;
        RecompQueue_PushIfNotQueued(sequenceQueue, AUDIOAPI_CMD_OP_REPLACE_SEQUENCE,
                                     seqId, 0, (void**)&copy);
        return;
    }
    if (seqId >= gAudioCtx.sequenceTable->header.numEntries) {
        return;
    }
    gAudioCtx.sequenceTable->entries[seqId] = *entry;
}

/* Restores entry from original ROM table (gSequenceTable). Only works READY+, vanilla IDs only. */
RECOMP_EXPORT void AudioApi_RestoreSequence(s32 seqId) {
    if (gAudioApiInitPhase < AUDIOAPI_INIT_READY) {
        return;
    }
    AudioTable* origTable = (AudioTable*)gSequenceTable;
    if (seqId >= origTable->header.numEntries) {
        return;
    }
    gAudioCtx.sequenceTable->entries[seqId] = origTable->entries[seqId];
}

/* Reads fontId at position fontNum from the font table. Returns -1 if out of bounds. */
RECOMP_EXPORT s32 AudioApi_GetSequenceFont(s32 seqId, s32 fontNum) {
    if (seqId >= gAudioCtx.sequenceTable->header.numEntries) {
        return -1;
    }

    s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    u8* entry = &gAudioCtx.sequenceFontTable[index];
    u8 numFonts = entry[0];

    if (fontNum >= numFonts) {
        return -1;
    }

    return entry[fontNum + 1];
}

/* Prepends fontId to seqId's font list (shifts existing right). Max 4 fonts. Returns new count. */
RECOMP_EXPORT s32 AudioApi_AddSequenceFont(s32 seqId, s32 fontId) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }
    if (seqId >= gAudioCtx.sequenceTable->header.numEntries) {
        return -1;
    }

    s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    u8* entry = &gAudioCtx.sequenceFontTable[index];
    u8 numFonts = entry[0];

    if (numFonts == MAX_FONTS_PER_SEQUENCE) {
        return -1;
    }

    for (s32 i = MAX_FONTS_PER_SEQUENCE; i > 1; i--) {
        entry[i] = entry[i - 1];
    }

    entry[1] = fontId;
    entry[0]++;

    return numFonts + 1;
}

/* Replaces the fontNum-th font for seqId. If fontNum >= numFonts, falls back to AddSequenceFont.
 * Font index is stored reversed: entry[numFonts - fontNum]. Queueable. */
RECOMP_EXPORT void AudioApi_ReplaceSequenceFont(s32 seqId, s32 fontNum, s32 fontId) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    if (seqId == NA_BGM_FROG_SONG) {
        return;
    }
    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(sequenceQueue, AUDIOAPI_CMD_OP_REPLACE_SEQUENCE_FONT,
                                     seqId, fontNum, (void**)&fontId);
        return;
    }
    if (seqId >= gAudioCtx.sequenceTable->header.numEntries || fontNum >= MAX_FONTS_PER_SEQUENCE) {
        return;
    }

    s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    u8* entry = &gAudioCtx.sequenceFontTable[index];
    u8 numFonts = entry[0];

    if (fontNum < 0) {
        return;
    }

    if (fontNum >= numFonts) {
        // Target has fewer fonts than expected, add the font instead
        AudioApi_AddSequenceFont(seqId, fontId);
        return;
    }

    entry[numFonts - fontNum] = fontId;
}

/* Restores fontNum-th font from original ROM font table (gSequenceFontTable). Vanilla IDs only. */
RECOMP_EXPORT void AudioApi_RestoreSequenceFont(s32 seqId, s32 fontNum) {
    if (gAudioApiInitPhase < AUDIOAPI_INIT_READY) {
        return;
    }
    if (seqId >= NA_BGM_MAX) {
        return;
    }

    s32 index = ((u16*)gSequenceFontTable)[seqId];
    u8* entry = &gSequenceFontTable[index];
    u8 numFonts = entry[0];
    u8 origFontId;

    if (fontNum >= numFonts) {
        return;
    }

    origFontId = entry[fontNum + 1];

    index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    entry = &gAudioCtx.sequenceFontTable[index];
    entry[fontNum + 1] = origFontId;
}

RECOMP_EXPORT u8 AudioApi_GetSequenceFlags(s32 seqId) {
    return AudioApi_GetSequenceFlagsInternal(seqId);
}

/* Sets per-sequence flag bitmask (SEQ_FLAG_ENEMY|FANFARE|RESTORE|RESUME|etc). Queueable. */
RECOMP_EXPORT void AudioApi_SetSequenceFlags(s32 seqId, u8 flags) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        RecompQueue_PushIfNotQueued(sequenceQueue, AUDIOAPI_CMD_OP_SET_SEQUENCE_FLAGS,
                                     seqId, 0, (void**)&flags);
        return;
    }
    AudioApi_SetSequenceFlagsInternal(seqId, flags);
}

RECOMP_EXPORT void AudioApi_RestoreSequenceFlags(s32 seqId) {
    if (gAudioApiInitPhase < AUDIOAPI_INIT_READY) {
        return;
    }
    if (seqId >= NA_BGM_MAX) {
        return;
    }
    AudioApi_SetSequenceFlagsInternal(seqId, sSeqFlags[seqId]);
}

/* Dispatch callback for RecompQueue_Drain. Executes deferred sequence mutations.
 * ReplaceSequence frees heap-copied entry after applying. */
void AudioApi_SequenceQueueDrain(RecompQueueCmd* cmd) {
    switch (cmd->op) {
    case AUDIOAPI_CMD_OP_REPLACE_SEQUENCE:
        AudioApi_ReplaceSequence(cmd->arg0, (AudioTableEntry*) cmd->asPtr);
        recomp_free(cmd->asPtr);
        break;
    case AUDIOAPI_CMD_OP_REPLACE_SEQUENCE_FONT:
        AudioApi_ReplaceSequenceFont(cmd->arg0, cmd->arg1, cmd->asInt);
        break;
    case AUDIOAPI_CMD_OP_SET_SEQUENCE_FLAGS:
        AudioApi_SetSequenceFlags(cmd->arg0, cmd->asUbyte);
        break;
    default:
        break;
    }
}

/* Post-load hook: relocates sequence data from audio heap temp buffer to persistent mod memory.
 * Also updates romAddr for ROM-loaded sequences to point to the persistent copy.
 * Finally fires AudioApi_SequenceLoaded event so mods can patch sequence data in-place. */
RECOMP_CALLBACK(".", AudioApi_SequenceLoadedInternal) void AudioApi_RelocateSequence(s32 seqId, void** ramAddrPtr) {
    if (IS_AUDIO_HEAP_MEMORY(*ramAddrPtr)) {
        size_t size = gAudioCtx.sequenceTable->entries[seqId].size;
        void* ramAddr = recomp_alloc(size);
        Lib_MemCpy(ramAddr, *ramAddrPtr, size);
        AudioHeap_LoadBufferFree(SEQUENCE_TABLE, seqId);
        *ramAddrPtr = ramAddr;
    }

    if (!IS_KSEG0(gAudioCtx.sequenceTable->entries[seqId].romAddr)) {
        gAudioCtx.sequenceTable->entries[seqId].romAddr = (uintptr_t)(*ramAddrPtr);
    }

    AudioApi_SequenceLoaded(seqId, (u8*)(*ramAddrPtr));
}

/* Doubles capacity of all 4 sequence arrays. All-or-nothing: on any alloc failure, frees
 * partial allocations and returns false. Frees old tables only if they were recomp_alloc'd. */
bool AudioApi_GrowSequenceTables() {
    u16 oldCapacity = sequenceTableCapacity;
    u16 newCapacity = sequenceTableCapacity << 1;
    size_t oldSize, newSize;
    AudioTable* newSeqTable = NULL;
    u8* newSeqFontTable = NULL;
    u8* newSeqFlags = NULL;
    u8* newSeqLoadStatus = NULL;

    // Grow gAudioCtx.sequenceTable
    oldSize = sizeof(AudioTableHeader) + oldCapacity * sizeof(AudioTableEntry);
    newSize = sizeof(AudioTableHeader) + newCapacity * sizeof(AudioTableEntry);
    newSeqTable = recomp_alloc(newSize);
    if (!newSeqTable) {
        goto cleanup;
    }
    Lib_MemSet(newSeqTable, 0, newSize);
    Lib_MemCpy(newSeqTable, gAudioCtx.sequenceTable, oldSize);

    // Grow gAudioCtx.sequenceFontTable
    newSeqFontTable = AudioApi_RebuildSequenceFontTable(oldCapacity, newCapacity);
    if (newSeqFontTable == NULL) {
        goto cleanup;
    }

    // Grow sExtSeqFlags
    oldSize = sizeof(u8) * oldCapacity;
    newSize = sizeof(u8) * newCapacity;
    newSeqFlags = recomp_alloc(newSize);
    if (!newSeqFlags) {
        goto cleanup;
    }
    Lib_MemSet(newSeqFlags, 0, newSize);
    Lib_MemCpy(newSeqFlags, sExtSeqFlags, oldSize);

    // Grow sExtSeqLoadStatus
    oldSize = sizeof(u8) * oldCapacity;
    newSize = sizeof(u8) * newCapacity;
    newSeqLoadStatus = recomp_alloc(newSize);
    if (!newSeqLoadStatus) {
        goto cleanup;
    }
    Lib_MemSet(newSeqLoadStatus, 0, newSize);
    Lib_MemCpy(newSeqLoadStatus, sExtSeqLoadStatus, oldSize);

    // Free old tables
    if (IS_RECOMP_ALLOC(gAudioCtx.sequenceTable)) recomp_free(gAudioCtx.sequenceTable);
    if (IS_RECOMP_ALLOC(gAudioCtx.sequenceFontTable)) recomp_free(gAudioCtx.sequenceFontTable);
    if (IS_RECOMP_ALLOC(sExtSeqFlags)) recomp_free(sExtSeqFlags);
    if (IS_RECOMP_ALLOC(sExtSeqLoadStatus)) recomp_free(sExtSeqLoadStatus);

    // Store new tables
    recomp_printf("AudioApi: Resized sequences tables to %d\n", newCapacity);
    gAudioCtx.sequenceTable = newSeqTable;
    gAudioCtx.sequenceFontTable = newSeqFontTable;
    sExtSeqFlags = newSeqFlags;
    sExtSeqLoadStatus = newSeqLoadStatus;
    sequenceTableCapacity = newCapacity;
    return true;

 cleanup:
    recomp_printf("AudioApi: Error resizing sequences tables to %d\n", newCapacity);
    if (newSeqTable != NULL) {
        recomp_free(newSeqTable);
    }
    if (newSeqFontTable != NULL) {
        recomp_free(newSeqFontTable);
    }
    if (newSeqFlags != NULL) {
        recomp_free(newSeqFlags);
    }
    if (newSeqLoadStatus != NULL) {
        recomp_free(newSeqLoadStatus);
    }
    return false;
}

/* Rebuilds the font table into a fixed-stride format from the vanilla variable-length format.
 * Vanilla format: u16 offsets[] -> variable-length (numFonts, fontId...) entries.
 * New format: u16 offsets[newCapacity] | (numFonts + fontIds[4]) * newCapacity (fixed 5 bytes each).
 * Total size: (2 + 5) * newCapacity bytes. Copies existing entries from old table for seqId < oldCapacity. */
u8* AudioApi_RebuildSequenceFontTable(u16 oldCapacity, u16 newCapacity) {

    size_t newSize = (sizeof(u16) + MAX_FONTS_PER_SEQUENCE + 1) * newCapacity;
    u8* newSeqFontTable = recomp_alloc(newSize);

    if (!newSeqFontTable) {
        return NULL;
    }
    Lib_MemSet(newSeqFontTable, 0, newSize);

    u16* header = (u16*)newSeqFontTable;
    u8* entries = newSeqFontTable + sizeof(u16) * newCapacity;

    for (u16 seqId = 0; seqId < newCapacity; seqId++) {
        // Write the offset into the header
        header[seqId] = (sizeof(u16) * newCapacity) + (seqId * (MAX_FONTS_PER_SEQUENCE + 1));

        // Find the entry in the old table and read the number of fonts
        if (seqId < oldCapacity) {
            s32 index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
            u8* entry = &gAudioCtx.sequenceFontTable[index];
            u8 numFonts = entry[0];

            // Copy old entry into new table
            Lib_MemCpy(entries + seqId * (MAX_FONTS_PER_SEQUENCE + 1), entry, numFonts + 1);
        }
    }

    return newSeqFontTable;
}
