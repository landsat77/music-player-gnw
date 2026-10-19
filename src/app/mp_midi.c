/*
 * MIDI files: a Standard MIDI File (format 0 / 1 / 2) sequencer driving
 * TinySoundFont (src/midi/tsf.h, changed under TSF_GNW: the 16 bit samples
 * of the SoundFont are read in place from the mapped flash, and only the
 * presets the song uses are loaded).
 *
 * The file stays in RAM as it is: one cursor per track, the next event is
 * the earliest of the tracks (no event list: a MIDI file of 100 KB can hold
 * 50000 events). Tempo changes, running status, SysEx skipped, SMPTE time
 * division. A first pass over the file gives the programs used (the
 * presets to load), the duration, the title. Seeking replays the events up
 * to the time asked (controllers, programs) without notes.
 */
#include <stdio.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_player.h"
#include "mp_midi.h"

#define TSF_IMPLEMENTATION
#define TSF_GNW
#define TSF_NO_STDIO
#define TSF_MALLOC  mp_malloc
#define TSF_FREE    mp_free
#define TSF_REALLOC mp_realloc
#define TSF_MEMCPY  memcpy
#define TSF_MEMSET  memset
#include <math.h>
#define TSF_POW     pow
#define TSF_POWF    powf
#define TSF_EXPF    expf
#define TSF_LOG     log
#define TSF_TAN     tan
#define TSF_LOG10   log10
#define TSF_SQRT    sqrt
#define TSF_SQRTF   sqrtf
#include "tsf.h"

#define MAX_TRACKS   64
#define MAX_VOICES   48
#define TAIL_MS      2500   /* after the last event: the releases */
#define GAIN_DB      -4.0f

typedef struct {
    const uint8_t *p, *end;
    uint32_t tick;          /* of the next event */
    uint8_t status;         /* running status */
    bool done;
} track_t;

static tsf *synth;
static uint8_t *smf;
static uint32_t smf_size;
static track_t trk[MAX_TRACKS];
static int ntrk;
static int division;        /* ticks per quarter note, or SMPTE */
static double smpte_tps;    /* SMPTE: ticks per second */
static uint32_t tempo_us;   /* per quarter note */
static uint32_t cur_tick;
static double to_next;      /* samples until the next event */
static bool events_done;
static uint32_t tail_left;  /* samples of the tail still to play */
static uint64_t played;     /* samples since the start */
static uint32_t duration_ms;
static char title[72], copyright[72];
static uint8_t melodic[128], drums[128]; /* programs used */
static uint8_t vu_vel[16];
static uint64_t vu_at[16];

/* ------------------------------------------------------------------ */
/* Reading the file                                                   */
/* ------------------------------------------------------------------ */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t varlen(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    const uint8_t *p = *pp;
    for (int i = 0; i < 4 && p < end; i++) {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80))
            break;
    }
    *pp = p;
    return v;
}

static void track_next_tick(track_t *t)
{
    if (t->p >= t->end) {
        t->done = true;
        return;
    }
    t->tick += varlen(&t->p, t->end);
}

static void rewind_tracks(void)
{
    const uint8_t *p = smf + 14, *end = smf + smf_size;
    int n = 0;
    while (p + 8 <= end && n < ntrk) {
        uint32_t len = be32(p + 4);
        const uint8_t *data = p + 8;
        if ((uint32_t)(end - data) < len)
            len = (uint32_t)(end - data);
        if (!memcmp(p, "MTrk", 4)) {
            trk[n].p = data;
            trk[n].end = data + len;
            trk[n].tick = 0;
            trk[n].status = 0;
            trk[n].done = false;
            track_next_tick(&trk[n]);
            n++;
        }
        p = data + len;
    }
    ntrk = n;
    cur_tick = 0;
    tempo_us = 500000;
    events_done = false;
}

/* the track with the next event (-1: none) */
static int next_track(void)
{
    int best = -1;
    for (int i = 0; i < ntrk; i++)
        if (!trk[i].done && (best < 0 || trk[i].tick < trk[best].tick))
            best = i;
    return best;
}

static double samples_per_tick(void)
{
    if (smpte_tps > 0)
        return (double)MP_MIDI_RATE / smpte_tps;
    return (double)tempo_us * MP_MIDI_RATE / 1e6 / division;
}

/* what one event does: mode 0 = scan (programs, duration, names), 1 =
 * play, 2 = seek (no notes) */
