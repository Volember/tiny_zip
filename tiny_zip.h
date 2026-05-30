/*
 * tiny_zip.h — Single-header ZIP archive library (C99)
 *
 * Usage:
 *   #define NZ_IMPLEMENTATION
 *   #include "tiny_zip.h"
 *
 * API:
 *   nz_archive* nz_create(const char* path);
 *   int         nz_add_file(nz_archive* ar, const char* name, int level);
 *   void        nz_finalize(nz_archive* ar);
 *
 * No external dependencies. Pure C99.
 * Implements: CRC-32, LZ77, Fixed Huffman Deflate, ZIP format.
 * Strictly no "hidden" heap allocations in inner loops. Memory is
 * allocated once at the start in nz_create.
 */
#ifndef tiny_zip_H
#define tiny_zip_H

#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

typedef struct nz_archive nz_archive;

/* Create a new ZIP archive at `path`. Returns NULL on failure. */
nz_archive *nz_create(const char *path);

/* Add a file to the archive. level: 0=store, 1-9=deflate. Returns 0 on success. */
int nz_add_file(nz_archive *ar, const char *name, int level);

/* Add a file to the archive with custom archive name. Returns 0 on success. */
int nz_add_file_ex(nz_archive *ar, const char *file_path, const char *archive_name, int level);

/* Write central directory and close. Always call this. */
void nz_finalize(nz_archive *ar);

#endif /* tiny_zip_H */

/* ======================================================================== */
#ifdef NZ_IMPLEMENTATION
/* ======================================================================== */

#include <stdlib.h>
#include <string.h>

/* ---------- configuration ---------- */
#define NZ_WBITS 15
#define NZ_WSIZE (1u << NZ_WBITS) /* 32768 */
#define NZ_WMASK (NZ_WSIZE - 1u)
#define NZ_HASH_BITS 15
#define NZ_HASH_SIZE (1u << NZ_HASH_BITS)
#define NZ_HASH_MASK (NZ_HASH_SIZE - 1u)
#define NZ_MIN_MATCH 3
#define NZ_MAX_MATCH 258
#define NZ_MAX_FILES 4096
#define NZ_IO_BUF (NZ_WSIZE * 2) /* 64 KB read buffer */
#define NZ_NIL 0xFFFFu

/* ---------- ZIP signatures ---------- */
#define NZ_SIG_LOCAL 0x04034b50u
#define NZ_SIG_CD 0x02014b50u
#define NZ_SIG_EOCD 0x06054b50u

/* ---------- internal: central-dir entry ---------- */
typedef struct
{
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_off;
    uint16_t method;
    uint16_t name_len;
    uint16_t mod_time;
    uint16_t mod_date;
    char name[512];
} nz_cd_entry;

/* ---------- bit writer ---------- */
typedef struct
{
    FILE *fp;
    uint32_t buf;
    int n;
    uint32_t total;
} nz_bits;

/* ---------- archive handle ---------- */
struct nz_archive
{
    FILE *fp;
    nz_cd_entry entries[NZ_MAX_FILES];
    int count;
    void *work_buf; /* Allocated once for LZ77 */
};

/* ================================================================ */
/*  CRC-32                                                          */
/* ================================================================ */
static uint32_t nz_crc_tab[256];
static int nz_crc_ready;

static void nz_crc_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++)
    {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0u);
        nz_crc_tab[i] = c;
    }
    nz_crc_ready = 1;
}

static uint32_t nz_crc32(uint32_t crc, const uint8_t *p, size_t len)
{
    if (!nz_crc_ready)
        nz_crc_init();
    crc = ~crc;
    while (len--)
        crc = nz_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ================================================================ */
/*  Little-endian helpers                                           */
/* ================================================================ */
static void nz_put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}
static void nz_put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

/* ================================================================ */
/*  Bit writer (LSB-first)                                          */
/* ================================================================ */
static void nz_bits_init(nz_bits *b, FILE *fp)
{
    b->fp = fp;
    b->buf = 0;
    b->n = 0;
    b->total = 0;
}
static void nz_bits_put(nz_bits *b, uint32_t val, int bits)
{
    b->buf |= val << b->n;
    b->n += bits;
    while (b->n >= 8)
    {
        fputc((int)(b->buf & 0xFF), b->fp);
        b->total++;
        b->buf >>= 8;
        b->n -= 8;
    }
}
static void nz_bits_flush(nz_bits *b)
{
    if (b->n > 0)
    {
        fputc((int)(b->buf & 0xFF), b->fp);
        b->total++;
    }
    b->buf = 0;
    b->n = 0;
}

