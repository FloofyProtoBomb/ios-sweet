/* SPDX-License-Identifier: MIT
 * UNVALIDATED, source-only original-FDT supply graph binding.
 * Source semantics: platform/sources.json, Lineage98a38783f42f5588bf00cd933e299aedc589bc10
 * holi.dtsi/holi-gpu.dtsi/holi-gdsc.dtsi and captured live nonidentity graph.
 * No voltage policy, ownership, power-ready flag or hardware write is inferred. */
#include "profile.h"
#include <libfdt.h>
#include <string.h>
#define NODE_LIMIT 8192u
static int one(const void *f,int node,const char *key,uint32_t want)
{
    int n;const fdt32_t *v=fdt_getprop(f,node,key,&n);
    return v&&n==4&&fdt32_to_cpu(*v)==want;
}
static int pair(const void *f,int node,const char *key,uint32_t a,uint32_t b)
{
    int n;const fdt32_t *v=fdt_getprop(f,node,key,&n);
    return v&&n==8&&fdt32_to_cpu(v[0])==a&&fdt32_to_cpu(v[1])==b;
}
static int text(const void *f,int node,const char *key,const char *want)
{
    int n;const char *v=fdt_getprop(f,node,key,&n);size_t bytes=strlen(want)+1;
    return v&&n==(int)bytes&&!memcmp(v,want,bytes);
}
static int absent(const void *f,int node,const char *key)
{ int n;return !fdt_getprop(f,node,key,&n)&&n==-FDT_ERR_NOTFOUND; }
static int boolean(const void *f,int node,const char *key)
{ int n;return fdt_getprop(f,node,key,&n)!=0&&n==0; }
static int enabled(const void *f,int node)
{
    for(unsigned depth=0;node>=0&&depth<32;depth++) {
        if(!absent(f,node,"status")&&!text(f,node,"status","ok")&&
            !text(f,node,"status","okay"))return 0;
        if(!node)return 1;
        node=fdt_parent_offset(f,node);
    }
    return 0;
}
static int unique(const void *f,const char *compatible)
{
    int node=fdt_node_offset_by_compatible(f,-1,compatible);
    return node>=0&&fdt_node_offset_by_compatible(f,node,compatible)==-FDT_ERR_NOTFOUND&&
        enabled(f,node)?node:-1;
}
/* Strict property vocabulary also rejects duplicate properties and silently
 * introduced timing/reset/retention/clock/mailbox variants. Unknown layouts
 * must be separately grounded and supported, not ignored by this profile. */
static int vocabulary(const void *f,int node,const char *const *names,size_t count)
{
    if(node<0||count>64)return 0;
    uint64_t seen=0;int offset=fdt_first_property_offset(f,node);
    for(unsigned checked=0;offset>=0&&checked<64;checked++) {
        const char *name=0;int bytes;
        if(!fdt_getprop_by_offset(f,offset,&name,&bytes)||!name)return 0;
        size_t i;for(i=0;i<count;i++)if(!strcmp(name,names[i]))break;
        if(i==count||(seen&(UINT64_C(1)<<i)))return 0;
        seen|=UINT64_C(1)<<i;offset=fdt_next_property_offset(f,offset);
    }
    return offset==-FDT_ERR_NOTFOUND;
}
/* Resolve only a canonical, unique actual phandle. A node name, first match,
 * stale overlay handle or caller-supplied offset cannot grant resource identity. */
