/*
 * The decoders of the player behind one API (mp_player.h).
 *
 *  - Modules (MOD, S3M, XM, IT): libxmp-lite, rendered at 44100 Hz mono.
 *    A module whose file is bigger than 3/4 of the largest free block of
 *    the heap (or that fails to load in RAM) is "big": its samples go to
 *    the external flash (load_to_flash(), libxmp changes in src/xmp/src/
 *    gnw.h), copied once and found again on the next plays.
 *  - MP3 (layer III): minimp3, streamed from the file; ID3v1 / ID3v2 tags,
 *    duration from the Xing / Info / VBRI header or the bitrate; resampled
 *    when the audio output cannot play its rate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "xmp.h"
#include "gnw.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_GNW_STATIC_SCRATCH
#include "minimp3.h"

#include "mp_platform.h"
#include "mp_player.h"
#include "mp_midi.h"

#define MODULE_RATE 44100

#ifdef MP_HOST
/* harness tests of the flash path (README) */
static bool getenv_force_flash(void) { return getenv("MP_FORCE_FLASH") != NULL; }
static bool getenv_nearest(void) { return getenv("MP_NEAREST") != NULL; }
#else
#define getenv_force_flash() false
#define getenv_nearest() false
#endif

static mp_track_info_t info;
static int kind;
static int out_rate;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool istarts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

static int strcasecmp_l1(const char *a, const char *b)
{
    for (;; a++, b++) {
        int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (c || !*a)
            return c;
    }
}

static bool is_module_name(const char *name)
{
    const char *e = ext_of(name);
    return ieq(e, "mod") || ieq(e, "s3m") || ieq(e, "xm") || ieq(e, "it") ||
           ieq(e, "nst") || ieq(e, "m15") || istarts(name, "mod.");
}

static bool is_mp3_name(const char *name)
{
    return ieq(ext_of(name), "mp3");
}

static bool is_midi_name(const char *name)
{
    const char *e = ext_of(name);
    return ieq(e, "mid") || ieq(e, "midi") || ieq(e, "kar") || ieq(e, "rmi");
}

bool mp_player_is_music(const char *name)
{
    return is_module_name(name) || is_mp3_name(name) || is_midi_name(name);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void mp_utf8_to_latin1(char *dst, const char *src, int size)
{
    const unsigned char *p = (const unsigned char *)src;
    int o = 0;
    while (*p && o + 1 < size) {
        unsigned c = *p;
        int n = 1;
        if (c >= 0xC0) {
            n = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
            int k = 1;
            while (k < n && (p[k] & 0xC0) == 0x80)
                k++;
            if (k < n) {
                n = 1; /* not UTF-8: as it is */
            } else if (n == 2) {
                c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
            } else {
                unsigned u = n == 3 ? ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F) : 0x10000;
                /* a few look-alikes: quotes, dashes, the euro, the ellipsis */
                c = u == 0x2018 || u == 0x2019 ? '\'' : u == 0x201C || u == 0x201D ? '"' :
                    u == 0x2013 || u == 0x2014 ? '-' : u == 0x2026 ? '.' : '?';
            }
        }
        dst[o++] = (char)c;
        p += n;
    }
    dst[o] = '\0';
}

/* the file name without its extension: the title when there is none */
static void title_from_name(const char *path)
{
    const char *b = base_name(path);
    mp_utf8_to_latin1(info.title, b, (int)sizeof(info.title));
    char *dot = strrchr(info.title, '.');
    if (dot && dot != info.title && !istarts(b, "mod."))
        *dot = '\0';
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || (unsigned char)s[n - 1] < 32))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ')
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static long file_size(FILE *f)
{
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, cur, SEEK_SET);
    return size;
}

/* ------------------------------------------------------------------ */
/* Modules                                                            */
/* ------------------------------------------------------------------ */
static xmp_context xmp;
static struct xmp_frame_info fi;
static int16_t *frame_buf;      /* the rest of the last frame */
static int frame_left;
static bool mod_ended;
static int mod_loops;

/* the flash blob of a big module */
static struct libxmp_gnw_flash gflash;
static mp_progress_cb flash_progress;
static uint32_t flash_done, flash_report;

static int flash_append(void *opaque, const void *buf, unsigned int len)
{
    (void)opaque;
    if (!mp_plat_flash_append(buf, len))
        return 0;
    flash_done += len;
    if (flash_progress && (flash_done - flash_report >= gflash.total / 20 || flash_done == gflash.total)) {
        flash_report = flash_done;
        flash_progress(flash_done, gflash.total, 0);
    }
    return 1;
}

