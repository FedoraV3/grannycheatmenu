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
     * Written back-to-front, one byte at a time, with the first byte last.
     *
     * This is live code with the game running: nothing suspends the other
     * threads, so one of them can execute this very instruction while the
     * copy is in progress. A forward memcpy can leave the opcode byte
     * replaced while the operands still belong to the old instruction --
     * over `test al, al` (84 C0) that briefly reads `0C C0`, but the other
     * interleaving gives `84 01`, which decodes as `test al, byte ptr [rcx]`
     * and dereferences whatever rcx happens to hold.
     *
     * Writing the opcode byte last means every intermediate state still
     * begins with the ORIGINAL opcode, so the instruction keeps its original
     * length and the operand bytes are the only thing in flux. That is not a
     * substitute for suspending threads the way MinHook does, but it removes
     * the interleaving that can fabricate a memory reference out of an
     * instruction that had none.
     */
    volatile uint8_t *target = (volatile uint8_t *)address;
    for (size_t i = len; i-- > 0; ) {
        target[i] = new_bytes[i];
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
