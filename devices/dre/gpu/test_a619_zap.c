/* SPDX-License-Identifier: MIT; synthetic ELF fixtures, never firmware bytes. */
#include "a619_zap.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char f[512],meta[4096],dst[8192];
static void w16(unsigned off,unsigned v) { f[off]=v; f[off+1]=v>>8; }
static void w32(unsigned off,unsigned v)
{ for (unsigned i=0;i<4;i++) f[off+i]=v>>(i*8); }
static void init(void)
{
    memset(f,0,sizeof(f)); memset(meta,0xcc,sizeof(meta)); memset(dst,0xa5,sizeof(dst));
    memcpy(f,"\177ELF\1\1\1",7);
    w16(16,2); w16(18,164); w32(20,1); w32(28,52);
    w16(40,52); w16(42,32); w16(44,3);
    w32(52+16,148); w32(52+24,0x07000000);
    w32(84+4,160); w32(84+12,0x6000); w32(84+16,32);
    w32(84+20,4096); w32(84+24,0x02200000); w32(84+28,4096);
    w32(116,1); w32(116+4,256); w32(116+12,0x5000);
    w32(116+16,16); w32(116+20,64); w32(116+24,0x08000007); w32(116+28,0x100000);
    for (unsigned i=0;i<32;i++) f[160+i]=(unsigned char)(i+1);
    for (unsigned i=0;i<16;i++) f[256+i]=(unsigned char)(0x80+i);
}
static const char *plan(struct dre_a619_zap_plan *p)
{ return dre_a619_zap_plan(f,sizeof(f),0x90000000,sizeof(dst),p); }
static void unchanged(const struct dre_a619_zap_plan *p)
{ const unsigned char *b=(const void *)p; for (size_t i=0;i<sizeof(*p);i++) assert(b[i]==0xaa); }
static unsigned char *read_file(const char *path,size_t *n)
{
    FILE *in=fopen(path,"rb"); assert(in); assert(!fseek(in,0,SEEK_END));
    long length=ftell(in); assert(length>0 && (unsigned long)length<=DRE_A619_ZAP_MAX_FILE);
    *n=(size_t)length; rewind(in); unsigned char *b=malloc(*n); assert(b);
    assert(fread(b,1,*n,in)==*n); assert(!fclose(in)); return b;
}
int main(int argc,char **argv)
{
    struct dre_a619_zap_plan p; unsigned checks=0;
#define OK() do { assert(!strcmp(plan(&p),"ZAP_PLAN_OK")); checks++; } while(0)
#define BAD(s) do { memset(&p,0xaa,sizeof(p)); assert(!strcmp(plan(&p),(s))); unchanged(&p); checks++; } while(0)
    init(); OK(); assert(p.memory_bytes==4096 && p.segments==1 && p.alignment==0x100000);
    assert(p.segment[0].memory_offset==0 && p.segment[0].memory_bytes==64);
    size_t n=0;
    assert(!strcmp(dre_a619_zap_metadata(f,sizeof(f),0x90000000,sizeof(dst),meta,sizeof(meta),&n),"ZAP_METADATA_OK")); checks++;
    assert(n==180 && !memcmp(meta,f,148) && !memcmp(meta+148,f+160,32) && meta[180]==0xcc);
    assert(!strcmp(dre_a619_zap_copy_segments(f,sizeof(f),0x90000000,dst,sizeof(dst)),"ZAP_SEGMENTS_COPIED")); checks++;
    assert(!memcmp(dst,f+256,16)); for (unsigned i=16;i<4096;i++) assert(!dst[i]); assert(dst[4096]==0xa5);
    for (unsigned i=0;i<7;i++) { init(); f[i]^=0xff; BAD("UNSUPPORTED_ELF"); }
    const unsigned header16[]={16,18,40,42,46,48,50};
    for (unsigned i=0;i<sizeof(header16)/sizeof(*header16);i++) { init(); w16(header16[i],0xff); BAD("UNSUPPORTED_ELF"); }
    const unsigned header32[]={20,28,32};
    for (unsigned i=0;i<sizeof(header32)/sizeof(*header32);i++) { init(); w32(header32[i],0xffffffff); BAD("UNSUPPORTED_ELF"); }
    init(); w16(44,2); BAD("INVALID_PROGRAM_HEADERS");
    init(); w16(44,11); BAD("INVALID_PROGRAM_HEADERS");
    init(); w32(52+16,147); BAD("INVALID_METADATA_HEADER");
    init(); w32(84+4,500); BAD("TRUNCATED_SEGMENT");
    init(); w32(84+16,0); BAD("INVALID_HASH_SEGMENT");
    init(); w32(84,1); BAD("INVALID_HASH_SEGMENT");
    init(); w32(84+4,100); BAD("INVALID_HASH_SEGMENT");
    init(); w32(116+16,65); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+20,0); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+24,7); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+28,3); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+12,0x5001); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+12,0xfffff000); w32(116+20,0x2000); BAD("INVALID_LOAD_SEGMENT");
    init(); w32(116+4,100); BAD("INVALID_LOAD_OFFSET");
    init(); w32(116+4,257); BAD("INVALID_LOAD_OFFSET");
    init(); w32(116+4,176); BAD("OVERLAPPING_FILE_SEGMENTS");
    init(); assert(!strcmp(dre_a619_zap_plan(f,sizeof(f),0x90001000,sizeof(dst),&p),"RESERVED_ALIGNMENT")); checks++;
    assert(!strcmp(dre_a619_zap_plan(f,sizeof(f),0x90000001,sizeof(dst),&p),"INVALID_RESERVED_REGION")); checks++;
    assert(!strcmp(dre_a619_zap_plan(f,sizeof(f),0xfffff000,8192,&p),"INVALID_RESERVED_REGION")); checks++;
    assert(!strcmp(dre_a619_zap_plan(f,sizeof(f),0x90000000,1,&p),"INVALID_RESERVED_REGION")); checks++;
    assert(!strcmp(dre_a619_zap_plan(f,51,0x90000000,sizeof(dst),&p),"INVALID_ARGUMENT")); checks++;
    assert(!strcmp(dre_a619_zap_plan(f,sizeof(f),0x90000000,sizeof(dst),(void *)f),"INVALID_ARGUMENT")); checks++;
    assert(!strcmp(dre_a619_zap_metadata(f,sizeof(f),0x90000000,sizeof(dst),meta,179,&n),"INVALID_METADATA_BUFFER")); checks++;
    assert(!strcmp(dre_a619_zap_metadata(f,sizeof(f),0x90000000,sizeof(dst),f,sizeof(f),&n),"INVALID_METADATA_BUFFER")); checks++;
    assert(!strcmp(dre_a619_zap_metadata(f,sizeof(f),0x90000000,sizeof(dst),meta,sizeof(meta),(void *)meta),"INVALID_METADATA_BUFFER")); checks++;
    assert(!strcmp(dre_a619_zap_copy_segments(f,sizeof(f),0x90000000,f,4096),"INVALID_LOAD_BUFFER")); checks++;
    printf("PASS %u ZAP full-ELF/metadata/relocation bounds cases; synthetic images only\n",checks);
    if (argc==3) {
        size_t nb,nm; unsigned char *b=read_file(argv[1],&nb),*m=read_file(argv[2],&nm);
        assert(!strcmp(dre_a619_zap_plan(b,nb,0x90000000,0x100000,&p),"ZAP_PLAN_OK"));
        unsigned char *metadata=malloc(DRE_A619_ZAP_MAX_METADATA); assert(metadata);
        assert(!strcmp(dre_a619_zap_metadata(b,nb,0x90000000,0x100000,metadata,DRE_A619_ZAP_MAX_METADATA,&n),"ZAP_METADATA_OK"));
        assert(nm==n && !memcmp(m,metadata,n));
        printf("PASS private ZAP layout: file=%zu metadata=%zu span=%u alignment=%u segments=%u; metadata matches private MDT\n",nb,n,p.memory_bytes,p.alignment,p.segments);
        free(metadata); free(b); free(m);
    } else assert(argc==1);
}
