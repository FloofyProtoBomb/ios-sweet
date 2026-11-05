/* SPDX-License-Identifier: MIT
 * Opt-in DE2118 read-only BSD disk. Reuses the handset-tested C engine,
 * with a shim-owned uncached low-DDR arena. No SMMU/clock/media writes. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include "libfdt/libfdt.h"
#include "storage_abi.h"
#include "dma_memory.h"
#include "ufs_read.h"
#include "ufs_handoff_dma.h"
#include "ufs_clock.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern int dre_ufs_get_handoff(uint64_t words[8]);
int dre_ufs_prepare_device(dev_t *device);
int dre_read32(uintptr_t address, uint32_t *out);
int dre_write32(uintptr_t address, uint32_t value);

volatile uint8_t *dre_kernel_ufs_arena;
uint64_t dre_kernel_ufs_arena_pa;
static struct { uint64_t pa, size; vm_offset_t va; } regions[] = {
    {0x04804000, 0x3000, 0}, {0x0c600000, 0x100000, 0}, {0x01400000, 0x1f0000, 0}
};
static struct dre_ufs_handoff_dma dma;
static struct dre_ufs_read_state media;
static uint8_t bounce[4096];
static int attempted, ready, dead, bmajor = -1, cmajor = -1;
static LCK_GRP_DECLARE(dre_ufs_group, "dre_ufs");
static LCK_MTX_DECLARE(dre_ufs_lock, &dre_ufs_group);

int dre_read32(uintptr_t address, uint32_t *out)
{
    if (address & 3) return 0;
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        if (regions[i].va && address >= regions[i].pa &&
            address - regions[i].pa <= regions[i].size - 4) {
            *out = *(volatile uint32_t *)(regions[i].va + address - regions[i].pa);
            return 1;
        }
    }
    return 0;
}
int dre_write32(uintptr_t address, uint32_t value)
{
    /* Only the proven engine's HCI list/interrupt/doorbell operations. */
    if (!regions[0].va || address < regions[0].pa) return 0;
    uint64_t off = address - regions[0].pa;
    if (off != 0x20 && off != 0x24 && off != 0x50 && off != 0x54 &&
        off != 0x58 && off != 0x5c && off != 0x60) return 0;
    if ((off == 0x20 && value != 1) || (off == 0x58 && value != 1) ||
        (off == 0x5c && value != ~1u) || (off == 0x60 && value > 1)) return 0;
    *(volatile uint32_t *)(regions[0].va + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int native_guard(void *cookie)
{
    struct dre_ufs_clock_state clocks;
    return !strcmp(dre_ufs_clocks(&clocks, 0), "READY") && dre_ufs_handoff_valid(cookie);
}
static int check_result(const char *result, const char *expected)
{
    if (!media.safe_to_handoff)
        panic("DRE UFS: unsafe DMA/host state (%s), reset required", result);
    if (strcmp(result, expected)) {
        printf("DRE UFS: read disabled: %s\n", result);
        dead = 1;
        return EIO;
    }
    return 0;
}
static uint64_t count_blocks(dev_t dev)
{
    return minor(dev) == 0 ? media.userdata_blocks : media.last_lba + 1;
}
static int disk_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
    if (flags & FWRITE) return EROFS;
    return ready && !dead && minor(dev) <= 1 ? 0 : ENXIO;
}
static int disk_close(__unused dev_t dev, __unused int flags, __unused int type,
                      __unused struct proc *p) { return 0; }
