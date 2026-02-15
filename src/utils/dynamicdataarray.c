/**
 * Generic dynamic array (type-erased std::vector equivalent for C).
 * Stores N elements of arbitrary fixed size in a contiguous heap buffer.
 * Uses recomp_alloc/recomp_free for memory (recomp mod runtime allocator).
 *
 * Struct: DynamicDataArray { void *data; size_t capacity, count, elementSize; }
 * Growth: 1.5x (formula: (cap+1)*3/2), default initial cap 16.
 * All element access is via byte-offset arithmetic: &data[index * elementSize].
 */
#include <utils/dynamicdataarray.h>
#include <libc/string.h>
#include <recomp/recomputils.h>
#include <utils/misc.h>

#define DEFAULT_CAPACITY 16
#define NEXT_CAPACITY(current) ((current == 0) ? DEFAULT_CAPACITY : ((current + 1) * 3 / 2))

/* Realloc to newCapacity: alloc+zero new buf, copy min(count,newCap) elements, free old. */
void resizeDynDataArr(DynamicDataArray *dArr, size_t newCapacity) {
    if (newCapacity > 0) {
        size_t newByteSize = newCapacity * dArr->elementSize;
        u32 *newData = recomp_alloc(newByteSize);
        Lib_MemSet(newData, 0, newByteSize);

        size_t min = dArr->count;
        if (min > newCapacity) {
            min = newCapacity;
        }

        size_t newCount = dArr->count;
        if (newCapacity < dArr->count) {
            newCount = newCapacity;
        }

        if (dArr->data) {
            memcpy(newData, dArr->data, min * dArr->elementSize);
            recomp_free(dArr->data);
        }

        dArr->count = newCount;
        dArr->capacity = newCapacity;
        dArr->data = newData;
    }
}

/* Zero all struct fields (does NOT free). */
void resetStruct(DynamicDataArray *dArr) {
    dArr->capacity = 0;
    dArr->count = 0;
    dArr->elementSize = 0;
    dArr->data = NULL;
}

/* Init array for elements of `elementSize` bytes; optionally pre-allocates. */
void DynDataArr_init(DynamicDataArray *dArr, size_t elementSize, size_t initialCapacity) {
    resetStruct(dArr);

    dArr->elementSize = elementSize;

    if (initialCapacity) {
        resizeDynDataArr(dArr, initialCapacity);
    }
}

/* Reset count to 0 (keeps buffer allocated). */
void DynDataArr_clear(DynamicDataArray *dArr) {
    dArr->count = 0;
}

/* Free buffer and zero struct. */
void DynDataArr_destroyMembers(DynamicDataArray *dArr) {
    recomp_free(dArr->data);
    resetStruct(dArr);
}

/* Append a zeroed slot, auto-grow if needed; returns ptr to new element. */
void *DynDataArr_createElement(DynamicDataArray *dArr) {
    if (dArr->elementSize < 1) {
        return NULL;
    }

    size_t newCount = dArr->count + 1;

    if (newCount > dArr->capacity) {
        resizeDynDataArr(dArr, NEXT_CAPACITY(dArr->capacity));
    }

    u8 *data = dArr->data;

    void *element = DynDataArr_get(dArr, dArr->count);

    dArr->count = newCount;

    return element;
}

/* Return ptr to element at `index` (no bounds check). */
void *DynDataArr_get(DynamicDataArray *dArr, size_t index) {
    if (dArr->elementSize < 1) {
        return NULL;
    }

    u8 *data = dArr->data;

    return &data[dArr->elementSize * index];
}

/* Overwrite element at `index` with `value`; returns false if out of bounds. */
bool DynDataArr_set(DynamicDataArray *dArr, size_t index, const void *value) {
    if (dArr->elementSize < 1 || index >= dArr->count) {
        return false;
    }

    memcpy(DynDataArr_get(dArr, index), value, dArr->elementSize);
    return true;
}

/* Copy `value` into a new slot at the end (push_back). */
void DynDataArr_push(DynamicDataArray *dArr, void *value) {
    if (dArr->elementSize < 1) {
        return;
    }

    memcpy(DynDataArr_createElement(dArr), value, dArr->elementSize);
}

/* Remove last element (logical only, no memset). */
bool DynDataArr_pop(DynamicDataArray *dArr) {
    if (dArr->count < 1) {
        return false;
    }

    dArr->count--;

    return true;
}

/* Remove element at `index` by shifting subsequent elements down. */
bool DynDataArr_removeByIndex(DynamicDataArray *dArr, size_t index) {
    if (dArr->elementSize < 1 || index >= dArr->count) {
        return false;
    }

    for (size_t i = index; i < dArr->count - 1; ++i) {
        memcpy(DynDataArr_get(dArr, i), DynDataArr_get(dArr, i + 1), dArr->elementSize);
    }

    dArr->count--;

    return true;
}

/* Find first element matching `value` and remove it. */
bool DynDataArr_removeByValue(DynamicDataArray *dArr, const void *value) {
    if (dArr->elementSize < 1) {
        return false;
    }

    for (size_t i = 0; i < dArr->count; ++i) {
        if (Utils_MemCmp(DynDataArr_get(dArr, i), value, dArr->elementSize) == 0) {
            DynDataArr_removeByIndex(dArr, i);
            return true;
        }
    }

    return false;
}
