/* SPDX-License-Identifier: MIT
 * Host-only fixture: real input.c + report.c, controlled controller/IRQ/HID
 * boundaries. No real service, transport ownership or phone is represented.
 */
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do { ++checks;if(!(e)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e);exit(1);} } while(0)
static unsigned checks;
struct fixture {
    struct dre_tn_spi spi;
    struct dre_tn_controller controller;
    struct dre_tn_input input;
    uint8_t packet[24][65];
    unsigned packet_count,read_count,samples,rearms,masks,queues,delay,submits,accepted,quiesces;
    unsigned fail_submit,fail_queue,fail_sample,fail_rearm,fail_quiesce,fault_read,generation_read,source_fault;
    unsigned force_high;
    uint64_t time;
    struct dre_tn_hid_frame events[64];
};
static struct fixture *active;
static int mask(void *c,void *lease)
{ struct fixture *f=c;CHECK(lease==f);++f->masks;return 0; }
static int queue_work(void *c,unsigned delay)
{ struct fixture *f=c;++f->queues;f->delay=delay;return f->fail_queue?1:0; }
static int sample(void *c,void *lease,unsigned *level)
{ struct fixture *f=c;CHECK(lease==f);++f->samples;if(f->fail_sample)return 1;*level=f->force_high||f->read_count>=f->packet_count;return 0; }
static int rearm(void *c,void *lease,unsigned *again)
{ struct fixture *f=c;CHECK(lease==f);++f->rearms;if(f->fail_rearm)return 1;*again=f->read_count<f->packet_count;return 0; }
static uint64_t time_now(void *c) { return ++((struct fixture *)c)->time; }
static enum dre_tn_result submit(void *c,const struct dre_tn_hid_frame *frame)
{
    struct fixture *f=c;++f->submits;
    CHECK(frame->version==1&&frame->generation&&!(frame->active_mask&~0x3ffu));
    CHECK(!frame->cancel_reason||!frame->active_mask);
    if(f->source_fault&&!frame->cancel_reason)return DRE_TN_OWNERSHIP;
    if(f->fail_submit){--f->fail_submit;return DRE_TN_INPUT;}
    CHECK(f->accepted<64);f->events[f->accepted++]=*frame;return DRE_TN_OK;
}
static int quiesce(void *c,void *lease)
{ struct fixture *f=c;CHECK(lease==f);++f->quiesces;return f->fail_quiesce?1:0; }
/* Deliberately injected controller boundary. Actual wire parsing is linked
 * from report.c; separate controller fixture covers real SPI framing below.
 */