static void module_info_fill(const char *path)
{
    struct xmp_module_info mi;
    xmp_get_module_info(xmp, &mi);
    snprintf(info.title, sizeof(info.title), "%s", mi.mod->name);
    trim(info.title);
    if (!info.title[0])
        title_from_name(path);
    /* the format: the extension (the type string is the tracker's name) */
    char f[8];
    const char *e = istarts(base_name(path), "mod.") ? "mod" : ext_of(path);
    int k = 0;
    for (; e[k] && k < 7; k++)
        f[k] = (char)toupper((unsigned char)e[k]);
    f[k] = '\0';
    if (!strcmp(f, "NST") || !strcmp(f, "M15"))
        snprintf(f, sizeof(f), "MOD");
    snprintf(info.format, sizeof(info.format), "%s, %d ch", f, mi.mod->chn);
    snprintf(info.artist, sizeof(info.artist), "%s", mi.mod->type);
    trim(info.artist);
}

static int module_start(const char *path)
{
    if (xmp_start_player(xmp, MODULE_RATE, XMP_FORMAT_MONO) != 0)
        return MP_ERR_MEMORY;
    /* samples in the flash: read only, the mixer must not write around
     * the loops (libxmp does that for its interpolation) */
    xmp_set_player(xmp, XMP_PLAYER_INTERP,
                   info.in_flash || getenv_nearest() ? XMP_INTERP_NEAREST : XMP_INTERP_LINEAR);
    xmp_get_frame_info(xmp, &fi);
    info.duration_ms = fi.total_time > 0 ? (uint32_t)fi.total_time : 0;
    module_info_fill(path);
    frame_left = 0;
    mod_ended = false;
    mod_loops = 0;
    out_rate = MODULE_RATE;
    return MP_OK;
}

/* Big module: the samples in the flash (see gnw.h) */
static int load_to_flash(const char *path)
{
    /* pass 1: the sizes of the samples */
    xmp_set_player(xmp, XMP_PLAYER_SMPCTL, XMP_SMPCTL_SKIP);
    int r = xmp_load_module(xmp, (char *)path);
    if (r != 0) {
        xmp_set_player(xmp, XMP_PLAYER_SMPCTL, 0);
        mp_plat_log("mp: %s: not a module (%d)\n", path, r);
        return r == -XMP_ERROR_SYSTEM ? MP_ERR_MEMORY : MP_ERR_FORMAT;
    }
    struct xmp_module_info mi;
    xmp_get_module_info(xmp, &mi);
    uint32_t total = 0;
    for (int i = 0; i < mi.mod->smp; i++)
        total += libxmp_gnw_slot_size(mi.mod->xxs[i].len, mi.mod->xxs[i].flg);
    xmp_release_module(xmp);
    xmp_set_player(xmp, XMP_PLAYER_SMPCTL, 0); /* only while no module is loaded */
    if (total == 0)
        total = 4;

    /* the key: the start of the file, its size, its name */
    FILE *f = fopen(path, "rb");
    if (!f)
        return MP_ERR_FILE;
    long size = file_size(f);
    uint8_t *head = mp_malloc(4096);
    if (!head) {
        fclose(f);
        return MP_ERR_MEMORY;
    }
    size_t n = fread(head, 1, 4096, f);
    fclose(f);
    uint32_t crc = crc32_update(0, head, n);
    mp_free(head);
    char key[96];
    snprintf(key, sizeof(key), "mp1|%08lx|%ld|%.60s", (unsigned long)crc, size, base_name(path));

    memset(&gflash, 0, sizeof(gflash));
    gflash.total = total;
    uint32_t have = 0;
    const uint8_t *blob = mp_plat_flash_lookup(key, &have);
    if (blob && have == total) {
        gflash.mode = LIBXMP_GNW_READ;
        gflash.base = blob;
        mp_plat_log("mp: samples in the flash (%lu bytes)\n", (unsigned long)total);
    } else {
        blob = mp_plat_flash_begin(key, total);
        if (!blob)
            return MP_ERR_FLASH;
        gflash.mode = LIBXMP_GNW_WRITE;
        gflash.base = blob;
        gflash.append = flash_append;
        flash_done = flash_report = 0;
        if (flash_progress)
            flash_progress(0, total, 0);
        mp_plat_log("mp: copying %lu bytes of samples to the flash\n", (unsigned long)total);
    }
    libxmp_gnw_flash_lo = gflash.base;
    libxmp_gnw_flash_hi = gflash.base + total;

    /* pass 2: the module, its samples written / found in the flash */
    libxmp_gnw_flash = &gflash;
    r = xmp_load_module(xmp, (char *)path);
    libxmp_gnw_flash = NULL;
    if (gflash.mode == LIBXMP_GNW_WRITE) {
        bool ok = r == 0 && !gflash.error;
        /* the rest of the room reserved (samples shorter than announced) */
        static const uint8_t zero[256];
        while (ok && gflash.offset < total) {
            uint32_t z = total - gflash.offset > sizeof(zero) ? sizeof(zero) : total - gflash.offset;
            ok = flash_append(NULL, zero, z) != 0;
            gflash.offset += z;
        }
        if (ok && flash_done == total && mp_plat_flash_finish() == blob) {
            mp_plat_log("mp: samples written\n");
        } else {
            mp_plat_flash_abort();
            if (r == 0)
                xmp_release_module(xmp);
            libxmp_gnw_flash_lo = libxmp_gnw_flash_hi = NULL;
            mp_plat_log("mp: writing the samples failed (%d, %d)\n", r, gflash.error);
            return r == 0 ? MP_ERR_FLASH : MP_ERR_MEMORY;
        }
    } else if (r != 0 || gflash.error) {
        if (r == 0)
            xmp_release_module(xmp);
        libxmp_gnw_flash_lo = libxmp_gnw_flash_hi = NULL;
        return MP_ERR_MEMORY;
    }
    info.in_flash = true;
    return MP_OK;
}

