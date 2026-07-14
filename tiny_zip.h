#ifndef ZIP_H
#define ZIP_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct zip_archive_t zip_archive_t;

zip_archive_t *zip_create(const char *path);
int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level);
int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name, int level);
int zip_finalize(zip_archive_t *ar);
#endif

#ifdef ZIP_IMPLEMENTATION

typedef struct {
    uint32_t crc32, comp_size, uncomp_size, local_off;
    uint16_t name_len;
    char name[512];
} zip_entry_t;

struct zip_archive_t {
    FILE *fp;
    zip_entry_t *entries;
    int count, capacity, error;
};

static void write_u16(FILE *f, uint16_t v) { uint8_t b[2] = {v&0xFF, v>>8}; fwrite(b, 1, 2, f); }
static void write_u32(FILE *f, uint32_t v) { uint8_t b[4] = {v&0xFF, (v>>8)&0xFF, (v>>16)&0xFF, v>>24}; fwrite(b, 1, 4, f); }

static uint32_t crc32_for_byte(uint32_t r) {
    for(int j = 0; j < 8; j++) r = (r & 1? 0xEDB88320 ^ (r >> 1) : r >> 1);
    return r;
}

static uint32_t get_crc(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    while (size--) crc = crc32_for_byte(crc ^ *data++) ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

zip_archive_t *zip_create(const char *path) {
    zip_archive_t *ar = calloc(1, sizeof(zip_archive_t));
    if (!ar) return NULL;
    ar->fp = fopen(path, "wb");
    if (!ar->fp) { free(ar); return NULL; }
    ar->capacity = 16;
    ar->entries = calloc(ar->capacity, sizeof(zip_entry_t));
    return ar;
}

static int write_header(zip_archive_t *ar, zip_entry_t *e, int is_cd) {
    write_u32(ar->fp, is_cd ? 0x02014b50 : 0x04034b50);
    if(is_cd) write_u16(ar->fp, 20);
    write_u16(ar->fp, 20); write_u16(ar->fp, 0); write_u16(ar->fp, 0);
    write_u16(ar->fp, 0); write_u16(ar->fp, 0);
    write_u32(ar->fp, e->crc32);
    write_u32(ar->fp, e->comp_size);
    write_u32(ar->fp, e->uncomp_size);
    write_u16(ar->fp, e->name_len);
    write_u16(ar->fp, 0);
    if(is_cd) { write_u16(ar->fp, 0); write_u16(ar->fp, 0); write_u16(ar->fp, 0); write_u32(ar->fp, 0); write_u32(ar->fp, e->local_off); }
    fwrite(e->name, 1, e->name_len, ar->fp);
    return ferror(ar->fp) ? -1 : 0;
}

int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name, int level) {
    if (ar->count >= ar->capacity) {
        void *tmp = realloc(ar->entries, (ar->capacity * 2) * sizeof(zip_entry_t));
        if (!tmp) return -1;
        ar->entries = tmp; ar->capacity *= 2;
    }
    zip_entry_t *e = &ar->entries[ar->count];
    e->name_len = (uint16_t)strlen(zip_name);
    memcpy(e->name, zip_name, e->name_len);
    e->local_off = (uint32_t)ftell(ar->fp);
    e->uncomp_size = (uint32_t)size;
    e->crc32 = get_crc(buf, size);
    e->comp_size = e->uncomp_size;
    
    if (write_header(ar, e, 0) < 0 || fwrite(buf, 1, size, ar->fp) != size) return -1;
    return ar->count++;
}

int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level) {
    FILE *f = fopen(fs_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, sz, f); fclose(f);
    int ret = zip_add_buffer(ar, buf, sz, zip_name, level);
    free(buf); return ret;
}

int zip_finalize(zip_archive_t *ar) {
    uint32_t start = (uint32_t)ftell(ar->fp);
    for(int i=0; i<ar->count; i++) write_header(ar, &ar->entries[i], 1);
    uint32_t end = (uint32_t)ftell(ar->fp);
    write_u32(ar->fp, 0x06054b50); write_u16(ar->fp, 0); write_u16(ar->fp, 0);
    write_u16(ar->fp, (uint16_t)ar->count); write_u16(ar->fp, (uint16_t)ar->count);
    write_u32(ar->fp, end - start); write_u32(ar->fp, start);
    int err = ferror(ar->fp);
    fclose(ar->fp); free(ar->entries); free(ar);
    return err ? -1 : 0;
}
#endif
