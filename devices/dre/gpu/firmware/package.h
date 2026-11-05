/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source. */
#ifndef DRE_GPU_FIRMWARE_PACKAGE_H
#define DRE_GPU_FIRMWARE_PACKAGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_FW_MAGIC UINT64_C(0x3146555047455244) /* DREGPUF1 */
#define DRE_GPU_FW_TRUST_MAGIC UINT64_C(0x3152545746455244) /* DREFWTR1 */
#define DRE_GPU_FW_HANDOFF_MAGIC UINT64_C(0x3148574647455244) /* DREGFWH1 */
#define DRE_GPU_FW_VERSION 1u
#define DRE_GPU_FW_HEADER_BYTES 256u
#define DRE_GPU_FW_ALIGNMENT 16384u
#define DRE_GPU_FW_FILE_MAX (1024u*1024u)
#define DRE_GPU_FW_PACKAGE_MAX (3u*1024u*1024u)
#define DRE_GPU_FW_METADATA_BYTES 65536u
#define DRE_GPU_FW_CHIP_ID UINT32_C(0x06010900)
enum dre_gpu_fw_kind { DRE_GPU_FW_SQE=1,DRE_GPU_FW_ZAP=2 };
/* Generated privately, linked into the actual target kernel. The package's
 * self-declared digests are NOT a trust anchor. Never commit its initializer. */
struct dre_gpu_fw_trust {
    uint64_t magic;
    uint32_t version,struct_bytes,chip_id,flags;
    uint64_t package_bytes,sqe_bytes,zap_bytes;
    uint8_t package_sha256[32],sqe_sha256[32],zap_sha256[32];
};
struct dre_gpu_fw_view { const uint8_t *bytes;size_t size; };
struct dre_gpu_fw_package {
    struct dre_gpu_fw_view package,sqe,zap;
    uint64_t sqe_offset,zap_offset;
};
/* Structure only. This never claims the firmware was authenticated or hashed. */
const char *dre_gpu_fw_parse(const void *,size_t,struct dre_gpu_fw_package *);
/* hash_matches must compute SHA256 and return EXACTLY1 for a digest match.
 * An absent trust anchor or self-declared package digest cannot satisfy it. */
const char *dre_gpu_fw_verify(const void *,size_t,const struct dre_gpu_fw_trust *,
    int (*hash_matches)(void *,const void *,size_t,const uint8_t[32]),void *,
    struct dre_gpu_fw_package *);
/* Fixed64-byte LE handoff: magic,version/bytes,packagePA/bytes,metadataPA/bytes,
 * chip/flags,CRC32. All ranges must be inside the independently checked boot
 * image and disjoint from UFS arena, original FDT and each other. */
int dre_gpu_fw_handoff_valid(const uint64_t firmware[8],const uint64_t storage[8]);
const struct dre_gpu_fw_trust *dre_gpu_fw_trusted_anchor(void);
#ifdef __cplusplus
}
static_assert(sizeof(dre_gpu_fw_trust)==144,"private firmware trust layout");
#else
_Static_assert(sizeof(struct dre_gpu_fw_trust)==144,"private firmware trust layout");
#endif
#endif
