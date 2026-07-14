#ifndef ZIP_H
#define ZIP_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct zip_archive_t zip_archive_t;

zip_archive_t *zip_create(const char *path);
int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level);
int zip_finalize(zip_archive_t *ar);
#endif

#ifdef ZIP_IMPLEMENTATION

typedef struct __attribute__((packed)) {
    uint32_t crc32, comp_size, uncomp_size, local_off;
    uint16_t name_len;
    char name[512];
} zip_entry_t;

struct zip_archive_t {
    FILE *fp;
    zip_entry_t *entries;
    int count, capacity;
};

static void write_u16(FILE *f, uint16_t v) {
    uint8_t b[2] = {v & 0xFF, v >> 8};
    fwrite(b, 1, 2, f);
}

static void write_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24};
    fwrite(b, 1, 4, f);
}

static uint32_t get_crc(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (-(int)(crc & 1) & 0xEDB88320);
    }
    return crc ^ 0xFFFFFFFF;
}

zip_archive_t *zip_create(const char *path) {
    zip_archive_t *ar = calloc(1, sizeof(zip_archive_t));
    ar->fp = fopen(path, "wb");
    ar->capacity = 16;
    ar->entries = calloc(ar->capacity, sizeof(zip_entry_t));
    return ar;
}

static int write_header(zip_archive_t *ar, zip_entry_t *e, int is_cd) {
    write_u32(ar->fp, is_cd ? 0x02014b50 : 0x04034b50);
    if (is_cd) write_u16(ar->fp, 20); 
    
    write_u16(ar->fp, 20);
    write_u16(ar->fp, 0);
    write_u16(ar->fp, 0);
    write_u16(ar->fp, 0);
    write_u16(ar->fp, 0);

    write_u32(ar->fp, e->crc32);
    write_u32(ar->fp, e->comp_size);
    write_u32(ar->fp, e->uncomp_size);
    write_u16(ar->fp, e->name_len);
    write_u16(ar->fp, 0);

    if (is_cd) {
        write_u16(ar->fp, 0);
        write_u16(ar->fp, 0);
        write_u16(ar->fp, 0);
        write_u32(ar->fp, 0);
        write_u32(ar->fp, e->local_off);
    }
    fwrite(e->name, 1, e->name_len, ar->fp);
    return 0;
}

int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name) {
    zip_entry_t *e = &ar->entries[ar->count];
    e->name_len = (uint16_t)strlen(zip_name);
    memcpy(e->name, zip_name, e->name_len);
    e->local_off = (uint32_t)ftell(ar->fp);
    e->uncomp_size = e->comp_size = (uint32_t)size;
    e->crc32 = get_crc(buf, size);

    write_header(ar, e, 0);
    fwrite(buf, 1, size, ar->fp);
    return ar->count++;
}

int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level) {
    FILE *f = fopen(fs_path, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    int ret = zip_add_buffer(ar, buf, sz, zip_name);
    free(buf);
    return ret;
}

int zip_finalize(zip_archive_t *ar) {
    uint32_t cd_start = (uint32_t)ftell(ar->fp);
    for (int i = 0; i < ar->count; i++) write_header(ar, &ar->entries[i], 1);
    uint32_t cd_size = (uint32_t)ftell(ar->fp) - cd_start;

    write_u32(ar->fp, 0x06054b50);
    write_u16(ar->fp, 0); write_u16(ar->fp, 0);
    write_u16(ar->fp, (uint16_t)ar->count);
    write_u16(ar->fp, (uint16_t)ar->count);
    write_u32(ar->fp, cd_size);
    write_u32(ar->fp, cd_start);
    write_u16(ar->fp, 0);
    fclose(ar->fp);
    free(ar->entries); free(ar);
    return 0;
}
#endif
