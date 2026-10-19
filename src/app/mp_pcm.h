/* WAV, FLAC and Ogg Vorbis files (mp_pcm.c), streamed from the SD card,
 * rendered as mono at mp_pcm_rate() */
#ifndef MP_PCM_H
#define MP_PCM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char title[72];      /* tags (Latin-1), "" if none */
    char artist[72];
    char format[40];     /* "FLAC 16 bit 44.1 kHz" */
    uint32_t duration_ms;
} mp_pcm_info_t;

bool mp_pcm_is_music(const char *name);
/* 0, -1: cannot read the file, -2: not a file the decoders know, -3: memory */
int mp_pcm_open(const char *path, mp_pcm_info_t *info);
void mp_pcm_close(void);
int mp_pcm_rate(void);
/* up to `frames` mono samples; fewer at the end */
int mp_pcm_render(int16_t *out, int frames);
uint32_t mp_pcm_position_ms(void);
void mp_pcm_seek_ms(uint32_t ms);

#endif
