/*
 * Headless Linux harness of the Music Player: the same player (core,
 * decoders, heap) with a virtual clock, scripted buttons, PPM screenshots
 * and WAV output.
 *
 *   mp_host <music dir> [options]
 *     -t ms            run for this long (virtual time, default 10000)
 *     -e ms            a screenshot every `ms`
 *     -S ms,ms,...     screenshots at these times
 *     -o dir           output folder (screenshots, saves; default .)
 *     -w file.wav      the audio output
 *     -s "ms:BTN:1,ms:BTN:0,..."  buttons (LEFT RIGHT UP DOWN A B GAME TIME)
 *     -p path          play this file (relative to the music dir) at start
 *     -H kb            heap size (780: the console)
 *     -f               French
 *     -q file -Q ms    write a savestate at `ms`
 *     -L file          load a savestate at start
 *     -P               profile: CPU time of the audio rendering
 *
 * Environment: MP_FORCE_FLASH=1 (every module through the flash path),
 * MP_FLASH_TWICE=1 (with -p: open the track twice: writes, then reads the
 * flash), MP_NEAREST=1 (no interpolation, to compare with the flash path).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "mp_platform.h"

static const char *music_root;
static const char *out_dir = ".";
static uint64_t now_us;
static uint64_t end_us = 10000000;
static uint32_t buttons;
static int audio_rate = 44100;
static FILE *wav;
static uint32_t wav_samples;
static int french;
static bool profile;
static double prof_cpu, prof_audio;

/* screenshots */
static uint32_t shot_every;
static uint32_t shot_at[64];
static int nshots, next_shot;
static uint32_t next_periodic;
static const uint8_t *last_canvas;
static const uint32_t *last_palette;

/* script */
typedef struct { uint32_t ms; uint32_t btn; int down; } ev_t;
static ev_t script[256];
static int nscript, next_ev;

/* savestate */
static const char *save_path;
static uint32_t save_ms;
static bool saved;

static void finish(void);
static char play_path[1100];   /* -p: played at the first service */

/* ------------------------------------------------------------------ */
uint32_t mp_plat_time_ms(void) { return (uint32_t)(now_us / 1000); }

int mp_plat_audio_rate(void) { return audio_rate; }

bool mp_plat_rate_supported(int rate)
{
    static const int r[] = { 8000, 11025, 12000, 16000, 22050, 32000, 44100, 48000 };
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++)
        if (r[i] == rate)
            return true;
    return false;
}

void mp_plat_set_audio_rate(int rate)
{
    if (rate > 0)
        audio_rate = rate;
}

void mp_plat_screen(bool on) { printf("mp_host: screen %s at %u ms\n", on ? "on" : "off", mp_plat_time_ms()); }
const char *mp_plat_music_root(void) { return music_root; }
const char *mp_plat_save_dir(void) { return out_dir; }
int mp_plat_language(void) { return french; }
uint32_t mp_plat_buttons(void) { return buttons; }

void mp_plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

bool mp_plat_list_dir(const char *path, mp_dir_cb cb, void *arg)
{
    DIR *d = opendir(path);
    if (!d)
        return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        cb(arg, e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size);
    }
    closedir(d);
    return true;
}

/* ---- the flash: anonymous mappings, read only once written ---- */
typedef struct { char key[128]; uint8_t *p; uint32_t size; } blob_t;
static blob_t blobs[32];
static int nblobs;
static uint8_t *w_p;
static uint32_t w_total, w_done;
static char w_key[128];

const uint8_t *mp_plat_flash_lookup(const char *key, uint32_t *size)
{
    for (int i = 0; i < nblobs; i++)
        if (!strcmp(blobs[i].key, key)) {
            *size = blobs[i].size;
            return blobs[i].p;
        }
    return NULL;
}

const uint8_t *mp_plat_flash_begin(const char *key, uint32_t total)
{
    if (nblobs == 32 || total > 64u << 20)
        return NULL;
    w_p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (w_p == MAP_FAILED)
        return NULL;
    memset(w_p, 0xff, total); /* erased flash */
    w_total = total;
    w_done = 0;
    snprintf(w_key, sizeof(w_key), "%s", key);
    return w_p;
}

