#include <core/load_status.h>
#include <recomp/modding.h>
#include <recomp/recompdata.h>
#include <recomp/recomputils.h>

/*
 * load_status.c — Fake load-status tracking and cache simulation for mod-loaded audio resources.
 *
 * Problem: In vanilla, sequences/soundfonts are DMA'd from ROM into heap pools, and the game
 * tracks their load state via fixed-size arrays (gAudioCtx.seqLoadStatus/fontLoadStatus). Mod
 * resources live permanently in RAM (no DMA needed), but the game's logic breaks if resources
 * appear permanently loaded (e.g. persistent cache pop = sequence stop). The vanilla arrays are
 * also too small for modded table indices.
 *
 * Solution: Intercept all load-status and cache-search functions with fakes that behave like
 * vanilla but back onto extensible data structures. Three caches are maintained:
 *
 *   Cache       | Backing              | Semantics
 *   ------------+----------------------+--------------------------------------------------
 *   permanent   | U32Hashset           | Survives heap reset. Vanilla: seq 0, fonts 0/1.
 *   persistent  | Stack array (16 max) | LIFO; popped to stop sequences. Flushed on reset.
 *   loaded      | U32Hashset           | "Ever loaded" flag — prevents double-init of RAM data.
 *
 * Cache keys are encoded as (tableType << 24 | realId) to form a unique u32 per resource.
 *
 * Extended load-status arrays (sExtSeqLoadStatus / sExtSoundFontLoadStatus) start as aliases
 * to the vanilla arrays and are swapped to larger allocations by load.c when tables grow.
 *
 * Special IDs: 0xFF and 0xFE are sentinel values used by the game for "no sequence" / "previous
 * sequence" — these are short-circuited to LOAD_STATUS_PERMANENT to avoid table lookups.
 */

#define MAX_PERSISTENT_CACHE_ENTRIES 16

typedef struct PersistentCacheEntry {
    s16 tableType;
    u32 id;
} PersistentCacheEntry;

typedef struct PersistentCache {
    s16 numEntries;
    PersistentCacheEntry entries[MAX_PERSISTENT_CACHE_ENTRIES];
} PersistentCache;

PersistentCache persistentCache;
U32HashsetHandle permanentCache;  /* survives heap reset */
U32HashsetHandle loadedCache;     /* "ever loaded" — prevents double-init */

/* Initially alias vanilla arrays; load.c replaces these with larger allocations when needed */
u8* sExtSeqLoadStatus = gAudioCtx.seqLoadStatus;
u8* sExtSoundFontLoadStatus = gAudioCtx.fontLoadStatus;

extern AudioTable* AudioLoad_GetLoadTable(s32 tableType);
/* Resolves high-byte bank IDs (0xHH__) to actual table indices */
extern u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 id);

RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_LoadStatusInit() {
    permanentCache = recomputil_create_u32_hashset();
    loadedCache = recomputil_create_u32_hashset();
}

// ======== LOAD STATUS FUNCTIONS ========
// Get/set per-resource load status using the extended arrays.
// Sentinel IDs (0xFF=none, 0xFE=previous) are treated as always-loaded.

s32 AudioApi_GetTableEntryLoadStatus(s32 tableType, s32 id) {
    if ((id & 0xFF) == 0xFF || (id & 0xFF) == 0xFE) return LOAD_STATUS_PERMANENT;

    AudioTable* table = AudioLoad_GetLoadTable(tableType);
    u32 realId = AudioLoad_GetRealTableIndex(tableType, id);
    if (tableType == SEQUENCE_TABLE) {
        return sExtSeqLoadStatus[realId];
    } else if (tableType == FONT_TABLE) {
        return sExtSoundFontLoadStatus[realId];
    } else {
        return LOAD_STATUS_NOT_LOADED;
    }
}

void AudioApi_SetTableEntryLoadStatus(s32 tableType, s32 id, s32 status) {
    if ((id & 0xFF) == 0xFF || (id & 0xFF) == 0xFE) return;

    AudioTable* table = AudioLoad_GetLoadTable(tableType);
    u32 realId = AudioLoad_GetRealTableIndex(tableType, id);
    if (tableType == SEQUENCE_TABLE) {
        sExtSeqLoadStatus[realId] = status;
    } else if (tableType == FONT_TABLE) {
        sExtSoundFontLoadStatus[realId] = status;
    }
}

/* Reset all non-permanent entries to NOT_LOADED and clear the persistent cache stack */
RECOMP_PATCH void AudioHeap_ResetLoadStatus(void) {
    s32 i;

    persistentCache.numEntries = 0;

    for (i = 0; i < gAudioCtx.soundFontTable->header.numEntries; i++) {
        if (AudioApi_GetTableEntryLoadStatus(FONT_TABLE, i) != LOAD_STATUS_PERMANENT) {
            AudioApi_SetTableEntryLoadStatus(FONT_TABLE, i, LOAD_STATUS_NOT_LOADED);
        }
    }
    for (i = 0; i < gAudioCtx.sequenceTable->header.numEntries; i++) {
        if (AudioApi_GetTableEntryLoadStatus(SEQUENCE_TABLE, i) != LOAD_STATUS_PERMANENT) {
            AudioApi_SetTableEntryLoadStatus(SEQUENCE_TABLE, i, LOAD_STATUS_NOT_LOADED);
        }
    }
}

