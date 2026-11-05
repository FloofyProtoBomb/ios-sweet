/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_A619_ZAP_MAX_SEGMENTS 8u
#define DRE_A619_ZAP_MAX_FILE (1024u*1024u)
#define DRE_A619_ZAP_MAX_METADATA (64u*1024u)
struct dre_a619_zap_segment {
    uint32_t file_offset, file_bytes, memory_offset, memory_bytes;
};
struct dre_a619_zap_plan {
    uint32_t metadata_header_bytes, metadata_hash_offset, metadata_hash_bytes;
    uint32_t memory_bytes, alignment, segments;
    struct dre_a619_zap_segment segment[DRE_A619_ZAP_MAX_SEGMENTS];
};
/* Strict full-ELF32 Hexagon/relocatable PAS-13 parser. No signature is claimed:
 * the caller must hash-verify the exact private firmware before use; the secure
 * PAS loader alone authenticates it. Split MDT/Bxx input is not accepted. */
const char *dre_a619_zap_plan(const void *file, size_t bytes,
    uint64_t reserved_pa, size_t reserved_bytes, struct dre_a619_zap_plan *plan);
/* Revalidates the complete plan and all CPU ranges before copying. Metadata is
 * ELF header/program headers immediately followed by the signed hash segment.
 * The load buffer is the actual exclusively owned reserved GPU microcode RAM;
 * no other physical memory may be substituted. Neither function calls SCM. */
const char *dre_a619_zap_metadata(const void *file, size_t bytes,
    uint64_t reserved_pa, size_t reserved_bytes, void *metadata, size_t capacity,
    size_t *metadata_bytes);
const char *dre_a619_zap_copy_segments(const void *file, size_t bytes,
    uint64_t reserved_pa, void *reserved_cpu, size_t reserved_bytes);
#ifdef __cplusplus
}
#endif
