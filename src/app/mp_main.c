/*
 * Music Player for the Game & Watch (Retro-Go SD), as a GWHB homebrew:
 * the retro-go side of the player (mp_platform.h). Display in LUT8 mode,
 * audio through the firmware's PCM ring, buttons, pause menu, savestates,
 * the heap over the free memory of the console, the flash blob store.
 *
 * Everything that reaches the firmware goes through the ABI bridge
 * (sdk/src/gw_core_bridge.*): this binary is not linked with the firmware.
 */
#include <odroid_system.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "gw_audio.h"
#include "gw_malloc.h"
#include "common.h"
#include "rom_manager.h"
#include "appid.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "odroid_settings.h"

#include "mp_platform.h"

#include "gw_core_i18n.h"
#include "gw_core_bridge.h" /* last: rewrites ACTIVE_FILE, common_emu_state, dma_counter */

/* firmware (gw_flash_alloc.c): the blob store */
bool store_data_begin(void *st, const char *key, uint32_t total_size);
bool store_data_append(void *st, const uint8_t *buf, uint32_t len);
const uint8_t *store_data_finish(void *st);
void store_data_abort(void *st);
const uint8_t *lookup_data_in_flash(const char *key, uint32_t *size_out);

/* Audio: the firmware's interrupt fills each DMA half from a ring the
 * player fills ahead (MP_RING_AHEAD samples): the music does not depend on
 * when the player gets to it (drawing, loading the next track...). */
#define MP_RING_SIZE   16384  /* power of 2 */
#define MP_RING_AHEAD  8192
#define MP_TICK_MS     16     /* buttons / menu / overlay rate */
#define MP_FLASH_CHUNK 4096

static odroid_gamepad_state_t joystick;
static uint32_t buttons;

static int16_t ring[MP_RING_SIZE];
static volatile uint16_t ring_head, ring_tail;
static uint32_t ring_written;
static bool ring_on;
static int audio_rate = 44100;

static uint32_t last_tick_ms;
static uint32_t time_offset;  /* time in the pause menu */
static int in_service;
static bool clut_valid;
static bool screen_lit = true;

static char save_dir[96];

/* The stack: 24 KB at the top of DTCM (firmware linker script). Painted at
 * start, the statistics give its low-water mark. */
#define MP_STACK_LOW  0x2001a020u
#define MP_STACK_TOP  0x20020000u
#define MP_STACK_MARK 0x5ab1e5edu
static bool stack_painted;

static void stack_paint(void)
{
    uint32_t sp = (uint32_t)__builtin_frame_address(0);
    if (sp <= MP_STACK_LOW + 1024 || sp > MP_STACK_TOP)
        return;
    for (volatile uint32_t *w = (uint32_t *)(MP_STACK_LOW + 32); (uint32_t)w < sp - 256; w++)
        *w = MP_STACK_MARK;
    stack_painted = true;
}

static unsigned stack_free(void)
{
    if (!stack_painted)
        return 0;
    const uint32_t *w = (const uint32_t *)(MP_STACK_LOW + 32);
    while ((uint32_t)w < MP_STACK_TOP && *w == MP_STACK_MARK)
        w++;
    return (unsigned)((uint32_t)w - MP_STACK_LOW);
}

/* statistics (log every 10 s) */
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
static uint32_t stat_t0, stat_audio_missed;
static uint64_t stat_idle_cycles;

/* ------------------------------------------------------------------ */
/* Platform: misc                                                     */
/* ------------------------------------------------------------------ */
uint32_t mp_plat_time_ms(void)
{
    return get_elapsed_time() - time_offset;
}

uint32_t mp_plat_buttons(void)
{
    return buttons;
}

const char *mp_plat_music_root(void)
{
    return RG_BASE_PATH_MUSIC;
}

const char *mp_plat_save_dir(void)
{
    return save_dir;
}

int mp_plat_language(void)
{
    static const gw_i18n_entry_t lang[] = { { "en", "0" }, { "fr", "1" }, GW_I18N_END };
    return gw_i18n(lang)[0] == '1' ? 1 : 0;
}

