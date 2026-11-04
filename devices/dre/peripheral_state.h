#pragma once
#include <stdint.h>

struct dre_usb_state { uint32_t power, master, config, id, control; };
struct dre_usb_detail { uint32_t config, control, status, endpoints, event_size; };
struct dre_touch_state { uint32_t votes, revision, status; };
struct dre_gpu_state { uint32_t cx, gx, status; };

const char *dre_usb_state(const void *dt, struct dre_usb_state *s);
const char *dre_usb_detail(const void *dt, struct dre_usb_detail *s);
const char *dre_touch_state(const void *dt, struct dre_touch_state *s);
const char *dre_gpu_state(const void *dt, struct dre_gpu_state *s);
