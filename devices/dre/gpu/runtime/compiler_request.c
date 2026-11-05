/* SPDX-License-Identifier: MIT */
#include "compiler_request.h"
#include <string.h>

static void put32(uint8_t *out, uint32_t value)
{ for(unsigned i=0;i<4;++i)out[i]=(uint8_t)(value>>(8*i)); }
static uint32_t get32(const uint8_t *in)
{ return (uint32_t)in[0]|(uint32_t)in[1]<<8|(uint32_t)in[2]<<16|(uint32_t)in[3]<<24; }

int dre_metal306_request13_size(size_t source_bytes,size_t argument_bytes,size_t *size)
{
    if(!size)return 0;
    *size=0;
    if(!source_bytes || source_bytes>DRE_METAL306_SOURCE_LIMIT ||
       !argument_bytes || argument_bytes>DRE_METAL306_ARGUMENT_LIMIT)return 0;
    /* Limits above make both 32-bit lengths and size_t additions safe, even
     * with a 32-bit host. Header includes each NUL; only source is padded. */
    size_t padded=(source_bytes+1+3)&~(size_t)3;
    *size=16+padded+argument_bytes+1;
    return 1;
}
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t ap=(uintptr_t)a,bp=(uintptr_t)b;
    if(an>UINTPTR_MAX-ap || bn>UINTPTR_MAX-bp)return 1;
    return ap<bp+bn && bp<ap+an;
}
int dre_metal306_request13(const void *source,size_t source_bytes,
    const void *arguments,size_t argument_bytes,void *output,size_t capacity,size_t *used)
{
    size_t required=0;
    if(!used)return 0;
    *used=0;
    if(!dre_metal306_request13_size(source_bytes,argument_bytes,&required) ||
       !source || !arguments || !output || capacity<required ||
       overlaps(source,source_bytes,output,required) ||
       overlaps(arguments,argument_bytes,output,required) ||
       memchr(source,0,source_bytes) || memchr(arguments,0,argument_bytes))return 0;
    uint8_t *out=(uint8_t *)output;
    size_t padded=(source_bytes+1+3)&~(size_t)3;
    memset(out,0,required);
    put32(out,(uint32_t)source_bytes+1);
    put32(out+4,0); /* no inline preprocessor metadata */
    put32(out+8,(uint32_t)argument_bytes+1);
    put32(out+12,0); /* no private compiler flags */
    memcpy(out+16,source,source_bytes);
    memcpy(out+16+padded,arguments,argument_bytes);
    *used=required;
    return 1;
}
int dre_metal306_reply(int32_t status,const void *reply,size_t bytes,
    struct dre_metal306_reply *out)
{
    if(!out)return 0;
    memset(out,0,sizeof(*out));
    if(status || !reply || bytes<104 || bytes>UINT32_MAX)return 0;
    const uint8_t *in=(const uint8_t *)reply;
    uint32_t header=get32(in+8),warnings=get32(in+12);
    uint32_t archive=get32(in+40),archive_bytes=get32(in+44);
    uint32_t debug=get32(in+48),debug_bytes=get32(in+52);
    /* The qualified archive serializer emits type3 with a 0x68-byte header.
     * These offsets do not describe the different legacy function-table reply. */
    if(get32(in)!=3 || get32(in+4) || header!=104 || warnings>bytes-header ||
       archive!=header+warnings || archive_bytes<4 || archive_bytes>bytes-archive)return 0;
    size_t end=(size_t)archive+archive_bytes;
    if(debug_bytes) {
        if(debug!=end || debug_bytes!=bytes-end)return 0;
    } else if(debug || end!=bytes)return 0;
    const uint8_t *library=in+archive;
    if(memcmp(library,"MTLB",4))return 0;
    out->warnings=in+header;out->warning_bytes=warnings;
    out->metallib=library;out->metallib_bytes=archive_bytes;
    return 1;
}
