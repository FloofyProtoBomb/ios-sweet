/* SPDX-License-Identifier: MIT
 * Exact selected-image metadata observer. Never allocates an MTLDevice, opens
 * an accelerator or submits work. Linking/reading metadata is not Metal proof.
 */
#import <Foundation/Foundation.h>
#include "base_metadata.h"
#include <stdio.h>
#include <unistd.h>
int main(void)
{
    int result=1;
    @autoreleasepool {
        struct dre_metal306_base_metadata observed={};
        enum dre_metal306_metadata_status status=dre_metal306_inspect_base_metadata(&observed);
        printf("METAL306_METADATA_STATUS=%d\n",(int)status);
        if(status==DRE_METAL306_METADATA_INSPECTED&&observed.version==DRE_METAL306_BASE_METADATA_VERSION&&
           observed.struct_bytes==sizeof(observed)&&observed.method_count==DRE_METAL306_BASE_METHODS&&
           observed.member_count==DRE_METAL306_BASE_MEMBERS) {
            printf("METAL306_METADATA_CLASS=%u/%u METHODS=%u MEMBERS=%u\n",observed.instance_bytes,
                observed.super_instance_bytes,observed.method_count,observed.member_count);
            for(uint32_t i=0;i<observed.method_count;i++)
                printf("METAL306_METHOD=%u ARGS=%u\n",observed.methods[i].method_id,observed.methods[i].argument_count);
            for(uint32_t i=0;i<observed.member_count;i++) {
                const auto &m=observed.members[i];
                printf("METAL306_MEMBER=%u OFFSET=%u BYTES=%u ALIGN=%u\n",m.member_id,m.offset,m.bytes,m.alignment);
            }
            result=0;
        }
        puts("METAL306_DEVICE_CREATED=0 GPU_WORK=0 PRIVATE_TABLES_QUALIFIED=0");
        fflush(stdout);
    }
    /* A bounded PID1 observation window lets an isolated QEMU runner capture
     * results before an expected init exit. Ordinary process invocation exits
     * immediately; the CI build never executes this iPhoneOS program. */
    if(getpid()==1)for(unsigned i=0;i<30;i++) {
        sleep(10);puts("METAL306_METADATA_OBSERVER_ALIVE");fflush(stdout);
    }
    return result;
}
