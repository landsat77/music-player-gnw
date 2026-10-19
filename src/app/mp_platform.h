/*
 * Contract between the player (mp_core.c, mp_player.c, mp_draw.c,
 * mp_alloc.c: portable C) and the platform it runs on:
 *  - src/app/mp_main.c : the Game & Watch homebrew (retro-go)
 *  - harness/mp_host.c : a headless Linux harness used to test the player
 */
#ifndef MP_PLATFORM_H
#define MP_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_WIDTH  320
#define MP_HEIGHT 240

/* Buttons of the console (mp_plat_buttons()) */
enum {
    MP_BTN_LEFT  = 1 << 0,
    MP_BTN_RIGHT = 1 << 1,
    MP_BTN_UP    = 1 << 2,
    MP_BTN_DOWN  = 1 << 3,
    MP_BTN_A     = 1 << 4,
    MP_BTN_B     = 1 << 5,
    MP_BTN_GAME  = 1 << 6,   /* START on Zelda units */
    MP_BTN_TIME  = 1 << 7,   /* SELECT on Zelda units */
};

/* Play modes */
enum { MP_MODE_REPEAT_ALL, MP_MODE_REPEAT_ONE, MP_MODE_SHUFFLE, MP_MODE_ONCE, MP_MODE_COUNT };

/* ---- implemented by the platform ---- */

/* Monotonic time in milliseconds (not counting the time in the menu). */
uint32_t mp_plat_time_ms(void);

/* Tops up the audio output (mp_core_audio_render()), reads the buttons,
 * runs the retro-go pause menu when asked. `idle`: nothing to do until the
 * next event (the platform may sleep a little). */
void mp_plat_service(bool idle);

/* Buttons held now (MP_BTN_*). */
uint32_t mp_plat_buttons(void);

/* Shows the canvas: MP_WIDTH x MP_HEIGHT palette indexes, the palette has
 * `ncolors` colours 0x00RRGGBB. */
void mp_plat_present(const uint8_t *canvas, const uint32_t *palette, int ncolors);

/* Output sample rate (mono s16). Changing it restarts the audio. */
void mp_plat_set_audio_rate(int rate);
int mp_plat_audio_rate(void);
/* a rate the audio output can play (else 0) */
bool mp_plat_rate_supported(int rate);

/* Screen on / off (backlight), to save the battery while music plays. */
void mp_plat_screen(bool on);

/* Directory listing: calls `cb` for each entry (any order). */
typedef void (*mp_dir_cb)(void *arg, const char *name, bool is_dir, uint32_t size);
bool mp_plat_list_dir(const char *path, mp_dir_cb cb, void *arg);

/* The music folder ("/music") and a folder for the player's files. */
const char *mp_plat_music_root(void);
const char *mp_plat_save_dir(void);

/* 0 = English, 1 = French */
int mp_plat_language(void);

/* Blob store in the external flash (big modules). lookup: the blob of
 * `key` (NULL if absent). begin / append / finish write one; the result of
 * finish is where it is (NULL on error). The address of the blob is known
 * after begin (mp_plat_flash_begin() returns it). */
const uint8_t *mp_plat_flash_lookup(const char *key, uint32_t *size);
const uint8_t *mp_plat_flash_begin(const char *key, uint32_t total);
bool mp_plat_flash_append(const void *buf, uint32_t len);
const uint8_t *mp_plat_flash_finish(void);
void mp_plat_flash_abort(void);

/* Logs (console: the debug output) */
void mp_plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- implemented by the core (mp_core.c) ---- */

/* Runs the player. Never returns (the pause menu quits). */
void mp_core_run(void) __attribute__((noreturn));

/* Renders `frames` mono samples at mp_plat_audio_rate(). */
void mp_core_audio_render(int16_t *out, int frames);

/* Savestates: the track and the position. A load takes effect at the next
 * loop of the player. */
bool mp_core_save_state(const char *path);
bool mp_core_load_state(const char *path);

/* Options of the pause menu */
int mp_core_get_mode(void);
void mp_core_set_mode(int mode);
int mp_core_get_screen_off(void);          /* seconds, 0 = never */
void mp_core_set_screen_off(int seconds);
/* the screen is shown again (a button was pressed in the menu...) */
void mp_core_wake(void);
/* harness: plays this file now */
bool mp_core_play_path(const char *path);
/* redraws the canvas (the background of the pause menu) */
const uint8_t *mp_core_canvas(const uint32_t **palette, int *ncolors);

/* ---- implemented by mp_alloc.c ---- */
void   mp_heap_init(void);
bool   mp_heap_add(void *base, size_t size);
void  *mp_malloc(size_t size);
void  *mp_calloc(size_t n, size_t size);
void  *mp_realloc(void *ptr, size_t size);
void   mp_free(void *ptr);
size_t mp_heap_used(void);
size_t mp_heap_peak(void);
size_t mp_heap_size(void);
size_t mp_heap_largest(void);

#ifdef __cplusplus
}
#endif

#endif