static int module_open(const char *path)
{
    xmp = xmp_create_context();
    if (!xmp)
        return MP_ERR_MEMORY;
    FILE *f = fopen(path, "rb");
    if (!f)
        return MP_ERR_FILE;
    long size = file_size(f);
    fclose(f);

    int r;
    bool big = getenv_force_flash() || (size_t)size > mp_heap_largest() / 4 * 3;
    if (!big) {
        r = xmp_load_module(xmp, (char *)path);
        if (r != 0) {
            mp_plat_log("mp: %s: load in RAM failed (%d), trying the flash\n", path, r);
            big = true;
        }
    }
    if (big) {
        r = load_to_flash(path);
        if (r != MP_OK)
            return r;
    }
#ifdef MP_HOST
    if (getenv("MP_DEBUG_SAMPLES")) {
        struct xmp_module_info mi;
        xmp_get_module_info(xmp, &mi);
        for (int i = 0; i < mi.mod->smp && i < 6; i++) {
            const struct xmp_sample *x = &mi.mod->xxs[i];
            printf("smp %d len %d flg %x data %p:", i, x->len, x->flg, (void *)x->data);
            if (x->data)
                for (int k = -4; k < 12; k++)
                    printf(" %02x", x->data[k]);
            printf("\n");
        }
    }
#endif
    r = module_start(path);
    if (r != MP_OK) {
        xmp_release_module(xmp);
        return r;
    }
    return MP_OK;
}

static void module_close(void)
{
    if (xmp) {
        xmp_end_player(xmp);
        xmp_release_module(xmp);
        xmp_free_context(xmp);
        xmp = NULL;
    }
    libxmp_gnw_flash_lo = libxmp_gnw_flash_hi = NULL;
}