/* ================================================================ */
/*  Fixed Huffman tables (RFC 1951 §3.2.6)                          */
/* ================================================================ */
static uint16_t nz_rev_bits(uint16_t v, int n)
{
    uint16_t r = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        r = (uint16_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

static void nz_fixed_lit(int sym, uint16_t *code, int *nbits)
{
    uint16_t c;
    int n;
    if (sym <= 143)
    {
        c = (uint16_t)(0x30 + sym);
        n = 8;
    }
    else if (sym <= 255)
    {
        c = (uint16_t)(0x190 + sym - 144);
        n = 9;
    }
    else if (sym <= 279)
    {
        c = (uint16_t)(sym - 256);
        n = 7;
    }
    else
    {
        c = (uint16_t)(0xC0 + sym - 280);
        n = 8;
    }
    *code = nz_rev_bits(c, n);
    *nbits = n;
}

static void nz_emit_lit(nz_bits *b, int sym)
{
    uint16_t c;
    int n;
    nz_fixed_lit(sym, &c, &n);
    nz_bits_put(b, c, n);
}

/* ---------- length / distance tables (RFC 1951 §3.2.5) ---------- */
static const uint16_t nz_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t nz_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t nz_dst_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const uint8_t nz_dst_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int nz_len_code(int len)
{
    int i;
    for (i = 28; i >= 0; i--)
        if (len >= nz_len_base[i])
            return i;
    return 0;
}
static int nz_dst_code(int dist)
{
    int i;
    for (i = 29; i >= 0; i--)
        if (dist >= nz_dst_base[i])
            return i;
    return 0;
}

static void nz_emit_match(nz_bits *b, int len, int dist)
{
    int li, di;
    uint16_t dc;

    /* length */
    li = nz_len_code(len);
    nz_emit_lit(b, li + 257);
    if (nz_len_extra[li])
        nz_bits_put(b, (uint32_t)(len - nz_len_base[li]), nz_len_extra[li]);

    /* distance — fixed: 5-bit reversed */
    di = nz_dst_code(dist);
    dc = nz_rev_bits((uint16_t)di, 5);
    nz_bits_put(b, dc, 5);
    if (nz_dst_extra[di])
        nz_bits_put(b, (uint32_t)(dist - nz_dst_base[di]), nz_dst_extra[di]);
}

/* ================================================================ */
/*  LZ77 engine with hash chains                                    */
/* ================================================================ */
typedef struct
{
    uint8_t win[NZ_IO_BUF]; /* 64 KB ring: [0..WSIZE-1] history, [WSIZE..2*WSIZE-1] lookahead */
    uint32_t wpos;          /* current position in win[] being processed */
    uint32_t wend;          /* valid data end in win[] */
    uint16_t head[NZ_HASH_SIZE];
    uint16_t prev[NZ_WSIZE];
    int max_chain;
    int nice_len;
} nz_lz77;

static uint32_t nz_hash3(const uint8_t *p)
{
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2]) & NZ_HASH_MASK;
}

static void nz_lz_init(nz_lz77 *lz, int level)
{
    memset(lz->head, 0xFF, sizeof(lz->head)); /* NZ_NIL */
    memset(lz->prev, 0xFF, sizeof(lz->prev));
    lz->wpos = 0;
    lz->wend = 0;
    /* tune by level */
    if (level <= 1)
    {
        lz->max_chain = 4;
        lz->nice_len = 8;
    }
    else if (level <= 3)
    {
        lz->max_chain = 16;
        lz->nice_len = 16;
    }
    else if (level <= 5)
    {
        lz->max_chain = 64;
        lz->nice_len = 32;
    }
    else if (level <= 7)
    {
        lz->max_chain = 256;
        lz->nice_len = 128;
    }
    else
    {
        lz->max_chain = 1024;
        lz->nice_len = 258;
    }
}

