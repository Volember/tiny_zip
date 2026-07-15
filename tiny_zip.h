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

    /* level: 0 = store (no compression, fastest).
       1..9  = deflate, increasing effort/ratio (same convention as zlib).
       If deflate does not actually shrink the data, the entry falls back
       to store automatically. */
    int zip_add_file(zip_archive_t *ar, const char *fs_path, const char *zip_name, int level);
    int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name);
    int zip_add_buffer_ex(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name, int level);
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
    uint16_t method; /* 0 = store, 8 = deflate */
    uint16_t flags;  /* general purpose bit flags (bit 11 = UTF-8 name) */
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
    int error; /* set to 1 the moment any write fails */
    zip_entry_t *entries;
    int count, capacity;
};

static int zip_write(zip_archive_t *ar, const void *ptr, size_t size)
{
    if (size == 0)
        return 1;

    if (ar->is_mem)
    {
        if (ar->size + size > ar->capacity_buf)
        {
            size_t new_cap = ar->capacity_buf ? ar->capacity_buf * 2 : 1024;
            while (ar->size + size > new_cap)
                new_cap *= 2;
            uint8_t *new_buf = (uint8_t *)realloc(ar->buf, new_cap);
            if (!new_buf)
            {
                ar->error = 1;
                return 0;
            }
            ar->buf = new_buf;
            ar->capacity_buf = new_cap;
        }
        memcpy(ar->buf + ar->size, ptr, size);
        ar->size += size;
        return 1;
    }
    else
    {
        size_t written = fwrite(ptr, 1, size, ar->fp);
        if (written != size)
        {
            ar->error = 1;
            return 0;
        }
        return 1;
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

static int write_u16(zip_archive_t *ar, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
    return zip_write(ar, b, 2);
}

static int write_u32(zip_archive_t *ar, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                    (uint8_t)((v >> 16) & 0xFF), (uint8_t)(v >> 24)};
    return zip_write(ar, b, 4);
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

/* Returns nonzero if any byte in the name is non-ASCII, in which case we
   set the EFS "language encoding" bit so unzip tools know the name is UTF-8. */
static uint16_t zip_name_flags(const char *name, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)name[i] >= 0x80)
            return 0x0800;
    return 0;
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
    int ok = 1;
    ok = ok && write_u32(ar, is_cd ? 0x02014b50 : 0x04034b50);
    if (is_cd)
        ok = ok && write_u16(ar, 20); /* version made by */
    ok = ok && write_u16(ar, 20);     /* version needed to extract */
    ok = ok && write_u16(ar, e->flags);
    ok = ok && write_u16(ar, e->method);
    ok = ok && write_u16(ar, 0); /* mod time */
    ok = ok && write_u16(ar, 0); /* mod date */
    ok = ok && write_u32(ar, e->crc32);
    ok = ok && write_u32(ar, e->comp_size);
    ok = ok && write_u32(ar, e->uncomp_size);
    ok = ok && write_u16(ar, e->name_len);
    ok = ok && write_u16(ar, 0); /* extra field length */
    if (is_cd)
    {
        ok = ok && write_u16(ar, 0); /* comment length */
        ok = ok && write_u16(ar, 0); /* disk number start */
        ok = ok && write_u16(ar, 0); /* internal attrs */
        ok = ok && write_u32(ar, 0); /* external attrs */
        ok = ok && write_u32(ar, e->local_off);
    }
    ok = ok && zip_write(ar, e->name, e->name_len);
    if (!ok)
        ar->error = 1;
    return ok;
}

/* ==========================================================================
 * Deflate (RFC 1951) — LZ77 hash-chain matcher + static/fixed Huffman coding.
 * Raw deflate stream, single final block. No zlib/gzip wrapper: exactly what
 * the ZIP "method 8" entry needs.
 * ========================================================================== */

#define ZIP_WINDOW_SIZE 32768u
#define ZIP_MIN_MATCH 3u
#define ZIP_MAX_MATCH 258u
#define ZIP_HASH_BITS 15
#define ZIP_HASH_SIZE (1u << ZIP_HASH_BITS)

typedef struct
{
    uint8_t *out;
    size_t out_size;
    size_t out_cap;
    uint32_t bitbuf;
    int bitcount;
} zip_bitwriter_t;