static int module_render(int16_t *out, int frames, bool loop)
{
    int done = 0;
    while (done < frames) {
        if (frame_left == 0) {
            if (mod_ended)
                break;
            if (xmp_play_frame(xmp) != 0) {
                mod_ended = true;
                break;
            }
            xmp_get_frame_info(xmp, &fi);
            if (fi.loop_count > mod_loops) {
                mod_loops = fi.loop_count;
                if (!loop) {
                    mod_ended = true;
                    break;
                }
            }
            frame_buf = (int16_t *)fi.buffer;
            frame_left = fi.buffer_size / 2;
            if (frame_left == 0)
                continue;
        }
        int n = frames - done < frame_left ? frames - done : frame_left;
        memcpy(out + done, frame_buf, (size_t)n * 2);
        frame_buf += n;
        frame_left -= n;
        done += n;
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* MP3                                                                */
/* ------------------------------------------------------------------ */
#define MP3_BUF 16384

static FILE *mp3f;
static mp3dec_t *mp3d;
static uint8_t *mp3buf;           /* MP3_BUF bytes of the file */
static int mp3buf_len, mp3buf_pos;
static int16_t *pcm;              /* one decoded frame (stereo) */
static int pcm_len, pcm_pos;      /* frames */
static long data_start, data_end; /* the MPEG frames in the file */
static long file_pos;             /* of the end of mp3buf */
static int src_rate, src_channels;
static uint64_t src_samples;      /* decoded (source rate) */
static uint32_t total_frames;     /* Xing / VBRI: frames, 0 unknown */
static uint8_t toc[100];
static bool have_toc;
static bool is_vbr;
static int bitrate_kbps;
static bool mp3_eof;
/* resampling (16.16) when the output rate differs */
static uint32_t rs_step, rs_frac;
static int16_t rs_prev, rs_cur;
static bool rs_primed;

static bool mp3_fill(void)
{
    if (mp3buf_pos > 0) {
        memmove(mp3buf, mp3buf + mp3buf_pos, (size_t)(mp3buf_len - mp3buf_pos));
        mp3buf_len -= mp3buf_pos;
        mp3buf_pos = 0;
    }
    long want = MP3_BUF - mp3buf_len;
    if (file_pos + want > data_end)
        want = data_end - file_pos;
    if (want <= 0)
        return false;
    size_t n = fread(mp3buf + mp3buf_len, 1, (size_t)want, mp3f);
    mp3buf_len += (int)n;
    file_pos += (long)n;
    return n > 0;
}

static void mp3_seek_file(long pos)
{
    if (pos < data_start)
        pos = data_start;
    if (pos > data_end)
        pos = data_end;
    fseek(mp3f, pos, SEEK_SET);
    file_pos = pos;
    mp3buf_len = mp3buf_pos = 0;
    pcm_len = pcm_pos = 0;
    mp3dec_init(mp3d);
    mp3_eof = false;
    rs_primed = false;
}

/* Latin-1 from the encodings of ID3v2 text frames */
static void id3_text(char *dst, size_t dstlen, const uint8_t *p, int len)
{
    if (len < 1 || dstlen == 0)
        return;
    int enc = p[0];
    p++;
    len--;
    size_t o = 0;
    if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        int i = 0;
        if (enc == 1 && len >= 2) {
            if (p[0] == 0xFE && p[1] == 0xFF)
                be = true;
            else if (p[0] == 0xFF && p[1] == 0xFE)
                be = false;
            i = 2;
        }
        for (; i + 1 < len && o + 1 < dstlen; i += 2) {
            unsigned c = be ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8);
            if (c == 0)
                break;
            if (c >= 0xD800 && c < 0xDC00) { /* a surrogate pair: not Latin-1 */
                i += 2;
                c = '?';
            }
            dst[o++] = (char)(c < 256 ? c : '?');
        }
    } else if (enc == 3) {
        for (int i = 0; i < len && o + 1 < dstlen && p[i]; ) {
            unsigned c = p[i];
            int n = 1;
            if (c >= 0xF0) { c = '?'; n = 4; }
            else if (c >= 0xE0) { c = '?'; n = 3; }
            else if (c >= 0xC0 && i + 1 < len) { c = ((c & 0x1F) << 6) | (p[i + 1] & 0x3F); n = 2; }
            dst[o++] = (char)(c < 256 ? c : '?');
            i += n;
        }
    } else {
        for (int i = 0; i < len && o + 1 < dstlen && p[i]; i++)
            dst[o++] = (char)p[i];
    }
    dst[o] = '\0';
    trim(dst);
}

static uint32_t syncsafe(const uint8_t *p)
{
    return (uint32_t)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

/* ID3v2 at the start of the file: the title and the artist; returns the
 * size of the tag (0: none) */
static long id3v2_read(void)
{
    uint8_t h[10];
    fseek(mp3f, 0, SEEK_SET);
    if (fread(h, 1, 10, mp3f) != 10 || memcmp(h, "ID3", 3) != 0)
        return 0;
    int ver = h[3];
    long size = (long)syncsafe(h + 6) + 10;
    if (h[5] & 0x10)
        size += 10; /* footer */
    long end = size - ((h[5] & 0x10) ? 10 : 0);
    long pos = 10;
    if (ver >= 3 && (h[5] & 0x40)) { /* extended header */
        uint8_t e[4];
        if (fread(e, 1, 4, mp3f) != 4)
            return size;
        uint32_t es = ver == 4 ? syncsafe(e) : (uint32_t)(e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]);
        pos += ver == 4 ? (long)es : (long)es + 4;
    }
    while (pos + 10 <= end && (!info.title[0] || !info.artist[0])) {
        uint8_t fh[10];
        fseek(mp3f, pos, SEEK_SET);
        int hl = ver == 2 ? 6 : 10;
        if (fread(fh, 1, (size_t)hl, mp3f) != (size_t)hl || fh[0] == 0)
            break;
        uint32_t fs;
        char id[5] = { 0 };
        if (ver == 2) {
            memcpy(id, fh, 3);
            fs = (uint32_t)(fh[3] << 16 | fh[4] << 8 | fh[5]);
        } else {
            memcpy(id, fh, 4);
            fs = ver == 4 ? syncsafe(fh + 4) : (uint32_t)(fh[4] << 24 | fh[5] << 16 | fh[6] << 8 | fh[7]);
        }
        if (fs == 0 || pos + hl + (long)fs > end)
            break;
        char *dst = NULL;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
            dst = info.title;
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1"))
            dst = info.artist;
        if (dst && fs < 512) {
            uint8_t buf[512];
            if (fread(buf, 1, fs, mp3f) == fs)
                id3_text(dst, sizeof(info.title), buf, (int)fs);
        }
        pos += hl + (long)fs;
    }
    return size;
}