void mp_plat_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
}

void mp_plat_screen(bool on)
{
    if (on == screen_lit)
        return;
    screen_lit = on;
    if (on)
        lcd_backlight_on();
    else
        lcd_backlight_off();
}

/* ------------------------------------------------------------------ */
/* Files                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    mp_dir_cb cb;
    void *arg;
    int n;
} scan_ctx_t;

static int scan_cb(const rg_scandir_t *file, void *arg)
{
    scan_ctx_t *c = arg;
    if (file->basename && file->basename[0])
        c->cb(c->arg, file->basename, file->is_dir, (uint32_t)file->size);
    if ((++c->n & 31) == 0)
        wdog_refresh();
    return RG_SCANDIR_CONTINUE;
}

bool mp_plat_list_dir(const char *path, mp_dir_cb cb, void *arg)
{
    if (!rg_storage_exists(path))
        return false;
    scan_ctx_t c = { cb, arg, 0 };
    return rg_storage_scandir(path, scan_cb, &c, RG_SCANDIR_FILES | RG_SCANDIR_DIRS);
}

/* ------------------------------------------------------------------ */
/* The flash (big modules)                                            */
/* ------------------------------------------------------------------ */
static uint32_t flash_st[16] __attribute__((aligned(8))); /* flash_stream_t */
static uint8_t *flash_buf;  /* appends gathered in MP_FLASH_CHUNK bytes */
static uint32_t flash_buf_len;
static bool flash_active;

const uint8_t *mp_plat_flash_lookup(const char *key, uint32_t *size)
{
    return lookup_data_in_flash(key, size);
}

const uint8_t *mp_plat_flash_begin(const char *key, uint32_t total)
{
    if (!flash_buf)
        flash_buf = mp_malloc(MP_FLASH_CHUNK);
    if (!flash_buf)
        return NULL;
    memset(flash_st, 0, sizeof(flash_st));
    if (!store_data_begin(flash_st, key, total))
        return NULL;
    flash_active = true;
    flash_buf_len = 0;
    /* flash_stream_t: key_crc, flash_address (the mapped base), ... */
    return (const uint8_t *)flash_st[1];
}

static bool flash_flush(void)
{
    if (!flash_buf_len)
        return true;
    bool ok = store_data_append(flash_st, flash_buf, flash_buf_len);
    flash_buf_len = 0;
    return ok;
}

bool mp_plat_flash_append(const void *buf, uint32_t len)
{
    const uint8_t *p = buf;
    while (len) {
        uint32_t n = MP_FLASH_CHUNK - flash_buf_len;
        if (n > len)
            n = len;
        memcpy(flash_buf + flash_buf_len, p, n);
        flash_buf_len += n;
        p += n;
        len -= n;
        if (flash_buf_len == MP_FLASH_CHUNK && !flash_flush())
            return false;
    }
    return true;
}

const uint8_t *mp_plat_flash_finish(void)
{
    if (!flash_active)
        return NULL;
    flash_active = false;
    if (!flash_flush()) {
        store_data_abort(flash_st);
        return NULL;
    }
    const uint8_t *p = store_data_finish(flash_st);
    mp_free(flash_buf);
    flash_buf = NULL;
    return p;
}

void mp_plat_flash_abort(void)
{
    if (flash_active)
        store_data_abort(flash_st);
    flash_active = false;
    mp_free(flash_buf);
    flash_buf = NULL;
}

/* ------------------------------------------------------------------ */
/* Display                                                            */
/* ------------------------------------------------------------------ */
static void show_canvas(uint8_t *fb, const uint8_t *canvas, const uint32_t *palette, int ncolors)
{
    if (!clut_valid) {
        static uint32_t clut[64];
        memcpy(clut, palette, (size_t)ncolors * 4);
        lcd_set_clut(clut, ncolors); /* applied at the next swap */
        clut_valid = true;
    }
    memcpy(fb, canvas, MP_WIDTH * MP_HEIGHT);
}