static void nz_lz_slide(nz_lz77 *lz)
{
    uint32_t i;
    /* move upper half to lower half */
    memmove(lz->win, lz->win + NZ_WSIZE, NZ_WSIZE);
    lz->wpos -= NZ_WSIZE;
    lz->wend -= NZ_WSIZE;
    /* adjust hash heads */
    for (i = 0; i < NZ_HASH_SIZE; i++)
    {
        uint16_t v = lz->head[i];
        lz->head[i] = (v >= NZ_WSIZE && v != NZ_NIL) ? (uint16_t)(v - NZ_WSIZE) : NZ_NIL;
    }
    for (i = 0; i < NZ_WSIZE; i++)
    {
        uint16_t v = lz->prev[i];
        lz->prev[i] = (v >= NZ_WSIZE && v != NZ_NIL) ? (uint16_t)(v - NZ_WSIZE) : NZ_NIL;
    }
}

static int nz_lz_find(nz_lz77 *lz, uint32_t pos, uint32_t *mdist)
{
    uint32_t h, cur, limit, avail;
    int best = NZ_MIN_MATCH - 1, chain = lz->max_chain;
    const uint8_t *s;

    if (pos + NZ_MIN_MATCH > lz->wend)
        return 0;

    h = nz_hash3(lz->win + pos);
    cur = lz->head[h];
    lz->prev[pos & NZ_WMASK] = (uint16_t)cur;
    lz->head[h] = (uint16_t)pos;

    limit = (pos > NZ_WSIZE) ? pos - NZ_WSIZE : 0;
    avail = lz->wend - pos;
    if (avail > NZ_MAX_MATCH)
        avail = NZ_MAX_MATCH;

    s = lz->win + pos;
    while (cur != NZ_NIL && cur >= limit && chain-- > 0)
    {
        const uint8_t *m = lz->win + cur;
        /* quick reject: check end byte first */
        if (m[best] == s[best])
        {
            int len = 0;
            while (len < (int)avail && m[len] == s[len])
                len++;
            if (len > best)
            {
                best = len;
                *mdist = pos - cur;
                if (best >= lz->nice_len || best >= (int)avail)
                    break;
            }
        }
        cur = lz->prev[cur & NZ_WMASK];
    }
    return (best >= NZ_MIN_MATCH) ? best : 0;
}

/* Insert hash for position without searching (used when skipping via match) */
static void nz_lz_insert(nz_lz77 *lz, uint32_t pos)
{
    uint32_t h;
    if (pos + NZ_MIN_MATCH > lz->wend)
        return;
    h = nz_hash3(lz->win + pos);
    lz->prev[pos & NZ_WMASK] = lz->head[h];
    lz->head[h] = (uint16_t)pos;
}

/* ================================================================ */
/*  Deflate compressor (fixed Huffman, single block)                */
/* ================================================================ */
static uint32_t nz_deflate_stream(nz_archive *ar, FILE *in, nz_bits *out, int level)
{
    nz_lz77 *lz = (nz_lz77 *)ar->work_buf;
    size_t rd;
    uint32_t crc = 0, pos;

    nz_lz_init(lz, level);

    /* initial fill */
    rd = fread(lz->win, 1, NZ_IO_BUF, in);
    lz->wend = (uint32_t)rd;
    crc = nz_crc32(crc, lz->win, rd);

    /* BFINAL=1, BTYPE=01 (fixed Huffman) */
    nz_bits_put(out, 1, 1); /* BFINAL */
    nz_bits_put(out, 1, 2); /* BTYPE = fixed */

    pos = 0;
    while (pos < lz->wend)
    {
        uint32_t dist = 0;
        int mlen;

        /* Slide window when needed */
        if (pos >= NZ_WSIZE + NZ_WSIZE - NZ_MAX_MATCH)
        {
            /* need more data — try to refill after slide */
            nz_lz_slide(lz);
            pos -= NZ_WSIZE;
            rd = fread(lz->win + lz->wend, 1, NZ_IO_BUF - lz->wend, in);
            if (rd > 0)
            {
                crc = nz_crc32(crc, lz->win + lz->wend, rd);
                lz->wend += (uint32_t)rd;
            }
        }

        mlen = nz_lz_find(lz, pos, &dist);
        if (mlen >= NZ_MIN_MATCH)
        {
            uint32_t k;
            nz_emit_match(out, mlen, (int)dist);
            /* insert hashes for the match body */
            for (k = 1; k < (uint32_t)mlen; k++)
                nz_lz_insert(lz, pos + k);
            pos += (uint32_t)mlen;
        }
        else
        {
            nz_emit_lit(out, lz->win[pos]);
            pos++;
        }
    }

    /* end of block symbol (256) */
    nz_emit_lit(out, 256);
    nz_bits_flush(out);

    return crc;
}

