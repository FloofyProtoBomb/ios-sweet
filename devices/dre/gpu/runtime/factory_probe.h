/* SPDX-License-Identifier: MIT */
#ifndef DRE_METAL_FACTORY_PROBE_H
#define DRE_METAL_FACTORY_PROBE_H
#include <stddef.h>
#include <stdint.h>

/* Only the read-only discovery calls used by Metal 306. No user-client open,
 * memory map, submission, Objective-C device construction or MMIO callback. */
struct dre_factory_ops {
    void *(*matching)(const char *);
    int (*services)(uint32_t, const void *, uint32_t *);
    uint32_t (*next)(uint32_t);
    int (*release_io)(uint32_t);
    const void *(*property)(uint32_t, const void *, const void *, uint32_t);
    const void *(*string_create)(const void *, const char *, uint32_t);
    unsigned long (*type_id)(const void *);
    unsigned long (*string_type_id)(void);
    long (*string_length)(const void *);
    unsigned char (*string_copy)(const void *, char *, long, uint32_t);
    void (*release_cf)(const void *);
};
#define DRE_FACTORY_LIMIT 16
#define DRE_FACTORY_NAME_BYTES 128
enum dre_factory_value {
    DRE_FACTORY_MISSING, DRE_FACTORY_STRING, DRE_FACTORY_WRONG_TYPE,
    DRE_FACTORY_UNREPRESENTABLE
};
struct dre_factory_entry {
    enum dre_factory_value name_state, class_state;
    char plugin[DRE_FACTORY_NAME_BYTES], class_name[DRE_FACTORY_NAME_BYTES];
};
enum dre_factory_status {
    DRE_FACTORY_METADATA = 0, DRE_FACTORY_NONE, DRE_FACTORY_BAD_OPS,
    DRE_FACTORY_ALLOCATION, DRE_FACTORY_LOOKUP, DRE_FACTORY_LIMIT_REACHED
};
struct dre_factory_observation {
    enum dre_factory_status status;
    int io_error;
    unsigned count;
    struct dre_factory_entry entries[DRE_FACTORY_LIMIT];
};
/* Single invocation, no retained registry/CF objects on return. The caller owns
 * its function table and serializes any test transport that is not thread-safe.
 * Success means metadata was read; it NEVER means a Metal device exists. */
enum dre_factory_status dre_factory_observe(const struct dre_factory_ops *,
                                          struct dre_factory_observation *);
#endif
