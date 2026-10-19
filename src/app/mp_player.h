/*
 * The decoders behind one API (mp_player.c): tracker modules with
 * libxmp-lite (MOD, S3M, XM, IT; the samples of the big ones in the
 * external flash), MP3 with minimp3 (streamed from the SD card).
 */
#ifndef MP_PLAYER_H
#define MP_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

enum {
    MP_OK = 0,
    MP_ERR_FILE = -1,     /* cannot read the file */
    MP_ERR_FORMAT = -2,   /* not a file the player knows */
    MP_ERR_MEMORY = -3,   /* too big */
    MP_ERR_FLASH = -4,    /* no room in the flash for its samples */
};

enum { MP_KIND_NONE, MP_KIND_MODULE, MP_KIND_MP3 };

typedef struct {
    int kind;
    char title[72];       /* Latin-1 */
    char artist[72];
    char format[40];      /* "XM, 16 ch", "MP3 192 kbps 44.1 kHz" */
    uint32_t duration_ms; /* 0: unknown */
    bool in_flash;        /* a big module: samples in the flash */
} mp_track_info_t;

typedef struct {
    int pos, npos, row, nrows, speed, bpm;
    int nch;
    uint8_t vu[32];       /* 0..64 per channel (the first 32) */
} mp_module_status_t;

/* progress of the copy of the samples to the flash (done / total bytes) */
typedef void (*mp_progress_cb)(uint32_t done, uint32_t total);

bool mp_player_is_music(const char *name);

int mp_player_open(const char *path, mp_progress_cb progress);
void mp_player_close(void);
bool mp_player_is_open(void);
const mp_track_info_t *mp_player_info(void);

/* rate of the samples mp_player_render() makes */
int mp_player_rate(void);

/* Renders up to `frames` mono samples; returns how many (fewer: the end of
 * the track). `loop`: a module plays on after its end (repeat one). */
int mp_player_render(int16_t *out, int frames, bool loop);

uint32_t mp_player_position_ms(void);
/* MP3: +-10 s, modules: +-1 position */
void mp_player_seek(int direction);
/* savestates: where the track is, and going back there */
uint32_t mp_player_locator(void);
void mp_player_relocate(uint32_t locator);

bool mp_player_module_status(mp_module_status_t *st);

#endif