void mp_plat_present(const uint8_t *canvas, const uint32_t *palette, int ncolors)
{
    wdog_refresh();
    if (!screen_lit)
        return;
    lcd_sleep_while_swap_pending();
    show_canvas(lcd_get_active_buffer(), canvas, palette, ncolors);
    common_ingame_overlay();
    lcd_swap();
}

/* the pause menu's background */
static void repaint(void)
{
    const uint32_t *pal;
    int n;
    const uint8_t *c = mp_core_canvas(&pal, &n);
    lcd_sleep_while_swap_pending();
    show_canvas(lcd_get_active_buffer(), c, pal, n);
    common_ingame_overlay();
}

static void *screenshot(void)
{
    const uint32_t *pal;
    int n;
    const uint8_t *c = mp_core_canvas(&pal, &n);
    lcd_wait_for_vblank();
    show_canvas(lcd_get_active_buffer(), c, pal, n);
    return lcd_get_active_buffer();
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */
static void ring_start(void)
{
    ring_head = ring_tail = 0;
    ring_written = 0;
    pcm_attach(ring, MP_RING_SIZE, &ring_head, &ring_tail);
    pcm_audio_setpos(0);
    pcm_audio_set(common_emu_sound_get_volume(), 1);
    pcm_audio_enable(1);
    ring_on = true;
}

static void ring_stop(void)
{
    if (ring_on)
        pcm_audio_enable(0);
    ring_on = false;
}

static void audio_begin(int rate)
{
    audio_rate = rate;
    odroid_audio_init(rate);
    int half = rate / 60;
    if (half > AUDIO_BUFFER_LENGTH)
        half = AUDIO_BUFFER_LENGTH;
    audio_start_playing((uint16_t)half);
    ring_start();
}

int mp_plat_audio_rate(void)
{
    return audio_rate;
}

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
    if (rate <= 0 || rate == audio_rate)
        return;
    ring_stop();
    audio_stop_playing();
    audio_begin(rate);
    mp_plat_log("mp: audio at %d Hz\n", rate);
}

/* tops the ring up to MP_RING_AHEAD samples ahead of the interrupt */
static void feed_audio(void)
{
    if (!ring_on)
        return;
    uint32_t played = pcm_audio_pos();
    if ((int32_t)(played - ring_written) > 0) {
        stat_audio_missed += played - ring_written;
        ring_written = played;
    }
    bool muted = common_emu_sound_loop_is_muted();
    pcm_audio_set(common_emu_sound_get_volume(), muted ? 0 : 1);
    int level = (ring_head - ring_tail) & (MP_RING_SIZE - 1);
    int want = MP_RING_AHEAD - level;
    while (want > 0) {
        int head = ring_head;
        int n = want;
        if (n > MP_RING_SIZE - head)
            n = MP_RING_SIZE - head;
        if (n > 1024)
            n = 1024;
        mp_core_audio_render(ring + head, n);
        ring_head = (uint16_t)((head + n) & (MP_RING_SIZE - 1));
        ring_written += n;
        want -= n;
    }
}

/* ------------------------------------------------------------------ */
/* Pause menu                                                         */
/* ------------------------------------------------------------------ */
static char mode_value[24], off_value[24];