static int bw_init(zip_bitwriter_t *bw, size_t initial_cap)
{
    bw->out = (uint8_t *)malloc(initial_cap ? initial_cap : 64);
    if (!bw->out)
        return 0;
    bw->out_size = 0;
    bw->out_cap = initial_cap ? initial_cap : 64;
    bw->bitbuf = 0;
    bw->bitcount = 0;
    return 1;
}

static int bw_ensure(zip_bitwriter_t *bw, size_t extra)
{
    if (bw->out_size + extra <= bw->out_cap)
        return 1;
    size_t new_cap = bw->out_cap * 2;
    while (new_cap < bw->out_size + extra)
        new_cap *= 2;
    uint8_t *p = (uint8_t *)realloc(bw->out, new_cap);
    if (!p)
        return 0;
    bw->out = p;
    bw->out_cap = new_cap;
    return 1;
}

/* Data elements other than Huffman codes (extra bits, BFINAL/BTYPE, ...) are
   packed LSB-first per RFC 1951 §3.1.1. */
static int bw_put_bits_lsb(zip_bitwriter_t *bw, uint32_t value, int count)
{
    if (count <= 0)
        return 1;
    bw->bitbuf |= (value & ((count < 32 ? (1u << count) : 0u) - 1u)) << bw->bitcount;
    bw->bitcount += count;
    while (bw->bitcount >= 8)
    {
        if (!bw_ensure(bw, 1))
            return 0;
        bw->out[bw->out_size++] = (uint8_t)(bw->bitbuf & 0xFF);
        bw->bitbuf >>= 8;
        bw->bitcount -= 8;
    }
    return 1;
}