/* ID3v1 at the end of the file: returns its size (0: none) */
static long id3v1_read(long size)
{
    if (size < 128)
        return 0;
    uint8_t t[128];
    fseek(mp3f, size - 128, SEEK_SET);
    if (fread(t, 1, 128, mp3f) != 128 || memcmp(t, "TAG", 3) != 0)
        return 0;
    if (!info.title[0]) {
        memcpy(info.title, t + 3, 30);
        info.title[30] = '\0';
        trim(info.title);
    }
    if (!info.artist[0]) {
        memcpy(info.artist, t + 33, 30);
        info.artist[30] = '\0';
        trim(info.artist);
    }
    return 128;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

/* The Xing / Info / VBRI header in the first frame: frames, TOC. Returns
 * true if the frame is that header (no audio: skipped). */
static bool vbr_header(const uint8_t *fr, int len, int channels, bool mpeg1)
{
    int off = mpeg1 ? (channels == 2 ? 36 : 21) : (channels == 2 ? 21 : 13);
    if (off + 8 <= len && (!memcmp(fr + off, "Xing", 4) || !memcmp(fr + off, "Info", 4))) {
        is_vbr = fr[off] == 'X';  /* "Info": a CBR file */
        uint32_t flags = be32(fr + off + 4);
        int p = off + 8;
        if ((flags & 1) && p + 4 <= len) {
            total_frames = be32(fr + p);
            p += 4;
        }
        if (flags & 2)
            p += 4;
        if ((flags & 4) && p + 100 <= len) {
            memcpy(toc, fr + p, 100);
            have_toc = true;
        }
        return true;
    }
    if (36 + 18 <= len && !memcmp(fr + 36, "VBRI", 4)) {
        is_vbr = true;
        total_frames = be32(fr + 36 + 14);
        return true;
    }
    return false;
}

static int mp3_open(const char *path)
{
    mp3f = fopen(path, "rb");
    if (!mp3f)
        return MP_ERR_FILE;
    mp3d = mp_malloc(sizeof(mp3dec_t));
    mp3buf = mp_malloc(MP3_BUF);
    pcm = mp_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!mp3d || !mp3buf || !pcm)
        return MP_ERR_MEMORY;
    long size = file_size(mp3f);
    data_start = id3v2_read();
    data_end = size - id3v1_read(size);
    if (data_end <= data_start)
        return MP_ERR_FORMAT;
    total_frames = 0;
    have_toc = false;
    is_vbr = false;
    mp3_seek_file(data_start);

    /* the first frame: rate, channels, a VBR header */
    mp3dec_frame_info_t fi3;
    int tries = 0;
    for (;;) {
        mp3_fill();
        if (mp3buf_len - mp3buf_pos < 4)
            return MP_ERR_FORMAT;
        int samples = mp3dec_decode_frame(mp3d, mp3buf + mp3buf_pos, mp3buf_len - mp3buf_pos, NULL, &fi3);
        if (samples > 0 && fi3.layer == 3)
            break;
        if (fi3.frame_bytes == 0 || ++tries > 64)
            return MP_ERR_FORMAT;
        mp3buf_pos += fi3.frame_bytes; /* garbage or another layer */
    }
    const uint8_t *fr = mp3buf + mp3buf_pos + fi3.frame_offset;
    int frlen = fi3.frame_bytes - fi3.frame_offset;
    bool mpeg1 = (fr[1] & 0x18) == 0x18;
    long first = file_pos - (mp3buf_len - mp3buf_pos) + fi3.frame_offset;
    if (vbr_header(fr, frlen, fi3.channels, mpeg1))
        data_start = first + frlen; /* the audio starts after it */
    else
        data_start = first;
    src_rate = fi3.hz;
    src_channels = fi3.channels;
    bitrate_kbps = fi3.bitrate_kbps;
    int spf = mpeg1 ? 1152 : 576;
    if (total_frames)
        info.duration_ms = (uint32_t)((uint64_t)total_frames * spf * 1000 / (uint32_t)src_rate);
    else if (bitrate_kbps)
        info.duration_ms = (uint32_t)((uint64_t)(data_end - data_start) * 8 / (uint32_t)bitrate_kbps);
    if (total_frames && info.duration_ms)
        bitrate_kbps = (int)((uint64_t)(data_end - data_start) * 8 / info.duration_ms);
    mp3_seek_file(data_start);
    src_samples = 0;

    out_rate = mp_plat_rate_supported(src_rate) ? src_rate : 48000;
    rs_step = (uint32_t)(((uint64_t)src_rate << 16) / (uint32_t)out_rate);
    if (!info.title[0])
        title_from_name(path);
    snprintf(info.format, sizeof(info.format), "MP3 %d kbps %d.%d kHz%s", bitrate_kbps,
             src_rate / 1000, (src_rate % 1000) / 100, is_vbr ? " VBR" : "");
    return MP_OK;
}

