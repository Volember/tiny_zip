#ifndef ZIP_H
#define ZIP_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h> /* Для wchar_t и size_t */

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct zip_archive_t zip_archive_t;

    zip_archive_t *zip_create(const char *path);
    zip_archive_t *zip_create_mem(void);
    int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level);
    int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name);
    int zip_add_buffer_w(zip_archive_t *ar, const wchar_t *wstr, const char *zip_name);
    int zip_finalize(zip_archive_t *ar);
    void *zip_finalize_mem(zip_archive_t *ar, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_H */

#ifdef ZIP_IMPLEMENTATION

#pragma pack(push, 1)
typedef struct
{
    uint32_t crc32, comp_size, uncomp_size, local_off;
    uint16_t name_len;
    char name[512];
} zip_entry_t;
#pragma pack(pop)

struct zip_archive_t
{
    FILE *fp;
    uint8_t *buf;
    size_t size;
    size_t capacity_buf;
    int is_mem;
    zip_entry_t *entries;
    int count, capacity;
};

static void zip_write(zip_archive_t *ar, const void *ptr, size_t size)
{
    if (ar->is_mem)
    {
        if (ar->size + size > ar->capacity_buf)
        {
            size_t new_cap = ar->capacity_buf ? ar->capacity_buf * 2 : 1024;
            while (ar->size + size > new_cap)
                new_cap *= 2;
            uint8_t *new_buf = (uint8_t *)realloc(ar->buf, new_cap);
            if (!new_buf)
                return;
            ar->buf = new_buf;
            ar->capacity_buf = new_cap;
        }
        memcpy(ar->buf + ar->size, ptr, size);
        ar->size += size;
    }
    else
    {
        fwrite(ptr, 1, size, ar->fp);
    }
}

static size_t zip_tell(zip_archive_t *ar)
{
    if (ar->is_mem)
    {
        return ar->size;
    }
    else
    {
        return (size_t)ftell(ar->fp);
    }
}

static void write_u16(zip_archive_t *ar, uint16_t v)
{
    uint8_t b[2] = {v & 0xFF, (uint8_t)(v >> 8)};
    zip_write(ar, b, 2);
}

static void write_u32(zip_archive_t *ar, uint32_t v)
{
    uint8_t b[4] = {v & 0xFF, (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)(v >> 24)};
    zip_write(ar, b, 4);
}

static uint32_t get_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (-(int)(crc & 1) & 0xEDB88320);
    }
    return crc ^ 0xFFFFFFFF;
}

zip_archive_t *zip_create(const char *path)
{
    zip_archive_t *ar = (zip_archive_t *)calloc(1, sizeof(zip_archive_t));
    if (!ar)
        return NULL;
    ar->fp = fopen(path, "wb");
    if (!ar->fp)
    {
        free(ar);
        return NULL;
    }
    ar->capacity = 16;
    ar->entries = (zip_entry_t *)calloc(ar->capacity, sizeof(zip_entry_t));
    if (!ar->entries)
    {
        fclose(ar->fp);
        free(ar);
        return NULL;
    }
    return ar;
}

zip_archive_t *zip_create_mem(void)
{
    zip_archive_t *ar = (zip_archive_t *)calloc(1, sizeof(zip_archive_t));
    if (!ar)
        return NULL;
    ar->is_mem = 1;
    ar->capacity = 16;
    ar->entries = (zip_entry_t *)calloc(ar->capacity, sizeof(zip_entry_t));
    if (!ar->entries)
    {
        free(ar);
        return NULL;
    }
    return ar;
}

static int write_header(zip_archive_t *ar, zip_entry_t *e, int is_cd)
{
    write_u32(ar, is_cd ? 0x02014b50 : 0x04034b50);
    if (is_cd)
        write_u16(ar, 20);
    write_u16(ar, 20);
    write_u16(ar, 0);
    write_u16(ar, 0);
    write_u16(ar, 0);
    write_u16(ar, 0);
    write_u32(ar, e->crc32);
    write_u32(ar, e->comp_size);
    write_u32(ar, e->uncomp_size);
    write_u16(ar, e->name_len);
    write_u16(ar, 0);
    if (is_cd)
    {
        write_u16(ar, 0);
        write_u16(ar, 0);
        write_u16(ar, 0);
        write_u32(ar, 0);
        write_u32(ar, e->local_off);
    }
    zip_write(ar, e->name, e->name_len);
    return 0;
}

