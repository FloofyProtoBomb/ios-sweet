/* Synthetic scratch-file integration test of the production helper in EL1. */
#include <stdint.h>
#include "qemu_disk.h"
static uint8_t data[4096] __attribute__((aligned(16)));
static uint8_t out[4096] __attribute__((aligned(16)));
static volatile uint64_t *result = (volatile uint64_t *)UINT64_C(0x401f0000);
static void finish(unsigned code)
{
    result[1] = code;
    __asm__ volatile("dsb sy" ::: "memory");
    result[0] = UINT64_C(0x44524553454d4931);
}
#define CHECK(c,n) do { if (!(c)) { finish(n); return; } } while(0)
static int pattern(const uint8_t *p, unsigned value)
{ for (unsigned i=0; i<4096; i++) if (p[i] != value) return 0; return 1; }
#if TEST_CASE == 7 || TEST_CASE == 8
static int truncate_test_file(void)
{
    static const char name[] = "dre-root.img";
    uint64_t args[3] = {(uintptr_t)name, 5, sizeof(name)-1};
    register uint64_t x0 __asm__("x0") = 1;
    register uint64_t x1 __asm__("x1") = (uintptr_t)args;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    if ((int64_t)x0 < 0) return 0;
    args[0] = x0; x0=2; x1=(uintptr_t)args;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    return x0 == 0;
}
#endif
void test_main(void)
{
    uint64_t bytes=999;
    for (unsigned i=0; i<4096; i++) { data[i]=(uint8_t)(i ^ 0x5a); out[i]=0xaa; }
#if TEST_CASE == 0
    CHECK(dre_qemu_disk_open(1,1,&bytes) && bytes==8192,1);
    CHECK(dre_qemu_disk_read(0,out) && pattern(out,0x11),2);
    CHECK(!dre_qemu_disk_read(2,out) && !dre_qemu_disk_write(UINT64_MAX,data),3);
    CHECK(dre_qemu_disk_write(1,data),4);
    CHECK(dre_qemu_disk_flush(),5);
    CHECK(dre_qemu_disk_read(1,out),6);
    for (unsigned i=0; i<4096; i++) CHECK(out[i]==data[i],7);
    CHECK(dre_qemu_disk_close(),8);
#elif TEST_CASE == 1
    CHECK(dre_qemu_disk_open(1,0,&bytes) && bytes==8192,1);
    CHECK(!dre_qemu_disk_write(0,data),2);
    CHECK(dre_qemu_disk_read(1,out) && pattern(out,0x22),3);
    CHECK(dre_qemu_disk_flush() && dre_qemu_disk_close(),4);
#elif TEST_CASE == 2
    CHECK(!dre_qemu_disk_open(0,1,&bytes) && bytes==0,1);
    CHECK(!dre_qemu_disk_read(0,out) && !dre_qemu_disk_write(0,data),2);
    CHECK(!dre_qemu_disk_flush() && !dre_qemu_disk_close(),3);
#elif TEST_CASE == 3
    CHECK(!dre_qemu_disk_open(1,1,&bytes) && bytes==0,1);
    CHECK(!dre_qemu_disk_read(0,out) && !dre_qemu_disk_write(0,data),2);
    CHECK(!dre_qemu_disk_flush() && !dre_qemu_disk_close(),3);
#elif TEST_CASE == 4 || TEST_CASE == 5 || TEST_CASE == 9
    CHECK(!dre_qemu_disk_open(1,1,&bytes) && bytes==0,1);
    CHECK(!dre_qemu_disk_read(0,out) && !dre_qemu_disk_write(0,data),2);
    CHECK(pattern(out,0xaa) && !dre_qemu_disk_flush(),3);
#elif TEST_CASE == 6
    CHECK(dre_qemu_disk_open(1,1,&bytes) && bytes==UINT64_C(0x100002000),1);
    CHECK(dre_qemu_disk_read(0x100000,out) && pattern(out,0x33),2);
    CHECK(dre_qemu_disk_write(0x100001,data) && dre_qemu_disk_flush(),3);
    CHECK(dre_qemu_disk_read(0x100001,out),4);
    for (unsigned i=0; i<4096; i++) CHECK(out[i]==data[i],5);
    CHECK(dre_qemu_disk_close(),6);
#elif TEST_CASE == 7
    CHECK(dre_qemu_disk_open(1,1,&bytes) && truncate_test_file(),1);
    CHECK(!dre_qemu_disk_read(0,out) && pattern(out,0xaa),2);
    CHECK(!dre_qemu_disk_write(0,data) && !dre_qemu_disk_flush(),3);
#elif TEST_CASE == 8
    CHECK(dre_qemu_disk_open(1,1,&bytes) && truncate_test_file(),1);
    CHECK(!dre_qemu_disk_flush(),2);
    CHECK(!dre_qemu_disk_read(0,out) && !dre_qemu_disk_write(0,data),3);
#endif
    finish(0);
}