static void mp3_close(void)
{
    if (mp3f)
        fclose(mp3f);
    mp3f = NULL;
    mp_free(mp3d);
    mp_free(mp3buf);
    mp_free(pcm);
    mp3d = NULL;
    mp3buf = NULL;
    pcm = NULL;
}

/* the next source sample (mono), false at the end */
static bool mp3_next(int16_t *s)
{
    while (pcm_pos >= pcm_len) {
        if (mp3_eof)
            return false;
        if (mp3buf_len - mp3buf_pos < 4096)
            mp3_fill();
        if (mp3buf_len - mp3buf_pos < 4) {
            mp3_eof = true;
            return false;
        }
        mp3dec_frame_info_t fi3;
        int n = mp3dec_decode_frame(mp3d, mp3buf + mp3buf_pos, mp3buf_len - mp3buf_pos, pcm, &fi3);
        if (fi3.frame_bytes == 0) {
            /* no frame in what is left */
            if (!mp3_fill()) {
                mp3_eof = true;
                return false;
            }
            continue;
        }
        mp3buf_pos += fi3.frame_bytes;
        if (n <= 0)
            continue;
        if (fi3.channels == 2) {
            for (int i = 0; i < n; i++)
                pcm[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) >> 1);
        }
        pcm_len = n;
        pcm_pos = 0;
    }
    *s = pcm[pcm_pos++];
    src_samples++;
    return true;
}

static int mp3_render(int16_t *out, int frames)
{
    int done = 0;
    if (out_rate == src_rate) {
        while (done < frames) {
            if (pcm_pos >= pcm_len) {
                int16_t s;
                if (!mp3_next(&s))
                    break;
                out[done++] = s;
                continue;
            }
            int n = pcm_len - pcm_pos;
            if (n > frames - done)
                n = frames - done;
            memcpy(out + done, pcm + pcm_pos, (size_t)n * 2);
            pcm_pos += n;
            src_samples += (uint64_t)n;
            done += n;
        }
        return done;
    }
    if (!rs_primed) {
        if (!mp3_next(&rs_prev) || !mp3_next(&rs_cur))
            return 0;
        rs_frac = 0;
        rs_primed = true;
    }
    while (done < frames) {
        out[done++] = (int16_t)(rs_prev + (((int32_t)(rs_cur - rs_prev) * (int32_t)rs_frac) >> 16));
        rs_frac += rs_step;
        while (rs_frac >= 0x10000) {
            rs_frac -= 0x10000;
            rs_prev = rs_cur;
            if (!mp3_next(&rs_cur))
                return done;
        }
    }
    return done;
}

static uint32_t mp3_position_ms(void)
{
    return src_rate ? (uint32_t)(src_samples * 1000 / (uint32_t)src_rate) : 0;
}

static void mp3_seek_ms(uint32_t ms)
{
    if (info.duration_ms && ms >= info.duration_ms)
        ms = info.duration_ms > 1000 ? info.duration_ms - 1000 : 0;
    long span = data_end - data_start;
    long off;
    if (!info.duration_ms) {
        off = bitrate_kbps ? (long)((uint64_t)ms * (uint32_t)bitrate_kbps / 8) : 0;
    } else if (have_toc) {
        float pct = (float)ms * 100.0f / (float)info.duration_ms;
        int a = (int)pct;
        if (a > 99)
            a = 99;
        float fa = toc[a];
        float fb = a < 99 ? toc[a + 1] : 256.0f;
        float x = fa + (fb - fa) * (pct - (float)a);
        off = (long)((double)x / 256.0 * (double)span);
    } else {
        off = (long)((uint64_t)span * ms / info.duration_ms);
    }
    mp3_seek_file(data_start + off);
    src_samples = (uint64_t)ms * (uint32_t)src_rate / 1000;
}

/* ------------------------------------------------------------------ */
/* MIDI                                                               */
/* ------------------------------------------------------------------ */
/* The SoundFont of a MIDI file: <name>.sf2 next to it, else the first
 * .sf2 of its folder, else the first .sf2 of the music folder. */
typedef struct {
    char best[96];
} sf2_find_t;

static void sf2_cb(void *arg, const char *name, bool is_dir, uint32_t size)
{
    sf2_find_t *f = arg;
    (void)size;
    if (is_dir || !ieq(ext_of(name), "sf2") || name[0] == '.')
        return;
    if (!f->best[0] || strcasecmp_l1(name, f->best) < 0)
        snprintf(f->best, sizeof(f->best), "%s", name);
}

