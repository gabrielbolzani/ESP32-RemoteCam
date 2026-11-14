#ifndef REMOTECAM_ZIP_STORE_H
#define REMOTECAM_ZIP_STORE_H

/* ZIP64 STORE streaming, PKWARE APPNOTE 6.3.10. No heap allocation.
 * The caller emits file data, then descriptors, then the central directory.
 * UTF-8 names and data descriptors are always enabled. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef int (*rc_zip_sink)(void *, const uint8_t *, size_t);
typedef struct {
    rc_zip_sink sink;
    void *context;
    uint64_t offset;
    int failed;
} rc_zip_writer;

typedef struct {
    const char *name;
    uint64_t size;
    uint64_t local_offset;
    uint32_t crc;
    uint16_t dos_time;
    uint16_t dos_date;
    int directory;
} rc_zip_entry;

static inline void rc_zip_u16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static inline void rc_zip_u32(uint8_t *p, uint32_t n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static inline void rc_zip_u64(uint8_t *p, uint64_t n) {
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static inline int rc_zip_write(rc_zip_writer *w, const void *data, size_t size) {
    if (w->failed || size > UINT64_MAX - w->offset) return 0;
    if (!w->sink(w->context, (const uint8_t *)data, size)) {
        w->failed = 1;
        return 0;
    }
    w->offset += size;
    return 1;
}
static inline uint32_t rc_zip_crc_update(uint32_t crc, const uint8_t *data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000UL, 0x1db71064UL, 0x3b6e20c8UL, 0x26d930acUL,
        0x76dc4190UL, 0x6b6b51f4UL, 0x4db26158UL, 0x5005713cUL,
        0xedb88320UL, 0xf00f9344UL, 0xd6d6a3e8UL, 0xcb61b38cUL,
        0x9b64c2b0UL, 0x86d3d2d4UL, 0xa00ae278UL, 0xbdbdf21cUL
    };
    size_t i;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return crc;
}
static inline uint64_t rc_zip_local_span(const rc_zip_entry *e) {
    return 30ULL + strlen(e->name) + 20ULL + e->size + 24ULL;
}
static inline int rc_zip_local(rc_zip_writer *w, const rc_zip_entry *e) {
    uint8_t h[30] = {0};
    uint8_t extra[20] = {0};
    size_t name_size = strlen(e->name);
    if (!name_size || name_size > 65535) return 0;
    rc_zip_u32(h, 0x04034b50UL);
    rc_zip_u16(h + 4, 45);
    rc_zip_u16(h + 6, 0x0808);
    rc_zip_u16(h + 8, 0);
    rc_zip_u16(h + 10, e->dos_time);
    rc_zip_u16(h + 12, e->dos_date);
    rc_zip_u32(h + 18, 0xffffffffUL);
    rc_zip_u32(h + 22, 0xffffffffUL);
    rc_zip_u16(h + 26, (uint16_t)name_size);
    rc_zip_u16(h + 28, sizeof(extra));
    rc_zip_u16(extra, 1);
    rc_zip_u16(extra + 2, 16);
    rc_zip_u64(extra + 4, e->size);
    rc_zip_u64(extra + 12, e->size);
    return rc_zip_write(w, h, sizeof(h)) && rc_zip_write(w, e->name, name_size) && rc_zip_write(w, extra, sizeof(extra));
}
static inline int rc_zip_descriptor(rc_zip_writer *w, const rc_zip_entry *e) {
    uint8_t h[24];
    rc_zip_u32(h, 0x08074b50UL);
    rc_zip_u32(h + 4, e->crc);
    rc_zip_u64(h + 8, e->size);
    rc_zip_u64(h + 16, e->size);
    return rc_zip_write(w, h, sizeof(h));
}
static inline int rc_zip_central(rc_zip_writer *w, const rc_zip_entry *e) {
    uint8_t h[46] = {0};
    uint8_t extra[28] = {0};
    size_t name_size = strlen(e->name);
    if (!name_size || name_size > 65535) return 0;
    rc_zip_u32(h, 0x02014b50UL);
    rc_zip_u16(h + 4, 45);
    rc_zip_u16(h + 6, 45);
    rc_zip_u16(h + 8, 0x0808);
    rc_zip_u16(h + 10, 0);
    rc_zip_u16(h + 12, e->dos_time);
    rc_zip_u16(h + 14, e->dos_date);
    rc_zip_u32(h + 16, e->crc);
    rc_zip_u32(h + 20, 0xffffffffUL);
    rc_zip_u32(h + 24, 0xffffffffUL);
    rc_zip_u16(h + 28, (uint16_t)name_size);
    rc_zip_u16(h + 30, sizeof(extra));
    rc_zip_u32(h + 38, e->directory ? 0x10 : 0x20);
    rc_zip_u32(h + 42, 0xffffffffUL);
    rc_zip_u16(extra, 1);
    rc_zip_u16(extra + 2, 24);
    rc_zip_u64(extra + 4, e->size);
    rc_zip_u64(extra + 12, e->size);
    rc_zip_u64(extra + 20, e->local_offset);
    return rc_zip_write(w, h, sizeof(h)) && rc_zip_write(w, e->name, name_size) && rc_zip_write(w, extra, sizeof(extra));
}
static inline int rc_zip_finish(rc_zip_writer *w, uint64_t central_start, uint64_t count) {
    uint8_t end64[56] = {0};
    uint8_t locator[20] = {0};
    uint8_t end[22] = {0};
    uint64_t end64_offset = w->offset;
    uint64_t central_size = w->offset - central_start;
    rc_zip_u32(end64, 0x06064b50UL);
    rc_zip_u64(end64 + 4, 44);
    rc_zip_u16(end64 + 12, 45);
    rc_zip_u16(end64 + 14, 45);
    rc_zip_u64(end64 + 24, count);
    rc_zip_u64(end64 + 32, count);
    rc_zip_u64(end64 + 40, central_size);
    rc_zip_u64(end64 + 48, central_start);
    rc_zip_u32(locator, 0x07064b50UL);
    rc_zip_u64(locator + 8, end64_offset);
    rc_zip_u32(locator + 16, 1);
    rc_zip_u32(end, 0x06054b50UL);
    rc_zip_u16(end + 8, 0xffff);
    rc_zip_u16(end + 10, 0xffff);
    rc_zip_u32(end + 12, 0xffffffffUL);
    rc_zip_u32(end + 16, 0xffffffffUL);
    return rc_zip_write(w, end64, sizeof(end64)) && rc_zip_write(w, locator, sizeof(locator)) && rc_zip_write(w, end, sizeof(end));
}
#endif
