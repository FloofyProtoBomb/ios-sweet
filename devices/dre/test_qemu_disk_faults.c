/* Model host failures and extreme64-bit ABI values, without giant files. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "qemu_disk.h"
static int scenario, calls, opens, closes, lengths, bad;
static uint64_t hz=62500000, file_handle, last_seek;
static uint8_t input[4096], output[4096];

uint64_t dre_qemu_disk_test_frequency(void) { return hz; }
int64_t dre_qemu_disk_test_call(uint64_t operation, uint64_t *args)
{
    calls++;
    if (operation==1) {
        const char *name=(const char *)(uintptr_t)args[0];
        const char expected[]="dre-root.img";
        for(unsigned i=0;i<sizeof(expected);i++) if(name[i]!=expected[i]) bad=1;
        if(args[2]!=sizeof(expected)-1 || args[1]!=(scenario==8?1u:3u)) bad=1;
        if(scenario==12 || (scenario==6 && opens)) return -1;
        file_handle=UINT64_C(0x100000001)+(unsigned)opens++;
        return (int64_t)file_handle;
    }
    if(args[0]!=file_handle || file_handle<=UINT32_MAX) bad=1;
    if(operation==12) {
        lengths++;
        if(scenario==0) return (int64_t)(UINT64_C(0x100000000)*4096);
        if(scenario==1) return (int64_t)((uint64_t)UINT32_MAX*4096);
        if(scenario==13) return -1;
        if(scenario==7 && lengths>1) return 4096;
        return 8192;
    }
    if(operation==2) { closes++; return scenario==5 || scenario==11 ? -1 : 0; }
    if(operation==10) { last_seek=args[1]; return scenario==2 ? -1 : 0; }
    if(operation==6) {
        if(args[2]!=4096) bad=1;
        uint8_t *target=(uint8_t *)(uintptr_t)args[1];
        for(unsigned i=0;i<4096;i++) target[i]=0x99;
        return scenario==3 ? 1 : 0;
    }
    if(operation==5) {
        if(args[2]!=4096) bad=1;
        const uint8_t *source=(const uint8_t *)(uintptr_t)args[1];
        for(unsigned i=0;i<4096;i++) if(source[i]!=input[i]) bad=1;
        return scenario==4 ? 4096 : 0;
    }
    bad=1; return -1;
}
#define CHECK(c,n) do { if(!(c)) { fprintf(stderr,"scenario%d check%d\n",scenario,n); return 1; } } while(0)
static int unchanged(void)
{ for(unsigned i=0;i<4096;i++) if(output[i]!=0xaa) return 0; return 1; }
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    scenario=atoi(argv[1]);
    for(unsigned i=0;i<4096;i++) { input[i]=(uint8_t)i; output[i]=0xaa; }
    uint64_t size=0;
    if(scenario==10) {
        CHECK(!dre_qemu_disk_open(2,1,&size),1);
        CHECK(!dre_qemu_disk_open(1,2,&size),2);
        CHECK(!dre_qemu_disk_open(1,1,0),3);
        CHECK(calls==0,4);
        CHECK(dre_qemu_disk_open(1,1,&size) && dre_qemu_disk_close() && !bad,5);
        return 0;
    } else if(scenario==14) {
        hz=19200000;
        CHECK(!dre_qemu_disk_open(1,1,&size) && calls==0,1);
        CHECK(!dre_qemu_disk_read(0,output) && !dre_qemu_disk_write(0,input),2);
        CHECK(!dre_qemu_disk_flush() && calls==0,3);
    } else if(scenario==0 || scenario==12 || scenario==13) {
        CHECK(!dre_qemu_disk_open(1,1,&size) && size==0,1);
        CHECK(opens==(scenario==12?0:1) && closes==(scenario==12?0:1),2);
    } else {
        CHECK(dre_qemu_disk_open(1,scenario!=8,&size),1);
        if(scenario==1) {
            CHECK(size==(uint64_t)UINT32_MAX*4096,2);
            CHECK(dre_qemu_disk_read((uint64_t)UINT32_MAX-1,output),3);
            CHECK(last_seek==((uint64_t)UINT32_MAX-1)*4096,4);
            int before=calls;
            CHECK(!dre_qemu_disk_read(UINT32_MAX,output) && calls==before,5);
            CHECK(dre_qemu_disk_close() && !bad,6);
            return 0;
        }
        if(scenario==8) {
            int before=calls;
            CHECK(!dre_qemu_disk_write(0,input) && calls==before,2);
            CHECK(dre_qemu_disk_read(0,output) && dre_qemu_disk_flush(),3);
            CHECK(dre_qemu_disk_close() && !bad,4);
            return 0;
        }
        if(scenario==9) {
            int before=calls; hz=19200000;
            CHECK(!dre_qemu_disk_read(0,output) && calls==before,2);
            hz=62500000;
        } else if(scenario==2 || scenario==3) {
            CHECK(!dre_qemu_disk_read(0,output) && unchanged(),2);
        } else if(scenario==4) CHECK(!dre_qemu_disk_write(0,input),2);
        else if(scenario==5 || scenario==6 || scenario==7) CHECK(!dre_qemu_disk_flush(),2);
        else if(scenario==11) CHECK(!dre_qemu_disk_close(),2);
        else return 2;
    }
    int before=calls;
    CHECK(!dre_qemu_disk_read(0,output) && !dre_qemu_disk_write(0,input),20);
    CHECK(!dre_qemu_disk_flush() && !dre_qemu_disk_close(),21);
    CHECK(!dre_qemu_disk_open(1,1,&size) && calls==before && !bad,22);
    return 0;
}
