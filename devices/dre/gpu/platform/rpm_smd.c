/* SPDX-License-Identifier: MIT
 * Original bounded implementation of the protocol published by vendor
 * drivers/rpmsg/rpm-smd.c and regulator/rpm-smd-regulator.c, pinned in sources.
 * Not the differently structured generic drivers/soc/qcom/smd-rpm.c ABI. */
#include "rpm_smd.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t n)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8)); }
static const uint32_t levels[]={0,16,32,48,64,128,192,256,320,384,416,512};
/* LIVE supply properties, not the smpe1/smpa5/smpa3 node path names. */
static const uint32_t types[]={0x78637772,0x786d7772,0x78677772}; /* rwcx/rwmx/rwgx */
static const uint32_t ids[]={0,0,0};
static int level_index(uint32_t level)
{ for(unsigned i=0;i<sizeof(levels)/sizeof(levels[0]);i++)if(level==levels[i])return (int)i;return -1; }
uint32_t dre_rpm_level_mask(uint32_t level)
{ int index=level_index(level);return index>0?1u<<index:0; }

int dre_rpm_wire_version(int present,uint64_t value)
{
    if(present==0)return value==0?0:-1;
    if(present!=1)return -1;
    if(value==0)return 0;
    return value==DRE_RPM_PROTOCOL_V1?1:-1;
}
size_t dre_rpm_encode_hold(unsigned version,uint32_t mid,unsigned rail,
    uint32_t level,uint8_t *out,size_t capacity)
{
    if(version>1 || mid<2 || rail>2 || !level || level_index(level)<0 || !out)return 0;
    size_t header=version?16:28,bytes=header+24;
    if(capacity<bytes)return 0;
    memset(out,0,bytes);
    if(!version) {
        put32(out,0x00716572);put32(out+4,(uint32_t)bytes-8);
        put32(out+8,mid);put32(out+16,types[rail]);put32(out+20,ids[rail]);put32(out+24,24);
    } else {
        put32(out,0x01000000u|((uint32_t)bytes-4));put32(out+4,mid);
        put32(out+8,types[rail]);put32(out+12,ids[rail]<<16|24);
    }
    put32(out+header,0x6e657773);put32(out+header+4,4);put32(out+header+8,1); /* swen */
    put32(out+header+12,0x6c766c76);put32(out+header+16,4);put32(out+header+20,level); /* vlvl */
    return bytes;
}
int dre_rpm_decode_ack(unsigned version,uint32_t expected,const uint8_t *p,
    size_t bytes,uint32_t *remote_error)
{
    if(remote_error)*remote_error=0;
    if(version>1 || expected<2 || !p || bytes>DRE_RPM_MAX_PACKET || bytes%4)return -1;
    uint32_t id;
    int result;
    if(!version) {
        if(bytes<20 || get32(p)!=0x00716572 || get32(p+4)!=bytes-8 ||
            get32(p+8)!=0x2367736d || get32(p+12)!=4)return -1; /* req, msg# */
        id=get32(p+16);result=1;
        if(bytes!=20) {
            if(bytes<32 || get32(p+20)!=0x00727265 || !get32(p+24) ||
                get32(p+24)>bytes-28 || ((get32(p+24)+3)&~3u)!=bytes-28)return -1;
            if(remote_error)*remote_error=1; /* bounded firmware string, never logged */
            result=-2;
        }
    } else {
        if(bytes<8 || (get32(p)>>24)!=1)return -1;
        uint32_t type=(get32(p)>>16)&255,length=get32(p)&65535;
        id=get32(p+4);
        if(type==3 && length==0 && bytes==8)result=1;
        else if(type==4 && length==4 && bytes==12 && get32(p+8)) {
            if(remote_error)*remote_error=get32(p+8);
            result=-2;
        } else return -1;
    }
    if(id<2)return -1;
    return id==expected?result:0;
}

