/* SPDX-License-Identifier: MIT
 * Restricted original GLINK-RPM intentless transport. Wire/FIFO facts are
 * pinned in sources.json. No inherited cursor resets and no arbitrary MMIO. */
#include "rpm_smd.h"
#include <string.h>
#define TOC_BYTES 256u
#define POLLS 1000000u
#define FRAME_BYTES (DRE_RPM_MAX_PACKET+16u)
enum { VERSION=0,VERSION_ACK=1,OPEN=2,OPEN_ACK=4,DATA=9,READ_NOTIF=13,SIGNALS=15 };
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t n)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8)); }
static int valid_ops(const struct dre_rpm_io *o)
{ return o && o->lease && o->read32 && o->write32 && o->barrier && o->notify && o->now; }
static int failure(struct dre_rpm_link *s,const char *error)
{ s->error=error;if(s->changed)s->unsafe=1;s->open=0;return -1; }
static int owned(struct dre_rpm_link *s,const struct dre_rpm_io *o)
{
    if(s->unsafe)return 0;
    if(!s->lease || o->lease(o->cookie)!=s->lease) { failure(s,"GLINK_OWNERSHIP_LOST");return 0; }
    return 1;
}
static int read32(struct dre_rpm_link *s,const struct dre_rpm_io *o,uint32_t off,uint32_t *v)
{
    if(off%4 || off>DRE_RPM_MSG_RAM_BYTES-4)return failure(s,"GLINK_READ_RANGE");
    if(!owned(s,o))return -1;
    if(!o->read32(o->cookie,off,v))return failure(s,"GLINK_READ_FAILED");
    o->barrier(o->cookie);
    return owned(s,o)?1:-1;
}
static int store(struct dre_rpm_link *s,const struct dre_rpm_io *o,uint32_t off,uint32_t value)
{
    if(off%4 || off>DRE_RPM_MSG_RAM_BYTES-4)return failure(s,"GLINK_WRITE_RANGE");
    if(!owned(s,o))return -1;
    s->changed=1; /* Even a failed callback may have reached shared RAM. */
    int result=o->write32(o->cookie,off,value);o->barrier(o->cookie);
    if(!result)return failure(s,"GLINK_WRITE_UNCERTAIN");
    return owned(s,o)?1:-1;
}
static int notify(struct dre_rpm_link *s,const struct dre_rpm_io *o)
{
    if(!owned(s,o))return -1;
    o->barrier(o->cookie);
    if(!o->notify(o->cookie))return failure(s,"GLINK_NOTIFY_UNCERTAIN");
    return owned(s,o)?1:-1;
}
static int within(struct dre_rpm_link *s,const struct dre_rpm_io *o,uint64_t start,uint64_t budget)
{
    uint64_t now=o->now(o->cookie);
    if(now<start || now-start>=budget)return failure(s,"GLINK_TRANSACTION_TIMEOUT");
    return owned(s,o)?1:-1;
}
static int pipe_indices(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    const struct dre_rpm_pipe *p,int tx,uint32_t *head,uint32_t *tail)
{
    if(p->bytes<128 || p->bytes>0x4000 || p->bytes%8 || p->offset%8 ||
        p->offset>DRE_RPM_MSG_RAM_BYTES-TOC_BYTES-8-p->bytes)return failure(s,"GLINK_PIPE_RANGE");
    if(read32(s,o,p->offset,tail)<0 || read32(s,o,p->offset+4,head)<0)return -1;
    if(*head>=p->bytes || *tail>=p->bytes || (*head|*tail)&7)return failure(s,"GLINK_INVALID_INDEX");
    if((tx?*head:*tail)!=p->cursor)return failure(s,"GLINK_CURSOR_OWNERSHIP_LOST");
    return 1;
}
static int parse_toc(struct dre_rpm_link *s,const struct dre_rpm_io *o)
{
    uint32_t toc[64],start=DRE_RPM_MSG_RAM_BYTES-TOC_BYTES;
    for(unsigned i=0;i<64;i++)if(read32(s,o,start+i*4,&toc[i])<0)return -1;
    if(toc[0]!=0x67727430 || !toc[1] || toc[1]>20)return failure(s,"GLINK_TOC_INVALID");
    unsigned found=0;
    for(unsigned i=0;i<toc[1];i++) {
        uint32_t id=toc[2+i*3],off=toc[3+i*3],bytes=toc[4+i*3];
        if(off%8 || bytes%8 || !bytes || bytes>0x4000 || off>start-8-bytes)return failure(s,"GLINK_TOC_RANGE");
        for(unsigned j=0;j<i;j++) {
            uint32_t other=toc[3+j*3],length=toc[4+j*3];
            if(off<other+8+length && other<off+8+bytes)return failure(s,"GLINK_TOC_OVERLAP");
        }
        struct dre_rpm_pipe *p=0;unsigned bit=0;
        if(id==0x61703272) { p=&s->tx;bit=1; } /* ap2r */
        if(id==0x72326170) { p=&s->rx;bit=2; } /* r2ap */
        if(p) {
            if(found&bit || bytes<128)return failure(s,"GLINK_TOC_DUPLICATE_OR_SMALL");
            p->offset=off;p->bytes=bytes;found|=bit;
        }
    }
    if(found!=3)return failure(s,"GLINK_TOC_MISSING_PIPE");
    /* Own only EMPTY cursors. Firmware need not have left them at zero. */
    for(unsigned i=0;i<2;i++) {
        struct dre_rpm_pipe *p=i?&s->rx:&s->tx;uint32_t head,tail;
        if(read32(s,o,p->offset,&tail)<0 || read32(s,o,p->offset+4,&head)<0)return -1;
        if(head!=tail || head>=p->bytes || head%8)return failure(s,"GLINK_INHERITED_FIFO_NOT_EMPTY");
        p->cursor=head;
    }
    return 1;
}
static int transmit(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    const uint8_t *frame,size_t bytes,uint64_t start,uint64_t budget)
{
    if(!bytes || bytes>FRAME_BYTES || bytes%8)return failure(s,"GLINK_TX_SIZE");
    if(bytes+8>s->tx.bytes)return failure(s,"GLINK_TX_TOO_LARGE");
    if(within(s,o,start,budget)<0)return -1;
    uint32_t head=0,tail=0;
    for(unsigned polls=0;polls<POLLS;polls++) {
        if(pipe_indices(s,o,&s->tx,1,&head,&tail)<0)return -1;
        uint32_t free=tail<=head?s->tx.bytes-head+tail:tail-head;
        if(free>=bytes+8) {
            for(unsigned i=0;i<bytes;i+=4)if(store(s,o,s->tx.offset+8+(head+i)%s->tx.bytes,get32(frame+i))<0)return -1;
            uint32_t next=(head+(uint32_t)bytes)%s->tx.bytes;
            o->barrier(o->cookie);
            if(within(s,o,start,budget)<0)return -1;
            if(store(s,o,s->tx.offset+4,next)<0)return -1;
            s->tx.cursor=next;
            if(notify(s,o)<0)return -1;
            return within(s,o,start,budget);
        }
        uint64_t now=o->now(o->cookie);
        if(now<start || now-start>=budget)return failure(s,"GLINK_TX_TIMEOUT");
    }
    return failure(s,"GLINK_TX_POLL_LIMIT");
}
static int control(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    unsigned command,unsigned p1,uint32_t p2,uint64_t start,uint64_t budget)
{
    uint8_t packet[24]={0};put32(packet,command|(uint32_t)p1<<16);put32(packet+4,p2);
    size_t bytes=8;
    if(command==OPEN) { memcpy(packet+8,"rpm_requests",13);bytes=24; }
    return transmit(s,o,packet,bytes,start,budget);
}
/* Copies and consumes one complete, aligned frame. Never consumes a partial
 * header/payload. Unsupported fragmentation/commands poison the live session. */