static bool find_sf2(const char *midi_path, char *out, size_t size)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", midi_path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return false;
    *slash = '\0';
    /* <name>.sf2 */
    snprintf(out, size, "%s", midi_path);
    char *dot = strrchr(out, '.');
    if (dot && dot > strrchr(out, '/')) {
        snprintf(dot, size - (size_t)(dot - out), ".sf2");
        FILE *f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return true;
        }
    }
    sf2_find_t fd;
    fd.best[0] = '\0';
    mp_plat_list_dir(dir, sf2_cb, &fd);
    if (fd.best[0]) {
        snprintf(out, size, "%s/%s", dir, fd.best);
        return true;
    }
    fd.best[0] = '\0';
    mp_plat_list_dir(mp_plat_music_root(), sf2_cb, &fd);
    if (fd.best[0]) {
        snprintf(out, size, "%s/%s", mp_plat_music_root(), fd.best);
        return true;
    }
    return false;
}

/* the whole SoundFont in the flash (copied once): its 16 bit samples are
 * played from there */
static const uint8_t *map_sf2(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    long size = file_size(f);
    uint8_t *buf = mp_malloc(4096);
    if (!buf || size < 12) {
        mp_free(buf);
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, 4096, f);
    char key[96];
    snprintf(key, sizeof(key), "sf2|%08lx|%ld|%.60s", (unsigned long)crc32_update(0, buf, n), size,
             base_name(path));
    uint32_t have = 0;
    const uint8_t *blob = mp_plat_flash_lookup(key, &have);
    if (blob && have == (uint32_t)size) {
        mp_free(buf);
        fclose(f);
        *size_out = have;
        return blob;
    }
    mp_plat_log("mp: copying the SoundFont %s to the flash (%ld bytes)\n", path, size);
    blob = mp_plat_flash_begin(key, (uint32_t)size);
    bool ok = blob != NULL;
    uint32_t done = 0, report = 0;
    fseek(f, 0, SEEK_SET);
    if (flash_progress && ok)
        flash_progress(0, (uint32_t)size, 1);
    while (ok && done < (uint32_t)size) {
        n = fread(buf, 1, 4096, f);
        if (n == 0) {
            ok = false;
            break;
        }
        ok = mp_plat_flash_append(buf, (uint32_t)n);
        done += (uint32_t)n;
        if (flash_progress && (done - report >= (uint32_t)size / 20 || done == (uint32_t)size)) {
            report = done;
            flash_progress(done, (uint32_t)size, 1);
        }
    }
    mp_free(buf);
    fclose(f);
    if (!ok) {
        if (blob)
            mp_plat_flash_abort();
        return NULL;
    }
    blob = mp_plat_flash_finish();
    *size_out = (uint32_t)size;
    return blob;
}

