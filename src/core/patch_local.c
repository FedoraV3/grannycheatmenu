#include "core/patch_local.h"
#include <string.h>

static int matches_at(const uint8_t *buf, const uint8_t *pattern,
                       const char *mask, size_t pattern_len) {
    for (size_t i = 0; i < pattern_len; i++) {
        if (mask[i] == 'x' && buf[i] != pattern[i]) return 0;
    }
    return 1;
}

static BOOL is_readable(DWORD protect) {
    if (protect & PAGE_GUARD) return FALSE;
    switch (protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return TRUE;
        default:
            return FALSE;
    }
}

uintptr_t aob_scan_local(uintptr_t start, size_t region_size,
                          const uint8_t *pattern, const char *mask, size_t pattern_len) {
    if (pattern_len == 0 || region_size < pattern_len) return 0;

    uintptr_t addr = start;
    uintptr_t end = start + region_size;

    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;

        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end > end) region_end = end;

        if (mbi.State == MEM_COMMIT && is_readable(mbi.Protect)) {
            const uint8_t *p = (const uint8_t *)addr;
            size_t len = region_end - addr;
            for (size_t i = 0; i + pattern_len <= len; i++) {
                if (matches_at(p + i, pattern, mask, pattern_len)) {
                    return addr + i;
                }
            }
        }

        addr = region_end;
    }

    return 0;
}

BOOL patch_bytes_local(uintptr_t address, const uint8_t *new_bytes, size_t len, uint8_t *old_bytes_out) {
    DWORD old_protect;
    if (!VirtualProtect((LPVOID)address, len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return FALSE;
    }

    if (old_bytes_out) {
        memcpy(old_bytes_out, (const void *)address, len);
    }

    /*
     * The whole patch has to land as ONE store wherever it can.
     *
     * This is live code with the game running and nothing suspends the other
     * threads, so one of them can be executing this very instruction while
     * the copy happens. Any byte-at-a-time order is unsafe, and the
     * back-to-front order that looks safest is the worse of the two: over
     * `test al, al` (84 C0) it leaves `84 00` in flight, and while ModRM C0
     * names the register al, ModRM 00 names [rax] -- so that intermediate
     * decodes as `test [rax], al` and dereferences a register holding a
     * boolean. Forward order merely gives `or al, 0C0h`, which is harmless.
     *
     * A single naturally-sized store cannot be observed half-done on x86, so
     * for the sizes actually in use -- 1 and 2 bytes -- there is no window at
     * all. For a longer patch the tail goes first and the leading 8 bytes
     * land as one store, which keeps the original opening instruction intact
     * and decodable until the very last write.
     */
    volatile uint8_t *target = (volatile uint8_t *)address;
    size_t head = len;
    if (head > 8) head = 8;
    else if (head > 4) head = 4;   /* 5..7 -> settle for a 4-byte head */
    else if (head == 3) head = 2;

    for (size_t i = len; i-- > head; ) {
        target[i] = new_bytes[i];
    }

    if (head == 8) {
        uint64_t word;
        memcpy(&word, new_bytes, 8);
        *(volatile uint64_t *)target = word;
    } else if (head == 4) {
        uint32_t word;
        memcpy(&word, new_bytes, 4);
        *(volatile uint32_t *)target = word;
    } else if (head == 2) {
        uint16_t word;
        memcpy(&word, new_bytes, 2);
        *(volatile uint16_t *)target = word;
    } else if (head == 1) {
        target[0] = new_bytes[0];
    }

    DWORD restored_protect;
    VirtualProtect((LPVOID)address, len, old_protect, &restored_protect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, len);

    return TRUE;
}

BOOL patch_bytes_checked(uintptr_t address, const uint8_t *expected,
                          const uint8_t *new_bytes, size_t len, uint8_t *old_bytes_out) {
    /* Read before touching the protection: if this isn't the instruction we
     * think it is, the safest thing is to have done nothing at all. */
    if (memcmp((const void *)address, expected, len) != 0) {
        return FALSE;
    }
    return patch_bytes_local(address, new_bytes, len, old_bytes_out);
}

BOOL restore_bytes_local(uintptr_t address, const uint8_t *old_bytes, size_t len) {
    return patch_bytes_local(address, old_bytes, len, NULL);
}
