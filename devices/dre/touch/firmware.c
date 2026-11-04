/* SPDX-License-Identifier: MIT
 * File-layout facts: published OnePlus nt36672c.c, nvt_bin_header_parser and
 * nvt_get_fw_need_write_size, kernel 98a38783f42f5588bf00cd933e299aedc589bc10.
 * This independent parser does not include or redistribute any firmware. */
#include "firmware.h"

static uint32_t word(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *dre_touch_parse_firmware(const uint8_t *p, size_t n,
                                     struct dre_touch_firmware *out)
{
    if (!p || !out || n < 4096 || n > 512 * 1024) return "SIZE";
    struct dre_touch_firmware fw = {0};
    for (size_t end = n / 4096 * 4096; end; end -= 4096) {
        const uint8_t *tail = p + end - 3;
        if ((tail[0] == 'N' && tail[1] == 'V' && tail[2] == 'T') ||
            (tail[0] == 'M' && tail[1] == 'O' && tail[2] == 'D')) {
            fw.write_length = end;
            break;
        }
    }
    if (!fw.write_length) return "END_MARKER";
    uint32_t version = fw.write_length - 4096;
    if ((unsigned)p[version] + p[version + 1] != 255) return "VERSION";
    fw.version = p[version];
    uint32_t header = word(p), dlm = word(p + 12);
    if (header < 0x30 || header > 0x4000 || header > fw.write_length || (header - 0x30) % 16)
        return "HEADER";
    unsigned info = (header - 0x30) / 16;
    unsigned overlays = p[0x28] & 0x10 ? p[0x28] & 15 : 0;
    fw.count = 2 + info + overlays;
    if (fw.count > DRE_TOUCH_FW_PARTS) return "PART_COUNT";
    for (unsigned i = 0; i < fw.count; i++) {
        struct dre_touch_partition *part = fw.parts + i;
        uint32_t last;
        if (i < 2) {
            part->file_offset = word(p + i * 12);
            part->address = word(p + i * 12 + 4);
            last = word(p + i * 12 + 8);
            part->crc = word(p + 0x18 + i * 4);
        } else {
            uint64_t offset = i < 2 + info ? 0x30 + (i - 2) * 16 :
                (uint64_t)dlm + (i - 2 - info) * 16;
            if (offset > fw.write_length || fw.write_length - offset < 16) return "OVERLAY_HEADER";
            part->address = word(p + offset);
            last = word(p + offset + 4);
            part->file_offset = word(p + offset + 8);
            part->crc = word(p + offset + 12);
        }
        if (!last) continue; /* Reserved/empty entries are skipped by the vendor. */
        if (part->file_offset >= fw.write_length || last >= fw.write_length - part->file_offset)
            return "FILE_RANGE";
        if (part->address > 0xffffff || last > 0xffffff - part->address) return "SRAM_RANGE";
        part->length = last + 1;
    }
    if (!fw.parts[0].length || !fw.parts[1].length) return "EMPTY_CORE";
    *out = fw;
    return 0;
}