static int midi_open(const char *path)
{
    char sf2[256];
    if (!find_sf2(path, sf2, sizeof(sf2)))
        return MP_ERR_NO_SF2;
    FILE *f = fopen(path, "rb");
    if (!f)
        return MP_ERR_FILE;
    long size = file_size(f);
    if (size < 22 || (size_t)size > mp_heap_largest() / 2) {
        fclose(f);
        return size < 22 ? MP_ERR_FORMAT : MP_ERR_MEMORY;
    }
    uint8_t *data = mp_malloc((size_t)size);
    if (!data) {
        fclose(f);
        return MP_ERR_MEMORY;
    }
    size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    /* RIFF MIDI (.rmi): the SMF is in its "data" chunk */
    uint32_t off = 0;
    if (n > 20 && !memcmp(data, "RIFF", 4)) {
        for (uint32_t i = 12; i + 8 < n && i < 256; i++)
            if (!memcmp(data + i, "MThd", 4)) {
                off = i;
                break;
            }
        memmove(data, data + off, n - off);
        n -= off;
    }
    uint32_t sf2_size = 0;
    const uint8_t *sf = map_sf2(sf2, &sf2_size);
    if (!sf) {
        mp_free(data);
        return MP_ERR_FLASH;
    }
    int r = mp_midi_open(data, (uint32_t)n, sf, sf2_size); /* keeps data */
    if (r != MP_OK)
        return r;
    snprintf(info.title, sizeof(info.title), "%s", mp_midi_title());
    trim(info.title);
    if (!info.title[0])
        title_from_name(path);
    snprintf(info.artist, sizeof(info.artist), "%s", mp_midi_copyright());
    trim(info.artist);
    const char *sb = base_name(sf2);
    snprintf(info.format, sizeof(info.format), "MIDI, %d tr, %.20s", mp_midi_tracks(), sb);
    info.duration_ms = mp_midi_duration_ms();
    out_rate = MP_MIDI_RATE;
    return MP_OK;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */
int mp_player_open(const char *path, mp_progress_cb progress)
{
    mp_player_close();
    memset(&info, 0, sizeof(info));
    flash_progress = progress;
    int r;
    if (is_mp3_name(path)) {
        kind = MP_KIND_MP3;
        r = mp3_open(path);
    } else if (is_midi_name(path)) {
        kind = MP_KIND_MIDI;
        r = midi_open(path);
    } else if (is_module_name(path)) {
        kind = MP_KIND_MODULE;
        r = module_open(path);
    } else {
        r = MP_ERR_FORMAT;
    }
    info.kind = kind;
    if (r != MP_OK) {
        mp_player_close();
        kind = MP_KIND_NONE;
    }
    return r;
}

void mp_player_close(void)
{
    if (kind == MP_KIND_MODULE)
        module_close();
    else if (kind == MP_KIND_MP3)
        mp3_close();
    else if (kind == MP_KIND_MIDI)
        mp_midi_close();
    kind = MP_KIND_NONE;
    info.kind = MP_KIND_NONE;
}

bool mp_player_is_open(void)
{
    return kind != MP_KIND_NONE;
}

const mp_track_info_t *mp_player_info(void)
{
    return &info;
}

int mp_player_rate(void)
{
    return kind == MP_KIND_NONE ? 0 : out_rate;
}

int mp_player_render(int16_t *out, int frames, bool loop)
{
    if (kind == MP_KIND_MODULE)
        return module_render(out, frames, loop);
    if (kind == MP_KIND_MP3) {
        int n = mp3_render(out, frames);
        if (n < frames && loop) {
            mp3_seek_file(data_start);
            src_samples = 0;
            n += mp3_render(out + n, frames - n);
        }
        return n;
    }
    if (kind == MP_KIND_MIDI)
        return mp_midi_render(out, frames, loop);
    return 0;
}

uint32_t mp_player_position_ms(void)
{
    if (kind == MP_KIND_MODULE)
        return fi.time > 0 ? (uint32_t)fi.time : 0;
    if (kind == MP_KIND_MP3)
        return mp3_position_ms();
    if (kind == MP_KIND_MIDI)
        return mp_midi_position_ms();
    return 0;
}

void mp_player_seek(int direction)
{
    if (kind == MP_KIND_MODULE) {
        if (direction > 0)
            xmp_next_position(xmp);
        else
            xmp_prev_position(xmp);
        frame_left = 0;
        mod_ended = false;
    } else if (kind == MP_KIND_MP3) {
        uint32_t now = mp3_position_ms();
        uint32_t to = direction > 0 ? now + 10000 : (now > 10000 ? now - 10000 : 0);
        mp3_seek_ms(to);
    } else if (kind == MP_KIND_MIDI) {
        uint32_t now = mp_midi_position_ms();
        mp_midi_seek_ms(direction > 0 ? now + 10000 : (now > 10000 ? now - 10000 : 0));
    }
}

uint32_t mp_player_locator(void)
{
    if (kind == MP_KIND_MODULE)
        return (uint32_t)fi.pos;
    if (kind == MP_KIND_MP3)
        return mp3_position_ms();
    if (kind == MP_KIND_MIDI)
        return mp_midi_position_ms();
    return 0;
}

void mp_player_relocate(uint32_t locator)
{
    if (kind == MP_KIND_MODULE) {
        if (locator > 0) {
            xmp_set_position(xmp, (int)locator);
            frame_left = 0;
        }
    } else if (kind == MP_KIND_MP3) {
        if (locator > 0)
            mp3_seek_ms(locator);
    } else if (kind == MP_KIND_MIDI) {
        if (locator > 0)
            mp_midi_seek_ms(locator);
    }
}

bool mp_player_module_status(mp_module_status_t *st)
{
    if (kind == MP_KIND_MIDI) {
        memset(st, 0, sizeof(*st));
        st->midi = true;
        st->nch = 16;
        st->voices = mp_midi_voices();
        st->bpm = mp_midi_bpm();
        mp_midi_levels(st->vu);
        return true;
    }
    if (kind != MP_KIND_MODULE)
        return false;
    st->midi = false;
    struct xmp_module_info mi;
    xmp_get_module_info(xmp, &mi);
    st->pos = fi.pos;
    st->npos = mi.mod->len;
    st->row = fi.row;
    st->nrows = fi.num_rows;
    st->speed = fi.speed;
    st->bpm = fi.bpm;
    st->nch = mi.mod->chn < 32 ? mi.mod->chn : 32;
    for (int i = 0; i < st->nch; i++) {
        const struct xmp_channel_info *c = &fi.channel_info[i];
        st->vu[i] = c->period ? (c->volume > 64 ? 64 : c->volume) : 0;
    }
    return true;
}
