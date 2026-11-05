/* SPDX-License-Identifier: MIT
 * Qualcomm MDT metadata format/relocation protocol from mdt_loader.c at
 * LineageOS 98a38783f42f5588bf00cd933e299aedc589bc10. Bounds validation and
 * implementation are project-owned; firmware/certificates are never embedded. */
#include "a619_zap.h"
#define TYPE_MASK (7u<<24)
#define TYPE_HASH (2u<<24)
#define RELOCATABLE (1u<<27)
static uint16_t u16(const uint8_t *p) { return p[0]|((uint16_t)p[1]<<8); }
static uint32_t u32(const uint8_t *p)
{ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int equal(const char *a,const char *b)
{
    while (*a && *a==*b) { a++; b++; }
    return *a==*b;
}
static int range(size_t off,size_t len,size_t size) { return off<=size && len<=size-off; }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n && m && a<b+m && b<a+n; }
static int cpu_range(const void *p,size_t n)
{ return p && n && (uintptr_t)p<=UINTPTR_MAX-n; }
static int cpu_overlap(const void *a,size_t n,const void *b,size_t m)
{ return overlap((uintptr_t)a,n,(uintptr_t)b,m); }
const char *dre_a619_zap_plan(const void *file,size_t bytes,
    uint64_t reserved_pa,size_t reserved_bytes,struct dre_a619_zap_plan *out)
{
    if (!file || !out || bytes<52 || bytes>DRE_A619_ZAP_MAX_FILE ||
        !cpu_range(file,bytes) || !cpu_range(out,sizeof(*out)) ||
        cpu_overlap(file,bytes,out,sizeof(*out))) return "INVALID_ARGUMENT";
    if (!reserved_pa || (reserved_pa&4095) || !reserved_bytes ||
        (reserved_bytes&4095) || reserved_pa>UINT32_MAX ||
        reserved_bytes>UINT64_C(0x100000000)-reserved_pa) return "INVALID_RESERVED_REGION";
    const uint8_t *f=file;
    if (f[0]!=0x7f || f[1]!='E' || f[2]!='L' || f[3]!='F' ||
        f[4]!=1 || f[5]!=1 || f[6]!=1 || u16(f+16)!=2 ||
        u16(f+18)!=164 || u32(f+20)!=1 || u16(f+40)!=52 ||
        u16(f+42)!=32 || u32(f+28)!=52 || u32(f+32) ||
        u16(f+46) || u16(f+48) || u16(f+50)) return "UNSUPPORTED_ELF";
    unsigned count=u16(f+44);
    if (count<3 || count>DRE_A619_ZAP_MAX_SEGMENTS+2 ||
        !range(52,count*32,bytes)) return "INVALID_PROGRAM_HEADERS";
    struct dre_a619_zap_plan p={0};
    p.alignment=4096;
    const uint8_t *header=f+52;
    p.metadata_header_bytes=u32(header+16);
    if (u32(header)!=0 || u32(header+4) ||
        p.metadata_header_bytes!=52+count*32 || p.metadata_header_bytes>bytes)
        return "INVALID_METADATA_HEADER";
    uint64_t min=UINT64_MAX,max=0;
    unsigned hash_count=0;
    for (unsigned i=1;i<count;i++) {
        const uint8_t *ph=f+52+i*32;
        uint32_t type=u32(ph),off=u32(ph+4),pa=u32(ph+12);
        uint32_t filesz=u32(ph+16),memsz=u32(ph+20),flags=u32(ph+24),align=u32(ph+28);
        if (!range(off,filesz,bytes)) return "TRUNCATED_SEGMENT";
        if ((flags&TYPE_MASK)==TYPE_HASH) {
            if (type!=0 || !filesz || ++hash_count!=1 ||
                off<p.metadata_header_bytes || filesz>DRE_A619_ZAP_MAX_METADATA-p.metadata_header_bytes)
                return "INVALID_HASH_SEGMENT";
            p.metadata_hash_offset=off; p.metadata_hash_bytes=filesz;
            continue;
        }
        if (type!=1 || !memsz || !(flags&RELOCATABLE) ||
            filesz>memsz || p.segments==DRE_A619_ZAP_MAX_SEGMENTS ||
            (align && (align&(align-1))) || align>0x1000000u ||
            pa%4096 || (uint64_t)pa+memsz>UINT32_MAX)
            return "INVALID_LOAD_SEGMENT";
        if (off<p.metadata_header_bytes || (filesz && off%4)) return "INVALID_LOAD_OFFSET";
        if (align>p.alignment) p.alignment=align;
        if (pa<min) min=pa;
        uint64_t end=((uint64_t)pa+memsz+4095)&~UINT64_C(4095);
        if (end>max) max=end;
        p.segment[p.segments++]=(struct dre_a619_zap_segment){off,filesz,pa,memsz};
    }
    if (!hash_count || !p.segments || min>=max || max-min>UINT32_MAX || max-min>reserved_bytes)
        return "INVALID_IMAGE_SPAN";
    if (reserved_pa&(p.alignment-1)) return "RESERVED_ALIGNMENT";
    p.memory_bytes=(uint32_t)(max-min);
    for (unsigned i=0;i<p.segments;i++) {
        struct dre_a619_zap_segment *s=&p.segment[i];
        s->memory_offset-=(uint32_t)min;
        if (overlap(s->file_offset,s->file_bytes,p.metadata_hash_offset,p.metadata_hash_bytes))
            return "OVERLAPPING_FILE_SEGMENTS";
        for (unsigned j=0;j<i;j++) {
            const struct dre_a619_zap_segment *prev=&p.segment[j];
            if (overlap(s->memory_offset,s->memory_bytes,prev->memory_offset,prev->memory_bytes))
                return "OVERLAPPING_MEMORY_SEGMENTS";
            if (overlap(s->file_offset,s->file_bytes,prev->file_offset,prev->file_bytes))
                return "OVERLAPPING_FILE_SEGMENTS";
        }
    }
    *out=p;
    return "ZAP_PLAN_OK";
}
const char *dre_a619_zap_metadata(const void *file,size_t bytes,
    uint64_t reserved_pa,size_t reserved_bytes,void *metadata,size_t capacity,
    size_t *metadata_bytes)
{
    struct dre_a619_zap_plan p;
    const char *status=dre_a619_zap_plan(file,bytes,reserved_pa,reserved_bytes,&p);
    if (!equal(status,"ZAP_PLAN_OK")) return status;
    size_t total=(size_t)p.metadata_header_bytes+p.metadata_hash_bytes;
    if (!cpu_range(metadata,capacity) || capacity>DRE_A619_ZAP_MAX_METADATA ||
        capacity<total || !cpu_range(metadata_bytes,sizeof(*metadata_bytes)) ||
        cpu_overlap(file,bytes,metadata,capacity) ||
        cpu_overlap(metadata_bytes,sizeof(*metadata_bytes),metadata,capacity) ||
        cpu_overlap(file,bytes,metadata_bytes,sizeof(*metadata_bytes))) return "INVALID_METADATA_BUFFER";
    const uint8_t *src=file; uint8_t *dst=metadata;
    for (size_t i=0;i<p.metadata_header_bytes;i++) dst[i]=src[i];
    for (size_t i=0;i<p.metadata_hash_bytes;i++) dst[p.metadata_header_bytes+i]=src[p.metadata_hash_offset+i];
    *metadata_bytes=total;
    return "ZAP_METADATA_OK";
}
const char *dre_a619_zap_copy_segments(const void *file,size_t bytes,
    uint64_t reserved_pa,void *reserved_cpu,size_t reserved_bytes)
{
    struct dre_a619_zap_plan p;
    const char *status=dre_a619_zap_plan(file,bytes,reserved_pa,reserved_bytes,&p);
    if (!equal(status,"ZAP_PLAN_OK")) return status;
    if (!cpu_range(reserved_cpu,reserved_bytes) || cpu_overlap(file,bytes,reserved_cpu,reserved_bytes))
        return "INVALID_LOAD_BUFFER";
    const uint8_t *src=file; uint8_t *dst=reserved_cpu;
    /* Zero the complete image span, including gaps/padding, before loading. */
    for (size_t i=0;i<p.memory_bytes;i++) dst[i]=0;
    for (unsigned i=0;i<p.segments;i++) {
        const struct dre_a619_zap_segment *s=&p.segment[i];
        for (size_t j=0;j<s->file_bytes;j++) dst[s->memory_offset+j]=src[s->file_offset+j];
    }
    return "ZAP_SEGMENTS_COPIED";
}