static void disk_strategy(buf_t bp)
{
    int error = 0;
    size_t count = buf_count(bp), done = 0;
    dev_t dev = buf_device(bp);
    caddr_t mapped = NULL;
    buf_setresid(bp, count);
    if (!(buf_flags(bp) & B_READ)) { error = EROFS; goto finish; }
    if (!ready || dead || minor(dev) > 1) { error = ENXIO; goto finish; }
    if (buf_blkno(bp) < 0 || (uint64_t)buf_blkno(bp) > UINT64_MAX / DEV_BSIZE) {
        error = EINVAL; goto finish;
    }
    uint64_t offset = (uint64_t)buf_blkno(bp) * DEV_BSIZE;
    uint64_t bytes = count_blocks(dev) * 4096;
    if (offset > bytes || (count % DEV_BSIZE)) { error = EINVAL; goto finish; }
    if (count > bytes - offset) count = bytes - offset;
    if (!count) goto finish;
    if ((error = buf_map(bp, &mapped))) goto finish;
    lck_mtx_lock(&dre_ufs_lock);
    while (done < count) {
        if (dead) { error = EIO; break; }
        uint64_t position = offset + done;
        uint64_t block = position / 4096 + (minor(dev) ? 0 : media.userdata_first);
        unsigned skip = position % 4096;
        size_t take = MIN(count - done, 4096 - skip);
        if (block > UINT32_MAX) { error = EINVAL; break; }
        error = check_result(dre_ufs_read_block_guarded(&dma.route, &media,
            native_guard, &dma, (uint32_t)block, bounce), "READ_PASS");
        if (error) break;
        memcpy(mapped + done, bounce + skip, take);
        done += take;
        buf_setresid(bp, buf_count(bp) - done);
    }
    lck_mtx_unlock(&dre_ufs_lock);
    buf_unmap(bp);
finish:
    if (error) buf_seterror(bp, error);
    buf_biodone(bp);
}
static int disk_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag,
                      __unused struct proc *p)
{
    if (!ready || dead || minor(dev) > 1) return ENXIO;
    switch (cmd) {
    case DKIOCGETBLOCKSIZE: *(uint32_t *)data = DEV_BSIZE; return 0;
    case DKIOCGETPHYSICALBLOCKSIZE: *(uint32_t *)data = 4096; return 0;
    case DKIOCSETBLOCKSIZE: return *(uint32_t *)data == DEV_BSIZE ? 0 : EINVAL;
    case DKIOCGETBLOCKCOUNT: *(uint64_t *)data = count_blocks(dev) * 8; return 0;
    case DKIOCISWRITABLE: *(uint32_t *)data = 0; return 0;
    case DKIOCGETMAXBLOCKCOUNTREAD: *(uint64_t *)data = 8; return 0;
    case DKIOCGETMAXBLOCKCOUNTWRITE: *(uint64_t *)data = 0; return 0;
    default: return ENOTTY;
    }
}
static int disk_psize(dev_t dev)
{
    return ready && minor(dev) <= 1 ? (int)MIN(count_blocks(dev) * 8, INT_MAX) : -1;
}
static int disk_read(dev_t dev, struct uio *uio, __unused int flags)
{
    return physio(disk_strategy, NULL, dev, B_READ, minphys, uio, DEV_BSIZE);
}
static int disk_write(__unused dev_t dev, __unused struct uio *uio, __unused int flags)
{ return EROFS; }
static const struct bdevsw disk_bdevsw = {
    .d_open = disk_open, .d_close = disk_close, .d_strategy = disk_strategy,
    .d_ioctl = disk_ioctl, .d_dump = eno_dump, .d_psize = disk_psize, .d_type = D_DISK,
};
static const struct cdevsw disk_cdevsw = {
    .d_open = disk_open, .d_close = disk_close, .d_read = disk_read, .d_write = disk_write,
    .d_ioctl = disk_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
    .d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = D_DISK,
};
static int board_ok(const void *dt)
{
    int len;
    const fdt32_t *ids = fdt_getprop(dt, 0, "oplus,dtsi_no", &len);
    if (fdt_node_check_compatible(dt, 0, "qcom,holi") || !ids || len < 4 || len % 4) return 0;
    int found = 0;
    for (int i = 0; i < len / 4; i++) {
        uint32_t id = fdt32_to_cpu(ids[i]);
        if (id == 133144 || id == 133180) found = 1;
    }
    int host = fdt_path_offset(dt, "/soc/ufshc@4804000");
    const fdt32_t *reg = fdt_getprop(dt, host, "reg", &len);
    return found && reg && len >= 8 && fdt32_to_cpu(reg[0]) == 0x04804000 &&
        fdt32_to_cpu(reg[1]) == 0x3000;
}
static int initialize(void)
{
    uint64_t w[8], frequency;
    int enabled = 0;
    PE_parse_boot_argn("dre_ufs", &enabled, sizeof(enabled));
    if (enabled != 1 || !dre_ufs_get_handoff(w) || !dre_storage_abi_valid(w)) return ENXIO;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (frequency != 19200000) return ENXIO;
    /* Byte copy from Device RAM: libc's unaligned/vector loads are unsuitable. */
    vm_offset_t dt_map = ml_io_map(w[3], w[4]);
    if (!dt_map) return ENOMEM;
    void *dt = kalloc_data(w[4], Z_WAITOK | Z_ZERO);
    if (!dt) { ml_io_unmap(trunc_page(dt_map), round_page(w[4] + (dt_map & PAGE_MASK))); return ENOMEM; }
    for (unsigned i = 0; i < w[4]; i++) ((uint8_t *)dt)[i] = ((volatile uint8_t *)dt_map)[i];
    ml_io_unmap(trunc_page(dt_map), round_page(w[4] + (dt_map & PAGE_MASK)));
    if (fdt_check_full(dt, w[4]) || fdt_totalsize(dt) != w[4] || !board_ok(dt) ||
        !dre_dma_ram_allowed(dt, w[1], w[2]) || !dre_dma_ram_allowed(dt, w[3], w[4])) {
        kfree_data(dt, w[4]); return ENXIO;
    }
    /* Kept for the driver's lifetime: route queries use this private snapshot. */
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        regions[i].va = ml_io_map(regions[i].pa, regions[i].size);
        if (!regions[i].va) return ENOMEM;
    }
    dre_kernel_ufs_arena_pa = w[1];
    dre_kernel_ufs_arena = (volatile uint8_t *)ml_io_map(w[1], w[2]);
    if (!dre_kernel_ufs_arena) return ENOMEM;
    struct dre_dma_route route;
    const char *result = dre_dma_route(dt, "/soc/ufshc@4804000", 0x60, &route);
    if (strcmp(result, "TRANSLATED")) return ENXIO;
    result = dre_ufs_handoff_begin(dt, &route, &dma);
    if (strcmp(result, "HANDOFF_BYPASS") || !native_guard(&dma)) return ENXIO;
    result = dre_ufs_read_guarded(&route, &media, native_guard, &dma);
    if (check_result(result, "GPT_PASS")) return EIO;
    if (media.last_lba > UINT32_MAX || media.last_lba >= UINT64_MAX / 4096) return ENXIO;
    bmajor = bdevsw_add(-1, &disk_bdevsw);
    if (bmajor < 0) return ENXIO;
    cmajor = cdevsw_add_with_bdev(-1, &disk_cdevsw, bmajor);
    if (cmajor < 0) { bdevsw_remove(bmajor, &disk_bdevsw); return ENXIO; }
    void *block = devfs_make_node(makedev(bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0600, "ufsroot0");
    void *raw = devfs_make_node(makedev(cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, "rufsroot0");
    void *whole = devfs_make_node(makedev(cmajor, 1), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, "rdremedia0");
    if (!block || !raw || !whole) {
        if (block) devfs_remove(block);
        if (raw) devfs_remove(raw);
        if (whole) devfs_remove(whole);
        cdevsw_remove(cmajor, &disk_cdevsw); bdevsw_remove(bmajor, &disk_bdevsw);
        return ENXIO;
    }
    ready = 1;
    printf("DRE UFS: native read-only disk ready, userdata blocks=%llu\n",
           (unsigned long long)media.userdata_blocks);
    return 0;
}
int dre_ufs_prepare_device(dev_t *device)
{
    lck_mtx_lock(&dre_ufs_lock);
    int error = 0;
    if (!attempted) {
        attempted = 1;
        error = initialize();
        if (error) printf("DRE UFS: native disk unavailable (%d)\n", error);
    }
    if (!ready || dead) error = ENXIO;
    if (!error && device) *device = makedev(bmajor, 0);
    lck_mtx_unlock(&dre_ufs_lock);
    return error;
}