static int reference(const void *f,int node,const char *key)
{
    int bytes;const fdt32_t *value=fdt_getprop(f,node,key,&bytes);
    if(!value||bytes!=4)return -1;
    uint32_t handle=fdt32_to_cpu(*value);if(!handle||handle==UINT32_MAX)return -1;
    int result=-1,at=-1;unsigned count=0;
    while((at=fdt_next_node(f,at,0))>=0) {
        if(++count>NODE_LIMIT)return -1;
        int aBytes,bBytes;const fdt32_t *a=fdt_getprop(f,at,"phandle",&aBytes);
        const fdt32_t *b=fdt_getprop(f,at,"linux,phandle",&bBytes);
        if((a&&aBytes!=4)||(b&&bBytes!=4)||(a&&b&&*a!=*b))return -1;
        if((a&&fdt32_to_cpu(*a)==handle)||(b&&fdt32_to_cpu(*b)==handle)) {
            if(result>=0)return -1;
            result=at;
        }
    }
    return at==-FDT_ERR_NOTFOUND&&result>=0&&enabled(f,result)?result:-1;
}
static int reg(const void *f,int soc,int node,uint32_t address,uint32_t bytes)
{ return node>=0&&fdt_parent_offset(f,node)==soc&&pair(f,node,"reg",address,bytes); }
static int syscon(const void *f,int soc,int owner,const char *key,uint32_t address)
{
    static const char *const names[]={"compatible","reg","phandle","linux,phandle","status"};
    int node=reference(f,owner,key);
    return reg(f,soc,node,address,4)&&text(f,node,"compatible","syscon")&&
        vocabulary(f,node,names,sizeof(names)/sizeof(*names));
}
static int gdsc(const void *f,int soc,int node,int gx)
{
    static const char *const cxNames[]={"compatible","reg","regulator-name","phandle",
        "linux,phandle","status","parent-supply","hw-ctrl-addr",
        "qcom,no-status-check-on-disable","qcom,gds-timeout","qcom,clk-dis-wait-val"};
    static const char *const gxNames[]={"compatible","reg","regulator-name","phandle",
        "linux,phandle","status","parent-supply","sw-reset","acd-reset",
        "acd-misc-reset","domain-addr","qcom,reset-aon-logic"};
    if(!reg(f,soc,node,gx?0x599100c:0x599106c,4)||
       !text(f,node,"compatible","qcom,gdsc")||!enabled(f,node))return 0;
    if(gx)return vocabulary(f,node,gxNames,sizeof(gxNames)/sizeof(*gxNames))&&
        text(f,node,"regulator-name","gpu_gx_gdsc")&&boolean(f,node,"qcom,reset-aon-logic")&&
        syscon(f,soc,node,"sw-reset",0x5991008)&&syscon(f,soc,node,"acd-reset",0x5991160)&&
        syscon(f,soc,node,"acd-misc-reset",0x5998004)&&syscon(f,soc,node,"domain-addr",0x5991508);
    return vocabulary(f,node,cxNames,sizeof(cxNames)/sizeof(*cxNames))&&
        text(f,node,"regulator-name","gpu_cx_gdsc")&&boolean(f,node,"qcom,no-status-check-on-disable")&&
        one(f,node,"qcom,gds-timeout",500)&&one(f,node,"qcom,clk-dis-wait-val",8)&&
        syscon(f,soc,node,"hw-ctrl-addr",0x5991540);
}
static int rail(const void *f,struct dre_holi_power_profile *p,unsigned i)
{
    static const char *const childNames[]={"compatible","regulator-name","phandle",
        "linux,phandle","status","qcom,set","regulator-min-microvolt",
        "regulator-max-microvolt","qcom,init-voltage-level","qcom,use-voltage-level",
        "qcom,proxy-consumer-enable","qcom,proxy-consumer-voltage"};
    static const char *const parentNames[]={"compatible","phandle","linux,phandle","status",
        "qcom,resource-name","qcom,resource-id","qcom,regulator-type","qcom,hpm-min-load",
        "proxy-supply","qcom,always-wait-for-ack"};
    static const char *const types[]={"rwcx","rwmx","rwgx"};
    static const uint32_t numbers[]={0x78637772,0x786d7772,0x78677772};
    int node=p->regulator[i],parent=fdt_parent_offset(f,node);uint32_t minimum=i==2?0:16,init=i==2?64:384;
    if(node<0||parent<0||fdt_parent_offset(f,parent)!=p->rpm||!enabled(f,node)||
       !text(f,node,"compatible","qcom,rpm-smd-regulator")||
       !text(f,parent,"compatible","qcom,rpm-smd-regulator-resource")||
       !vocabulary(f,node,childNames,sizeof(childNames)/sizeof(*childNames))||
       !vocabulary(f,parent,parentNames,sizeof(parentNames)/sizeof(*parentNames))||
       !text(f,parent,"qcom,resource-name",types[i])||!one(f,parent,"qcom,resource-id",0)||
       !one(f,parent,"qcom,regulator-type",1)||!one(f,node,"qcom,set",3)||
       !one(f,node,"regulator-min-microvolt",minimum)||!one(f,node,"regulator-max-microvolt",512)||
       !one(f,node,"qcom,init-voltage-level",init)||!boolean(f,node,"qcom,use-voltage-level"))return 0;
    if(i<2) {
        if(reference(f,parent,"proxy-supply")!=node||!boolean(f,node,"qcom,proxy-consumer-enable")||
           !pair(f,node,"qcom,proxy-consumer-voltage",384,512))return 0;
        p->proxy_minimum[i]=384;p->proxy_maximum[i]=512;
        if(!absent(f,parent,"qcom,always-wait-for-ack")&&!boolean(f,parent,"qcom,always-wait-for-ack"))return 0;
    } else if(!absent(f,parent,"proxy-supply")||!absent(f,node,"qcom,proxy-consumer-enable")||
        !absent(f,node,"qcom,proxy-consumer-voltage")||!boolean(f,parent,"qcom,always-wait-for-ack"))return 0;
    p->resource[i]=parent;p->init_level[i]=init;
    p->resources[i]=(struct dre_rpm_resource){.type=numbers[i],.id=0,
        .minimum_level=minimum,.maximum_level=512,.allowed_levels=0};
    return 1;
}
const char *dre_holi_power_profile_from_fdt(const void *f,size_t bytes,struct dre_holi_power_profile *out)
{
    if(!f||!out||(uintptr_t)out>UINTPTR_MAX-sizeof(*out)||
       bytes<sizeof(struct fdt_header)||bytes>2u*1024u*1024u||
       (uintptr_t)f>UINTPTR_MAX-bytes||fdt_check_full(f,bytes)||fdt_totalsize(f)!=bytes||
       fdt_node_check_compatible(f,0,"qcom,holi"))return "HOLI_PROFILE_INVALID_ORIGINAL_FDT";
    if((uintptr_t)out<(uintptr_t)f+bytes&&(uintptr_t)f<(uintptr_t)out+sizeof(*out))
        return "HOLI_PROFILE_OUTPUT_ALIAS";
    /* Keep output unusable on every partial graph refusal. */
    memset(out,0,sizeof(*out));struct dre_holi_power_profile p;memset(&p,0,sizeof(p));
    int soc=fdt_path_offset(f,"/soc"),n;
    const fdt32_t *ranges=fdt_getprop(f,soc,"ranges",&n);
    if(soc<0||fdt_address_cells(f,0)!=2||fdt_address_cells(f,soc)!=1||fdt_size_cells(f,soc)!=1||
       !ranges||n!=16||ranges[0]||ranges[1]||ranges[2]||fdt32_to_cpu(ranges[3])<0x59a0000)
        return "HOLI_PROFILE_SOC_TRANSLATION_UNSUPPORTED";
    p.gpu=unique(f,"qcom,adreno-gpu-a619-holi");p.gpucc=unique(f,"qcom,holi-gpucc");
    p.rpm=unique(f,"qcom,rpm-smd");
    if(p.gpu<0||p.gpucc<0||p.rpm<0||!one(f,p.gpu,"qcom,chipid",DRE_HOLI_CHIP)||
       fdt_parent_offset(f,p.gpu)!=soc||!reg(f,soc,p.gpucc,0x5990000,0x9000)||
       fdt_parent_offset(f,p.rpm)!=soc||!text(f,p.rpm,"rpm-channel-name","rpm_requests")||
       !one(f,p.rpm,"rpm-channel-type",15)||!absent(f,p.rpm,"reg"))
        return "HOLI_PROFILE_CORE_NODES_UNSUPPORTED";
    p.cx_gdsc=reference(f,p.gpu,"vddcx-supply");p.gx_gdsc=reference(f,p.gpu,"vdd-supply");
    if(!gdsc(f,soc,p.cx_gdsc,0)||!gdsc(f,soc,p.gx_gdsc,1))return "HOLI_PROFILE_GDSC_VARIANT_UNSUPPORTED";
    static const char *const supplies[]={"vdd_cx-supply","vdd_mx-supply","vdd_gx-supply"};
    for(unsigned i=0;i<3;i++) {
        p.regulator[i]=reference(f,p.gpucc,supplies[i]);
        if(!rail(f,&p,i))return "HOLI_PROFILE_RPM_RESOURCE_UNSUPPORTED";
        for(unsigned j=0;j<i;j++)if(p.regulator[j]==p.regulator[i]||p.resource[j]==p.resource[i])
            return "HOLI_PROFILE_SUPPLY_ALIAS";
    }
    if(reference(f,p.cx_gdsc,"parent-supply")!=p.regulator[0]||
       reference(f,p.gx_gdsc,"parent-supply")!=p.regulator[2]||
       reference(f,p.gpu,"vdd-parent-supply")!=p.regulator[2]||
       !one(f,p.gpu,"vdd-parent-min-corner",16))return "HOLI_PROFILE_PARENT_SUPPLY_MISMATCH";
    p.config=(struct dre_holi_power_config){.chip_id=DRE_HOLI_CHIP,.timer_hz=19200000,.xo_hz=19200000,
        .gfx_nominal_hz=253000000,.gmu_nominal_hz=200000000,.variant_features=3,
        .cx_timeout_us=500,.gx_timeout_us=100,.cx_clk_dis_wait=8,
        .gpucc_base=0x5990000,.gcc_base=0x1400000,.cx_gdscr=0x599106c,.cx_status=0x5991540,
        .gx_gdscr=0x599100c,.gx_sw_reset=0x5991008,.gx_acd_reset=0x5991160,
        .gx_misc_reset=0x5998004,.gx_domain=0x5991508};
    /* All fields below retain zero masks/floors. Actual inherited aggregate
     * policy, ACKs and held clock/reset/IRQ/SCM providers are still required. */
    *out=p;return "HOLI_POWER_GRAPH_OBSERVED_POLICY_REQUIRED";
}
