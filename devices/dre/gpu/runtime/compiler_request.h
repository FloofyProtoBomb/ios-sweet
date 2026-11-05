/* SPDX-License-Identifier: MIT */
#ifndef DRE_METAL306_COMPILER_REQUEST_H
#define DRE_METAL306_COMPILER_REQUEST_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Qualified no-inline-metadata, flags=0 subset of Metal306 request13. This
 * serializes source for Apple's local compiler; it does not compile or execute
 * a shader. Bounds below are port policy, not advertised Metal capabilities. */
#define DRE_METAL306_SOURCE_LIMIT (16u * 1024u * 1024u)
#define DRE_METAL306_ARGUMENT_LIMIT (64u * 1024u)
int dre_metal306_request13_size(size_t source_bytes, size_t argument_bytes, size_t *size);
int dre_metal306_request13(const void *source, size_t source_bytes,
    const void *arguments, size_t argument_bytes, void *output, size_t capacity, size_t *used);
struct dre_metal306_reply {
    const uint8_t *warnings, *metallib;
    size_t warning_bytes, metallib_bytes;
};
/* Validates the selected 104-byte archive envelope and MTLB magic only. A
 * legacy function-table reply is rejected. A nonzero return does not establish
 * library semantic validity, A619 compilation, or successful GPU execution. */
int dre_metal306_reply(int32_t status, const void *reply, size_t bytes,
    struct dre_metal306_reply *out);
#ifdef __cplusplus
}
#endif
#endif