static const gw_i18n_entry_t i18n_mode[] = {
    { "en", "Play mode" }, { "fr", "Mode de lecture" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_off[] = {
    { "en", "Screen off" }, { "fr", "Extinction \xc3\xa9" "cran" }, GW_I18N_END
};
static const gw_i18n_entry_t i18n_modes[MP_MODE_COUNT][3] = {
    { { "en", "Repeat all" }, { "fr", "Tout r\xc3\xa9p\xc3\xa9ter" }, GW_I18N_END },
    { { "en", "Repeat one" }, { "fr", "R\xc3\xa9p\xc3\xa9ter un" }, GW_I18N_END },
    { { "en", "Shuffle" }, { "fr", "Al\xc3\xa9" "atoire" }, GW_I18N_END },
    { { "en", "Play once" }, { "fr", "Une fois" }, GW_I18N_END },
};
static const gw_i18n_entry_t i18n_never[] = {
    { "en", "Never" }, { "fr", "Jamais" }, GW_I18N_END
};
static const int off_choices[] = { 0, 30, 60, 300 };

static void mode_label(void)
{
    snprintf(mode_value, sizeof(mode_value), "%s", gw_i18n(i18n_modes[mp_core_get_mode()]));
}

static void off_label(void)
{
    int s = mp_core_get_screen_off();
    if (s == 0)
        snprintf(off_value, sizeof(off_value), "%s", gw_i18n(i18n_never));
    else if (s < 60)
        snprintf(off_value, sizeof(off_value), "%d s", s);
    else
        snprintf(off_value, sizeof(off_value), "%d min", s / 60);
}

static bool mode_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int m = mp_core_get_mode();
    if (event == ODROID_DIALOG_PREV)
        mp_core_set_mode((m + MP_MODE_COUNT - 1) % MP_MODE_COUNT);
    else if (event == ODROID_DIALOG_NEXT || event == ODROID_DIALOG_ENTER)
        mp_core_set_mode((m + 1) % MP_MODE_COUNT);
    mode_label();
    (void)option;
    return false;
}

static bool off_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int n = (int)(sizeof(off_choices) / sizeof(off_choices[0]));
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (off_choices[i] == mp_core_get_screen_off())
            cur = i;
    if (event == ODROID_DIALOG_PREV)
        mp_core_set_screen_off(off_choices[(cur + n - 1) % n]);
    else if (event == ODROID_DIALOG_NEXT || event == ODROID_DIALOG_ENTER)
        mp_core_set_screen_off(off_choices[(cur + 1) % n]);
    off_label();
    (void)option;
    return false;
}

static odroid_dialog_choice_t options[3];

static void setup_options(void)
{
    options[0] = (odroid_dialog_choice_t){ 300, gw_i18n(i18n_mode), mode_value, 1, &mode_cb };
    options[1] = (odroid_dialog_choice_t){ 301, gw_i18n(i18n_off), off_value, 1, &off_cb };
    options[2] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;
}

/* ------------------------------------------------------------------ */
/* Buttons, service                                                   */
/* ------------------------------------------------------------------ */
static uint32_t map_buttons(const odroid_gamepad_state_t *j)
{
    uint32_t b = 0;
    if (j->values[ODROID_INPUT_LEFT])  b |= MP_BTN_LEFT;
    if (j->values[ODROID_INPUT_RIGHT]) b |= MP_BTN_RIGHT;
    if (j->values[ODROID_INPUT_UP])    b |= MP_BTN_UP;
    if (j->values[ODROID_INPUT_DOWN])  b |= MP_BTN_DOWN;
    if (j->values[ODROID_INPUT_A])     b |= MP_BTN_A;
    if (j->values[ODROID_INPUT_B])     b |= MP_BTN_B;
    if (j->values[ODROID_INPUT_START] || j->values[ODROID_INPUT_X])  b |= MP_BTN_GAME;
    if (j->values[ODROID_INPUT_SELECT] || j->values[ODROID_INPUT_Y]) b |= MP_BTN_TIME;
    return b;
}

static void run_input_loop(void)
{
    uint32_t t0 = get_elapsed_time();
    mode_label();
    off_label();
    bool was_lit = screen_lit;
    common_emu_input_loop(&joystick, options, &repaint);
    uint32_t spent = get_elapsed_time() - t0;
    if (spent > 100) {
        /* the menu was open: its time is not the player's, a clean picture */
        time_offset += spent;
        if (!was_lit)
            mp_plat_screen(true);
        lcd_sleep_while_swap_pending();
        lcd_clear_buffers();
        clut_valid = false; /* the menu used the CLUT */
        mp_core_wake();
    }
}

void mp_plat_watchdog(void)
{
    wdog_refresh();
}