enum dre_tn_result dre_tn_controller_report(struct dre_tn_controller *c,struct dre_touch_frame *out)
{
    struct fixture *f=active;CHECK(c==&f->controller);++f->read_count;
    if(f->fault_read==f->read_count){c->state=DRE_TN_CONTROLLER_FAULT;return DRE_TN_TIMEOUT;}
    if(f->generation_read==f->read_count)++c->generation;
    CHECK(f->read_count<=f->packet_count);
    enum dre_touch_result r=dre_touch_decode(f->packet[f->read_count-1],65,1079,2399,out);
    return r==DRE_TOUCH_OK?DRE_TN_OK:r==DRE_TOUCH_BAD_CHECKSUM?DRE_TN_CHECKSUM:
        r==DRE_TOUCH_WATCHDOG_PATTERN?DRE_TN_RECOVERY:DRE_TN_PROTOCOL;
}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));active=f;f->spi.state=DRE_TN_RUNNING;f->spi.lease=f;
    f->controller=(struct dre_tn_controller){.spi=&f->spi,.state=DRE_TN_CONTROLLER_READY,.generation=7};
    struct dre_tn_input_ops o={f,mask,queue_work,sample,rearm,time_now,submit,quiesce};
    CHECK(dre_tn_input_start(&f->input,&f->controller,&o)==DRE_TN_OK);
}
static uint8_t *packet(struct fixture *f)
{ CHECK(f->packet_count<24);return f->packet[f->packet_count++]; }
static void point(uint8_t *p,unsigned slot,unsigned id,unsigned x,unsigned y)
{
    p+=slot*6;p[0]=(uint8_t)(id*8+2);p[1]=(uint8_t)(x>>4);p[2]=(uint8_t)(y>>4);
    p[3]=(uint8_t)((x&15)*16+(y&15));p[4]=9;p[5]=100;
}
static void seal(uint8_t *p) { unsigned sum=0;for(unsigned i=0;i<64;i++)sum+=p[i];p[64]=(uint8_t)-sum; }
static void press(struct fixture *f,unsigned id)
{ uint8_t *p=packet(f);point(p,0,id,50+id,80+id);seal(p); }
static void run(struct fixture *f)
{ CHECK(dre_tn_input_irq(&f->input)==DRE_TN_OK);CHECK(dre_tn_input_worker(&f->input)==DRE_TN_OK); }
static void lifecycle(void)
{
    struct fixture f;init(&f);
    uint8_t *p=packet(&f);point(p,0,9,900,1800);point(p,1,2,100,200);seal(p);
    run(&f);CHECK(f.accepted==1&&f.events[0].active_mask==0x102);
    p=packet(&f);point(p,0,2,101,201);point(p,1,9,901,1801);seal(p);run(&f);
    CHECK(f.events[1].contacts[8].x==901&&f.events[1].contacts[1].x==101);
    CHECK(f.events[1].generation==7&&f.events[1].cancel_reason==0);
    p=packet(&f);point(p,0,9,902,1802);seal(p);run(&f);
    CHECK(f.events[2].active_mask==0x100&&f.events[2].contacts[8].x==902);
    p=packet(&f);seal(p);run(&f);CHECK(f.events[3].active_mask==0&&!f.events[3].cancel_reason);
    CHECK(f.input.valid_reports==4&&f.input.submitted_frames==4);
}
static void held_and_corrupt(void)
{
    struct fixture f;init(&f);press(&f,3);f.fail_submit=1;
    CHECK(dre_tn_input_irq(&f.input)==DRE_TN_OK);
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_INPUT);
    CHECK(f.input.has_held&&!f.input.submitted.active_mask&&f.delay==10&&!f.accepted);
    uint64_t stamp=f.input.held.timestamp;
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_OK);
    CHECK(f.accepted==1&&f.events[0].timestamp==stamp&&f.input.submitted.active_mask==4);
    uint8_t *p=packet(&f);p[0]=0x19; /* bad checksum, not a release */
    CHECK(dre_tn_input_irq(&f.input)==DRE_TN_OK);
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_CHECKSUM);
    CHECK(f.input.rejected_reports==1&&f.accepted==1&&f.input.submitted.active_mask==4);
    p=packet(&f);memset(p,0xfd,6);
    CHECK(dre_tn_input_irq(&f.input)==DRE_TN_OK);
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_RECOVERY);
    CHECK(f.accepted==1&&f.input.submitted.active_mask==4);
}
static void terminal_loss(void)
{
    for(unsigned mode=0;mode<5;mode++) {
        struct fixture f;init(&f);press(&f,4);run(&f);
        press(&f,6);f.fail_submit=1;
        CHECK(dre_tn_input_irq(&f.input)==DRE_TN_OK);
        CHECK(dre_tn_input_worker(&f.input)==DRE_TN_INPUT);CHECK(f.input.has_held);
        unsigned reads=f.read_count,rearms=f.rearms,samples=f.samples;
        if(mode==0)f.controller.state=DRE_TN_CONTROLLER_FAULT;
        if(mode==1)++f.controller.generation;
        if(mode==2)f.spi.state=DRE_TN_QUARANTINED;
        if(mode==3)f.input.held.generation++;
        if(mode==4)f.source_fault=1;
        f.fail_submit=1;
        enum dre_tn_result r=mode==4?dre_tn_input_source_lost(&f.input):dre_tn_input_worker(&f.input);
        CHECK(r==DRE_TN_INPUT&&f.input.device_lost&&!f.input.has_held);
        CHECK(f.input.submitted.active_mask==8&&f.accepted==1&&f.delay==10);
        CHECK(f.read_count==reads&&f.rearms==rearms&&f.samples==samples);
        unsigned submitted=f.submits;
        r=mode==4?dre_tn_input_source_lost(&f.input):dre_tn_input_worker(&f.input);
        CHECK(r==DRE_TN_RECOVERY&&f.submits==submitted+1&&f.accepted==2);
        CHECK(!f.events[1].active_mask&&f.events[1].cancel_reason==DRE_TN_DEVICE_LOST);
        CHECK(f.events[1].generation==7&&f.events[1].contacts[3].x==54);
        CHECK(!f.input.submitted.active_mask&&f.read_count==reads&&f.rearms==rearms);
        CHECK(dre_tn_input_start(&f.input,&f.controller,&f.input.ops)==DRE_TN_STATE);
    }
}
static void held_delivery_preserves_gpio_edge(void)
{
    struct fixture f;
    init(&f);
    press(&f, 3);
    f.fail_submit = 2;
    CHECK(dre_tn_input_irq(&f.input) == DRE_TN_OK);
    CHECK(dre_tn_input_worker(&f.input) == DRE_TN_INPUT);
    CHECK(f.input.has_held && f.read_count == 1);
    press(&f, 6);
    f.force_high = 1;
    CHECK(dre_tn_input_irq(&f.input) == DRE_TN_OK);
    CHECK(dre_tn_input_worker(&f.input) == DRE_TN_INPUT);
    CHECK(f.input.has_held && f.read_count == 1 && !f.accepted);
    CHECK(dre_tn_input_worker(&f.input) == DRE_TN_OK);
    CHECK(f.read_count == 2 && f.accepted == 2);
    CHECK(f.events[0].active_mask == 4 && f.events[1].active_mask == 32);
}
static void faults_and_stop(void)
{
    struct fixture f;init(&f);press(&f,1);run(&f);
    f.fail_submit=1;f.fail_queue=1;
    CHECK(dre_tn_input_source_lost(&f.input)==DRE_TN_PROVIDER);
    CHECK(f.input.submitted.active_mask==1&&f.input.device_lost&&f.accepted==1);
    f.fail_queue=0;CHECK(dre_tn_input_source_lost(&f.input)==DRE_TN_RECOVERY);
    init(&f);press(&f,10);run(&f);f.fail_submit=1;
    CHECK(dre_tn_input_stop(&f.input)==DRE_TN_INPUT);
    CHECK(f.input.submitted.active_mask==0x200&&f.input.stopping&&f.quiesces==1);
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_STATE);
    CHECK(dre_tn_input_source_lost(&f.input)==DRE_TN_STATE);
    CHECK(dre_tn_input_stop(&f.input)==DRE_TN_OK&&f.quiesces==2);
    CHECK(f.accepted==2&&!f.events[1].active_mask&&f.events[1].cancel_reason==DRE_TN_STOPPED_BY_OWNER);
    init(&f);press(&f,2);run(&f);f.fail_sample=1;
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_RECOVERY&&f.input.device_lost);
    CHECK(f.accepted==2&&!f.input.submitted.active_mask);
    init(&f);press(&f,2);run(&f);f.fail_rearm=1;
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_RECOVERY&&f.input.device_lost);
    for(unsigned mode=0;mode<2;mode++) {
        init(&f);press(&f,1);run(&f);press(&f,8);
        if(mode)f.generation_read=f.read_count+1;else f.fault_read=f.read_count+1;
        CHECK(dre_tn_input_irq(&f.input)==DRE_TN_OK);
        CHECK(dre_tn_input_worker(&f.input)==DRE_TN_RECOVERY);
        CHECK(f.accepted==2&&f.events[1].cancel_reason==DRE_TN_DEVICE_LOST&&
            !f.input.submitted.active_mask&&f.input.device_lost);
    }
    init(&f);f.input.worker_active=1;unsigned calls=f.submits;
    CHECK(dre_tn_input_source_lost(&f.input)==DRE_TN_STATE&&f.submits==calls);
    CHECK(dre_tn_input_worker(&f.input)==DRE_TN_STATE&&f.submits==calls);
    f.input.worker_active=0;f.fail_quiesce=1;
    CHECK(dre_tn_input_stop(&f.input)==DRE_TN_OWNERSHIP);
}
int main(void)
{ lifecycle();held_and_corrupt();terminal_loss();held_delivery_preserves_gpio_edge();faults_and_stop();printf("PASS %u production input/report boundary checks\n",checks);return 0; }