/* ================================================================ */
/*  Store (level 0): raw copy in stored Deflate blocks              */
/* ================================================================ */
#define NZ_STORE_BLK 65535u

static uint32_t nz_store_stream(FILE *in, FILE *fp, uint32_t *comp_out)
{
    uint8_t buf[NZ_STORE_BLK];
    uint32_t crc = 0, total = 0;
    size_t rd;

    while ((rd = fread(buf, 1, NZ_STORE_BLK, in)) > 0)
    {
        crc = nz_crc32(crc, buf, rd);
        fwrite(buf, 1, rd, fp);
        total += (uint32_t)rd;
    }

    *comp_out = total;
    return crc;
}

/* ================================================================ */
/*  DOS date/time helpers                                           */
/* ================================================================ */

/* ================================================================ */
/*  ZIP local file header                                           */
/* ================================================================ */
static void nz_write_local(FILE *fp, const nz_cd_entry *e)
{
    nz_put32(fp, NZ_SIG_LOCAL);
    nz_put16(fp, 20);        /* version needed */
    nz_put16(fp, 0);         /* flags */
    nz_put16(fp, e->method); /* method */
    nz_put16(fp, e->mod_time);
    nz_put16(fp, e->mod_date);
    nz_put32(fp, e->crc32);
    nz_put32(fp, e->comp_size);
    nz_put32(fp, e->uncomp_size);
    nz_put16(fp, e->name_len);
    nz_put16(fp, 0); /* extra len */
    fwrite(e->name, 1, e->name_len, fp);
}

/* ================================================================ */
/*  ZIP central directory entry                                     */
/* ================================================================ */
static void nz_write_cd(FILE *fp, const nz_cd_entry *e)
{
    nz_put32(fp, NZ_SIG_CD);
    nz_put16(fp, 20); /* version made by */
    nz_put16(fp, 20); /* version needed */
    nz_put16(fp, 0);  /* flags */
    nz_put16(fp, e->method);
    nz_put16(fp, e->mod_time);
    nz_put16(fp, e->mod_date);
    nz_put32(fp, e->crc32);
    nz_put32(fp, e->comp_size);
    nz_put32(fp, e->uncomp_size);
    nz_put16(fp, e->name_len);
    nz_put16(fp, 0);    /* extra len */
    nz_put16(fp, 0);    /* comment len */
    nz_put16(fp, 0);    /* disk start */
    nz_put16(fp, 0);    /* internal attrs */
    nz_put32(fp, 0x20); /* external attrs: archive bit */
    nz_put32(fp, e->local_off);
    fwrite(e->name, 1, e->name_len, fp);
}

/* ================================================================ */
/*  Public API                                                      */
/* ================================================================ */

nz_archive *nz_create(const char *path)
{
    nz_archive *ar = (nz_archive *)calloc(1, sizeof(nz_archive));
    if (!ar)
        return NULL;
    ar->fp = fopen(path, "wb");
    if (!ar->fp)
    {
        free(ar);
        return NULL;
    }
    ar->count = 0;
    if (sizeof(nz_lz77) > 1024 * 1024)
    {
        fclose(ar->fp);
        free(ar);
        return NULL;
    }
    ar->work_buf = calloc(1, sizeof(nz_lz77));
    if (!ar->work_buf)
    {
        fclose(ar->fp);
        free(ar);
        return NULL;
    }
    return ar;
}

