/*
 * WAV, FLAC and Ogg Vorbis files (mp_pcm.h), streamed from the SD card:
 * dr_wav, dr_flac and stb_vorbis decode them, the channels are mixed to
 * mono, then the samples go to the output rate:
 *  - the file's rate when the audio output can play it,
 *  - above 48 kHz: halved (averages of 2, 4... samples) down to a rate
 *    it can play (96 -> 48 kHz, 88.2 -> 44.1 kHz, 192 -> 48 kHz),
 *  - otherwise resampled to 48 kHz (linear interpolation).
 * This file is compiled with mp_glue.h: the codecs use the player's heap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mp_platform.h"
#include "mp_pcm.h"
#include "mp_player.h"

static void *alloc_cb(size_t sz, void *u) { (void)u; return mp_malloc(sz); }
static void *realloc_cb(void *p, size_t sz, void *u) { (void)u; return mp_realloc(p, sz); }
static void free_cb(void *p, void *u) { (void)u; mp_free(p); }

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include "dr_flac.h"

#define STB_VORBIS_GNW
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION_TEST
#include "stb_vorbis.c"

enum { PCM_NONE, PCM_WAV, PCM_FLAC, PCM_OGG };

#define BLOCK 1024 /* frames decoded at a time */

static int type;
static FILE *pf;
static drwav *wav;
static drflac *flac;
static stb_vorbis *ogg;
static int channels, src_rate, out_rate, decim;
static uint64_t total_frames;   /* 0: unknown */
static uint64_t pos_frames;     /* decoded, source rate */
static int16_t *dec;            /* BLOCK * channels */
static int16_t *mono;           /* BLOCK */
static int mono_len, mono_pos;
static bool eof;
static char tag_title[72], tag_artist[72];
/* linear resampling (16.16) */
static uint32_t rs_step, rs_frac;
static int16_t rs_prev, rs_cur;
static bool rs_primed;

/* ------------------------------------------------------------------ */
/* File callbacks                                                     */
/* ------------------------------------------------------------------ */
static size_t on_read(void *u, void *buf, size_t n)
{
    (void)u;
    return fread(buf, 1, n, pf);
}

static int whence_of(int origin)
{
    return origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET;
}

static drwav_bool32 wav_seek(void *u, int off, drwav_seek_origin o)
{
    (void)u;
    return fseek(pf, off, whence_of((int)o)) == 0;
}

static drwav_bool32 wav_tell(void *u, drwav_int64 *cur)
{
    (void)u;
    *cur = ftell(pf);
    return *cur >= 0;
}

static drflac_bool32 flac_seek(void *u, int off, drflac_seek_origin o)
{
    (void)u;
    return fseek(pf, off, whence_of((int)o)) == 0;
}

static drflac_bool32 flac_tell(void *u, drflac_int64 *cur)
{
    (void)u;
    *cur = ftell(pf);
    return *cur >= 0;
}

/* ------------------------------------------------------------------ */
/* Tags                                                               */
/* ------------------------------------------------------------------ */
/* UTF-8 to Latin-1 into dst, from n bytes */
static void tag_copy(char *dst, size_t size, const char *s, size_t n)
{
    char tmp[160];
    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    mp_utf8_to_latin1(dst, tmp, (int)size);
}

/* "TITLE=..." / "ARTIST=..." (Vorbis comments, FLAC and Ogg) */
static void vorbis_comment(const char *c, size_t n)
{
    if (n > 6 && !strncasecmp(c, "TITLE=", 6) && !tag_title[0])
        tag_copy(tag_title, sizeof(tag_title), c + 6, n - 6);
    else if (n > 7 && !strncasecmp(c, "ARTIST=", 7) && !tag_artist[0])
        tag_copy(tag_artist, sizeof(tag_artist), c + 7, n - 7);
}

static void flac_meta(void *u, drflac_metadata *m)
{
    (void)u;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT)
        return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &len)) != NULL)
        vorbis_comment(c, len);
}