void mp_plat_service(bool idle)
{
    if (in_service)
        return;
    in_service = 1;

    wdog_refresh();
    feed_audio();

    uint32_t now = get_elapsed_time();
    if (now - last_tick_ms >= MP_TICK_MS) {
        last_tick_ms = now;
        odroid_input_read_gamepad(&joystick);
        if (!screen_lit && joystick.values[ODROID_INPUT_VOLUME]) {
            /* PAUSE/SET with the screen off: light it first */
            mp_plat_screen(true);
            mp_core_wake();
        }
        lcd_sleep_while_swap_pending();
        run_input_loop();
        buttons = map_buttons(&joystick);
        feed_audio();
    }

    if (now - stat_t0 >= 10000) {
        uint32_t ms = now - stat_t0;
        uint32_t idle_ms = (uint32_t)(stat_idle_cycles / 280000u);
        if (idle_ms > ms)
            idle_ms = ms;
        printf("mp: idle %lu%%, heap %u / peak %u, largest %u, stack free %u, audio underrun %lu samples\n",
               (unsigned long)(idle_ms * 100 / ms), (unsigned)mp_heap_used(), (unsigned)mp_heap_peak(),
               (unsigned)mp_heap_largest(), stack_free(), (unsigned long)stat_audio_missed);
        stat_audio_missed = 0;
        stat_idle_cycles = 0;
        stat_t0 = now;
    }

    in_service = 0;
    if (idle) {
        uint32_t c0 = DWT_CYCCNT;
        __WFI(); /* SysTick (1 kHz) or the audio DMA wake us up */
        stat_idle_cycles += (uint32_t)(DWT_CYCCNT - c0);
    }
}

/* ------------------------------------------------------------------ */
/* Savestates                                                         */
/* ------------------------------------------------------------------ */
static bool load_state(const char *path)
{
    return mp_core_load_state(path);
}

static bool save_state(const char *path)
{
    return mp_core_save_state(path);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */
static void add_region(const char *what, void *p, size_t size)
{
    bool ok = p && size && mp_heap_add(p, size);
    printf("mp: heap region %s: %u bytes at %p%s\n", what, (unsigned)size, p, ok ? "" : " (not used)");
}

void app_main(uint8_t load, uint8_t start_paused, int8_t save_slot)
{
    gw_core_bridge_init();
    *(volatile uint32_t *)0xE000EDFC |= 1u << 24; /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001000 |= 1u;       /* DWT_CTRL.CYCCNTENA */
    printf("Music Player start\n");
    stack_paint();

    odroid_system_init(APPID_HOMEBREW, audio_rate);
    odroid_system_emu_init(&load_state, &save_state, &screenshot, NULL, NULL, NULL, NULL);

    rg_storage_mkdir(ODROID_BASE_PATH_SAVES "/music_player");
    snprintf(save_dir, sizeof(save_dir), ODROID_BASE_PATH_SAVES "/music_player");

    /* the heap: what is free in RAM_EMU, the LCD memory freed by LUT8,
     * DTCM below the stack, ITCM */
    lcd_setup_framebuffers(LCD_MODE_LUT8);
    lcd_clear_buffers();
    mp_heap_init();
    size_t ram = ram_get_free_size();
    add_region("RAM_EMU", ram_malloc(ram), ram);
    uint8_t *pool;
    size_t pool_size;
    lcd_get_bonus_pool(&pool, &pool_size);
    add_region("LCD", pool, pool_size);
    size_t dtc = dtc_get_free_size();
    if (dtc > 1024)
        add_region("DTCM", dtc_malloc(dtc - 512), dtc - 512);
    itc_init();
    size_t itc = itc_get_free_size();
    if (itc > 1024) {
        uint8_t *p = itc_malloc(itc);
        /* ITCM starts at address 0: keep the first 256 bytes out (NULL) */
        if (p != (uint8_t *)0xffffffff)
            add_region("ITCM", p + 256, itc - 256);
    }

    if (load)
        odroid_system_emu_load_state(save_slot);

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / 60 + 0.5f);

    audio_begin(audio_rate);
    last_tick_ms = get_elapsed_time();
    stat_t0 = last_tick_ms;
    setup_options();

    mp_core_run(); /* never returns */
}
