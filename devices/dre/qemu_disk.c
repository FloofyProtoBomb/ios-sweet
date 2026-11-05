/* SPDX-License-Identifier: MIT
 * AArch64 semihosted file provider for explicit, privileged QEMU tests.
 * Fixed relative filename, no path arguments, creation, deletion or shell.
 * SYS_OPEN modes1/3 preserve the existing file; all ABI fields are64 bits.
 */
#include "qemu_disk.h"

#define BLOCK 4096u
#define QEMU_FREQUENCY UINT64_C(62500000)
enum { SYS_OPEN=1, SYS_CLOSE=2, SYS_WRITE=5, SYS_READ=6, SYS_SEEK=10, SYS_FLEN=12 };
static const char filename[] = "dre-root.img";
static int authorized, write_enabled, ready, poisoned;
static int64_t handle = -1;
static uint64_t file_bytes;
static volatile uint8_t bounce[BLOCK];
static const char *state = "DISABLED";

static uint64_t frequency(void)
{
#ifdef DRE_QEMU_DISK_HOST_TEST
    extern uint64_t dre_qemu_disk_test_frequency(void);
    return dre_qemu_disk_test_frequency();
#else
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#endif
}

static int can_trap(void) { return authorized == 1 && frequency() == QEMU_FREQUENCY; }
static int64_t semihost(uint64_t operation, uint64_t *arguments)
{
    /* No caller, including cleanup, can execute HLT on the19.2 MHz phone. */
    if (!can_trap()) return -1;
#ifdef DRE_QEMU_DISK_HOST_TEST
    extern int64_t dre_qemu_disk_test_call(uint64_t, uint64_t *);
    return dre_qemu_disk_test_call(operation, arguments);
#else
    register uint64_t x0 __asm__("x0") = operation;
    register uint64_t x1 __asm__("x1") = (uintptr_t)arguments;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    return (int64_t)x0;
#endif
}

static int fail(const char *reason)
{
    state = reason;
    if (handle >= 0 && can_trap()) {
        uint64_t args[1] = {(uint64_t)handle};
        (void)semihost(SYS_CLOSE, args);
    }
    handle = -1; ready = 0; poisoned = 1;
    return 0;
}
static int live(void)
{
    if (poisoned || !ready) return 0;
    if (!can_trap()) return fail("FREQUENCY_CHANGED");
    return 1;
}
static int64_t open_file(void)
{
    uint64_t args[3] = {(uintptr_t)filename, write_enabled ? 3u : 1u, sizeof(filename)-1};
    return semihost(SYS_OPEN, args);
}
static int64_t length(void)
{
    uint64_t args[1] = {(uint64_t)handle};
    return semihost(SYS_FLEN, args);
}
static int valid_length(int64_t size)
{
    return size > 0 && !((uint64_t)size % BLOCK) && (uint64_t)size / BLOCK <= UINT32_MAX;
}
int dre_qemu_disk_open(int enabled, int writable, uint64_t *bytes)
{
    if (bytes) *bytes = 0;
    if (poisoned) return 0;
    if (ready) { state = "ALREADY_OPEN"; return 0; }
    if (enabled != 1 || (writable != 0 && writable != 1) || !bytes) {
        state = "DISABLED"; return 0;
    }
    if (frequency() != QEMU_FREQUENCY) { state = "NOT_QEMU_FREQUENCY"; return 0; }
    authorized = 1; write_enabled = writable;
    handle = open_file();
    if (handle < 0) return fail("OPEN_FAILED");
    int64_t size = length();
    if (!valid_length(size)) return fail("INVALID_LENGTH");
    file_bytes = (uint64_t)size; *bytes = file_bytes; ready = 1;
    state = write_enabled ? "VIRTUAL_FILE_RW" : "VIRTUAL_FILE_RO";
    return 1;
}
static int seek_block(uint64_t relative_block)
{
    if (!live()) return 0;
    if (relative_block >= file_bytes / BLOCK) { state = "BLOCK_RANGE"; return 0; }
    uint64_t args[2] = {(uint64_t)handle, relative_block * BLOCK};
    if (semihost(SYS_SEEK, args) != 0) return fail("SEEK_FAILED");
    return 1;
}
int dre_qemu_disk_read(uint64_t relative_block, uint8_t out[BLOCK])
{
    if (!out) { state = "INVALID_BUFFER"; return 0; }
    if (!seek_block(relative_block)) return 0;
    uint64_t args[3] = {(uint64_t)handle, (uintptr_t)bounce, BLOCK};
    /* A short read returns bytes NOT read. Do not publish partial output. */
    if (semihost(SYS_READ, args) != 0) return fail("READ_FAILED");
    for (unsigned i=0; i<BLOCK; i++) out[i] = bounce[i];
    state = "VIRTUAL_READ_PASS";
    return 1;
}
int dre_qemu_disk_write(uint64_t relative_block, const uint8_t in[BLOCK])
{
    if (!in) { state = "INVALID_BUFFER"; return 0; }
    if (!live()) return 0;
    if (!write_enabled) { state = "READ_ONLY"; return 0; }
    if (!seek_block(relative_block)) return 0;
    for (unsigned i=0; i<BLOCK; i++) bounce[i] = in[i];
    uint64_t args[3] = {(uint64_t)handle, (uintptr_t)bounce, BLOCK};
    if (semihost(SYS_WRITE, args) != 0) return fail("WRITE_FAILED");
    state = "VIRTUAL_WRITE_PASS";
    return 1;
}
int dre_qemu_disk_flush(void)
{
    if (!live()) return 0;
    uint64_t args[1] = {(uint64_t)handle};
    handle = -1;
    if (semihost(SYS_CLOSE, args) != 0) return fail("FLUSH_CLOSE_FAILED");
    handle = open_file();
    if (handle < 0) return fail("FLUSH_OPEN_FAILED");
    int64_t size = length();
    if (!valid_length(size) || (uint64_t)size != file_bytes) return fail("LENGTH_CHANGED");
    state = "VIRTUAL_STDIO_FLUSH";
    return 1;
}
int dre_qemu_disk_close(void)
{
    if (!live()) return 0;
    uint64_t args[1] = {(uint64_t)handle};
    handle = -1;
    if (semihost(SYS_CLOSE, args) != 0) return fail("CLOSE_FAILED");
    ready = 0; authorized = 0; state = "CLOSED";
    return 1;
}
const char *dre_qemu_disk_status(void) { return state; }