static void do_event(track_t *t, int mode, bool first_track)
{
    const uint8_t *end = t->end;
    uint8_t st = *t->p;
    if (st & 0x80) {
        t->p++;
        if (st < 0xf0)
            t->status = st;
    } else {
        st = t->status; /* running status */
        if (!st) {      /* broken: skip a byte */
            t->p++;
            return;
        }
    }
    if (st == 0xff) {
        if (t->p >= end)
            return;
        uint8_t type = *t->p++;
        uint32_t len = varlen(&t->p, end);
        const uint8_t *d = t->p;
        if (len > (uint32_t)(end - d))
            len = (uint32_t)(end - d);
        t->p += len;
        if (type == 0x51 && len >= 3) {
            tempo_us = (uint32_t)d[0] << 16 | d[1] << 8 | d[2];
            if (tempo_us == 0)
                tempo_us = 500000;
        } else if (type == 0x2f) {
            t->done = true;
        } else if (mode == 0 && first_track && (type == 0x03 || type == 0x02)) {
            char *dst = type == 0x03 ? title : copyright;
            if (!dst[0]) {
                size_t n = len < sizeof(title) - 1 ? len : sizeof(title) - 1;
                memcpy(dst, d, n);
                dst[n] = '\0';
                for (size_t i = 0; i < n; i++)
                    if ((unsigned char)dst[i] < 32)
                        dst[i] = ' ';
            }
        }
        return;
    }
    if (st == 0xf0 || st == 0xf7) {
        uint32_t len = varlen(&t->p, end);
        t->p += len < (uint32_t)(end - t->p) ? len : (uint32_t)(end - t->p);
        return;
    }
    if (st >= 0xf0) /* system common without data (rare in files) */
        return;
    int ch = st & 15, type = st & 0xf0;
    int n1 = (type == 0xc0 || type == 0xd0) ? 1 : 2;
    if (end - t->p < n1) {
        t->p = end;
        return;
    }
    int a = t->p[0] & 0x7f, b = n1 == 2 ? t->p[1] & 0x7f : 0;
    t->p += n1;

    if (mode == 0) {
        if (type == 0xc0) {
            if (ch == 9)
                drums[a] = 1;
            else
                melodic[a] = 1;
        } else if (type == 0x90 && b) {
            /* a note on a channel without program change: program 0 */
            if (ch == 9)
                drums[0] = 1;
            else
                melodic[0] = 1;
        }
        return;
    }
    switch (type) {
    case 0x90:
        if (mode == 1) {
            if (b) {
                tsf_channel_note_on(synth, ch, a, b / 127.0f);
                vu_vel[ch] = (uint8_t)(b / 2);
                vu_at[ch] = played;
            } else {
                tsf_channel_note_off(synth, ch, a);
            }
        }
        break;
    case 0x80:
        if (mode == 1)
            tsf_channel_note_off(synth, ch, a);
        break;
    case 0xb0:
        tsf_channel_midi_control(synth, ch, a, b);
        break;
    case 0xc0:
        tsf_channel_set_presetnumber(synth, ch, a, ch == 9);
        break;
    case 0xe0:
        tsf_channel_set_pitchwheel(synth, ch, a | (b << 7));
        break;
    default: /* aftertouch: not used by TinySoundFont */
        break;
    }
}

