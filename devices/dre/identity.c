/* Runtime OnePlus identity. No modem NV reads, generated IMEIs or storage writes.
 * UUIDv5 uses the same URL namespace/name as the previous Python builder.
 * SHA-1 is used only for this RFC 9562 identifier, never as a trust primitive. */
#include <libfdt.h>
#include "identity.h"

static char serial[64];
static uint8_t host_uuid[16];

static int serial_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-';
}

static int copy_serial(const char *value, unsigned len)
{
    if (!len || len >= sizeof(serial)) return 0;
    for (unsigned i = 0; i < len; i++)
        if (!serial_char((unsigned char)value[i])) return 0;
    for (unsigned i = 0; i < sizeof(serial); i++) serial[i] = i < len ? value[i] : 0;
    return 1;
}

static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void make_uuid(void)
{
    static const uint8_t url_namespace[16] = {
        0x6b,0xa7,0xb8,0x11,0x9d,0xad,0x11,0xd1,0x80,0xb4,0x00,0xc0,0x4f,0xd4,0x30,0xc8};
    static const char prefix[] = "urn:oneplus:dre:";
    uint8_t message[128] = {0};
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) message[n++] = url_namespace[i];
    for (unsigned i = 0; i < sizeof(prefix) - 1; i++) message[n++] = prefix[i];
    for (unsigned i = 0; serial[i]; i++) message[n++] = serial[i];
    unsigned padded = n < 56 ? 64 : 128;
    message[n] = 0x80;
    message[padded - 2] = (n * 8) >> 8;
    message[padded - 1] = n * 8;
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (unsigned block = 0; block < padded; block += 64) {
        uint32_t w[80];
        for (unsigned i = 0; i < 16; i++) {
            const uint8_t *p = message + block + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (unsigned i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
            else { f = b ^ c ^ d; k = 0xca62c1d6; }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (unsigned i = 0; i < 16; i++) host_uuid[i] = h[i / 4] >> (24 - 8 * (i % 4));
    host_uuid[6] = (host_uuid[6] & 15) | 0x50;
    host_uuid[8] = (host_uuid[8] & 63) | 0x80;
}

const char *dre_identity_detect(const void *dt)
{
    serial[0] = 0;
    if (fdt_check_header(dt)) return "invalid identity FDT";
    int chosen = fdt_path_offset(dt, "/chosen"), len;
    const char *args = fdt_getprop(dt, chosen, "bootargs", &len);
    const char key[] = "androidboot.serialno=";
    int found = 0;
    if (args) {
        if (len < 1 || args[len - 1]) return "unterminated boot arguments";
        for (int i = 0; i < len && args[i]; ) {
            while (i < len && args[i] == ' ') i++;
            int end = i;
            while (end < len && args[end] && args[end] != ' ') end++;
            if (end - i >= (int)sizeof(key) - 1 && !memcmp(args + i, key, sizeof(key) - 1)) {
                if (found++ || !copy_serial(args + i + sizeof(key) - 1,
                                           end - i - (sizeof(key) - 1))) {
                    serial[0] = 0;
                    return "invalid or duplicate bootloader serial";
                }
            }
            i = end;
        }
    }
    if (!found) {
        const char *value = fdt_getprop(dt, 0, "serial-number", &len);
        if (!value || len < 2 || value[len - 1] || !copy_serial(value, len - 1))
            return "hardware serial unavailable";
    }
    make_uuid();
    return 0;
}

const char *dre_identity_serial(void) { return serial[0] ? serial : 0; }

struct adt_identity {
    uint8_t *root_serial, *hardware_serial, *uuid;
    unsigned nodes, hardware_nodes, chosen_nodes;
};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int name_is(const uint8_t *value, uint32_t size, const char *name)
{
    unsigned n = 0;
    while (name[n]) n++;
    return size > n && !memcmp(value, name, n) && !value[n];
}

static int walk_adt(uint8_t *adt, uint32_t size, uint32_t *offset,
                    unsigned depth, struct adt_identity *id)
{
    if (depth > 16 || ++id->nodes > 256 || *offset > size || size - *offset < 8) return 0;
    uint32_t props = le32(adt + *offset), children = le32(adt + *offset + 4);
    *offset += 8;
    if (props > 256 || children > 256) return 0;
    uint8_t *name = 0, *node_serial = 0, *uuid = 0;
    uint32_t name_size = 0;
    for (unsigned i = 0; i < props; i++) {
        if (size - *offset < 36) return 0;
        uint8_t *key = adt + *offset;
        if (!memchr(key, 0, 32)) return 0;
        uint32_t len = le32(key + 32) & 0x7fffffff;
        *offset += 36;
        uint32_t aligned = (len + 3) & ~3u;
        if (aligned > size - *offset) return 0;
        uint8_t *value = adt + *offset;
        if (!strcmp((const char *)key, "name")) {
            if (name || !len || !memchr(value, 0, len)) return 0;
            name = value; name_size = len;
        }
        if (!strcmp((const char *)key, "serial-number")) {
            if (node_serial || len != 64) return 0;
            node_serial = value;
        }
        if (!strcmp((const char *)key, "dre,host-uuid")) {
            if (uuid || len != 16) return 0;
            uuid = value;
        }
        *offset += aligned;
    }
    if (!name) return 0;
    if (!depth) id->root_serial = node_serial;
    if (depth == 1 && name_is(name, name_size, "oneplus-hardware")) {
        if (++id->hardware_nodes != 1) return 0;
        id->hardware_serial = node_serial;
    }
    if (depth == 1 && name_is(name, name_size, "chosen")) {
        if (++id->chosen_nodes != 1) return 0;
        id->uuid = uuid;
    }
    for (unsigned i = 0; i < children; i++)
        if (!walk_adt(adt, size, offset, depth + 1, id)) return 0;
    return 1;
}

const char *dre_identity_fixup(void *adt, uint32_t size)
{
    if (!serial[0]) return "hardware serial not detected";
    struct adt_identity id = {0};
    uint32_t offset = 0;
    if (!walk_adt(adt, size, &offset, 0, &id) || offset != size ||
        !id.root_serial || !id.hardware_serial || !id.uuid)
        return "invalid identity ADT slots";
    /* No mutation occurs until the entire tree and all slots are validated. */
    for (unsigned i = 0; i < 64; i++) id.root_serial[i] = id.hardware_serial[i] = serial[i];
    for (unsigned i = 0; i < 16; i++) id.uuid[i] = host_uuid[i];
    return 0;
}