bool mp_plat_flash_append(const void *buf, uint32_t len)
{
    if (!w_p || w_done + len > w_total)
        return false;
    memcpy(w_p + w_done, buf, len);
    w_done += len;
    return true;
}

const uint8_t *mp_plat_flash_finish(void)
{
    if (!w_p || w_done != w_total) {
        mp_plat_flash_abort();
        return NULL;
    }
    mprotect(w_p, w_total, PROT_READ);
    snprintf(blobs[nblobs].key, sizeof(blobs[nblobs].key), "%s", w_key);
    blobs[nblobs].p = w_p;
    blobs[nblobs].size = w_total;
    nblobs++;
    const uint8_t *p = w_p;
    w_p = NULL;
    return p;
}

void mp_plat_flash_abort(void)
{
    if (w_p)
        munmap(w_p, w_total);
    w_p = NULL;
}

/* ---- screen ---- */
static void write_ppm(const char *name)
{
    if (!last_canvas)
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", MP_WIDTH, MP_HEIGHT);
    for (int i = 0; i < MP_WIDTH * MP_HEIGHT; i++) {
        uint32_t c = last_palette[last_canvas[i]];
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static uint8_t shown[MP_WIDTH * MP_HEIGHT];
static uint32_t shown_pal[256];

void mp_plat_present(const uint8_t *canvas, const uint32_t *palette, int ncolors)
{
    memcpy(shown, canvas, sizeof(shown));
    memcpy(shown_pal, palette, (size_t)ncolors * 4);
    last_canvas = shown;
    last_palette = shown_pal;
}

/* ---- audio ---- */
static void wav_header(void)
{
    uint32_t data = wav_samples * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 1; memcpy(h + 22, &s, 2);
    v = (uint32_t)audio_rate; memcpy(h + 24, &v, 4);
    v = (uint32_t)audio_rate * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

static uint64_t audio_done_us;   /* audio rendered up to this time */
static int32_t audio_peak;

static void render_audio(void)
{
    static int16_t buf[4096];
    while (audio_done_us < now_us) {
        uint64_t frames = (now_us - audio_done_us) * (uint64_t)audio_rate / 1000000;
        if (frames == 0)
            frames = 1;
        if (frames > 4096)
            frames = 4096;
        struct timespec a, b;
        if (profile)
            clock_gettime(CLOCK_MONOTONIC, &a);
        mp_core_audio_render(buf, (int)frames);
        if (profile) {
            clock_gettime(CLOCK_MONOTONIC, &b);
            prof_cpu += (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
            prof_audio += (double)frames / audio_rate;
        }
        for (uint64_t i = 0; i < frames; i++) {
            int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > audio_peak)
                audio_peak = v;
        }
        if (wav) {
            fwrite(buf, 2, (size_t)frames, wav);
            wav_samples += (uint32_t)frames;
        }
        audio_done_us += frames * 1000000 / (uint64_t)audio_rate;
    }
}

static const char *btn_name(uint32_t b);
static uint32_t btn_of(const char *s)
{
    static const char *names[] = { "LEFT", "RIGHT", "UP", "DOWN", "A", "B", "GAME", "TIME" };
    for (int i = 0; i < 8; i++)
        if (!strcmp(s, names[i]))
            return 1u << i;
    return 0;
}
static const char *btn_name(uint32_t b) { (void)b; return ""; }

void mp_plat_watchdog(void) {}

void mp_plat_service(bool idle)
{
    if (play_path[0]) {
        char p[1100];
        snprintf(p, sizeof(p), "%s", play_path);
        play_path[0] = '\0';
        printf("mp_host: play %s: %s\n", p, mp_core_play_path(p) ? "ok" : "FAILED");
        if (getenv("MP_FLASH_TWICE"))
            printf("mp_host: play again %s: %s\n", p, mp_core_play_path(p) ? "ok" : "FAILED");
    }
    now_us += idle ? 10000 : 1000;
    render_audio();
    uint32_t ms = mp_plat_time_ms();
    while (next_ev < nscript && script[next_ev].ms <= ms) {
        if (script[next_ev].down)
            buttons |= script[next_ev].btn;
        else
            buttons &= ~script[next_ev].btn;
        next_ev++;
    }
    if (shot_every && ms >= next_periodic) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06u.ppm", next_periodic);
        write_ppm(name);
        next_periodic += shot_every;
    }
    while (next_shot < nshots && ms >= shot_at[next_shot]) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06u.ppm", shot_at[next_shot]);
        write_ppm(name);
        next_shot++;
    }
    if (save_path && !saved && ms >= save_ms) {
        saved = true;
        printf("mp_host: savestate %s at %u ms: %s\n", save_path, ms,
               mp_core_save_state(save_path) ? "ok" : "FAILED");
    }
    if (now_us >= end_us)
        finish();
}

static void finish(void)
{
    write_ppm("frame_end.ppm");
    if (wav) {
        wav_header();
        fclose(wav);
    }
    printf("mp_host: %u ms, audio peak %d, rate %d, heap used %zu / peak %zu / size %zu\n",
           mp_plat_time_ms(), audio_peak, audio_rate, mp_heap_used(), mp_heap_peak(), mp_heap_size());
    if (profile && prof_audio > 0)
        printf("mp_host: audio rendering: %.3f s of CPU for %.1f s of audio (%.2f%%)\n",
               prof_cpu, prof_audio, prof_cpu * 100 / prof_audio);
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mp_host <music dir> [options] (see mp_host.c)\n");
        return 2;
    }
    music_root = argv[1];
    size_t heap_kb = 780;
    const char *play = NULL, *load = NULL, *wav_path = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-t") && v) { end_us = (uint64_t)atol(v) * 1000; i++; }
        else if (!strcmp(a, "-e") && v) { shot_every = (uint32_t)atol(v); next_periodic = shot_every; i++; }
        else if (!strcmp(a, "-S") && v) {
            char *s = strdup(v);
            for (char *t = strtok(s, ","); t && nshots < 64; t = strtok(NULL, ","))
                shot_at[nshots++] = (uint32_t)atol(t);
            free(s);
            i++;
        }
        else if (!strcmp(a, "-o") && v) { out_dir = v; i++; }
        else if (!strcmp(a, "-w") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "-s") && v) {
            char *s = strdup(v);
            for (char *t = strtok(s, ","); t && nscript < 256; t = strtok(NULL, ",")) {
                char name[16];
                unsigned ms;
                int down;
                if (sscanf(t, "%u:%15[A-Z]:%d", &ms, name, &down) == 3) {
                    script[nscript].ms = ms;
                    script[nscript].btn = btn_of(name);
                    script[nscript].down = down;
                    nscript++;
                }
            }
            free(s);
            i++;
        }
        else if (!strcmp(a, "-p") && v) { play = v; i++; }
        else if (!strcmp(a, "-H") && v) { heap_kb = (size_t)atol(v); i++; }
        else if (!strcmp(a, "-f")) { french = 1; }
        else if (!strcmp(a, "-q") && v) { save_path = v; i++; }
        else if (!strcmp(a, "-Q") && v) { save_ms = (uint32_t)atol(v); i++; }
        else if (!strcmp(a, "-L") && v) { load = v; i++; }
        else if (!strcmp(a, "-P")) { profile = true; }
        else { fprintf(stderr, "mp_host: unknown option %s\n", a); return 2; }
    }
    (void)btn_name;
    mp_heap_init();
    void *heap = malloc(heap_kb * 1024);
    mp_heap_add(heap, heap_kb * 1024);
    if (wav_path) {
        wav = fopen(wav_path, "wb+");
        if (wav)
            wav_header();
    }
    if (load)
        printf("mp_host: load %s: %s\n", load, mp_core_load_state(load) ? "ok" : "FAILED");
    if (play)
        snprintf(play_path, sizeof(play_path), "%s/%s", music_root, play);
    mp_core_run();
}
