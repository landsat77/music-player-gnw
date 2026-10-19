/* Chiptunes with Game_Music_Emu (mp_gme.c): NSF / NSFE, GBS, SPC, VGM /
 * VGZ, GYM, HES, KSS, AY, SAP */
#ifndef MP_GME_H
#define MP_GME_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char song[72];       /* "" when the file has no names */
    char game[72];
    char author[72];
    char system[24];     /* "Nintendo NES" */
    uint32_t length_ms;  /* played length, fade included */
    int voices;
} mp_gme_track_t;

bool mp_gme_is_music(const char *name);
int mp_gme_rate_for(const char *name);
/* `data` must stay valid until mp_gme_close(); `m3u_path`: a playlist that
 * may be next to the file (NULL: none). 0, -2: not a file it knows,
 * -3: memory. */
int mp_gme_open(const void *data, long size, int sample_rate, const char *m3u_path);
void mp_gme_close(void);
int mp_gme_tracks(void);
int mp_gme_track(void);
int mp_gme_start(int track, mp_gme_track_t *t);
/* repeat one: the track plays on (no fade) */
void mp_gme_set_loop(bool loop);
int mp_gme_render(int16_t *out, int frames);
uint32_t mp_gme_position_ms(void);
void mp_gme_seek_ms(uint32_t ms);

#endif