/* ------------------------------------------------------------------ */
/* Decoding                                                           */
/* ------------------------------------------------------------------ */
/* the next BLOCK (or fewer) frames, mixed to mono, in mono[] */
static bool decode_block(void)
{
    if (eof)
        return false;
    int n = 0;
    if (type == PCM_WAV) {
        n = (int)drwav_read_pcm_frames_s16(wav, BLOCK, dec);
    } else if (type == PCM_FLAC) {
        n = (int)drflac_read_pcm_frames_s16(flac, BLOCK, dec);
    } else if (type == PCM_OGG) {
        /* one buffer per channel (stb_vorbis would add the channels
         * together when it mixes them, not average them) */
        short *ch[8];
        for (int c = 0; c < channels; c++)
            ch[c] = dec + c * BLOCK;
        n = stb_vorbis_get_samples_short(ogg, channels, ch, BLOCK);
    }
    if (n <= 0) {
        eof = true;
        return false;
    }
    if (type == PCM_OGG) {
        for (int i = 0; i < n; i++) {
            int32_t acc = 0;
            for (int c = 0; c < channels; c++)
                acc += dec[c * BLOCK + i];
            mono[i] = (int16_t)(acc / channels);
        }
    } else {
        if (channels == 1) {
            memcpy(mono, dec, (size_t)n * 2);
        } else if (channels == 2) {
            for (int i = 0; i < n; i++)
                mono[i] = (int16_t)(((int32_t)dec[2 * i] + dec[2 * i + 1]) >> 1);
        } else {
            for (int i = 0; i < n; i++) {
                int32_t s = 0;
                for (int c = 0; c < channels; c++)
                    s += dec[i * channels + c];
                mono[i] = (int16_t)(s / channels);
            }
        }
    }
    pos_frames += (uint64_t)n;
    mono_len = n;
    mono_pos = 0;
    return true;
}

/* the next sample at the source rate / decim */
static bool next_sample(int16_t *s)
{
    int32_t acc = 0;
    for (int k = 0; k < decim; k++) {
        if (mono_pos >= mono_len && !decode_block())
            return false;
        acc += mono[mono_pos++];
    }
    *s = (int16_t)(acc / decim);
    return true;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */
int mp_pcm_kind(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return PCM_NONE;
    dot++;
    if (!strcasecmp(dot, "wav") || !strcasecmp(dot, "wave"))
        return PCM_WAV;
    if (!strcasecmp(dot, "flac"))
        return PCM_FLAC;
    if (!strcasecmp(dot, "ogg") || !strcasecmp(dot, "oga"))
        return PCM_OGG;
    return PCM_NONE;
}

bool mp_pcm_is_music(const char *name)
{
    return mp_pcm_kind(name) != PCM_NONE;
}

static const drwav_allocation_callbacks wav_alloc = { NULL, alloc_cb, realloc_cb, free_cb };
static const drflac_allocation_callbacks flac_alloc = { NULL, alloc_cb, realloc_cb, free_cb };

int mp_pcm_open(const char *path, mp_pcm_info_t *out)
{
    mp_pcm_close();
    tag_title[0] = tag_artist[0] = '\0';
    type = mp_pcm_kind(path);
    pf = fopen(path, "rb");
    if (!pf) {
        type = PCM_NONE;
        return -1;
    }
    const char *name = "WAV";
    int bits = 16;
    if (type == PCM_WAV) {
        wav = mp_malloc(sizeof(drwav));
        if (!wav)
            goto nomem;
        if (!drwav_init_with_metadata(wav, on_read, wav_seek, wav_tell, NULL, 0, &wav_alloc)) {
            mp_free(wav);
            wav = NULL;
            goto bad;
        }
        channels = wav->channels;
        src_rate = (int)wav->sampleRate;
        total_frames = wav->totalPCMFrameCount;
        bits = wav->bitsPerSample;
        for (drwav_uint32 i = 0; i < wav->metadataCount; i++) {
            const drwav_metadata *m = &wav->pMetadata[i];
            if (m->type == drwav_metadata_type_list_info_title && !tag_title[0])
                tag_copy(tag_title, sizeof(tag_title), m->data.infoText.pString, m->data.infoText.stringLength);
            else if (m->type == drwav_metadata_type_list_info_artist && !tag_artist[0])
                tag_copy(tag_artist, sizeof(tag_artist), m->data.infoText.pString, m->data.infoText.stringLength);
        }
        uint16_t t = wav->translatedFormatTag;
        name = t == DR_WAVE_FORMAT_ADPCM || t == DR_WAVE_FORMAT_DVI_ADPCM ? "WAV ADPCM" :
               t == DR_WAVE_FORMAT_IEEE_FLOAT ? "WAV float" :
               t == DR_WAVE_FORMAT_ALAW || t == DR_WAVE_FORMAT_MULAW ? "WAV G.711" : "WAV";
    } else if (type == PCM_FLAC) {
        flac = drflac_open_with_metadata(on_read, flac_seek, flac_tell, flac_meta, NULL, &flac_alloc);
        if (!flac)
            goto bad;
        channels = flac->channels;
        src_rate = (int)flac->sampleRate;
        total_frames = flac->totalPCMFrameCount;
        bits = flac->bitsPerSample;
        name = "FLAC";
    } else if (type == PCM_OGG) {
        int err = 0;
        ogg = stb_vorbis_open_file(pf, 0, &err, NULL);
        if (!ogg) {
            mp_plat_log("mp: ogg error %d\n", err);
            goto bad;
        }
        stb_vorbis_info vi = stb_vorbis_get_info(ogg);
        channels = vi.channels;
        src_rate = (int)vi.sample_rate;
        total_frames = stb_vorbis_stream_length_in_samples(ogg);
        stb_vorbis_comment vc = stb_vorbis_get_comment(ogg);
        for (int i = 0; i < vc.comment_list_length; i++)
            vorbis_comment(vc.comment_list[i], strlen(vc.comment_list[i]));
        name = "Ogg Vorbis";
        bits = 0;
    } else {
        goto bad;
    }
    if (channels < 1 || channels > 8 || src_rate < 1000)
        goto bad;
    dec = mp_malloc((size_t)BLOCK * (size_t)channels * 2);
    mono = mp_malloc(BLOCK * 2);
    if (!dec || !mono)
        goto nomem;

    /* the output rate */
    decim = 1;
    while (src_rate / decim > 48000 && (src_rate / decim) % 2 == 0)
        decim *= 2;
    int r = src_rate / decim;
    out_rate = mp_plat_rate_supported(r) ? r : 48000;
    rs_step = (uint32_t)(((uint64_t)r << 16) / (uint32_t)out_rate);
    rs_primed = false;
    pos_frames = 0;
    mono_len = mono_pos = 0;
    eof = false;

    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "%s", tag_title);
    snprintf(out->artist, sizeof(out->artist), "%s", tag_artist);
    int khz10 = (src_rate + 50) / 100;
    if (bits)
        snprintf(out->format, sizeof(out->format), "%s %d bit %d.%d kHz%s", name, bits, khz10 / 10, khz10 % 10,
                 channels == 1 ? " mono" : "");
    else
        snprintf(out->format, sizeof(out->format), "%s %d.%d kHz%s", name, khz10 / 10, khz10 % 10,
                 channels == 1 ? " mono" : "");
    out->duration_ms = total_frames ? (uint32_t)(total_frames * 1000 / (uint32_t)src_rate) : 0;
    return 0;

nomem:
    mp_pcm_close();
    return -3;
bad:
    mp_pcm_close();
    return -2;
}

