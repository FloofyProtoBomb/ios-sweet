/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DRE_RPM_MSG_RAM_BASE UINT64_C(0x045f0000)
#define DRE_RPM_MSG_RAM_BYTES 0x7000u
#define DRE_RPM_TIMER_HZ 19200000u
#define DRE_RPM_MAX_PACKET 128u
#define DRE_RPM_PROTOCOL_V1 UINT64_C(0x31726576)
#define DRE_RPM_CX 0u
#define DRE_RPM_MX 1u
#define DRE_RPM_GX 2u

/* Returns -1 for an unknown mapped version; absent version reg is vendor v0.
 * reg_present must distinguish an absent FDT property from a failed read. */
int dre_rpm_wire_version(int reg_present, uint64_t value);
/* Bit order is NONE,RET,RET_PLUS,MIN_SVS,LOW_SVS,SVS,SVS_PLUS,NOM,
 * NOM_PLUS,TURBO,TURBO_NO_CPR,BINNING. Zero/off and unknown values return0. */
uint32_t dre_rpm_level_mask(uint32_t level);
/* Active-set, swen=1 + vlvl only. Fixed Holi CX/MX/GX resource allowlist.
 * This codec alone does not acquire a rail. No UV/corner/sleep/decrease API. */
size_t dre_rpm_encode_hold(unsigned version, uint32_t message_id, unsigned rail,
    uint32_t level, uint8_t *output, size_t capacity);
/* 1 matching success; 0 a well-formed foreign ACK; -1 malformed; -2 NACK.
 * Unknown legacy/service encodings never establish an acknowledged vote. */
int dre_rpm_decode_ack(unsigned version, uint32_t expected_id,
    const uint8_t *packet, size_t bytes, uint32_t *remote_error);

struct dre_rpm_io {
    /* The caller holds one exclusive transport lease for the entire session.
     * lease changes on ownership loss, remote reset, mapping loss or suspend.
     * read/write offsets refer ONLY to the qualified 0x7000-byte RPM msg RAM.
     * notify is the separately qualified RPM IPCC mailbox, not an MMIO guess.
     * All callbacks must be bounded; barrier orders MMIO data/head publication. */
    uint64_t (*lease)(void *);
    int (*read32)(void *, uint32_t offset, uint32_t *);
    int (*write32)(void *, uint32_t offset, uint32_t);
    void (*barrier)(void *);
    int (*notify)(void *);
    uint64_t (*now)(void *); /* checked 19.2-MHz monotonic counter */
    void *cookie;
};
struct dre_rpm_pipe { uint32_t offset, bytes, cursor; };
struct dre_rpm_link {
    uint64_t lease;
    struct dre_rpm_pipe tx, rx;
    unsigned version, local_cid, remote_cid;
    unsigned version_ack, remote_version, local_open, local_ack, remote_open;
    int changed, unsafe, open;
    const char *error;
};
struct dre_rpm_config {
    uint64_t physical_base;
    uint32_t bytes, timer_hz, channel_type;
    int version_reg_present;
    uint64_t version_register;
};

/* Fresh, exclusive, EMPTY FIFO ownership is required. Never zeroes or rewinds
 * inherited indices. Negotiates GLINK v1/features0, opens rpm_requests using
 * both channel IDs, and waits for both OPEN directions. Timeout <=1 second.
 * No IRQ registration, clock/voltage write or rail-ready flag is hidden here.
 * Any uncertain publication poisons the session; retain prior held votes and
 * mappings until real remote reset/recovery. Do not free/reinitialize live state. */
const char *dre_rpm_glink_open(struct dre_rpm_link *, const struct dre_rpm_io *,
    const struct dre_rpm_config *, uint64_t budget_ticks);
int dre_rpm_glink_send(struct dre_rpm_link *, const struct dre_rpm_io *,
    const uint8_t *, size_t, uint64_t budget_ticks);
/* Poll: 1 packet, 0 no complete packet, -1 unsafe/protocol failure. */
int dre_rpm_glink_receive(struct dre_rpm_link *, const struct dre_rpm_io *,
    uint8_t *, size_t, size_t *);

struct dre_rpm_resource {
    uint32_t type, id, minimum_level, maximum_level, allowed_levels;
};
struct dre_rpm_votes {
    struct dre_rpm_link link;
    struct dre_rpm_resource resource[3];
    uint32_t next_id, held_level[3], acked_id[3];
    uint32_t pending_id, pending_rail, pending_level, last_remote_error;
    int pending, unsafe;
    const char *error;
};
/* Resource rows must come from the live FDT supply graph and a QUALIFIED
 * platform-wide voltage plan. CX/MX are shared rails: a GPU-only minimum is
 * not a safe floor for CPU/memory consumers. This checks exact rwcx/rwmx/
 * rwgx resource IDs0 and permitted discrete RPM levels; it cannot invent that
 * platform plan. Keep its aggregate floors held while any dependent clock is
 * active. This restricted bring-up provider only raises/holds active votes. */
const char *dre_rpm_votes_open(struct dre_rpm_votes *, const struct dre_rpm_io *,
    const struct dre_rpm_config *, const struct dre_rpm_resource resources[3],
    uint64_t budget_ticks);
/* Returns RPM_VOTE_ACKED only after a matching real ACK and final lease check.
 * ACK means the firmware accepted this request, not a GPU power/domain proof.
 * Failures after publication never lower/restore/release an uncertain vote. */
const char *dre_rpm_hold_level(struct dre_rpm_votes *, const struct dre_rpm_io *,
    unsigned rail, uint32_t level, uint64_t budget_ticks);
#ifdef __cplusplus
}
#endif
