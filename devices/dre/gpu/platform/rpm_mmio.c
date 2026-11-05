/* SPDX-License-Identifier: MIT
 * SEND_ID packing/offset from pinned drivers/soc/qcom/qcom_ipcc.c and live FDT. */
#include "rpm_mmio.h"
static uint64_t owner(void *cookie)
{
    struct dre_rpm_mmio *m=cookie;uint64_t current=m->ops.lease(m->ops.cookie);
    return current==m->lease?current:0;
}
static int read_ram(void *cookie,uint32_t off,uint32_t *value)
{
    struct dre_rpm_mmio *m=cookie;
    if(!owner(m) || off%4 || off>DRE_RPM_MSG_RAM_BYTES-4 || !value)return 0;
    int result=m->ops.read32(m->ops.cookie,DRE_RPM_MSG_RAM_BASE+off,value);
    m->ops.barrier(m->ops.cookie);return result && owner(m);
}
static int write_ram(void *cookie,uint32_t off,uint32_t value)
{
    struct dre_rpm_mmio *m=cookie;
    if(!owner(m) || off%4 || off>DRE_RPM_MSG_RAM_BYTES-4)return 0;
    int result=m->ops.write32(m->ops.cookie,DRE_RPM_MSG_RAM_BASE+off,value);
    m->ops.barrier(m->ops.cookie);return result && owner(m);
}
static void barrier(void *cookie)
{ struct dre_rpm_mmio *m=cookie;m->ops.barrier(m->ops.cookie); }
static int notify_rpm(void *cookie)
{
    struct dre_rpm_mmio *m=cookie;if(!owner(m))return 0;
    m->ops.barrier(m->ops.cookie);
    int result=m->ops.write32(m->ops.cookie,DRE_RPM_IPCC_BASE+0x0c,0);
    m->ops.barrier(m->ops.cookie);return result && owner(m);
}
static uint64_t now(void *cookie)
{ struct dre_rpm_mmio *m=cookie;return m->ops.now(m->ops.cookie); }
int dre_rpm_bind_mmio(struct dre_rpm_mmio *m,const struct dre_rpm_mmio_ops *o,
    const struct dre_rpm_mmio_map *map,struct dre_rpm_io *out)
{
    if(!m || !o || !map || !out || !o->lease || !o->read32 || !o->write32 || !o->barrier || !o->now)return 0;
    if(map->message_base!=DRE_RPM_MSG_RAM_BASE || map->message_bytes!=DRE_RPM_MSG_RAM_BYTES ||
        map->ipcc_base!=DRE_RPM_IPCC_BASE || map->ipcc_bytes!=DRE_RPM_IPCC_BYTES || map->client_id || map->signal_id)return 0;
    uint64_t lease=o->lease(o->cookie);if(!lease)return 0;
    m->ops=*o;m->lease=lease;*out=(struct dre_rpm_io){owner,read_ram,write_ram,barrier,notify_rpm,now,m};return 1;
}