void mp_pcm_close(void)
{
    if (wav) {
        drwav_uninit(wav);
        mp_free(wav);
        wav = NULL;
    }
    if (flac) {
        drflac_close(flac);
        flac = NULL;
    }
    if (ogg) {
        stb_vorbis_close(ogg);
        ogg = NULL;
    }
    if (pf)
        fclose(pf);
    pf = NULL;
    mp_free(dec);
    mp_free(mono);
    dec = mono = NULL;
    type = PCM_NONE;
}

int mp_pcm_rate(void)
{
    return out_rate;
}

int mp_pcm_render(int16_t *out, int frames)
{
    int done = 0;
    if (out_rate * decim == src_rate) {
        if (decim == 1) {
            while (done < frames) {
                if (mono_pos >= mono_len && !decode_block())
                    break;
                int n = mono_len - mono_pos;
                if (n > frames - done)
                    n = frames - done;
                memcpy(out + done, mono + mono_pos, (size_t)n * 2);
                mono_pos += n;
                done += n;
            }
            return done;
        }
        while (done < frames && next_sample(&out[done]))
            done++;
        return done;
    }
    if (!rs_primed) {
        if (!next_sample(&rs_prev) || !next_sample(&rs_cur))
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
            if (!next_sample(&rs_cur))
                return done;
        }
    }
    return done;
}

uint32_t mp_pcm_position_ms(void)
{
    /* what was played: decoded minus what is left of the block */
    uint64_t p = pos_frames - (uint64_t)(mono_len - mono_pos);
    return src_rate ? (uint32_t)(p * 1000 / (uint32_t)src_rate) : 0;
}

void mp_pcm_seek_ms(uint32_t ms)
{
    uint64_t f = (uint64_t)ms * (uint32_t)src_rate / 1000;
    if (total_frames && f >= total_frames)
        f = total_frames > (uint64_t)src_rate ? total_frames - (uint64_t)src_rate : 0;
    bool ok = false;
    if (type == PCM_WAV)
        ok = drwav_seek_to_pcm_frame(wav, f);
    else if (type == PCM_FLAC)
        ok = drflac_seek_to_pcm_frame(flac, f);
    else if (type == PCM_OGG)
        ok = stb_vorbis_seek(ogg, (unsigned int)f) != 0;
    if (!ok)
        return;
    pos_frames = f;
    mono_len = mono_pos = 0;
    eof = false;
    rs_primed = false;
}