static int valid_resources(const struct dre_rpm_resource *r)
{
    if(!r)return 0;
    for(unsigned i=0;i<3;i++) {
        int lo=level_index(r[i].minimum_level),hi=level_index(r[i].maximum_level);
        if(r[i].type!=types[i] || r[i].id!=ids[i] || lo<0 || hi<lo ||
            r[i].minimum_level!=(i==2?0u:16u) || r[i].maximum_level!=512 ||
            !r[i].allowed_levels || (r[i].allowed_levels&~0xffeu))return 0;
        for(unsigned b=0;b<12;b++)if((r[i].allowed_levels&(1u<<b)) && ((int)b<lo || (int)b>hi))return 0;
    }
    return 1;
}
const char *dre_rpm_votes_open(struct dre_rpm_votes *s,const struct dre_rpm_io *o,
    const struct dre_rpm_config *cfg,const struct dre_rpm_resource resources[3],uint64_t budget)
{
    if(!s || s->unsafe || s->pending || s->link.changed || s->next_id)return "RPM_FRESH_STATE_REQUIRED";
    if(!valid_resources(resources))return "RPM_RESOURCE_PLAN_REJECTED";
    const char *result=dre_rpm_glink_open(&s->link,o,cfg,budget);
    if(!s->link.open) { s->unsafe=s->link.unsafe;return s->error=result; }
    memcpy(s->resource,resources,sizeof(s->resource));s->next_id=2;
    return s->error="RPM_ENDPOINT_OPEN";
}
const char *dre_rpm_hold_level(struct dre_rpm_votes *s,const struct dre_rpm_io *o,
    unsigned rail,uint32_t level,uint64_t budget)
{
    if(!s || !o || !o->lease || !o->now || !o->read32 || !o->write32 || !o->barrier || !o->notify)return "RPM_NOT_OPEN";
    if(s->unsafe || s->link.unsafe)return "RPM_RESET_REQUIRED";
    if(!s->link.open || !valid_resources(s->resource))return "RPM_NOT_OPEN";
    if(s->pending)return "RPM_REQUEST_PENDING";
    if(rail>2 || !budget || budget>DRE_RPM_TIMER_HZ)return "RPM_INVALID_REQUEST";
    int index=level_index(level);
    if(!level || index<0 || level<s->resource[rail].minimum_level || level>s->resource[rail].maximum_level ||
        !(s->resource[rail].allowed_levels&(1u<<index)) || level<s->held_level[rail])return "RPM_LEVEL_PLAN_REJECTED";
    if(s->next_id<2 || s->next_id==UINT32_MAX)return "RPM_MESSAGE_IDS_EXHAUSTED";
    if(o->lease(o->cookie)!=s->link.lease) { s->unsafe=1;s->link.unsafe=1;s->link.open=0;return s->error="RPM_OWNERSHIP_LOST"; }
    uint8_t packet[DRE_RPM_MAX_PACKET];size_t count=dre_rpm_encode_hold(s->link.version,s->next_id,rail,level,packet,sizeof(packet));
    if(!count)return "RPM_ENCODE_FAILED";
    uint32_t mid=s->next_id++;s->pending=1;s->error=0;
    s->pending_id=mid;s->pending_rail=rail;s->pending_level=level;s->last_remote_error=0;
    uint64_t start=o->now(o->cookie);
    if(!dre_rpm_glink_send(&s->link,o,packet,count,budget))goto failed;
    for(unsigned polls=0;polls<1000000;polls++) {
        size_t bytes=0;
        int got=dre_rpm_glink_receive(&s->link,o,packet,sizeof(packet),&bytes);
        if(got<0)goto failed;
        uint64_t now=o->now(o->cookie);
        if(now<start || now-start>=budget) { s->error="RPM_ACK_TIMEOUT";goto failed; }
        if(got) {
            uint32_t remote=0;int ack=dre_rpm_decode_ack(s->link.version,mid,packet,bytes,&remote);
            s->last_remote_error=remote;
            if(ack!=1) { s->error=ack==-2?"RPM_NACK":ack==0?"RPM_FOREIGN_ACK":"RPM_MALFORMED_ACK";goto failed; }
            if(o->lease(o->cookie)!=s->link.lease) { s->error="RPM_ACK_OWNERSHIP_LOST";goto failed; }
            s->held_level[rail]=level;s->acked_id[rail]=mid;s->pending=0;
            return s->error="RPM_VOTE_ACKED";
        }
    }
    s->error="RPM_ACK_POLL_LIMIT";
failed:
    s->unsafe=1;s->link.unsafe=1;s->link.open=0;
    /* No rollback request: it could lower a vote already accepted by firmware. */
    if(!s->error)s->error=s->link.error?s->link.error:"RPM_TRANSPORT_FAILED";
    return s->error;
}