int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name)
{
    if (ar->count >= ar->capacity)
    {
        int new_capacity = ar->capacity * 2;
        zip_entry_t *new_entries = (zip_entry_t *)realloc(ar->entries, new_capacity * sizeof(zip_entry_t));
        if (!new_entries)
            return -1;
        ar->entries = new_entries;
        ar->capacity = new_capacity;
    }
    zip_entry_t *e = &ar->entries[ar->count];
    e->name_len = (uint16_t)strlen(zip_name);
    if (e->name_len > 511)
        e->name_len = 511; /* Защита от переполнения буфера имени */
    memcpy(e->name, zip_name, e->name_len);
    e->name[e->name_len] = '\0';
    e->local_off = (uint32_t)zip_tell(ar);
    e->uncomp_size = e->comp_size = (uint32_t)size;
    e->crc32 = get_crc((const uint8_t *)buf, size);
    write_header(ar, e, 0);
    zip_write(ar, buf, size);
    return ar->count++;
}

int zip_add_buffer_w(zip_archive_t *ar, const wchar_t *wstr, const char *zip_name)
{
    if (!wstr)
        return -1;

    /* 1. Считаем длину будущего UTF-8 буфера */
    size_t utf8_len = 0;
    const wchar_t *p = wstr;
    while (*p)
    {
        uint32_t cp = (uint32_t)*p++;
        /* Обработка суррогатных пар для UTF-16 (Windows) */
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (*p >= 0xDC00 && *p <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (*p - 0xDC00);
                p++;
            }
        }
        utf8_len += (cp <= 0x7F) ? 1 : (cp <= 0x7FF) ? 2
                                   : (cp <= 0xFFFF)  ? 3
                                                     : 4;
    }

    if (utf8_len == 0)
    {
        return zip_add_buffer(ar, "", 0, zip_name);
    }

    /* 2. Выделяем временную память под UTF-8 */
    char *buf = (char *)malloc(utf8_len);
    if (!buf)
        return -1;

    /* 3. Кодируем wchar_t в UTF-8 */
    char *dst = buf;
    p = wstr;
    while (*p)
    {
        uint32_t cp = (uint32_t)*p++;
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (*p >= 0xDC00 && *p <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (*p - 0xDC00);
                p++;
            }
        }
        if (cp <= 0x7F)
        {
            *dst++ = (char)cp;
        }
        else if (cp <= 0x7FF)
        {
            *dst++ = (char)(0xC0 | (cp >> 6));
            *dst++ = (char)(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF)
        {
            *dst++ = (char)(0xE0 | (cp >> 12));
            *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *dst++ = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            *dst++ = (char)(0xF0 | (cp >> 18));
            *dst++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *dst++ = (char)(0x80 | (cp & 0x3F));
        }
    }

    /* 4. Записываем буфер в архив */
    int ret = zip_add_buffer(ar, buf, utf8_len, zip_name);
    free(buf);
    return ret;
}

int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level)
{
    (void)level; /* Временная заглушка, так как сжатие пока не используется */
    FILE *f = fopen(fs_path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf)
    {
        fclose(f);
        return -1;
    }
    size_t read_bytes = fread(buf, 1, sz, f);
    fclose(f);
    int ret = zip_add_buffer(ar, buf, read_bytes, zip_name);
    free(buf);
    return ret;
}

int zip_finalize(zip_archive_t *ar)
{
    if (!ar)
        return -1;
    if (ar->is_mem)
    {
        free(ar->buf);
        free(ar->entries);
        free(ar);
        return 0;
    }
    uint32_t cd_start = (uint32_t)zip_tell(ar);
    for (int i = 0; i < ar->count; i++)
        write_header(ar, &ar->entries[i], 1);
    uint32_t cd_size = (uint32_t)zip_tell(ar) - cd_start;
    write_u32(ar, 0x06054b50);
    write_u16(ar, 0);
    write_u16(ar, 0);
    write_u16(ar, (uint16_t)ar->count);
    write_u16(ar, (uint16_t)ar->count);
    write_u32(ar, cd_size);
    write_u32(ar, cd_start);
    write_u16(ar, 0);
    fclose(ar->fp);
    free(ar->entries);
    free(ar);
    return 0;
}

void *zip_finalize_mem(zip_archive_t *ar, size_t *out_size)
{
    if (!ar || !ar->is_mem)
        return NULL;

    uint32_t cd_start = (uint32_t)zip_tell(ar);
    for (int i = 0; i < ar->count; i++)
        write_header(ar, &ar->entries[i], 1);
    uint32_t cd_size = (uint32_t)zip_tell(ar) - cd_start;
    write_u32(ar, 0x06054b50);
    write_u16(ar, 0);
    write_u16(ar, 0);
    write_u16(ar, (uint16_t)ar->count);
    write_u16(ar, (uint16_t)ar->count);
    write_u32(ar, cd_size);
    write_u32(ar, cd_start);
    write_u16(ar, 0);

    void *buf = ar->buf;
    if (out_size)
        *out_size = ar->size;

    free(ar->entries);
    free(ar);
    return buf;
}

#endif /* ZIP_IMPLEMENTATION */
