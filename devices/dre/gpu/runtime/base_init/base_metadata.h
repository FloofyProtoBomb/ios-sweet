/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only Metal306 metadata inspection. No initializer calls.
 */
#ifndef DRE_METAL306_BASE_METADATA_H
#define DRE_METAL306_BASE_METADATA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_METAL306_BASE_METADATA_VERSION 1u
#define DRE_METAL306_BASE_METHODS 8u
#define DRE_METAL306_BASE_MEMBERS 13u
enum dre_metal306_metadata_status {
    DRE_METAL306_METADATA_INSPECTED=0,
    DRE_METAL306_METADATA_INVALID=-1,
    DRE_METAL306_METADATA_IMAGE_MISMATCH=-2,
    DRE_METAL306_METADATA_CLASS_MISMATCH=-3,
    DRE_METAL306_METADATA_METHOD_MISMATCH=-4,
    DRE_METAL306_METADATA_MEMBER_MISMATCH=-5
};
enum dre_metal306_base_method {
    DRE_METAL306_BASE_INIT=0,DRE_METAL306_ACCELERATOR_INIT=1,
    DRE_METAL306_INIT_LIMITS=2,DRE_METAL306_INIT_FEATURE_QUERIES=3,
    DRE_METAL306_INIT_GPU_FAMILIES=4,DRE_METAL306_BASE_DEALLOC=5,
    DRE_METAL306_INIT_SERIALIZATION=6,DRE_METAL306_CXX_CONSTRUCT=7
};
enum dre_metal306_base_member {
    DRE_METAL306_LIMITS=0,DRE_METAL306_SUPPORTED_FAMILIES=1,
    DRE_METAL306_FEATURE_QUERIES=2,DRE_METAL306_COMPILER_MANAGER=3,
    DRE_METAL306_LIBRARY_BUILDER=4,DRE_METAL306_PIPELINE_LIBRARY_BUILDER=5,
    DRE_METAL306_SERIAL_QUEUE=6,DRE_METAL306_PIPELINE_SERIALIZATION_QUEUE=7,
    DRE_METAL306_CONCURRENT_QUEUE=8,DRE_METAL306_PROGRESS_ONCE=9,
    DRE_METAL306_CONNECTION_ONCE=10,DRE_METAL306_ARCHITECTURE=11,
    DRE_METAL306_PIPELINE_COLLECTION=12
};
/* Metadata observations only. No IMP, object, private table, CPU address or
 * writable ivar pointer is exported. Kind is the original type encoding's
 * first byte (@, ^ or {), not a fabricated private C structure definition. */
struct dre_metal306_base_method_metadata {
    uint32_t method_id,argument_count;
    uint64_t unslid_address;
};
struct dre_metal306_base_member_metadata {
    uint32_t member_id,offset,bytes,alignment;
    uint32_t encoding_kind,reserved;
};
struct dre_metal306_base_metadata {
    uint32_t version,struct_bytes,instance_bytes,super_instance_bytes;
    uint32_t method_count,member_count,reserved[2];
    struct dre_metal306_base_method_metadata methods[DRE_METAL306_BASE_METHODS];
    struct dre_metal306_base_member_metadata members[DRE_METAL306_BASE_MEMBERS];
};
/* Caller supplies writable, independent output storage. The function clears
 * it on failure. It does not dlopen a framework, allocate a device, invoke an
 * initializer/feature method, inspect an object's ivar contents or grant a
 * publication decision. METADATA_INSPECTED does not qualify base initialization.
 * Build as plain-arm64 Objective-C++ with ARC and the official iPhoneOS SDK.
 */
enum dre_metal306_metadata_status dre_metal306_inspect_base_metadata(
    struct dre_metal306_base_metadata *);
#ifdef __cplusplus
}
#endif
#endif