static int frame(struct dre_rpm_link *s,const struct dre_rpm_io *o,uint8_t *data,size_t *count)
{
    uint32_t head,tail;
    if(pipe_indices(s,o,&s->rx,0,&head,&tail)<0)return -1;
    uint32_t avail=head>=tail?head-tail:s->rx.bytes-tail+head;
    if(avail<8)return 0;
    for(unsigned i=0;i<8;i+=4) { uint32_t value;if(read32(s,o,s->rx.offset+8+(tail+i)%s->rx.bytes,&value)<0)return -1;put32(data+i,value); }
    unsigned command=get32(data)&65535;size_t bytes=8;
    if(command==OPEN) {
        if(get32(data+4)!=13)return failure(s,"GLINK_UNKNOWN_ENDPOINT_LENGTH");
        bytes=24;
    } else if(command==DATA) {
        if(avail<16)return 0;
        for(unsigned i=8;i<16;i+=4) { uint32_t value;if(read32(s,o,s->rx.offset+8+(tail+i)%s->rx.bytes,&value)<0)return -1;put32(data+i,value); }
        uint32_t payload=get32(data+8);
        if(!payload || payload>DRE_RPM_MAX_PACKET || get32(data+12) || get32(data+4))return failure(s,"GLINK_FRAGMENT_OR_INTENT_UNSUPPORTED");
        bytes=(16+payload+7)&~7u;
    } else if(command!=VERSION && command!=VERSION_ACK && command!=OPEN_ACK && command!=READ_NOTIF && command!=SIGNALS)
        return failure(s,"GLINK_COMMAND_UNSUPPORTED");
    if(bytes>=s->rx.bytes || bytes>FRAME_BYTES)return failure(s,"GLINK_RX_TOO_LARGE");
    if(avail<bytes)return 0;
    for(unsigned i=command==DATA?16:8;i<bytes;i+=4) { uint32_t value;if(read32(s,o,s->rx.offset+8+(tail+i)%s->rx.bytes,&value)<0)return -1;put32(data+i,value); }
    uint32_t next=(tail+(uint32_t)bytes)%s->rx.bytes;
    o->barrier(o->cookie);
    if(store(s,o,s->rx.offset,next)<0)return -1;
    s->rx.cursor=next;*count=bytes;return 1;
}
const char *dre_rpm_glink_open(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    const struct dre_rpm_config *cfg,uint64_t budget)
{
    if(!s || !cfg || !valid_ops(o))return "GLINK_INVALID_ARGUMENT";
    if(s->changed || s->open || s->unsafe)return "GLINK_FRESH_STATE_REQUIRED";
    if(cfg->physical_base!=DRE_RPM_MSG_RAM_BASE || cfg->bytes!=DRE_RPM_MSG_RAM_BYTES ||
        cfg->timer_hz!=DRE_RPM_TIMER_HZ || cfg->channel_type!=15 || !budget || budget>DRE_RPM_TIMER_HZ)return "GLINK_CONFIG_REJECTED";
    int version=dre_rpm_wire_version(cfg->version_reg_present,cfg->version_register);
    if(version<0)return "GLINK_RPM_VERSION_UNKNOWN";
    s->version=(unsigned)version;s->local_cid=1;s->lease=o->lease(o->cookie);
    if(!s->lease || parse_toc(s,o)<0)return s->error?s->error:"GLINK_NO_OWNERSHIP";
    uint64_t start=o->now(o->cookie);
    if(control(s,o,VERSION,1,0,start,budget)<0)return s->error;
    for(unsigned polls=0;polls<POLLS;polls++) {
        uint8_t packet[FRAME_BYTES];size_t bytes=0;
        int received=frame(s,o,packet,&bytes);
        if(received<0)return s->error;
        if(received) {
            unsigned cmd=get32(packet)&65535,p1=get32(packet)>>16;uint32_t p2=get32(packet+4);
            if(cmd==VERSION) {
                if(p1!=1) { failure(s,"GLINK_VERSION_UNSUPPORTED");return s->error; }
                s->remote_version=1;
                if(control(s,o,VERSION_ACK,1,0,start,budget)<0)return s->error;
            } else if(cmd==VERSION_ACK) {
                if(p1!=1 || p2) { failure(s,"GLINK_VERSION_ACK_REJECTED");return s->error; }
                s->version_ack=1;
            } else if(cmd==OPEN) {
                if((!s->version_ack && !s->remote_version) || !p1 || s->remote_open || memcmp(packet+8,"rpm_requests",13)) {
                    failure(s,"GLINK_UNKNOWN_ENDPOINT");return s->error;
                }
                s->remote_cid=p1;s->remote_open=1;
                if(control(s,o,OPEN_ACK,p1,0,start,budget)<0)return s->error;
            } else if(cmd==OPEN_ACK) {
                if(!s->local_open || p1!=s->local_cid || p2 || s->local_ack) { failure(s,"GLINK_BAD_OPEN_ACK");return s->error; }
                s->local_ack=1;
            } else if(cmd==READ_NOTIF) {
                if(p1 || p2 || notify(s,o)<0) { failure(s,"GLINK_READ_NOTIFY_FAILED");return s->error; }
            } else { failure(s,"GLINK_UNEXPECTED_OPEN_FRAME");return s->error; }
        }
        if(s->version_ack && !s->local_open) {
            s->local_open=1;
            if(control(s,o,OPEN,s->local_cid,13,start,budget)<0)return s->error;
        }
        if(s->local_ack && s->remote_open && within(s,o,start,budget)>0) { s->open=1;return s->error="GLINK_ENDPOINT_OPEN"; }
        if(s->unsafe)return s->error;
        uint64_t now=o->now(o->cookie);
        if(now<start || now-start>=budget) { failure(s,"GLINK_OPEN_TIMEOUT");return s->error; }
    }
    failure(s,"GLINK_OPEN_POLL_LIMIT");return s->error;
}
int dre_rpm_glink_send(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    const uint8_t *data,size_t bytes,uint64_t budget)
{
    if(!s || !valid_ops(o) || !s->open || !data || !bytes || bytes>DRE_RPM_MAX_PACKET || !budget || budget>DRE_RPM_TIMER_HZ)return 0;
    uint8_t packet[FRAME_BYTES]={0};put32(packet,DATA|s->local_cid<<16);put32(packet+8,(uint32_t)bytes);memcpy(packet+16,data,bytes);
    return transmit(s,o,packet,(bytes+16+7)&~7u,o->now(o->cookie),budget)>0;
}
int dre_rpm_glink_receive(struct dre_rpm_link *s,const struct dre_rpm_io *o,
    uint8_t *packet,size_t capacity,size_t *count)
{
    if(!s || !valid_ops(o) || !s->open || !packet || !count)return -1;
    *count=0;
    uint8_t message[FRAME_BYTES];size_t bytes=0;int got=frame(s,o,message,&bytes);
    if(got<=0)return got;
    unsigned cmd=get32(message)&65535,cid=get32(message)>>16;uint32_t value=get32(message+4);
    if(cmd==READ_NOTIF && !cid && !value)return notify(s,o)<0?-1:0;
    if(cmd==SIGNALS && cid==s->remote_cid && !(value&0x0fffffffu))return 0;
    if(cmd!=DATA || cid!=s->remote_cid)return failure(s,"GLINK_UNEXPECTED_ENDPOINT_FRAME");
    size_t payload=get32(message+8);
    if(payload>capacity)return failure(s,"GLINK_PACKET_CAPACITY");
    memcpy(packet,message+16,payload);*count=payload;return owned(s,o)?1:-1;
}
