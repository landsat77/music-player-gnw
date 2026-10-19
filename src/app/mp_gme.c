/*
 * Chiptunes (mp_gme.h) with Game_Music_Emu: the sound chips of the game
 * consoles and computers are emulated (NES, Game Boy, Super Nintendo,
 * Master System / Mega Drive, ZX Spectrum / Amstrad CPC).
 *
 * The file is loaded whole (by mp_player.c: in RAM, or mapped from the
 * external flash for big VGM files, which Game_Music_Emu plays in place).
 * Many formats hold several tracks (a whole game's soundtrack in an NSF):
 * mp_gme_start() plays one of them. A track without a known length plays
 * for 2:30 then fades out (or stops after a few seconds of silence).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "gme.h"
#include "mp_platform.h"
#include "mp_player.h"
#include "mp_gme.h"

#define FADE_MS 8000

static Music_Emu *emu;
static int rate;
static int ntracks, cur;
static int play_ms;           /* the length played (before the fade) */
static bool looping;          /* repeat one: no fade */
static short *stereo;         /* gme renders stereo */
#define STEREO_FRAMES 512
static int voices;
static char sys_name[24];

static const char *const exts[] = {
    "nsf", "nsfe", "gbs", "vgm", "vgz", "spc", "ay", NULL
};

bool mp_gme_is_music(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (int i = 0; exts[i]; i++)
        if (!strcasecmp(dot + 1, exts[i]))
            return true;
    return false;
}

int mp_gme_rate_for(const char *name)
{
    /* the SPC700 DSP runs at 32 kHz: no resampling then. The others at
     * 32 kHz too: lighter for the CPU, and the speaker cannot tell. */
    (void)name;
    return 32000;
}

int mp_gme_open(const void *data, long size, int sample_rate, const char *m3u_path)
{
    mp_gme_close();
    rate = sample_rate;
    gme_err_t err = gme_open_data_inplace(data, size, &emu, sample_rate);
    if (err || !emu) {
        mp_plat_log("mp: gme: %s\n", err ? err : "?");
        emu = NULL;
        return strstr(err ? err : "", "memory") ? -3 : -2;
    }
    if (m3u_path) {
        /* a playlist next to the file: names and lengths of the tracks */
        FILE *f = fopen(m3u_path, "rb");
        if (f) {
            fclose(f);
            if (gme_load_m3u(emu, m3u_path))
                mp_plat_log("mp: gme: the playlist %s is not usable\n", m3u_path);
        }
    }
    stereo = mp_malloc(STEREO_FRAMES * 2 * sizeof(short));
    if (!stereo) {
        mp_gme_close();
        return -3;
    }
    gme_set_autoload_playback_limit(emu, 0); /* the fade is set below */
    ntracks = gme_track_count(emu);
    voices = gme_voice_count(emu);
    snprintf(sys_name, sizeof(sys_name), "%s", gme_type_system(gme_type(emu)));
    cur = -1;
    return ntracks > 0 ? 0 : -2;
}

void mp_gme_close(void)
{
    if (emu)
        gme_delete(emu);
    emu = NULL;
    mp_free(stereo);
    stereo = NULL;
    ntracks = 0;
    cur = -1;
}

int mp_gme_tracks(void)
{
    return ntracks;
}

int mp_gme_track(void)
{
    return cur;
}

static void copy_l1(char *dst, size_t size, const char *s)
{
    /* the tags are ASCII or Latin-1 (some are Shift-JIS: shown as is) */
    snprintf(dst, size, "%s", s ? s : "");
    size_t n = strlen(dst);
    while (n && dst[n - 1] == ' ')
        dst[--n] = '\0';
}

static void apply_fade(void)
{
    if (!emu)
        return;
    if (looping)
        gme_set_fade_msecs(emu, -1, FADE_MS); /* never */
    else
        gme_set_fade_msecs(emu, play_ms, FADE_MS);
}

int mp_gme_start(int track, mp_gme_track_t *t)
{
    if (!emu || track < 0 || track >= ntracks)
        return -2;
    memset(t, 0, sizeof(*t));
    gme_info_t *gi = NULL;
    play_ms = 150000;
    if (!gme_track_info(emu, &gi, track) && gi) {
        copy_l1(t->song, sizeof(t->song), gi->song);
        copy_l1(t->game, sizeof(t->game), gi->game);
        copy_l1(t->author, sizeof(t->author), gi->author);
        if (gi->length > 0)
            play_ms = gi->length;
        else if (gi->loop_length > 0)
            play_ms = (gi->intro_length > 0 ? gi->intro_length : 0) + gi->loop_length * 2;
        gme_free_info(gi);
    }
    gme_err_t err = gme_start_track(emu, track);
    if (err) {
        mp_plat_log("mp: gme: track %d: %s\n", track, err);
        return -2;
    }
    cur = track;
    apply_fade();
    t->length_ms = (uint32_t)play_ms + FADE_MS;
    snprintf(t->system, sizeof(t->system), "%s", sys_name);
    t->voices = voices;
    return 0;
}

void mp_gme_set_loop(bool loop)
{
    if (loop != looping) {
        looping = loop;
        apply_fade();
    }
}

int mp_gme_render(int16_t *out, int frames)
{
    if (!emu || cur < 0)
        return 0;
    int done = 0;
    while (done < frames) {
        if (gme_track_ended(emu))
            break;
        int n = frames - done;
        if (n > STEREO_FRAMES)
            n = STEREO_FRAMES;
        if (gme_play(emu, n * 2, stereo))
            break;
        for (int i = 0; i < n; i++)
            out[done + i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
        done += n;
    }
    return done;
}

uint32_t mp_gme_position_ms(void)
{
    return emu && cur >= 0 ? (uint32_t)gme_tell(emu) : 0;
}

void mp_gme_seek_ms(uint32_t ms)
{
    if (!emu || cur < 0)
        return;
    /* backwards: the track starts again and runs up to there (fast,
     * without sound) */
    gme_seek(emu, (int)ms);
    apply_fade();
}
