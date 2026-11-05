/* SPDX-License-Identifier: MIT -- UNVALIDATED; no writes or MMIO. */
#include "region.h"
#include "../../libfdt/libfdt.h"
static uint64_t pair(const fdt32_t *p)
{ return (uint64_t)fdt32_to_cpu(p[0])<<32|fdt32_to_cpu(p[1]); }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n&&m&&a<b+m&&b<a+n; }
static int text(const void *p,int n,const char *s)
{
    if(!p||n<=0)return 0;
    for(int i=0;i<n;i++)if(((const char *)p)[i]!=s[i]||(!s[i]&&i!=n-1))return 0;
    return s[n-1]==0;
}
static int ram(const void *dt,uint64_t base,uint64_t bytes)
{
    if(fdt_address_cells(dt,0)!=2||fdt_size_cells(dt,0)!=2)return 0;
    int node,length,covered=0;
    fdt_for_each_subnode(node,dt,0) {
        if(fdt_stringlist_search(dt,node,"device_type","memory")<0)continue;
        if(fdt_getprop(dt,node,"linux,usable-memory",0))return 0;
        const fdt32_t *reg=fdt_getprop(dt,node,"reg",&length);
        if(!reg||length<=0||length%16)return 0;
        for(int i=0;i<length/4;i+=4) {
            uint64_t a=pair(reg+i),n=pair(reg+i+2);
            if(n>UINT64_MAX-a)return 0;
            if(a<=base&&base-a<=n&&bytes<=n-(base-a))covered=1;
        }
    }
    return covered;
}
const char *dre_gpu_fw_reserved_region(const void *dt,struct dre_gpu_fw_region *out)
{
    if(!dt||!out)return "FW_REGION_INVALID_ARGUMENT";
    int node=-1,found=-1,length=0;
    while((node=fdt_node_offset_by_compatible(dt,node,"qcom,pil-tz-generic"))>=0) {
        const fdt32_t *id=fdt_getprop(dt,node,"qcom,pas-id",&length);
        if(!id||length!=4||fdt32_to_cpu(*id)!=13)continue;
        if(found>=0)return "FW_REGION_DUPLICATE_PAS13";
        const void *name=fdt_getprop(dt,node,"qcom,firmware-name",&length);
        if(!text(name,length,"a615_zap"))return "FW_REGION_WRONG_PAS_FIRMWARE";
        const void *status=fdt_getprop(dt,node,"status",&length);
        if(status&&!text(status,length,"ok")&&!text(status,length,"okay"))
            return "FW_REGION_DISABLED_PAS13";
        found=node;
    }
    if(node!=-FDT_ERR_NOTFOUND||found<0)return "FW_REGION_PAS13_MISSING";
    const fdt32_t *handle=fdt_getprop(dt,found,"memory-region",&length);
    if(!handle||length!=4||!fdt32_to_cpu(*handle))return "FW_REGION_INVALID_PHANDLE";
    uint32_t phandle=fdt32_to_cpu(*handle);
    int region=fdt_node_offset_by_phandle(dt,phandle);
    unsigned matches=0;
    for(node=fdt_next_node(dt,-1,0);node>=0;node=fdt_next_node(dt,node,0))
        if(fdt_get_phandle(dt,node)==phandle)matches++;
    if(node!=-FDT_ERR_NOTFOUND||matches!=1)return "FW_REGION_NONUNIQUE_PHANDLE";
    int parent=fdt_path_offset(dt,"/reserved-memory");
    if(region<0||parent<0||fdt_parent_offset(dt,region)!=parent||
       fdt_address_cells(dt,parent)!=2||fdt_size_cells(dt,parent)!=2||
       !fdt_getprop(dt,parent,"ranges",&length)||length||
       !fdt_getprop(dt,region,"no-map",&length)||length||
       fdt_getprop(dt,region,"reusable",0))return "FW_REGION_NOT_FIXED_NOMAP";
    const fdt32_t *reg=fdt_getprop(dt,region,"reg",&length);
    if(!reg||length!=16)return "FW_REGION_BAD_REG";
    uint64_t pa=pair(reg),bytes=pair(reg+2);
    if(pa<UINT64_C(0x80000000)||!bytes||bytes>1024*1024||((pa|bytes)&4095)||
       pa>=UINT64_C(0x100000000)||bytes>UINT64_C(0x100000000)-pa)
        return "FW_REGION_UNSUPPORTED_PHYSICAL_RANGE";
    uint64_t base=pa&~UINT64_C(16383),end=(pa+bytes+16383)&~UINT64_C(16383);
    if(!ram(dt,base,end-base))return "FW_REGION_NOT_COVERED_BY_RAM";
    struct dre_gpu_fw_region result={pa,bytes,base,end-base,phandle,0};
    if(base!=pa||end!=pa+bytes)result.limitations|=DRE_GPU_FW_REGION_SUBPAGE;
    int other;
    fdt_for_each_subnode(other,dt,parent) {
        if(other==region)continue;
        const fdt32_t *range=fdt_getprop(dt,other,"reg",&length);
        if(!range)continue; /* future dynamic pools do not grant GPU ownership */
        if(length<=0||length%16)return "FW_REGION_MALFORMED_NEIGHBOR";
        for(int i=0;i<length/4;i+=4) {
            uint64_t a=pair(range+i),n=pair(range+i+2);
            if(n>UINT64_MAX-a)return "FW_REGION_OVERFLOWING_NEIGHBOR";
            if(overlap(pa,bytes,a,n))return "FW_REGION_OVERLAPPING_RESERVED_OWNER";
            if(overlap(base,end-base,a,n))result.limitations|=DRE_GPU_FW_REGION_ADJACENT_RESERVED;
        }
    }
    int reservations=fdt_num_mem_rsv(dt);
    if(reservations<0)return "FW_REGION_BAD_MEMRESERVE";
    for(int i=0;i<reservations;i++) {
        uint64_t a,n;
        if(fdt_get_mem_rsv(dt,i,&a,&n)||n>UINT64_MAX-a)return "FW_REGION_BAD_MEMRESERVE";
        if(overlap(pa,bytes,a,n))return "FW_REGION_MEMRESERVE_OWNER_UNKNOWN";
        if(overlap(base,end-base,a,n))result.limitations|=DRE_GPU_FW_REGION_ADJACENT_RESERVED;
    }
    *out=result;
    return "FW_REGION_GEOMETRY_ONLY";
}