int nz_add_file(nz_archive *ar, const char *name, int level)
{
    nz_cd_entry *e;
    FILE *in = NULL;
    long local_start, data_end;
    int i;

    if (!ar || !ar->fp || ar->count >= NZ_MAX_FILES)
        return -1;
    if (!name)
        return -1;
    if (level < 0)
        level = 0;
    if (level > 9)
        level = 9;

    e = &ar->entries[ar->count];
    memset(e, 0, sizeof(*e));
    const char *store_name = name;
    if (store_name[0] && store_name[1] == ':')
        store_name += 2;
    while (*store_name == '/' || *store_name == '\\')
        store_name++;

    /* Check for path traversal */
    if (strstr(store_name, "..") != NULL)
        return -1;
    if (store_name[0] == '/' || (store_name[0] && store_name[1] == ':'))
        return -1;

    size_t name_len = strlen(store_name);
    if (name_len > 511)
        name_len = 511;
    e->name_len = (uint16_t)name_len;

    int is_dir = 0;
    for (i = 0; i < e->name_len; i++)
    {
        e->name[i] = (store_name[i] == '\\') ? '/' : store_name[i];
    }
    e->name[e->name_len] = '\0';

    /* Check if it's a directory using file system */
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(name);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        is_dir = 1;
        /* Ensure directory name ends with '/' */
        if (e->name_len > 0 && e->name[e->name_len - 1] != '/')
        {
            e->name[e->name_len++] = '/';
            e->name[e->name_len] = '\0';
        }
    }
#else
    struct stat st;
    if (stat(name, &st) == 0 && S_ISDIR(st.st_mode))
    {
        is_dir = 1;
        /* Ensure directory name ends with '/' */
        if (e->name_len > 0 && e->name[e->name_len - 1] != '/')
        {
            e->name[e->name_len++] = '/';
            e->name[e->name_len] = '\0';
        }
    }
#endif

    if (!is_dir)
    {
        in = fopen(name, "rb");
        if (!in)
            return -1;
    }

    e->mod_time = 0;
    e->mod_date = (uint16_t)((44 << 9) | (1 << 5) | 1);
    e->method = (level == 0 || is_dir) ? 0 : 8;

    local_start = ftell(ar->fp);
    e->local_off = (uint32_t)local_start;

    if (in)
    {
        fseek(in, 0, SEEK_END);
        long file_size = ftell(in);
        if (file_size < 0 || file_size > 0xFFFFFFFF)
        {
            fclose(in);
            return -1;
        }
        e->uncomp_size = (uint32_t)file_size;
        fseek(in, 0, SEEK_SET);
    }
    else
    {
        e->uncomp_size = 0;
    }

    /* write local header with zeros for crc/sizes */
    nz_write_local(ar->fp, e);

    /* compress */
    if (is_dir)
    {
        e->comp_size = 0;
        e->crc32 = 0;
    }
    else if (level == 0)
    {
        uint32_t comp = 0;
        e->crc32 = nz_store_stream(in, ar->fp, &comp);
        e->comp_size = comp;
    }
    else
    {
        nz_bits bits;
        nz_bits_init(&bits, ar->fp);
        e->crc32 = nz_deflate_stream(ar, in, &bits, level);
        e->comp_size = bits.total;
    }
    if (in)
        fclose(in);

    data_end = ftell(ar->fp);

    /* patch the local header with real values */
    fseek(ar->fp, local_start, SEEK_SET);
    nz_write_local(ar->fp, e);
    fseek(ar->fp, data_end, SEEK_SET);

    ar->count++;
    return 0;
}

