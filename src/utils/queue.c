#include <utils/queue.h>
#include <recomp/modding.h>
#include <recomp/recomputils.h>

/**
 * Generic dynamic-array command queue for N64 recomp mods.
 * Inspired by MM's global audio command queue (AudioCmd).
 *
 * Struct layout (see queue.h):
 *   RecompQueueCmd { u32 op, arg0, arg1; union { void* data/asPtr; f32 asFloat; s32 asInt; u32 asUInt; u16 asUShort; s8/u8 } }
 *   RecompQueue    { RecompQueueCmd* entries; u16 numEntries, capacity; }
 *
 * Lifecycle: Create -> Push(es) -> Drain (calls callback per entry, resets count) -> Destroy.
 * Memory: uses recomp_alloc/recomp_free (host-side allocator). Grows by 2x, never shrinks.
 * Push stores op + two u32 args + a type-punned union value (deref'd from void** data, NULL-safe).
 * Drain iterates entries in FIFO order, invokes user callback, then resets numEntries to 0.
 * Not thread-safe. Designed for single-threaded init/update registration patterns.
 */

#define QUEUE_INITIAL_CAPACITY 16

/* Allocate queue + initial entries buffer (16 slots). Returns NULL on alloc failure. */
RecompQueue* RecompQueue_Create() {
    RecompQueue* queue = recomp_alloc(sizeof(RecompQueue));
    if (!queue) return NULL;

    queue->entries = recomp_alloc(sizeof(RecompQueueCmd) * QUEUE_INITIAL_CAPACITY);
    if (!queue->entries) {
        recomp_free(queue);
        return NULL;
    }

    queue->numEntries = 0;
    queue->capacity = QUEUE_INITIAL_CAPACITY;
    return queue;
}

/* Double capacity: alloc new buffer, zero-fill, copy old entries, free old. Returns false on OOM. */
bool RecompQueue_Grow(RecompQueue* queue) {
    u16 oldCapacity = queue->capacity;
    u16 newCapacity = queue->capacity << 1; // capacity *= 2; u16 max = 65535
    size_t oldSize = sizeof(RecompQueueCmd) * oldCapacity;
    size_t newSize = sizeof(RecompQueueCmd) * newCapacity;

    RecompQueueCmd* newEntries = recomp_alloc(newSize);
    if (!newEntries) {
        return false;
    }
    Lib_MemSet(newEntries, 0, newSize);  // MM engine memset
    Lib_MemCpy(newEntries, queue->entries, oldSize); // MM engine memcpy
    recomp_free(queue->entries);

    queue->entries = newEntries;
    queue->capacity = newCapacity;
    return true;
}

/* Free entries array + queue struct. NULL-safe. Does NOT free data ptrs inside entries. */
void RecompQueue_Destroy(RecompQueue* queue) {
    if (!queue) return;
    recomp_free(queue->entries);
    recomp_free(queue);
}

/* Append cmd {op, arg0, arg1, *data} to queue. Auto-grows on full. data=NULL stores NULL in union. */
bool RecompQueue_Push(RecompQueue* queue, u32 op, u32 arg0, u32 arg1, void** data) {
    if (queue->numEntries >= queue->capacity) {
        if (!RecompQueue_Grow(queue)) {
            return false;
        }
    }
    queue->entries[queue->numEntries++] = (RecompQueueCmd){ op, arg0, arg1, (data ? *data : NULL) };
    return true;
}

/* Push only if no existing entry matches (op, arg0, arg1). Prevents duplicate commands. */
bool RecompQueue_PushIfNotQueued(RecompQueue* queue, u32 op, u32 arg0, u32 arg1, void** data) {
    if (!RecompQueue_IsCmdNotQueued(queue, op, arg0, arg1)) {
        return false; // duplicate found, skip
    }
    return RecompQueue_Push(queue, op, arg0, arg1, data);
}

/* Linear scan: returns true if NO entry matches (op, arg0, arg1), false if duplicate exists. */
bool RecompQueue_IsCmdNotQueued(RecompQueue* queue, u32 op, u32 arg0, u32 arg1) {
    for (s32 i = 0; i < queue->numEntries; i++) {
        RecompQueueCmd* cmd = &queue->entries[i];
        if (cmd->op == op && cmd->arg0 == arg0 && cmd->arg1 == arg1) {
            return false;
        }
    }
    return true;
}

/* Process all entries FIFO via drainFunc callback, then reset count to 0 (entries stay allocated). */
void RecompQueue_Drain(RecompQueue* queue, void (*drainFunc)(RecompQueueCmd* cmd)) {
    for (s32 i = 0; i < queue->numEntries; i++) {
        drainFunc(&queue->entries[i]);
    }
    queue->numEntries = 0;
}

/* Discard all entries without processing. Buffer stays allocated at current capacity. */
void RecompQueue_Empty(RecompQueue* queue) {
    queue->numEntries = 0;
}
