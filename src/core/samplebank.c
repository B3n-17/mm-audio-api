#include <global.h>
#include <recomp/modding.h>
#include <recomp/recomputils.h>
#include <utils/misc.h>
#include <utils/queue.h>
#include <core/init.h>

/*
 * samplebank.c — Public API for adding/replacing/restoring samplebank table entries.
 *
 * Samplebanks are the raw ADPCM sample data that soundfonts reference. The vanilla game has
 * a fixed AudioTable (gSampleBankTable) with 3 entries. This file lets mods add new banks or
 * replace existing ones, with the table auto-growing (capacity doubles) when full.
 *
 * Follows the standard Audio API queue pattern:
 *   - During QUEUEING phase: commands are deferred into sampleBankQueue
 *   - During QUEUED phase (ReadyInternal): queue is drained and destroyed
 *   - After READY phase: commands execute immediately
 *
 * Note: These functions are not yet tested.
 */

#define NA_SAMPLEBANK_MAX 0x03 /* vanilla table size */

typedef enum {
    AUDIOAPI_CMD_OP_REPLACE_SAMPLEBANK,
} AudioApiSampleBankQueueOp;

RecompQueue* sampleBankQueue;
u16 sampleBankTableCapacity = NA_SAMPLEBANK_MAX; /* current allocated capacity, grows by 2x */

void AudioApi_SampleBankQueueDrain(RecompQueueCmd* cmd);
bool AudioApi_GrowSampleBankTables();

RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_SampleBankInit() {
    sampleBankQueue = RecompQueue_Create();
}

RECOMP_CALLBACK(".", AudioApi_ReadyInternal) void AudioApi_SampleBankReady() {
    RecompQueue_Drain(sampleBankQueue, AudioApi_SampleBankQueueDrain);
    RecompQueue_Destroy(sampleBankQueue);
}

/* Append a new samplebank entry to the table. Returns the new bank ID, or -1 on failure.
 * Caller must set entry->romAddr to a KSEG0 pointer to the ADPCM data in RAM. */
RECOMP_EXPORT s32 AudioApi_AddSampleBank(AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return -1;
    }

    // Find the next available bank ID
    s32 newBankId = gAudioCtx.sampleBankTable->header.numEntries;
    if (newBankId >= sampleBankTableCapacity) {
        if (!AudioApi_GrowSampleBankTables()) {
            return -1;
        }
    }

    gAudioCtx.sampleBankTable->header.numEntries++;
    gAudioCtx.sampleBankTable->entries[newBankId] = *entry;

    return newBankId;
}

/* Replace an existing samplebank entry. During QUEUEING phase, deferred via queue
 * (entry is heap-copied). PushIfNotQueued ensures only the last replace per bankId wins. */
RECOMP_EXPORT void AudioApi_ReplaceSampleBank(s32 bankId, AudioTableEntry* entry) {
    if (gAudioApiInitPhase == AUDIOAPI_INIT_NOT_READY) {
        return;
    }
    if (gAudioApiInitPhase == AUDIOAPI_INIT_QUEUEING) {
        AudioTableEntry* copy = recomp_alloc(sizeof(AudioTableEntry));
        if (!copy) {
            return;
        }
        *copy = *entry;
        RecompQueue_PushIfNotQueued(sampleBankQueue, AUDIOAPI_CMD_OP_REPLACE_SAMPLEBANK,
                                    bankId, 0, (void**)&copy);
        return;
    }
    if (bankId >= gAudioCtx.sampleBankTable->header.numEntries) {
        return;
    }
    gAudioCtx.sampleBankTable->entries[bankId] = *entry;
}

/* Restore a samplebank entry to its vanilla ROM table value. Only works after READY phase
 * since it reads from the original gSampleBankTable (not the live gAudioCtx copy). */
RECOMP_EXPORT void AudioApi_RestoreSampleBank(s32 bankId) {
    if (gAudioApiInitPhase < AUDIOAPI_INIT_READY) {
        return;
    }
    if (bankId >= gSampleBankTable.header.numEntries) {
        return;
    }

    gAudioCtx.sampleBankTable->entries[bankId] = gSampleBankTable.entries[bankId];
}

void AudioApi_SampleBankQueueDrain(RecompQueueCmd* cmd) {
    switch (cmd->op) {
    case AUDIOAPI_CMD_OP_REPLACE_SAMPLEBANK:
        AudioApi_ReplaceSampleBank(cmd->arg0, (AudioTableEntry*) cmd->asPtr);
        recomp_free(cmd->asPtr);
        break;
    default:
        break;
    }
}

/* Double the samplebank table capacity. Allocates a new table via recomp_alloc, copies
 * existing entries, and frees the old table (if it was also a recomp_alloc). */
bool AudioApi_GrowSampleBankTables() {
    u16 oldCapacity = sampleBankTableCapacity;
    u16 newCapacity = sampleBankTableCapacity << 1;
    size_t oldSize, newSize;
    AudioTable* newSampleBankTable = NULL;

    // Grow gAudioCtx.sampleBankTable
    oldSize = sizeof(AudioTableHeader) + oldCapacity * sizeof(AudioTableEntry);
    newSize = sizeof(AudioTableHeader) + newCapacity * sizeof(AudioTableEntry);
    newSampleBankTable = recomp_alloc(newSize);
    if (!newSampleBankTable) {
        goto cleanup;
    }
    Lib_MemSet(newSampleBankTable, 0, newSize);
    Lib_MemCpy(newSampleBankTable, gAudioCtx.sampleBankTable, oldSize);

    // Free old tables
    if (IS_RECOMP_ALLOC(gAudioCtx.sampleBankTable)) recomp_free(gAudioCtx.sampleBankTable);

    // Store new tables
    recomp_printf("AudioApi: Resized SampleBank tables to %d\n", newCapacity);
    gAudioCtx.sampleBankTable = newSampleBankTable;
    sampleBankTableCapacity = newCapacity;
    return true;

 cleanup:
    recomp_printf("AudioApi: Error resizing SampleBank tables to %d\n", newCapacity);
    if (newSampleBankTable != NULL) {
        recomp_free(newSampleBankTable);
    }
    return false;
}
