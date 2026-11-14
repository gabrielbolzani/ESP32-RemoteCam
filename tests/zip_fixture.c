#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/zip_store.h"

static int file_sink(void *context, const uint8_t *data, size_t size) {
    return fwrite(data, 1, size, (FILE *)context) == size;
}
static int reject_sink(void *context, const uint8_t *data, size_t size) {
    (void)context;
    (void)data;
    (void)size;
    return 0;
}
static int sample(const char *path) {
    static const uint8_t binary[] = {255,216,0,1,2,255,0,217};
    static const char text[] = "123456789";
    rc_zip_entry entries[5];
    const uint8_t *payload[5] = {NULL,NULL,NULL,binary,(const uint8_t *)text};
    const char *names[5] = {"sessao/","sessao/sub/","sessao/vazia/","sessao/sub/foto.jpg","sessao/legenda-\xc3\xa7.txt"};
    FILE *file = fopen(path, "wb");
    rc_zip_writer writer = {file_sink,file,0,0};
    uint64_t central_start;
    unsigned i;
    if (!file) return 0;
    memset(entries, 0, sizeof(entries));
    for (i = 0; i < 5; ++i) {
        entries[i].name = names[i];
        entries[i].directory = i < 3;
        entries[i].dos_date = (46 << 9) | (11 << 5) | 14;
        entries[i].dos_time = 12 << 11;
        entries[i].size = i == 3 ? sizeof(binary) : (i == 4 ? strlen(text) : 0);
        entries[i].local_offset = writer.offset;
        entries[i].crc = rc_zip_crc_update(0xffffffffUL, payload[i], (size_t)entries[i].size) ^ 0xffffffffUL;
        if (!rc_zip_local(&writer, &entries[i])) return 0;
        if (entries[i].size && !rc_zip_write(&writer, payload[i], (size_t)entries[i].size)) return 0;
        if (!rc_zip_descriptor(&writer, &entries[i])) return 0;
    }
    central_start = writer.offset;
    for (i = 0; i < 5; ++i) if (!rc_zip_central(&writer, &entries[i])) return 0;
    if (!rc_zip_finish(&writer, central_start, 5)) return 0;
    return fclose(file) == 0;
}
static int many(const char *path) {
    unsigned i;
    char name[80];
    FILE *file = fopen(path, "wb");
    rc_zip_writer writer = {file_sink,file,0,0};
    rc_zip_entry entry;
    uint64_t central_start;
    uint64_t local = 0;
    if (!file) return 0;
    memset(&entry, 0, sizeof(entry));
    entry.dos_date = 33;
    for (i = 0; i < 65536; ++i) {
        sprintf(name, "sessao/f%05u", i);
        entry.name = name;
        if (!rc_zip_local(&writer, &entry) || !rc_zip_descriptor(&writer, &entry)) return 0;
    }
    central_start = writer.offset;
    for (i = 0; i < 65536; ++i) {
        sprintf(name, "sessao/f%05u", i);
        entry.name = name;
        entry.local_offset = local;
        if (!rc_zip_central(&writer, &entry)) return 0;
        local += rc_zip_local_span(&entry);
    }
    if (!rc_zip_finish(&writer, central_start, 65536)) return 0;
    return fclose(file) == 0;
}
static int large_headers(const char *path) {
    FILE *file = fopen(path, "wb");
    rc_zip_writer writer = {file_sink,file,0,0};
    rc_zip_entry entry;
    if (!file) return 0;
    memset(&entry, 0, sizeof(entry));
    entry.name = "grande.bin";
    entry.size = (1ULL << 33) + 123;
    entry.local_offset = (1ULL << 34) + 456;
    entry.crc = 0x12345678;
    if (!rc_zip_central(&writer, &entry)) return 0;
    return fclose(file) == 0;
}
int main(int argc, char **argv) {
    rc_zip_writer rejected = {reject_sink,NULL,0,0};
    if (argc != 4) return 2;
    if ((rc_zip_crc_update(0xffffffffUL,(const uint8_t *)"123456789",9) ^ 0xffffffffUL) != 0xcbf43926UL) return 3;
    if (rc_zip_write(&rejected,"x",1) || !rejected.failed) return 4;
    if (!sample(argv[1])) return 5;
    if (!many(argv[2])) return 6;
    if (!large_headers(argv[3])) return 7;
    return 0;
}