int nz_add_file_ex(nz_archive *ar, const char *file_path, const char *archive_name, int level)
{
    nz_cd_entry *e;
    FILE *in = NULL;
    long local_start, data_end;
    int i;

    if (!ar || !ar->fp || ar->count >= NZ_MAX_FILES)
        return -1;
    if (!file_path || !archive_name)
        return -1;
    if (level < 0)
        level = 0;
    if (level > 9)
        level = 9;

    e = &ar->entries[ar->count];
    memset(e, 0, sizeof(*e));
    const char *store_name = archive_name;
    while (*store_name == '/' || *store_name == '\\')
        store_name++;

    /* Check for path traversal */
    if (strstr(store_name, "..") != NULL)
        return -1;
    if (store_name[0] == '/' || (store_name[0] && store_name[1] == ':'))
        return -1;

    size_t name_len = strlen(store_name);
    if (name_len > 511)
        name_len = 511;
    e->name_len = (uint16_t)name_len;

    int is_dir = 0;
    for (i = 0; i < e->name_len; i++)
    {
        e->name[i] = (store_name[i] == '\\') ? '/' : store_name[i];
    }
    e->name[e->name_len] = '\0';

    /* Check if it's a directory using file system */
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(file_path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        is_dir = 1;
        /* Ensure directory name ends with '/' */
        if (e->name_len > 0 && e->name[e->name_len - 1] != '/')
        {
            e->name[e->name_len++] = '/';
            e->name[e->name_len] = '\0';
        }
    }
#else
    struct stat st;
    if (stat(file_path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        is_dir = 1;
        /* Ensure directory name ends with '/' */
        if (e->name_len > 0 && e->name[e->name_len - 1] != '/')
        {
            e->name[e->name_len++] = '/';
            e->name[e->name_len] = '\0';
        }
    }
#endif

    if (!is_dir)
    {
        in = fopen(file_path, "rb");
        if (!in)
            return -1;
    }

    e->mod_time = 0;
    e->mod_date = (uint16_t)((44 << 9) | (1 << 5) | 1);
    e->method = (level == 0 || is_dir) ? 0 : 8;

    local_start = ftell(ar->fp);
    e->local_off = (uint32_t)local_start;

    if (in)
    {
        fseek(in, 0, SEEK_END);
        long file_size = ftell(in);
        if (file_size < 0 || file_size > 0xFFFFFFFF)
        {
            fclose(in);
            return -1;
        }
        e->uncomp_size = (uint32_t)file_size;
        fseek(in, 0, SEEK_SET);
    }
    else
    {
        e->uncomp_size = 0;
    }

    /* write local header with zeros for crc/sizes */
    nz_write_local(ar->fp, e);

    /* compress */
    if (is_dir)
    {
        e->comp_size = 0;
        e->crc32 = 0;
    }
    else if (level == 0)
    {
        uint32_t comp = 0;
        e->crc32 = nz_store_stream(in, ar->fp, &comp);
        e->comp_size = comp;
    }
    else
    {
        nz_bits bits;
        nz_bits_init(&bits, ar->fp);
        e->crc32 = nz_deflate_stream(ar, in, &bits, level);
        e->comp_size = bits.total;
    }
    if (in)
        fclose(in);

    data_end = ftell(ar->fp);

    /* patch the local header with real values */
    fseek(ar->fp, local_start, SEEK_SET);
    nz_write_local(ar->fp, e);
    fseek(ar->fp, data_end, SEEK_SET);

    ar->count++;
    return 0;
}

void nz_finalize(nz_archive *ar)
{
    long cd_start, cd_end;
    int i;

    if (!ar)
        return;
    if (!ar->fp)
    {
        free(ar->work_buf);
        free(ar);
        return;
    }

    /* write central directory */
    cd_start = ftell(ar->fp);
    if (cd_start < 0)
    {
        free(ar->work_buf);
        free(ar);
        return;
    }
    for (i = 0; i < ar->count; i++)
        nz_write_cd(ar->fp, &ar->entries[i]);
    cd_end = ftell(ar->fp);
    if (cd_end < 0 || cd_end < cd_start)
    {
        free(ar->work_buf);
        free(ar);
        return;
    }

    /* write EOCD */
    nz_put32(ar->fp, NZ_SIG_EOCD);
    nz_put16(ar->fp, 0);                             /* disk # */
    nz_put16(ar->fp, 0);                             /* disk # with CD */
    nz_put16(ar->fp, (uint16_t)ar->count);           /* entries on disk */
    nz_put16(ar->fp, (uint16_t)ar->count);           /* total entries */
    nz_put32(ar->fp, (uint32_t)(cd_end - cd_start)); /* CD size */
    nz_put32(ar->fp, (uint32_t)cd_start);            /* CD offset */
    nz_put16(ar->fp, 0);                             /* comment len */

    fclose(ar->fp);
    ar->fp = NULL;
    free(ar->work_buf);
    free(ar);
}

#endif /* NZ_IMPLEMENTATION */