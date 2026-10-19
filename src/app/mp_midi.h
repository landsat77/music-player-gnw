/* MIDI files (mp_midi.c): a Standard MIDI File sequencer playing a
 * SoundFont with TinySoundFont */
#ifndef MP_MIDI_H
#define MP_MIDI_H

#include <stdint.h>
#include <stdbool.h>

#define MP_MIDI_RATE 32000

/* `smf`: the file (kept by the sequencer, mp_free()d by mp_midi_close()),
 * `sf2`: the SoundFont, in memory for as long as the song plays (mapped
 * flash). Returns 0, or an MP_ERR_* (mp_player.h). */
int mp_midi_open(uint8_t *smf, uint32_t smf_size, const uint8_t *sf2, uint32_t sf2_size);
void mp_midi_close(void);
int mp_midi_render(int16_t *out, int frames, bool loop);
uint32_t mp_midi_duration_ms(void);
uint32_t mp_midi_position_ms(void);
void mp_midi_seek_ms(uint32_t ms);
/* the name of the song (track name meta event) and its copyright */
const char *mp_midi_title(void);
const char *mp_midi_copyright(void);
int mp_midi_tracks(void);
/* now: voices playing, tempo, level of the 16 channels (0..64) */
int mp_midi_voices(void);
int mp_midi_bpm(void);
void mp_midi_levels(uint8_t *vu16);

#endif
