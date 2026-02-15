/*
 * misc.c - Shared utilities: pointer refcounting, memory/string helpers, debug hex dump, FNV-1a hash.
 *
 * Refcounter: Generic pointer reference counter backed by a recomp U32 memory hashmap (ptr->u16 count).
 *   Auto-creates entries on first inc, auto-erases on dec to 0. Thread-unsafe.
 *   init_refcounter() runs via RECOMP_CALLBACK on mod init.
 *
 * Utils_MemCmp: Custom memcmp (needed because libc memcmp may not be available in recomp env).
 * Utils_StrDup: strdup using recomp_alloc (mod heap, not libc malloc).
 * print_bytes: Hex dump to recomp_printf, 16 bytes/line, address prefix, space every 2 bytes.
 * fnv_32a_buf: Standard FNV-1a 32-bit hash. Init with FNV1_32A_INIT (0x811c9dc5). Chainable via hval param.
 */
#include <utils/misc.h>
#include <libc/string.h>
#include <recomp/recompdata.h>
#include <recomp/recomputils.h>

/* Global refcount store: maps pointer (as u32 key) -> u16 count. */
static U32MemoryHashmapHandle refcounter;

/* Auto-called on mod init by recomp runtime. */
RECOMP_CALLBACK("*", recomp_on_init) void init_refcounter() {
    refcounter = recomputil_create_u32_memory_hashmap(sizeof(u16));
}

/* Increment refcount for ptr. Creates entry if new. Returns new count, or 0 on hashmap failure. */
u32 refcounter_inc(void* ptr) {
    collection_key_t key = (uintptr_t)ptr;
    if (!recomputil_u32_memory_hashmap_contains(refcounter, key)) {
        recomputil_u32_memory_hashmap_create(refcounter, key);
    }
    u16* count = (u16*)recomputil_u32_memory_hashmap_get(refcounter, key);
    if (count == NULL) {
        return 0;
    }
    return ++(*count);
}

/* Decrement refcount for ptr. Erases entry when count hits 0. Returns new count (0 = freed/missing). */
u32 refcounter_dec(void* ptr) {
    collection_key_t key = (uintptr_t)ptr;
    u16* count = (u16*)recomputil_u32_memory_hashmap_get(refcounter, key);
    if (count == NULL) {
        return 0;
    }
    if (--(*count) == 0) {
        recomputil_u32_memory_hashmap_erase(refcounter, key);
        return 0;
    }
    return *count;
}

/* Get current refcount for ptr. Returns 0 if untracked. */
u32 refcounter_get(void* ptr) {
    collection_key_t key = (uintptr_t)ptr;
    u16* count = (u16*)recomputil_u32_memory_hashmap_get(refcounter, key);
    return count ? *count : 0;
}

/* Byte-by-byte memory compare. Returns 0 if equal, else difference of first mismatched byte. */
int Utils_MemCmp(const void *a, const void *b, size_t size) {
    const char *c = a;
    const char *d = b;

    for (size_t i = 0; i < size; ++i) {
        if (*c != *d) {
            return *c - *d;
        }

        c++;
        d++;
    }

    return 0;
}

/* Duplicate string using recomp_alloc (mod heap). Caller must manage lifetime. */
char *Utils_StrDup(const char *s) {
    char *newStr = recomp_alloc(strlen(s) + 1);

    char *c = newStr;

    while (*s != '\0') {
        *c = *s;
        s++;
        c++;
    }

    *c = '\0';

    return newStr;
}

/* Debug: hex dump `size` bytes at `ptr`. Format: "ADDR: XXYY XXYY ...\n", 16 bytes/line. */
void print_bytes(void* ptr, int size) {
    unsigned char *p = ptr;
    int i;
    for (i = 0; i < size; i++) {
        if (i % 16 == 0) {
            recomp_printf("%08x: ", &ptr[i]);
        }
        recomp_printf("%02X", p[i]);
        if (i % 16 == 15) {
            recomp_printf("\n");
        } else if (i % 2 == 1) {
            recomp_printf(" ");
        }
    }
    recomp_printf("\n");
}

/*
 * FNV-1a 32-bit hash over a buffer. Chain calls by passing prev result as hval.
 * First call: use FNV1_32A_INIT (0x811c9dc5). Prime is 0x01000193 (expanded via shifts below).
 */
Fnv32_t fnv_32a_buf(void *buf, size_t len, Fnv32_t hval) {
    unsigned char *bp = (unsigned char *)buf;
    unsigned char *be = bp + len;

    while (bp < be) {
        hval ^= (Fnv32_t)*bp++;
        hval += (hval<<1) + (hval<<4) + (hval<<7) + (hval<<8) + (hval<<24);
    }

    return hval;
}
