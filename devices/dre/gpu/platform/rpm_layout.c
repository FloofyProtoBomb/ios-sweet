/* SPDX-License-Identifier: MIT
 * UNVALIDATED original source-only metadata interpreter; not a device driver.
 * Register/TOC facts: Lineage98a38783f42f5588bf00cd933e299aedc589bc10
 * drivers/irqchip/qcom-mpm.c, drivers/rpmsg/qcom_glink_rpm.c, holi.dtsi.
 * No vendor implementation code or private memory snapshot is included.
 */
#include "rpm_layout.h"
#include "../../libfdt/libfdt.h"
#include <string.h>

#define MPM_OFFSET UINT32_C(0x1b8)
#define MPM_DECLARED UINT32_C(0x1000)
#define MPM_IRQS 96u
#define MPM_IPC UINT64_C(0x0f40000c)
#define TOC_MAGIC UINT32_C(0x67727430)

static int bytes_range(const void *p,size_t bytes)
{ return p&&bytes&&(uintptr_t)p<=UINTPTR_MAX-bytes; }
static int overlap(const void *a,size_t n,const void *b,size_t m)
{ return (uintptr_t)a<(uintptr_t)b+m&&(uintptr_t)b<(uintptr_t)a+n; }
static uint32_t little32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int property_words(const void *f,int node,const char *name,const uint32_t *want,unsigned count)
{
    int bytes;const fdt32_t *v=fdt_getprop(f,node,name,&bytes);
    if(!v||bytes!=(int)(count*4))return 0;
    for(unsigned i=0;i<count;i++)if(fdt32_to_cpu(v[i])!=want[i])return 0;
    return 1;
}
static int text(const void *f,int node,const char *name,const char *wanted,size_t wanted_bytes)
{
    int bytes;const void *v=fdt_getprop(f,node,name,&bytes);
    return v&&bytes==(int)wanted_bytes&&!memcmp(v,wanted,wanted_bytes);
}
static int absent(const void *f,int node,const char *name)
{ int bytes;return !fdt_getprop(f,node,name,&bytes)&&bytes==-FDT_ERR_NOTFOUND; }
static int enabled(const void *f,int node)
{
    for(unsigned depth=0;node>=0&&depth<32;depth++) {
        if(!absent(f,node,"status")&&!text(f,node,"status","ok",3)&&
           !text(f,node,"status","okay",5))return 0;
        if(!node)return 1;node=fdt_parent_offset(f,node);
    }
    return 0;
}
static int unique(const void *f,const char *compatible)
{
    int node=fdt_node_offset_by_compatible(f,-1,compatible);
    return node>=0&&enabled(f,node)&&
        fdt_node_offset_by_compatible(f,node,compatible)==-FDT_ERR_NOTFOUND?node:-1;
}
static struct dre_rpm_layout source_layout(void)
{
    struct dre_rpm_layout result={0};
    result.message_pa=DRE_RPM_MSG_RAM_BASE;result.message_bytes=DRE_RPM_MSG_RAM_BYTES;
    result.foreign_mpm_ipc_pa=MPM_IPC;result.foreign_mpm_ipc_bytes=4;
    result.mpm_irqs=MPM_IRQS;result.mpm_words=(MPM_IRQS+31)/32;
    result.mpm_declared=(struct dre_rpm_layout_extent){MPM_OFFSET,MPM_DECLARED};
    /* Pinned MPM_REGISTER skips the first two timer words and computes
     * ((bank*DIV_ROUND_UP(irqs,32))+index+2)*4. Five banks0..4, no sixth bank.
     */
    result.mpm_linux_footprint=(struct dre_rpm_layout_extent){MPM_OFFSET,
        (2+5*result.mpm_words)*4};
    result.mpm_registers[0]=(struct dre_rpm_layout_extent){MPM_OFFSET,8};
    for(unsigned bank=0;bank<5;bank++)result.mpm_registers[bank+1]=
        (struct dre_rpm_layout_extent){MPM_OFFSET+(2+bank*result.mpm_words)*4,result.mpm_words*4};
    result.mpm_unknown_tail=(struct dre_rpm_layout_extent){
        MPM_OFFSET+result.mpm_linux_footprint.bytes,MPM_DECLARED-result.mpm_linux_footprint.bytes};
    result.toc=(struct dre_rpm_layout_extent){DRE_RPM_MSG_RAM_BYTES-DRE_RPM_LAYOUT_TOC_BYTES,
        DRE_RPM_LAYOUT_TOC_BYTES};
    return result;
}
static int same_extent(struct dre_rpm_layout_extent a,struct dre_rpm_layout_extent b)
{ return a.offset==b.offset&&a.bytes==b.bytes; }
static int exact_layout(const struct dre_rpm_layout *l)
{
    struct dre_rpm_layout e=source_layout();
    if(l->message_pa!=e.message_pa||l->message_bytes!=e.message_bytes||
       l->foreign_mpm_ipc_pa!=e.foreign_mpm_ipc_pa||l->foreign_mpm_ipc_bytes!=e.foreign_mpm_ipc_bytes||
       l->mpm_irqs!=e.mpm_irqs||l->mpm_words!=e.mpm_words||
       !same_extent(l->mpm_declared,e.mpm_declared)||
       !same_extent(l->mpm_linux_footprint,e.mpm_linux_footprint)||
       !same_extent(l->mpm_unknown_tail,e.mpm_unknown_tail)||!same_extent(l->toc,e.toc))return 0;
    for(unsigned i=0;i<6;i++)if(!same_extent(l->mpm_registers[i],e.mpm_registers[i]))return 0;
    return 1;
}
const char *dre_rpm_layout_from_fdt(const void *f,size_t bytes,struct dre_rpm_layout *out)
{
    if(!out||!bytes_range(out,sizeof(*out)))return "RPM_LAYOUT_OUTPUT_REQUIRED";
    if(!bytes_range(f,bytes)||overlap(f,bytes,out,sizeof(*out)))return "RPM_LAYOUT_SEPARATE_ORIGINAL_FDT_REQUIRED";
    *out=(struct dre_rpm_layout){0};
    if(bytes<40||bytes>2u*1024u*1024u||fdt_check_full(f,bytes)||fdt_totalsize(f)!=bytes||
       fdt_node_check_compatible(f,0,"qcom,holi")||fdt_address_cells(f,0)!=2)
        return "RPM_LAYOUT_ORIGINAL_HOLI_FDT_REQUIRED";
    int soc=fdt_path_offset(f,"/soc"),ram=unique(f,"qcom,rpm-msg-ram"),
        mpm=unique(f,"qcom,mpm-gic-holi"),glink=unique(f,"qcom,glink-rpm");
    const uint32_t ram_reg[]={0x045f0000,0x7000};
    const uint32_t mpm_reg[]={0x045f01b8,0x1000,0x0f40000c,4};
    const uint32_t irq_count[]={MPM_IRQS};
    if(soc<0||ram<0||mpm<0||glink<0||fdt_address_cells(f,soc)!=1||fdt_size_cells(f,soc)!=1||
       fdt_parent_offset(f,ram)!=soc||fdt_parent_offset(f,mpm)!=soc||fdt_parent_offset(f,glink)!=soc||
       !property_words(f,ram,"reg",ram_reg,2)||!property_words(f,mpm,"reg",mpm_reg,4)||
       !property_words(f,mpm,"qcom,num-mpm-irqs",irq_count,1)||
       !text(f,mpm,"reg-names","vmpm\0ipc",9))return "RPM_LAYOUT_RESOURCE_PROFILE_UNSUPPORTED";
    int range_bytes;const fdt32_t *ranges=fdt_getprop(f,soc,"ranges",&range_bytes);
    if(!ranges||range_bytes!=16||ranges[0]||ranges[1]||ranges[2]||
       fdt32_to_cpu(ranges[3])<MPM_IPC+4)return "RPM_LAYOUT_SOC_TRANSLATION_UNSUPPORTED";
    /* Resolve a single actual phandle reference, also rejecting a duplicate
     * phandle owner. This describes resource identity, never an acquired lease.
     */
    int n;const fdt32_t *reference=fdt_getprop(f,glink,"qcom,rpm-msg-ram",&n);
    uint32_t phandle=fdt_get_phandle(f,ram);
    if(!reference||n!=4||!phandle||phandle==UINT32_MAX||fdt32_to_cpu(*reference)!=phandle)
        return "RPM_LAYOUT_MESSAGE_RAM_REFERENCE_UNSUPPORTED";
    unsigned seen=0,count=0;int at=-1;
    while((at=fdt_next_node(f,at,0))>=0) {
        if(++count>8192)return "RPM_LAYOUT_FDT_NODE_BOUND";
        uint32_t a=0,b=0;int a_bytes,b_bytes;
        const fdt32_t *p=fdt_getprop(f,at,"phandle",&a_bytes);
        const fdt32_t *q=fdt_getprop(f,at,"linux,phandle",&b_bytes);
        if((p&&a_bytes!=4)||(q&&b_bytes!=4)||(!p&&a_bytes!=-FDT_ERR_NOTFOUND)||
           (!q&&b_bytes!=-FDT_ERR_NOTFOUND))return "RPM_LAYOUT_PHANDLE_FORMAT_UNSUPPORTED";
        if(p)a=fdt32_to_cpu(*p);if(q)b=fdt32_to_cpu(*q);
        if((p&&(!a||a==UINT32_MAX))||(q&&(!b||b==UINT32_MAX))||(p&&q&&a!=b))
            return "RPM_LAYOUT_PHANDLE_FORMAT_UNSUPPORTED";
        if((p&&a==phandle)||(q&&b==phandle)) {
            if(at!=ram||++seen!=1)return "RPM_LAYOUT_DUPLICATE_RAM_PHANDLE";
        }
    }
    if(at!=-FDT_ERR_NOTFOUND||seen!=1)return "RPM_LAYOUT_MESSAGE_RAM_REFERENCE_UNSUPPORTED";
    *out=source_layout();return "RPM_LAYOUT_FDT_DESCRIBED_UNOWNED";
}
static int intersects(uint32_t offset,uint64_t bytes,struct dre_rpm_layout_extent e)
{
    return bytes&&e.bytes&&(uint64_t)offset<(uint64_t)e.offset+e.bytes&&
        (uint64_t)e.offset<(uint64_t)offset+bytes;
}
const char *dre_rpm_layout_describe_toc(const struct dre_rpm_layout *layout,
    const void *snapshot,size_t snapshot_bytes,struct dre_rpm_layout_report *out)
{
    if(!out||!bytes_range(out,sizeof(*out)))return "RPM_LAYOUT_REPORT_REQUIRED";
    if(!layout||!bytes_range(layout,sizeof(*layout))||
       overlap(layout,sizeof(*layout),out,sizeof(*out)))return "RPM_LAYOUT_SEPARATE_SOURCE_DESCRIPTION_REQUIRED";
    if(snapshot&&(!bytes_range(snapshot,snapshot_bytes)||overlap(snapshot,snapshot_bytes,out,sizeof(*out))))
        return "RPM_LAYOUT_SEPARATE_CAPTURED_TOC_REQUIRED";
    *out=(struct dre_rpm_layout_report){0};out->tx_index=out->rx_index=UINT32_MAX;
    if(!exact_layout(layout))return "RPM_LAYOUT_SOURCE_DESCRIPTION_UNSUPPORTED";
    out->layout=*layout;
    if(!snapshot||snapshot_bytes!=DRE_RPM_LAYOUT_TOC_BYTES)return "RPM_LAYOUT_EXACT_CAPTURED_TOC_REQUIRED";
    const uint8_t *toc=snapshot;
    if(little32(toc)!=TOC_MAGIC)out->problems|=DRE_RPM_LAYOUT_BAD_MAGIC;
    uint32_t count=little32(toc+4);
    if(!count||count>DRE_RPM_LAYOUT_MAX_ENTRIES)out->problems|=DRE_RPM_LAYOUT_BAD_COUNT;
    if(out->problems)return "RPM_LAYOUT_TOC_FORMAT_REFUSED_UNOWNED";
    out->count=count;
    for(uint32_t i=0;i<count;i++) {
        struct dre_rpm_layout_entry *e=&out->entry[i];const uint8_t *p=toc+8+i*12;
        e->id=little32(p);e->offset=little32(p+4);e->declared_bytes=little32(p+8);
        e->kind=e->id==DRE_RPM_LAYOUT_AP_TO_RPM?DRE_RPM_LAYOUT_TX:
            e->id==DRE_RPM_LAYOUT_RPM_TO_AP?DRE_RPM_LAYOUT_RX:DRE_RPM_LAYOUT_FOREIGN;
        e->described_bytes=e->declared_bytes;
        if(e->kind!=DRE_RPM_LAYOUT_FOREIGN) {
            /* Actual pinned parser maps tail+0,head+4,fifo+8 and uses size
             * as native.length. Account for both cursor words, including a
             * last payload word beyond a superficially in-range offset+size.
             */
            e->described_bytes+=8;
            if((e->offset&7)||(e->declared_bytes&7)||e->declared_bytes<128||
               e->declared_bytes>0x4000)e->problems|=DRE_RPM_LAYOUT_PIPE_TRANSPORT_LIMIT;
            uint32_t *index=e->kind==DRE_RPM_LAYOUT_TX?&out->tx_index:&out->rx_index;
            if(*index!=UINT32_MAX)e->problems|=e->kind==DRE_RPM_LAYOUT_TX?
                DRE_RPM_LAYOUT_DUPLICATE_TX:DRE_RPM_LAYOUT_DUPLICATE_RX;
            else *index=i;
        } else {
            ++out->foreign_entries;e->problems|=DRE_RPM_LAYOUT_FOREIGN_ENTRY_SEMANTICS;
            /* Unknown IDs are NOT invented FIFO headers. Their declared span
             * is retained for conflict evidence only; any extra/dynamic use
             * and every apparently unused gap remain unowned and unknown.
             */
        }
        if(!e->declared_bytes||e->offset>layout->message_bytes||
           e->described_bytes>(uint64_t)layout->message_bytes-e->offset)
            e->problems|=DRE_RPM_LAYOUT_ENTRY_RANGE;
        if(intersects(e->offset,e->described_bytes,layout->toc))e->problems|=DRE_RPM_LAYOUT_TOC_OVERLAP;
        if(intersects(e->offset,e->described_bytes,layout->mpm_linux_footprint))
            e->problems|=DRE_RPM_LAYOUT_MPM_LINUX_CONFLICT;
        if(intersects(e->offset,e->described_bytes,layout->mpm_unknown_tail))
            e->problems|=DRE_RPM_LAYOUT_MPM_UNKNOWN_CONFLICT;
        for(uint32_t j=0;j<i;j++) {
            struct dre_rpm_layout_entry *other=&out->entry[j];
            if(e->described_bytes&&other->described_bytes&&
               (uint64_t)e->offset<(uint64_t)other->offset+other->described_bytes&&
               (uint64_t)other->offset<(uint64_t)e->offset+e->described_bytes) {
                e->problems|=DRE_RPM_LAYOUT_ENTRY_OVERLAP;
                other->problems|=DRE_RPM_LAYOUT_ENTRY_OVERLAP;
                e->overlaps|=1u<<j;other->overlaps|=1u<<i;
            }
        }
    }
    for(uint32_t i=0;i<count;i++)out->problems|=out->entry[i].problems;
    if(out->tx_index==UINT32_MAX)out->problems|=DRE_RPM_LAYOUT_MISSING_TX;
    if(out->rx_index==UINT32_MAX)out->problems|=DRE_RPM_LAYOUT_MISSING_RX;
    return out->problems?"RPM_LAYOUT_TOC_CONFLICTS_DESCRIBED_UNOWNED":
        "RPM_LAYOUT_TOC_DESCRIBED_UNOWNED";
}