/* runs the events of the current tick; then the time to the next one */
static void run_events(int mode)
{
    for (;;) {
        int i = next_track();
        if (i < 0) {
            events_done = true;
            return;
        }
        if (trk[i].tick > cur_tick) {
            to_next += (double)(trk[i].tick - cur_tick) * samples_per_tick();
            cur_tick = trk[i].tick;
            return;
        }
        do_event(&trk[i], mode, i == 0);
        if (!trk[i].done)
            track_next_tick(&trk[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Synth                                                              */
/* ------------------------------------------------------------------ */
static int want_preset(void *arg, int bank, int preset)
{
    (void)arg;
    if (preset < 0 || preset > 127)
        return 0;
    if (bank >= 128)
        return drums[preset] || preset == 0;
    return melodic[preset];
}

static void channels_reset(void)
{
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_midi_control(synth, ch, 121, 0); /* reset controllers */
        tsf_channel_set_presetnumber(synth, ch, 0, ch == 9);
        tsf_channel_midi_control(synth, ch, 7, 100);  /* GM default volume */
    }
    memset(vu_vel, 0, sizeof(vu_vel));
}

static void restart(void)
{
    tsf_note_off_all(synth);
    rewind_tracks();
    to_next = 0;
    played = 0;
    tail_left = (uint32_t)((uint64_t)TAIL_MS * MP_MIDI_RATE / 1000);
}

int mp_midi_open(uint8_t *data, uint32_t size, const uint8_t *sf2, uint32_t sf2_size)
{
    mp_midi_close();
    smf = data;
    smf_size = size;
    if (size < 22 || memcmp(data, "MThd", 4) || be32(data + 4) < 6)
        return MP_ERR_FORMAT;
    int tracks = data[10] << 8 | data[11];
    int div = data[12] << 8 | data[13];
    if (div & 0x8000) {
        int fps = 256 - (div >> 8);
        smpte_tps = (double)(fps == 29 ? 29.97 : fps) * (div & 0xff);
        division = 1;
        if (smpte_tps <= 0)
            return MP_ERR_FORMAT;
    } else {
        smpte_tps = 0;
        division = div ? div : 96;
    }
    ntrk = tracks < MAX_TRACKS ? tracks : MAX_TRACKS;
    rewind_tracks();
    if (ntrk == 0)
        return MP_ERR_FORMAT;

    /* scan: programs, duration, names */
    memset(melodic, 0, sizeof(melodic));
    memset(drums, 0, sizeof(drums));
    title[0] = copyright[0] = '\0';
    double samples = 0;
    to_next = 0;
    for (;;) {
        run_events(0);
        if (events_done)
            break;
        samples += to_next;
        to_next = 0;
    }
    duration_ms = (uint32_t)(samples * 1000.0 / MP_MIDI_RATE) + TAIL_MS / 2;

    synth = tsf_load_memory_inplace(sf2, (int)sf2_size, want_preset, NULL);
    if (!synth)
        return MP_ERR_MEMORY;
    tsf_set_output(synth, TSF_MONO, MP_MIDI_RATE, GAIN_DB);
    if (!tsf_set_max_voices(synth, MAX_VOICES))
        return MP_ERR_MEMORY;
    channels_reset();
    if (!synth->channels || synth->channels->channelNum < 16)
        return MP_ERR_MEMORY;
    restart();
    return MP_OK;
}

void mp_midi_close(void)
{
    if (synth)
        tsf_close(synth);
    synth = NULL;
    mp_free(smf);
    smf = NULL;
    ntrk = 0;
}

int mp_midi_render(int16_t *out, int frames, bool loop)
{
    int done = 0;
    while (done < frames) {
        if (!events_done && to_next < 1.0) {
            run_events(1);
            continue;
        }
        int n = frames - done;
        if (!events_done) {
            if (n > (int)to_next)
                n = (int)to_next;
        } else {
            /* the releases of the last notes */
            if (tail_left == 0 || tsf_active_voice_count(synth) == 0) {
                if (!loop)
                    break;
                restart();
                continue;
            }
            if ((uint32_t)n > tail_left)
                n = (int)tail_left;
            tail_left -= (uint32_t)n;
        }
        tsf_render_short(synth, out + done, n, 0);
        done += n;
        to_next -= n;
        played += (uint64_t)n;
    }
    return done;
}

uint32_t mp_midi_duration_ms(void) { return duration_ms; }
uint32_t mp_midi_position_ms(void) { return (uint32_t)(played * 1000 / MP_MIDI_RATE); }

void mp_midi_seek_ms(uint32_t ms)
{
    if (!synth)
        return;
    if (duration_ms && ms >= duration_ms)
        ms = duration_ms > 1000 ? duration_ms - 1000 : 0;
    tsf_reset(synth);
    tsf_set_max_voices(synth, MAX_VOICES);
    channels_reset();
    restart();
    double target = (double)ms * MP_MIDI_RATE / 1000.0, pos = 0;
    while (!events_done) {
        if (to_next < 1.0) {
            run_events(2); /* controllers and programs, no notes */
            continue;
        }
        if (pos + to_next > target)
            break;
        pos += to_next;
        to_next = 0;
    }
    if (!events_done)
        to_next -= target - pos;
    played = (uint64_t)target;
}

const char *mp_midi_title(void) { return title; }
const char *mp_midi_copyright(void) { return copyright; }
int mp_midi_tracks(void) { return ntrk; }
int mp_midi_voices(void) { return synth ? tsf_active_voice_count(synth) : 0; }
int mp_midi_bpm(void) { return tempo_us ? (int)(60000000u / tempo_us) : 120; }

void mp_midi_levels(uint8_t *vu16)
{
    for (int ch = 0; ch < 16; ch++) {
        uint64_t age = played - vu_at[ch];
        uint32_t fade = (uint32_t)(age * 1000 / MP_MIDI_RATE); /* ms */
        int v = vu_vel[ch] - (int)(fade / 12);
        vu16[ch] = (uint8_t)(v > 0 ? (v > 64 ? 64 : v) : 0);
    }
}