static uint32_t zip_bit_reverse(uint32_t v, int bits)
{
    uint32_t r = 0;
    for (int i = 0; i < bits; i++)
    {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

/* Huffman codes ARE packed MSB-first (§3.1.1), so we reverse the bits of the
   canonical code before feeding them into the LSB-first bit packer above. */
static int bw_put_huffman(zip_bitwriter_t *bw, uint32_t code, int len)
{
    return bw_put_bits_lsb(bw, zip_bit_reverse(code, len), len);
}

static int bw_flush(zip_bitwriter_t *bw)
{
    if (bw->bitcount > 0)
    {
        if (!bw_ensure(bw, 1))
            return 0;
        bw->out[bw->out_size++] = (uint8_t)(bw->bitbuf & 0xFF);
        bw->bitbuf = 0;
        bw->bitcount = 0;
    }
    return 1;
}

/* Canonical Huffman code construction, RFC 1951 §3.2.2. Given fixed code
   lengths it reproduces the standard fixed-Huffman literal/length table
   (8 bits for 0-143, 9 bits for 144-255, 7 bits for 256-279, 8 bits for
   280-287). */
static void zip_build_fixed_lit_codes(uint16_t codes[288], uint8_t lens[288])
{
    int i;
    for (i = 0; i <= 143; i++)
        lens[i] = 8;
    for (i = 144; i <= 255; i++)
        lens[i] = 9;
    for (i = 256; i <= 279; i++)
        lens[i] = 7;
    for (i = 280; i <= 287; i++)
        lens[i] = 8;

    int bl_count[16] = {0};
    for (i = 0; i < 288; i++)
        bl_count[lens[i]]++;

    int code = 0;
    int next_code[16] = {0};
    for (int bits = 1; bits <= 15; bits++)
    {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }
    for (i = 0; i < 288; i++)
        codes[i] = (uint16_t)next_code[lens[i]]++;
}

/* Fixed Huffman distance codes are all 5 bits, assigned in symbol order
   (0..29), which the canonical algorithm above reduces to codes[i] = i. */
static void zip_build_fixed_dist_codes(uint16_t codes[30])
{
    for (int i = 0; i < 30; i++)
        codes[i] = (uint16_t)i;
}

static const uint16_t zip_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t zip_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint32_t zip_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t zip_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int zip_find_len_code(uint32_t length, uint32_t *extra_bits, uint32_t *extra_value)
{
    for (int i = 28; i >= 0; i--)
    {
        if (length >= zip_len_base[i])
        {
            *extra_bits = zip_len_extra[i];
            *extra_value = length - zip_len_base[i];
            return 257 + i;
        }
    }
    *extra_bits = 0;
    *extra_value = 0;
    return 257;
}

static int zip_find_dist_code(uint32_t dist, uint32_t *extra_bits, uint32_t *extra_value)
{
    for (int i = 29; i >= 0; i--)
    {
        if (dist >= zip_dist_base[i])
        {
            *extra_bits = zip_dist_extra[i];
            *extra_value = dist - zip_dist_base[i];
            return i;
        }
    }
    *extra_bits = 0;
    *extra_value = 0;
    return 0;
}

static uint32_t zip_hash3(const uint8_t *p)
{
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
    return (v * 2654435761u) >> (32 - ZIP_HASH_BITS);
}

static uint32_t zip_find_match(const uint8_t *data, size_t size, size_t pos,
                               const int32_t *head, const int32_t *prev,
                               int max_chain, uint32_t *out_dist)
{
    uint32_t best_len = 0;
    uint32_t best_dist = 0;

    size_t max_match = size - pos;
    if (max_match > ZIP_MAX_MATCH)
        max_match = ZIP_MAX_MATCH;

    uint32_t h = zip_hash3(data + pos);
    int32_t cand = head[h];
    int chain = max_chain;

    while (cand >= 0 && chain-- > 0)
    {
        size_t dist = pos - (size_t)cand;
        if (dist > ZIP_WINDOW_SIZE)
            break;

        uint32_t len = 0;
        while (len < max_match && data[cand + len] == data[pos + len])
            len++;

        if (len > best_len)
        {
            best_len = len;
            best_dist = (uint32_t)dist;
            if (len >= max_match)
                break;
        }

        cand = prev[cand];
    }

    if (best_len >= ZIP_MIN_MATCH)
    {
        *out_dist = best_dist;
        return best_len;
    }
    return 0;
}

/* Compresses data[0..size) into a freshly malloc'd raw-deflate stream.
   Returns 0 on failure (caller should fall back to store), 1 on success
   with *out_buf and *out_size set (caller owns the buffer and must free it). */
static int zip_deflate(const uint8_t *data, size_t size, uint8_t **out_buf,
                       size_t *out_size, int level)
{
    if (size == 0)
        return 0;

    zip_bitwriter_t bw;
    if (!bw_init(&bw, size / 2 + 64))
        return 0;

    uint16_t lit_codes[288];
    uint8_t lit_lens[288];
    uint16_t dist_codes[30];
    zip_build_fixed_lit_codes(lit_codes, lit_lens);
    zip_build_fixed_dist_codes(dist_codes);

    int32_t *head = (int32_t *)malloc(sizeof(int32_t) * ZIP_HASH_SIZE);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * size);
    if (!head || !prev)
    {
        free(head);
        free(prev);
        free(bw.out);
        return 0;
    }
    for (uint32_t i = 0; i < ZIP_HASH_SIZE; i++)
        head[i] = -1;

    int max_chain = 8 + level * 16;
    if (max_chain > 256)
        max_chain = 256;
    if (max_chain < 8)
        max_chain = 8;

    size_t hash_limit = (size >= ZIP_MIN_MATCH) ? (size - ZIP_MIN_MATCH + 1) : 0;
    int ok = 1;

    ok = ok && bw_put_bits_lsb(&bw, 1, 1); /* BFINAL = 1 (only block) */
    ok = ok && bw_put_bits_lsb(&bw, 1, 2); /* BTYPE  = 01 (fixed Huffman) */

    size_t pos = 0;
    while (ok && pos < size)
    {
        uint32_t dist = 0;
        uint32_t len = (pos < hash_limit)
                           ? zip_find_match(data, size, pos, head, prev, max_chain, &dist)
                           : 0;

        if (len >= ZIP_MIN_MATCH)
        {
            uint32_t ex_bits, ex_val;
            int lcode = zip_find_len_code(len, &ex_bits, &ex_val);
            ok = ok && bw_put_huffman(&bw, lit_codes[lcode], lit_lens[lcode]);
            if (ex_bits)
                ok = ok && bw_put_bits_lsb(&bw, ex_val, (int)ex_bits);

            uint32_t dex_bits, dex_val;
            int dcode = zip_find_dist_code(dist, &dex_bits, &dex_val);
            ok = ok && bw_put_huffman(&bw, dist_codes[dcode], 5);
            if (dex_bits)
                ok = ok && bw_put_bits_lsb(&bw, dex_val, (int)dex_bits);

            size_t end = pos + len;
            while (pos < end)
            {
                if (pos < hash_limit)
                {
                    uint32_t h = zip_hash3(data + pos);
                    prev[pos] = head[h];
                    head[h] = (int32_t)pos;
                }
                pos++;
            }
        }
        else
        {
            ok = ok && bw_put_huffman(&bw, lit_codes[data[pos]], lit_lens[data[pos]]);
            if (pos < hash_limit)
            {
                uint32_t h = zip_hash3(data + pos);
                prev[pos] = head[h];
                head[h] = (int32_t)pos;
            }
            pos++;
        }
    }

    ok = ok && bw_put_huffman(&bw, lit_codes[256], lit_lens[256]); /* end-of-block */
    ok = ok && bw_flush(&bw);

    free(head);
    free(prev);

    if (!ok)
    {
        free(bw.out);
        return 0;
    }

    *out_buf = bw.out;
    *out_size = bw.out_size;
    return 1;
}

/* ==========================================================================
 * Public entry points
 * ========================================================================== */

int zip_add_buffer_ex(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name, int level)
{
    if (!ar || !zip_name)
        return -1;

    if (ar->count >= ar->capacity)
    {
        int new_capacity = ar->capacity * 2;
        zip_entry_t *new_entries = (zip_entry_t *)realloc(ar->entries, (size_t)new_capacity * sizeof(zip_entry_t));
        if (!new_entries)
            return -1;
        ar->entries = new_entries;
        ar->capacity = new_capacity;
    }

    zip_entry_t *e = &ar->entries[ar->count];
    e->name_len = (uint16_t)strlen(zip_name);
    if (e->name_len > 511)
        e->name_len = 511; /* защита от переполнения буфера имени */
    memcpy(e->name, zip_name, e->name_len);
    e->name[e->name_len] = '\0';
    e->flags = zip_name_flags(zip_name, e->name_len);
    e->local_off = (uint32_t)zip_tell(ar);
    e->uncomp_size = (uint32_t)size;
    e->crc32 = get_crc((const uint8_t *)buf, size);

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    int use_deflate = 0;

    if (level > 0 && size > 0)
    {
        if (zip_deflate((const uint8_t *)buf, size, &compressed, &compressed_size, level) &&
            compressed_size < size)
        {
            use_deflate = 1;
        }
    }

    int hok, wok;
    if (use_deflate)
    {
        e->method = 8;
        e->comp_size = (uint32_t)compressed_size;
        hok = write_header(ar, e, 0);
        wok = hok && zip_write(ar, compressed, compressed_size);
        free(compressed);
    }
    else
    {
        free(compressed); /* no-op if NULL, or discards a deflate result that didn't help */
        e->method = 0;
        e->comp_size = (uint32_t)size;
        hok = write_header(ar, e, 0);
        wok = hok && zip_write(ar, buf, size);
    }

    if (!wok)
        return -1;

    return ar->count++;
}

int zip_add_buffer(zip_archive_t *ar, const void *buf, size_t size, const char *zip_name)
{
    return zip_add_buffer_ex(ar, buf, size, zip_name, 0);
}

/* Decodes one (possibly surrogate-pair) UTF-16 code unit sequence into a
   Unicode code point. Malformed / unpaired surrogates become U+FFFD instead
   of leaking invalid bytes into the UTF-8 output. Shared between the sizing
   pass and the encoding pass so both always agree on lengths. */
static uint32_t zip_utf16_next(const wchar_t **pp)
{
    const wchar_t *p = *pp;
    uint32_t cp = (uint32_t)*p++;

    if (sizeof(wchar_t) == 2)
    {
        /* wchar_t holds UTF-16 code units (Windows): combine surrogate pairs. */
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            uint32_t lo = (uint32_t)*p;
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p++;
            }
            else
            {
                cp = 0xFFFD; /* lone high surrogate */
            }
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            cp = 0xFFFD; /* lone low surrogate */
        }
    }
    else
    {
        /* wchar_t already holds a full code point (UTF-32, e.g. Linux/macOS).
           Surrogate values and out-of-range values are never valid standalone
           Unicode scalar values, so reject them instead of emitting broken
           (CESU-8-like) UTF-8 for them. */
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            cp = 0xFFFD;
    }

    *pp = p;
    return cp;
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
        uint32_t cp = zip_utf16_next(&p);
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

    /* 3. Кодируем в UTF-8 (та же последовательность code points, что и выше) */
    char *dst = buf;
    p = wstr;
    while (*p)
    {
        uint32_t cp = zip_utf16_next(&p);

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
    FILE *f = fopen(fs_path, "rb");
    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) /* fseek/ftell can fail (e.g. fs_path is a directory) */
    {
        fclose(f);
        return -1;
    }
    if ((unsigned long)sz > 0xFFFFFFFFUL)
    {
        /* Classic ZIP (no Zip64) stores sizes in 32-bit fields, so this is a
           real format limit -- and it also guards against directories/special
           files that some libc's fseek/ftell report as having a bogus, huge
           size instead of failing outright. */
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return -1;
    }

    uint8_t *buf = NULL;
    if (sz > 0)
    {
        buf = (uint8_t *)malloc((size_t)sz);
        if (!buf)
        {
            fclose(f);
            return -1;
        }
    }

    size_t read_bytes = (sz > 0) ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);

    if (read_bytes != (size_t)sz) /* short/failed read: don't archive garbage */
    {
        free(buf);
        return -1;
    }

    int ret = zip_add_buffer_ex(ar, buf, read_bytes, zip_name, level);
    free(buf);
    return ret;
}

