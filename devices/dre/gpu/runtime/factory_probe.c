/* SPDX-License-Identifier: MIT */
#include "factory_probe.h"
#include <string.h>
#define UTF8_ENCODING UINT32_C(0x08000100)

static enum dre_factory_value property_string(const struct dre_factory_ops *ops,
    uint32_t service, const void *key, char result[DRE_FACTORY_NAME_BYTES])
{
    const void *value = ops->property(service, key, NULL, 0);
    enum dre_factory_value state = DRE_FACTORY_MISSING;
    if (value) {
        state = DRE_FACTORY_WRONG_TYPE;
        if (ops->type_id(value) == ops->string_type_id()) {
            state = DRE_FACTORY_UNREPRESENTABLE;
            long length = ops->string_length(value);
            if (length > 0 && length < DRE_FACTORY_NAME_BYTES &&
                ops->string_copy(value, result, DRE_FACTORY_NAME_BYTES, UTF8_ENCODING)) {
                /* The report uses bounded ASCII identifiers. Metal accepts a
                 * CFString; non-ASCII names are reported as unrepresentable,
                 * never mistaken for a missing service or a usable plugin. */
                size_t actual = 0;
                while (actual < DRE_FACTORY_NAME_BYTES && result[actual]) {
                    unsigned char c = (unsigned char)result[actual];
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_')) break;
                    ++actual;
                }
                if (actual == (size_t)length && !result[actual])
                    state = DRE_FACTORY_STRING;
            }
        }
        ops->release_cf(value);
    }
    if (state != DRE_FACTORY_STRING) result[0] = 0;
    return state;
}

enum dre_factory_status dre_factory_observe(const struct dre_factory_ops *ops,
    struct dre_factory_observation *out)
{
    if (!out) return DRE_FACTORY_BAD_OPS;
    memset(out, 0, sizeof(*out));
    out->status = DRE_FACTORY_BAD_OPS;
    if (!ops || !ops->matching || !ops->services || !ops->next || !ops->release_io ||
        !ops->property || !ops->string_create || !ops->type_id ||
        !ops->string_type_id || !ops->string_length || !ops->string_copy ||
        !ops->release_cf) return out->status;
    const void *name = ops->string_create(NULL, "MetalPluginName", UTF8_ENCODING);
    const void *cls = ops->string_create(NULL, "MetalPluginClassName", UTF8_ENCODING);
    uint32_t iterator = 0;
    if (!name || !cls) { out->status = DRE_FACTORY_ALLOCATION; goto done; }
    void *dictionary = ops->matching("IOAcceleratorES");
    if (!dictionary) { out->status = DRE_FACTORY_ALLOCATION; goto done; }
    /* IOKit consumes the matching dictionary on both success and failure. */
    out->io_error = ops->services(0, dictionary, &iterator);
    if (out->io_error || !iterator) { out->status = DRE_FACTORY_LOOKUP; goto done; }
    for (;;) {
        uint32_t service = ops->next(iterator);
        if (!service) break;
        if (out->count == DRE_FACTORY_LIMIT) {
            ops->release_io(service);
            out->status = DRE_FACTORY_LIMIT_REACHED;
            goto done;
        }
        struct dre_factory_entry *entry = &out->entries[out->count++];
        entry->name_state = property_string(ops, service, name, entry->plugin);
        entry->class_state = property_string(ops, service, cls, entry->class_name);
        ops->release_io(service);
    }
    out->status = out->count ? DRE_FACTORY_METADATA : DRE_FACTORY_NONE;
done:
    if (iterator) ops->release_io(iterator);
    if (name) ops->release_cf(name);
    if (cls) ops->release_cf(cls);
    return out->status;
}

#ifndef DRE_FACTORY_LIBRARY_ONLY
#include <stdio.h>
#include <dlfcn.h>

/* iPhoneOS does not ship the macOS IOKitLib headers in every SDK. These are
 * the fixed public IOKit/CoreFoundation C signatures, resolved in the process.
 * Resolving them does not open a user client or construct a Metal device. */
int main(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "usage: %s (read-only discovery; no arguments)\n", argv[0]);
        return 2;
    }
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!io) io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_LOCAL);
    struct dre_factory_ops ops = {0};
    /* POSIX dlsym's function-pointer conversion is valid on Darwin's ARM64 ABI. */
#define LOAD(handle, member, symbol) do { \
    void *pointer = (handle) ? dlsym(handle, symbol) : NULL; \
    _Static_assert(sizeof(ops.member) == sizeof(pointer), "Darwin function pointer ABI"); \
    memcpy(&ops.member, &pointer, sizeof(pointer)); \
} while (0)
    LOAD(io, matching, "IOServiceMatching");
    LOAD(io, services, "IOServiceGetMatchingServices");
    LOAD(io, next, "IOIteratorNext");
    LOAD(io, release_io, "IOObjectRelease");
    LOAD(io, property, "IORegistryEntryCreateCFProperty");
    LOAD(cf, string_create, "CFStringCreateWithCString");
    LOAD(cf, type_id, "CFGetTypeID");
    LOAD(cf, string_type_id, "CFStringGetTypeID");
    LOAD(cf, string_length, "CFStringGetLength");
    LOAD(cf, string_copy, "CFStringGetCString");
    LOAD(cf, release_cf, "CFRelease");
#undef LOAD
    struct dre_factory_observation result;
    dre_factory_observe(&ops, &result);
    printf("{\"schema\":1,\"status\":%u,\"io_error\":%d,\"count\":%u,"
        "\"metal_acceleration_pass\":false,\"discovery_metadata_only\":true,\"entries\":[",
        (unsigned)result.status, result.io_error, result.count);
    for (unsigned i = 0; i < result.count; ++i) {
        struct dre_factory_entry *entry = &result.entries[i];
        /* Strings contain only identifiers after the validation above. */
        printf("%s{\"plugin_state\":%u,\"class_state\":%u,\"plugin\":\"%s\",\"class\":\"%s\"}",
            i ? "," : "", (unsigned)entry->name_state, (unsigned)entry->class_state,
            entry->plugin, entry->class_name);
    }
    puts("]}");
    if (cf) dlclose(cf);
    if (io) dlclose(io);
    /* Zero denotes a complete metadata observation, never an acceleration pass. */
    return result.status == DRE_FACTORY_METADATA || result.status == DRE_FACTORY_NONE ? 0 : 2;
}
#endif