RECOMP_PATCH s32 AudioLoad_IsSeqLoadComplete(s32 seqId) {
    return AudioApi_GetTableEntryLoadStatus(SEQUENCE_TABLE, seqId) >= LOAD_STATUS_COMPLETE;
}

RECOMP_PATCH void AudioLoad_SetSeqLoadStatus(s32 seqId, s32 status) {
    AudioApi_SetTableEntryLoadStatus(SEQUENCE_TABLE, seqId, status);
}

RECOMP_PATCH s32 AudioLoad_IsFontLoadComplete(s32 fontId) {
    return AudioApi_GetTableEntryLoadStatus(FONT_TABLE, fontId) >= LOAD_STATUS_COMPLETE;
}

RECOMP_PATCH void AudioLoad_SetFontLoadStatus(s32 fontId, s32 status) {
    AudioApi_SetTableEntryLoadStatus(FONT_TABLE, fontId, status);
}

// ======== FAKE CACHE FUNCTIONS ========
// Vanilla searches DMA'd heap copies; these return the RAM pointer directly (stored in romAddr
// field by load.c) if the resource passes the appropriate cache-level check.

RECOMP_PATCH void* AudioHeap_SearchCaches(s32 tableType, s32 cache, s32 id) {
    AudioTable* table = AudioLoad_GetLoadTable(tableType);
    u32 realId = AudioLoad_GetRealTableIndex(tableType, id);
    u32 key = tableType << 24 | realId;

    if (!recomputil_u32_hashset_contains(permanentCache, key) && cache == CACHE_PERMANENT) {
        return NULL;
    }
    if (!recomputil_u32_hashset_contains(loadedCache, key)) {
        return NULL;
    }
    /* romAddr is repurposed to hold the RAM pointer for mod-loaded resources (KSEG0 = in RAM) */
    if (!IS_KSEG0(table->entries[realId].romAddr)) {
        return NULL;
    }
    return (void*)table->entries[realId].romAddr;
}

RECOMP_PATCH void* AudioHeap_SearchRegularCaches(s32 tableType, s32 cache, s32 id) {
    return AudioHeap_SearchCaches(tableType, CACHE_EITHER, id);
}

RECOMP_PATCH void* AudioHeap_SearchPermanentCache(s32 tableType, s32 id) {
    return AudioHeap_SearchCaches(tableType, CACHE_PERMANENT, id);
}

/* Register a resource in the appropriate cache tier(s). Called after a successful load. */
void AudioApi_PushFakeCache(s32 tableType, s32 cachePolicy, s32 id) {
    u32 realId = AudioLoad_GetRealTableIndex(tableType, id);
    u32 key = tableType << 24 | realId;
    PersistentCacheEntry* entry;
    s32 i;

    // Always push into loaded cache, this lets us only initialize an entry once
    recomputil_u32_hashset_insert(loadedCache, key);

    if (cachePolicy == CACHE_LOAD_PERMANENT) {
        recomputil_u32_hashset_insert(permanentCache, key);
    }

    if (cachePolicy == CACHE_LOAD_PERSISTENT && persistentCache.numEntries < MAX_PERSISTENT_CACHE_ENTRIES) {
        for (i = 0; i < persistentCache.numEntries; i++) {
            entry = &persistentCache.entries[i];
            if (entry->tableType == tableType && entry->id == realId) {
                return;
            }
        }

        entry = &persistentCache.entries[persistentCache.numEntries++];
        entry->tableType = tableType;
        entry->id = realId;
    }
}

/* Pop most-recent persistent entry of tableType (LIFO). This is how the game stops sequences —
 * it pops the font/seq from persistent cache, marking it unloaded and discarding font data. */
RECOMP_PATCH void AudioHeap_PopPersistentCache(s32 tableType) {
    PersistentCacheEntry* entry = NULL;
    s32 i;

    // Find the most recent entry of tableType, or return if not found
    for (i = persistentCache.numEntries - 1; i >= 0; i--) {
        if (persistentCache.entries[i].tableType == tableType) {
            entry = &persistentCache.entries[i];
            break;
        }
    }

    if (entry == NULL) {
        return;
    }

    // Discard entry and set load status
    if (tableType == FONT_TABLE) {
        AudioHeap_DiscardFont(entry->id);
    }

    AudioApi_SetTableEntryLoadStatus(tableType, entry->id, LOAD_STATUS_NOT_LOADED);

    // Decrement numEntries and shift
    for (persistentCache.numEntries--; i < persistentCache.numEntries; i++) {
        persistentCache.entries[i] = persistentCache.entries[i + 1];
    }
}