int zip_finalize(zip_archive_t *ar)
{
    if (!ar)
        return -1;

    if (ar->is_mem)
    {
        int had_error = ar->error;
        free(ar->buf);
        free(ar->entries);
        free(ar);
        return had_error ? -1 : 0;
    }

    int ok = 1;
    uint32_t cd_start = (uint32_t)zip_tell(ar);
    for (int i = 0; i < ar->count && ok; i++)
        ok = write_header(ar, &ar->entries[i], 1);

    uint32_t cd_size = (uint32_t)zip_tell(ar) - cd_start;
    ok = ok && write_u32(ar, 0x06054b50);
    ok = ok && write_u16(ar, 0);
    ok = ok && write_u16(ar, 0);
    ok = ok && write_u16(ar, (uint16_t)ar->count);
    ok = ok && write_u16(ar, (uint16_t)ar->count);
    ok = ok && write_u32(ar, cd_size);
    ok = ok && write_u32(ar, cd_start);
    ok = ok && write_u16(ar, 0);

    int had_error = ar->error || !ok;
    fclose(ar->fp);
    free(ar->entries);
    free(ar);
    return had_error ? -1 : 0;
}

void *zip_finalize_mem(zip_archive_t *ar, size_t *out_size)
{
    if (!ar || !ar->is_mem)
        return NULL;

    int ok = 1;
    uint32_t cd_start = (uint32_t)zip_tell(ar);
    for (int i = 0; i < ar->count && ok; i++)
        ok = write_header(ar, &ar->entries[i], 1);

    uint32_t cd_size = (uint32_t)zip_tell(ar) - cd_start;
    ok = ok && write_u32(ar, 0x06054b50);
    ok = ok && write_u16(ar, 0);
    ok = ok && write_u16(ar, 0);
    ok = ok && write_u16(ar, (uint16_t)ar->count);
    ok = ok && write_u16(ar, (uint16_t)ar->count);
    ok = ok && write_u32(ar, cd_size);
    ok = ok && write_u32(ar, cd_start);
    ok = ok && write_u16(ar, 0);

    if (!ok || ar->error)
    {
        free(ar->buf);
        free(ar->entries);
        free(ar);
        return NULL;
    }

    void *buf = ar->buf;
    if (out_size)
        *out_size = ar->size;

    free(ar->entries);
    free(ar);
    return buf;
}

#endif /* ZIP_IMPLEMENTATION */
