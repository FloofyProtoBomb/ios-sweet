/* Exercise the same A619 packet on the physical DE2118 while LineageOS
 * supplies clocks, firmware, SMMU and scheduling. No physical addresses. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define __user
#include "msm_kgsl.h"
#include "a619_packet.h"

static void check(int result, const char *operation)
{
    if (result < 0) {
        perror(operation);
        exit(1);
    }
}

int main(void)
{
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    check(fd, "open KGSL");
    struct kgsl_devinfo info = {0};
    struct kgsl_device_getproperty query = {KGSL_PROP_DEVICE_INFO, &info, sizeof(info)};
    check(ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &query), "device info");
    if (info.chip_id != 0x06010900) {
        fprintf(stderr, "unexpected chip ID %#x\n", info.chip_id);
        return 1;
    }
    struct kgsl_drawctxt_create ctx = {.flags = KGSL_CONTEXT_PREAMBLE |
            KGSL_CONTEXT_PER_CONTEXT_TS | KGSL_CONTEXT_NO_GMEM_ALLOC};
    check(ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &ctx), "create GPU context");
    struct kgsl_gpumem_alloc_id page = {.size = 4096};
    check(ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &page), "allocate GPU memory");
    uint32_t *cpu = mmap(NULL, page.mmapsize, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, (off_t)page.id * sysconf(_SC_PAGESIZE));
    if (cpu == MAP_FAILED) { perror("map GPU memory"); return 1; }
    volatile uint32_t *result = cpu + 64;
    *result = 0;
    const uint32_t marker = 0x44524531;
    a619_write_marker(cpu, page.gpuaddr + 64 * sizeof(*cpu), marker);
    __asm__ volatile("dsb sy" ::: "memory");
    struct kgsl_command_object ib = {.gpuaddr = page.gpuaddr, .size = 16,
                                     .id = page.id, .flags = KGSL_CMDLIST_IB};
    struct kgsl_gpu_command submit = {.cmdlist = (uintptr_t)&ib, .cmdsize = sizeof(ib),
                                      .numcmds = 1, .context_id = ctx.drawctxt_id};
    check(ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &submit), "submit IB");
    struct kgsl_device_waittimestamp_ctxtid wait = {ctx.drawctxt_id, submit.timestamp, 2000};
    check(ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait), "wait for GPU");
    __asm__ volatile("dsb sy" ::: "memory");
    int ok = *result == marker;
    printf("A619 chip=%#x GPU marker=%#x expected=%#x %s\n",
           info.chip_id, *result, marker, ok ? "PASS" : "FAIL");
    munmap(cpu, page.mmapsize);
    close(fd);
    return !ok;
}
