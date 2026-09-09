#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Signature-scan our own process's memory for a byte pattern.
 *
 * In-process equivalent of aob_scan_process() — since we're scanning
 * ourselves, no `ReadProcessMemory` is needed, we just dereference pointers
 * directly. Walks `[start, start + region_size)` one `VirtualQuery` region
 * at a time, skipping anything not `MEM_COMMIT` or not readable (including
 * `PAGE_GUARD` pages), so it's safe to pass a size that runs past the end
 * of a module into unrelated memory.
 *
 * @param start       Address to start scanning from, e.g. a module base
 *                    from `GetModuleHandleW`.
 * @param region_size How many bytes forward from `start` to scan.
 * @param pattern     Bytes to match. Values at wildcard positions are ignored.
 * @param mask        Same length as `pattern`. `'x'` = byte must match,
 *                    `'?'` = wildcard (matches anything).
 * @param pattern_len Length of `pattern` and `mask`.
 * @return Address of the first match, or 0 if none found.
 */
uintptr_t aob_scan_local(uintptr_t start, size_t region_size,
                          const uint8_t *pattern, const char *mask, size_t pattern_len);

/**
 * @brief Overwrite bytes at an address in our own process.
 *
 * Flips the page to `PAGE_EXECUTE_READWRITE` with `VirtualProtect` (no
 * handle needed — always operates on the calling process), `memcpy`s the
 * new bytes in, restores the original protection, then calls
 * `FlushInstructionCache` so the CPU doesn't execute a stale cached copy of
 * patched code.
 *
 * @param address       Address to patch, typically from aob_scan_local() or
 *                       `module_base + known_offset` (see offsets.h).
 * @param new_bytes     Bytes to write.
 * @param len           Number of bytes in `new_bytes` (and `old_bytes_out`).
 * @param old_bytes_out Optional. If non-NULL, the original bytes are saved
 *                       here before being overwritten, so the patch can be
 *                       reverted later with restore_bytes_local(). Pass NULL
 *                       to skip.
 * @return TRUE on success, FALSE if VirtualProtect failed.
 */
BOOL patch_bytes_local(uintptr_t address, const uint8_t *new_bytes, size_t len, uint8_t *old_bytes_out);

/**
 * @brief patch_bytes_local(), but only if the target holds what you expect.
 *
 * Every RVA in this project is tied to one build of GameAssembly.dll. When
 * the game updates they all shift, and an unverified write then stamps over
 * arbitrary code and saves the wreckage as the "original" -- so even undoing
 * it restores garbage. Comparing first turns that into a clean refusal.
 *
 * @param address   Where to patch.
 * @param expected  The bytes that must already be there, `len` of them.
 * @param new_bytes The replacement, also `len` bytes.
 * @param len       Length of all three buffers.
 * @param old_bytes_out Receives the original bytes; may be NULL.
 * @return FALSE if the target didn't match, or the write failed.
 */
BOOL patch_bytes_checked(uintptr_t address, const uint8_t *expected,
                          const uint8_t *new_bytes, size_t len, uint8_t *old_bytes_out);

/**
 * @brief Undo a previous patch_bytes_local() call.
 *
 * @param address   Same address that was patched.
 * @param old_bytes The bytes captured by patch_bytes_local()'s `old_bytes_out`.
 * @param len       Number of bytes to restore.
 * @return TRUE on success, FALSE on failure.
 */
BOOL restore_bytes_local(uintptr_t address, const uint8_t *old_bytes, size_t len);
