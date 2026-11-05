/* SPDX-License-Identifier: MIT
 * UNVALIDATED. NT36672C no-flash wire/register facts: matching OnePlus driver
 * at98a38783. Original implementation; no firmware is included or generated.
 */
#include "nt36672c.h"
#include <string.h>

enum { EVENT_BASE = 0x22d00, RESET_STATUS = 0x22d60, HOST_COMMAND = 0x22d50,
       FW_INFO = 0x22d78, SOFTWARE_RESET = 0x3f0fe, BOOT_READY = 0x3f10d,
       BLD_CRC_ENABLE = 0x3f30e, CRC_FLAGS = 0x3f133 };

static enum dre_tn_result broken(struct dre_tn_controller *c, enum dre_tn_result r)
{
    c->last_error = r;
    c->state = DRE_TN_CONTROLLER_FAULT;
    return r;
}
static enum dre_tn_result page(struct dre_tn_controller *c, uint32_t address)
{
    uint8_t tx[3] = {0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7)}, rx[3];
    return dre_tn_spi_transfer(c->spi, tx, rx, sizeof(tx));
}
static enum dre_tn_result read_data(struct dre_tn_controller *c, uint32_t address,
                                    uint8_t *out, size_t bytes)
{
    uint8_t tx[130], rx[130];
    if (!out || !bytes || bytes > 128 || address > 0xffffff || bytes > 0x1000000u-address)
        return DRE_TN_ARGUMENT;
    size_t done = 0;
    while (done < bytes) {
        uint32_t here = address + (uint32_t)done;
        size_t chunk = 128 - (here & 127);
        if (chunk > bytes-done) chunk = bytes-done;
        enum dre_tn_result r = page(c, here);
        if (r) return r;
        memset(tx, 0, chunk + 2);
        tx[0] = here & 127; /* READ clearsbit7; one dummy byte follows. */
        r = dre_tn_spi_transfer(c->spi, tx, rx, chunk + 2);
        if (r) return r;
        memcpy(out + done, rx + 2, chunk);
        done += chunk;
    }
    return DRE_TN_OK;
}
static enum dre_tn_result write_data(struct dre_tn_controller *c, uint32_t address,
                                     const uint8_t *data, size_t bytes)
{
    uint8_t tx[129], rx[129];
    if (!data || !bytes || address > 0xffffff || bytes > 0x1000000u-address)
        return DRE_TN_ARGUMENT;
    for (size_t done = 0; done < bytes; ) {
        uint32_t here = address + (uint32_t)done;
        size_t chunk = 128 - (here & 127);
        if (chunk > bytes-done) chunk = bytes-done;
        enum dre_tn_result r = page(c, here);
        if (r) return r;
        tx[0] = (here & 127) | 128; /* WRITE setsbit7, no dummy byte. */
        memcpy(tx + 1, data + done, chunk);
        r = dre_tn_spi_transfer(c->spi, tx, rx, chunk + 1);
        if (r) return r;
        done += chunk;
    }
    return DRE_TN_OK;
}
static enum dre_tn_result byte(struct dre_tn_controller *c, uint32_t a, uint8_t b)
{ return write_data(c, a, &b, 1); }
static enum dre_tn_result put_number(struct dre_tn_controller *c, uint32_t a,
                                     uint32_t n, unsigned bytes)
{
    uint8_t p[4];
    for (unsigned i = 0; i != bytes; ++i) p[i] = (uint8_t)(n >> (8*i));
    return write_data(c, a, p, bytes);
}
static uint32_t word(const uint8_t p[4])
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void pause_ms(struct dre_tn_controller *c, unsigned ms)
{
    /* No unit conversion of CNTVCT into nanoseconds is assumed here. */
    while (ms--) c->spi->ops.delay_us(c->spi->ops.cookie, 1000);
}
static enum dre_tn_result wait_reset(struct dre_tn_controller *c, uint8_t minimum,
                                     unsigned attempts)
{
    for (unsigned i = 0; i != attempts; ++i) {
        uint8_t state[5];
        pause_ms(c, 10);
        enum dre_tn_result r = read_data(c, RESET_STATUS, state, sizeof(state));
        if (r) return r;
        c->reset_state = state[0];
        if (state[0] >= minimum && state[0] <= 0xaf) return DRE_TN_OK;
    }
    return DRE_TN_TIMEOUT;
}
enum dre_tn_result dre_tn_controller_boot(struct dre_tn_controller *c,
    struct dre_tn_spi *spi, const uint8_t *firmware, size_t bytes,
    dre_tn_sha256_fn sha256, void *sha_cookie)
{
    if (!c || !spi || !firmware || !sha256) return DRE_TN_ARGUMENT;
    if (c->state != DRE_TN_CONTROLLER_OFF || spi->state != DRE_TN_RUNNING)
        return DRE_TN_STATE;
    uint8_t hash[32];
    struct dre_touch_firmware fw;
    if (bytes != spi->profile.firmware_size || bytes != 139264 ||
        sha256(sha_cookie, firmware, bytes, hash) ||
        memcmp(hash, spi->profile.firmware_sha256, sizeof(hash)) ||
        dre_touch_parse_firmware(firmware, bytes, &fw) || fw.write_length != bytes)
        return DRE_TN_FIRMWARE;
    /* No untrusted firmware-derived control-register writes. This conservative
     * SRAM ceiling excludes the0x3f000 control bank. Exact private partition
     * placement still needs tomorrow's source/data qualification. */
    for (unsigned i = 0; i != fw.count; ++i) {
        const struct dre_touch_partition *p = fw.parts + i;
        if (!p->length) continue;
        if (p->address >= 0x3f000 || p->length > 0x3f000-p->address ||
            (i < 2 && p->length > 0x20000)) return DRE_TN_FIRMWARE;
    }
    *c = (struct dre_tn_controller){.spi = spi, .state = DRE_TN_CONTROLLER_BOOTING};
    enum dre_tn_result r = byte(c, SOFTWARE_RESET, 0x69);
    if (r) return broken(c, r);
    pause_ms(c, 5);
    uint8_t trim[6];
    r = read_data(c, 0x1f64e, trim, sizeof(trim));
    if (r) return broken(c, r);
    if (trim[0] != 0x0c || trim[3] != 0x72 || trim[4] != 0x66 || trim[5] != 0x03)
        return broken(c, DRE_TN_PROFILE); /* No vendor unknown-trim fallback. */
    /* Detect/reset may change the page/MCU state. Begin the vendor's actual
     * hardware-CRC download transaction with a fresh bootloader reset. */
    r = byte(c, SOFTWARE_RESET, 0x69);
    if (r) return broken(c, r);
    pause_ms(c, 5);
    for (unsigned i = 0; i != fw.count; ++i) {
        const struct dre_touch_partition *p = fw.parts + i;
        if (!p->length) continue;
        r = write_data(c, p->address, firmware + p->file_offset, p->length);
        if (r) return broken(c, r);
    }
    const uint32_t destinations[2] = {0x3f128,0x3f12c};
    const uint32_t lengths[2] = {0x3f118,0x3f130};
    const uint32_t golden[2] = {0x3f100,0x3f104};
    const uint32_t results[2] = {0x3f120,0x3f124};
    for (unsigned i = 0; i != 2; ++i) {
        /* Stored CRC length is LAST INDEX, not the number of copied bytes.
         * The matched trim table selects hardware-CRC revision2/17bit length. */
        r = put_number(c, destinations[i], fw.parts[i].address, 3);
        if (!r) r = put_number(c, lengths[i], fw.parts[i].length-1, 3);
        if (!r) r = put_number(c, golden[i], fw.parts[i].crc, 4);
        if (r) return broken(c, r);
    }
    uint8_t enable;
    r = read_data(c, BLD_CRC_ENABLE, &enable, 1);
    if (!r) r = byte(c, BLD_CRC_ENABLE, enable | 128);
    if (!r) r = byte(c, RESET_STATUS, 0);
    if (!r) r = byte(c, HOST_COMMAND, 0xae);
    if (!r) r = byte(c, BOOT_READY, 1);
    if (r) return broken(c, r);
    pause_ms(c, 5);
    r = wait_reset(c, 0xa0, 11);
    if (!r) r = read_data(c, CRC_FLAGS, &c->crc_flags, 1);
    if (r) return broken(c, r);
    if (!(c->crc_flags & 4) || (c->crc_flags & 3)) return broken(c, DRE_TN_FIRMWARE);
    for (unsigned i = 0; i != 2; ++i) {
        uint8_t g[4], actual[4];
        r = read_data(c, golden[i], g, sizeof(g));
        if (!r) r = read_data(c, results[i], actual, sizeof(actual));
        if (r) return broken(c, r);
        c->crc_result[i] = word(actual);
        if (word(g) != fw.parts[i].crc || c->crc_result[i] != fw.parts[i].crc)
            return broken(c, DRE_TN_FIRMWARE);
    }
    r = wait_reset(c, 0xa3, 101);
    uint8_t info[16];
    if (!r) r = read_data(c, FW_INFO, info, sizeof(info));
    if (r) return broken(c, r);
    /* Matching vendor firmware exposes version and complement here. Its
     * driver does not qualify raw coordinate maxima from these other bytes. */
    if ((unsigned)info[0] + info[1] != 255 || info[0] != fw.version)
        return broken(c, DRE_TN_FIRMWARE);
    r = page(c, EVENT_BASE);
    if (r) return broken(c, r);
    c->firmware_version = info[0];
    c->generation = 1;
    c->state = DRE_TN_CONTROLLER_READY;
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_controller_report(struct dre_tn_controller *c,
                                            struct dre_touch_frame *out)
{
    if (!c || !out || !c->spi) return DRE_TN_ARGUMENT;
    if (c->state != DRE_TN_CONTROLLER_READY) return DRE_TN_STATE;
    uint8_t payload[DRE_TOUCH_REPORT_BYTES];
    enum dre_tn_result r = read_data(c, EVENT_BASE, payload, sizeof(payload));
    if (r) return broken(c, r);
    struct dre_touch_frame frame;
    c->last_decode = dre_touch_decode(payload, sizeof(payload),
        (uint16_t)(c->spi->profile.width-1), (uint16_t)(c->spi->profile.height-1), &frame);
    if (c->last_decode == DRE_TOUCH_WATCHDOG_PATTERN) {
        if (++c->watchdog_count >= 4) return broken(c, DRE_TN_RECOVERY);
        return DRE_TN_RECOVERY;
    }
    c->watchdog_count = 0;
    if (c->last_decode == DRE_TOUCH_FIRMWARE_RECOVERY) return broken(c, DRE_TN_RECOVERY);
    if (c->last_decode == DRE_TOUCH_BAD_CHECKSUM) return DRE_TN_CHECKSUM;
    if (c->last_decode != DRE_TOUCH_OK) return DRE_TN_PROTOCOL;
    *out = frame;
    return DRE_TN_OK;
}
