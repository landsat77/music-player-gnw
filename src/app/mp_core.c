/*
 * The player: a browser of the music folder, the "now playing" screen, the
 * playlist (the music files of the folder of the track), the play modes,
 * the settings, the savestates. Portable: the platform (mp_platform.h)
 * gives the buttons, the audio output, the screen and the files.
 *
 * Single thread: mp_core_run() loops on mp_plat_service(), which asks for
 * samples (mp_core_audio_render()) and runs the pause menu.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mp_platform.h"
#include "mp_player.h"
#include "mp_draw.h"

#define PATH_MAX_LEN   256
#define ROW_H          12
#define LIST_Y         18
#define LIST_ROWS      16
#define FOOTER_Y       214
#define SCOPE_LEN      320
#define REPEAT_DELAY   380
#define REPEAT_RATE    70
#define SEEK_RATE      250
#define TOAST_MS       2200
#define STATE_MAGIC    0x3153504du /* "MPS1" */
#define SETTINGS_MAGIC 0x3154504du /* "MPT1" */

/* ------------------------------------------------------------------ */
/* Texts                                                              */
/* ------------------------------------------------------------------ */
enum {
    T_TITLE, T_NOW_PLAYING, T_EMPTY, T_NO_FOLDER, T_PAUSED, T_STOPPED,
    T_MODE_ALL, T_MODE_ONE, T_MODE_SHUFFLE, T_MODE_ONCE,
    T_ERR_FILE, T_ERR_FORMAT, T_ERR_MEMORY, T_ERR_FLASH,
    T_COPY, T_COPY2, T_POS, T_ROW, T_TEMPO, T_NOTHING, T_HELP_BROWSER, T_HELP_PLAYING,
    T_COUNT
};
static const char *const texts[2][T_COUNT] = {
    {
        "Music Player", "Now playing", "No music here", "No /music folder on the SD card",
        "Paused", "Stopped",
        "Repeat all", "Repeat one", "Shuffle", "Play once",
        "Cannot read the file", "Unknown format", "Too big for the memory", "No room in the flash",
        "Copying the samples to the flash", "(first play only)", "Pos", "Row", "Tempo",
        "Nothing playing",
        "A:open B:back GAME:playing TIME:mode",
        "A:pause  </>:track  ^v:seek  B:back",
    },
    {
        "Music Player", "Lecture en cours", "Pas de musique ici", "Pas de dossier /music sur la carte SD",
        "En pause", "Arr\xeat\xe9",
        "Tout r\xe9p\xe9ter", "R\xe9p\xe9ter un", "Al\xe9" "atoire", "Une fois",
        "Fichier illisible", "Format inconnu", "Trop gros pour la m\xe9moire", "Plus de place en flash",
        "Copie des samples en flash", "(premi\xe8re lecture seulement)", "Pos", "Ligne", "Tempo",
        "Rien en lecture",
        "A:ouvrir B:retour GAME:lecture TIME:mode",
        "A:pause </>:piste ^v:avance B:retour",
    },
};
static int lang;
static const char *T(int id) { return texts[lang][id]; }

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t name;   /* offset in the pool */
    uint32_t size;
    uint8_t is_dir;
} entry_t;

typedef struct {
    entry_t *e;
    int count, cap;
    char *pool;
    uint32_t pool_len, pool_cap;
} dirlist_t;

static uint8_t *canvas; /* on the heap: in the LCD memory on the console (best fit) */
static char cwd[PATH_MAX_LEN];
static dirlist_t list;
static int sel, top;
static bool root_missing;

/* the playlist: the music files of a folder */
static char pl_dir[PATH_MAX_LEN];
static dirlist_t pl;
static int pl_index = -1;
static int *pl_order;     /* shuffle */
static int pl_pos;        /* in pl_order */

static enum { SCR_BROWSER, SCR_PLAYING } screen;
static bool paused;
static int mode = MP_MODE_REPEAT_ALL;
static int screen_off_s;
static bool screen_on = true;
static uint32_t last_input_ms;
static volatile bool track_ended;
static char toast[80];
static uint32_t toast_until;
static bool dirty = true;
static uint32_t last_draw_ms;

static int16_t scope[SCOPE_LEN];
static int scope_pos;

/* savestate waiting to be loaded */
static struct {
    uint32_t magic;
    uint32_t locator;
    uint8_t mode, paused, screen, reserved;
    char path[PATH_MAX_LEN];
} pending;
static bool have_pending;

static uint32_t rng = 0x12345678u;
static uint32_t xrand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ------------------------------------------------------------------ */
/* Folders                                                            */
/* ------------------------------------------------------------------ */
static const char *ent_name(const dirlist_t *d, int i) { return d->pool + d->e[i].name; }

static void dir_clear(dirlist_t *d)
{
    d->count = 0;
    d->pool_len = 0;
}

static void dir_free(dirlist_t *d)
{
    mp_free(d->e);
    mp_free(d->pool);
    memset(d, 0, sizeof(*d));
}

static bool dir_add(dirlist_t *d, const char *name, bool is_dir, uint32_t size)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 64;
        entry_t *e = mp_realloc(d->e, (size_t)cap * sizeof(entry_t));
        if (!e)
            return false;
        d->e = e;
        d->cap = cap;
    }
    uint32_t len = (uint32_t)strlen(name) + 1;
    if (d->pool_len + len > d->pool_cap) {
        uint32_t cap = d->pool_cap ? d->pool_cap * 2 : 2048;
        while (cap < d->pool_len + len)
            cap *= 2;
        char *p = mp_realloc(d->pool, cap);
        if (!p)
            return false;
        d->pool = p;
        d->pool_cap = cap;
    }
    memcpy(d->pool + d->pool_len, name, len);
    d->e[d->count].name = d->pool_len;
    d->e[d->count].size = size;
    d->e[d->count].is_dir = is_dir;
    d->pool_len += len;
    d->count++;
    return true;
}

static void list_cb(void *arg, const char *name, bool is_dir, uint32_t size)
{
    dirlist_t *d = arg;
    if (name[0] == '.')
        return;
    if (is_dir || mp_player_is_music(name))
        dir_add(d, name, is_dir, size);
}

/* folders first, then names without regard to case */
static const dirlist_t *sort_list;
static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    const char *s = sort_list->pool + x->name, *t = sort_list->pool + y->name;
    for (;; s++, t++) {
        int c = tolower((unsigned char)*s) - tolower((unsigned char)*t);
        if (c || !*s)
            return c;
    }
}

static bool read_dir(dirlist_t *d, const char *path)
{
    dir_clear(d);
    bool ok = mp_plat_list_dir(path, list_cb, d);
    sort_list = d;
    if (d->count > 1)
        qsort(d->e, (size_t)d->count, sizeof(entry_t), cmp_entry);
    return ok;
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

static void open_dir(const char *path, const char *select_name)
{
    snprintf(cwd, sizeof(cwd), "%s", path);
    bool ok = read_dir(&list, cwd);
    root_missing = !ok && !strcmp(cwd, mp_plat_music_root());
    sel = top = 0;
    if (select_name) {
        for (int i = 0; i < list.count; i++)
            if (!strcmp(ent_name(&list, i), select_name)) {
                sel = i;
                break;
            }
    }
    if (sel >= LIST_ROWS)
        top = sel - LIST_ROWS / 2;
    dirty = true;
}

static bool at_root(void)
{
    return !strcmp(cwd, mp_plat_music_root());
}

static void go_parent(void)
{
    if (at_root())
        return;
    char name[PATH_MAX_LEN];
    char *slash = strrchr(cwd, '/');
    if (!slash)
        return;
    snprintf(name, sizeof(name), "%s", slash + 1);
    char parent[PATH_MAX_LEN];
    snprintf(parent, sizeof(parent), "%.*s", (int)(slash - cwd), cwd);
    if (!parent[0])
        snprintf(parent, sizeof(parent), "/");
    open_dir(parent, name);
}

/* ------------------------------------------------------------------ */
/* Playlist                                                           */
/* ------------------------------------------------------------------ */
static void shuffle(void)
{
    if (!pl_order)
        return;
    for (int i = 0; i < pl.count; i++)
        pl_order[i] = i;
    for (int i = pl.count - 1; i > 0; i--) {
        int j = (int)(xrand() % (uint32_t)(i + 1));
        int t = pl_order[i];
        pl_order[i] = pl_order[j];
        pl_order[j] = t;
    }
}

/* the playlist of the folder `dir` (its music files) */
static bool load_playlist(const char *dir)
{
    if (pl.count && !strcmp(pl_dir, dir))
        return true;
    snprintf(pl_dir, sizeof(pl_dir), "%s", dir);
    read_dir(&pl, dir);
    /* files only */
    int n = 0;
    for (int i = 0; i < pl.count; i++)
        if (!pl.e[i].is_dir)
            pl.e[n++] = pl.e[i];
    pl.count = n;
    mp_free(pl_order);
    pl_order = pl.count ? mp_malloc((size_t)pl.count * sizeof(int)) : NULL;
    shuffle();
    pl_index = -1;
    return pl.count > 0;
}

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_until = mp_plat_time_ms() + TOAST_MS;
    dirty = true;
}

static void draw_progress(uint32_t done, uint32_t total);

static int error_text(int r)
{
    switch (r) {
    case MP_ERR_FILE: return T_ERR_FILE;
    case MP_ERR_MEMORY: return T_ERR_MEMORY;
    case MP_ERR_FLASH: return T_ERR_FLASH;
    default: return T_ERR_FORMAT;
    }
}

/* plays track `index` of the playlist */
static bool play_index(int index)
{
    if (index < 0 || index >= pl.count)
        return false;
    char path[PATH_MAX_LEN * 2];
    join(path, sizeof(path), pl_dir, ent_name(&pl, index));
    mp_player_close();
    track_ended = false;
    int r = mp_player_open(path, draw_progress);
    pl_index = index;
    if (pl_order)
        for (int i = 0; i < pl.count; i++)
            if (pl_order[i] == index)
                pl_pos = i;
    if (r != MP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: %.40s", T(error_text(r)), ent_name(&pl, index));
        show_toast(msg);
        mp_plat_log("mp: %s: error %d\n", path, r);
        return false;
    }
    mp_plat_set_audio_rate(mp_player_rate());
    paused = false;
    dirty = true;
    mp_plat_log("mp: playing %s (%s, %lu ms)\n", path, mp_player_info()->format,
                (unsigned long)mp_player_info()->duration_ms);
    return true;
}

/* the next / previous track for the mode; false: the end (play once) */
static bool step_track(int dir, bool user)
{
    if (!pl.count)
        return false;
    for (int tries = 0; tries < pl.count; tries++) {
        int next;
        if (mode == MP_MODE_SHUFFLE && pl_order) {
            int p = pl_pos + dir;
            if (p >= pl.count) {
                shuffle();
                p = 0;
            } else if (p < 0) {
                p = pl.count - 1;
            }
            pl_pos = p;
            next = pl_order[p];
        } else {
            next = (pl_index < 0 ? 0 : pl_index) + dir;
            if (next >= pl.count) {
                if (mode == MP_MODE_ONCE && !user)
                    return false;
                next = 0;
            } else if (next < 0) {
                next = pl.count - 1;
            }
        }
        if (play_index(next))
            return true;
        pl_index = next; /* skip the broken one */
        if (mode == MP_MODE_SHUFFLE)
            continue;
    }
    return false;
}

static void stop_playing(void)
{
    mp_player_close();
    paused = false;
    dirty = true;
}

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */
static void settings_path(char *out, size_t size)
{
    snprintf(out, size, "%s/settings.bin", mp_plat_save_dir());
}

static void settings_save(void)
{
    char path[PATH_MAX_LEN];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t s[3] = { SETTINGS_MAGIC, (uint32_t)mode, (uint32_t)screen_off_s };
    fwrite(s, sizeof(s), 1, f);
    fclose(f);
}

static void settings_load(void)
{
    char path[PATH_MAX_LEN];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    uint32_t s[3];
    if (fread(s, sizeof(s), 1, f) == 1 && s[0] == SETTINGS_MAGIC && s[1] < MP_MODE_COUNT) {
        mode = (int)s[1];
        screen_off_s = (int)s[2];
    }
    fclose(f);
}

int mp_core_get_mode(void) { return mode; }
void mp_core_set_mode(int m)
{
    if (m < 0 || m >= MP_MODE_COUNT || m == mode)
        return;
    mode = m;
    if (mode == MP_MODE_SHUFFLE)
        shuffle();
    settings_save();
    dirty = true;
}
int mp_core_get_screen_off(void) { return screen_off_s; }
void mp_core_set_screen_off(int s)
{
    screen_off_s = s;
    settings_save();
}

void mp_core_wake(void)
{
    last_input_ms = mp_plat_time_ms();
    if (!screen_on) {
        screen_on = true;
        mp_plat_screen(true);
    }
    dirty = true;
}

/* ------------------------------------------------------------------ */
/* Savestates                                                         */
/* ------------------------------------------------------------------ */
bool mp_core_save_state(const char *path)
{
    if (!mp_player_is_open() || pl_index < 0)
        return false;
    memset(&pending, 0, sizeof(pending));
    pending.magic = STATE_MAGIC;
    pending.locator = mp_player_locator();
    pending.mode = (uint8_t)mode;
    pending.paused = paused;
    pending.screen = (uint8_t)screen;
    join(pending.path, sizeof(pending.path), pl_dir, ent_name(&pl, pl_index));
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(&pending, sizeof(pending), 1, f) == 1;
    fclose(f);
    return ok;
}

bool mp_core_load_state(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = fread(&pending, sizeof(pending), 1, f) == 1 && pending.magic == STATE_MAGIC;
    fclose(f);
    pending.path[sizeof(pending.path) - 1] = '\0';
    have_pending = ok;
    return ok;
}

static void apply_pending(void)
{
    have_pending = false;
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s", pending.path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    const char *name = slash + 1;
    if (pending.mode < MP_MODE_COUNT)
        mode = pending.mode;
    pl.count = 0; /* read the folder again */
    open_dir(dir, name);
    if (!load_playlist(dir))
        return;
    for (int i = 0; i < pl.count; i++) {
        if (!strcmp(ent_name(&pl, i), name)) {
            if (play_index(i)) {
                mp_player_relocate(pending.locator);
                paused = pending.paused;
                screen = pending.screen ? SCR_PLAYING : SCR_BROWSER;
            }
            break;
        }
    }
    dirty = true;
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */
void mp_core_audio_render(int16_t *out, int frames)
{
    int n = 0;
    if (mp_player_is_open() && !paused && !track_ended &&
        mp_player_rate() == mp_plat_audio_rate()) {
        n = mp_player_render(out, frames, mode == MP_MODE_REPEAT_ONE);
        if (n < frames)
            track_ended = true;
    }
    if (n < frames)
        memset(out + n, 0, (size_t)(frames - n) * sizeof(int16_t));
    /* the oscilloscope: the last samples */
    int step = frames > SCOPE_LEN ? frames - SCOPE_LEN : 0;
    for (int i = step; i < frames; i++) {
        scope[scope_pos] = out[i];
        scope_pos = (scope_pos + 1) % SCOPE_LEN;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */
static void fmt_time(char *out, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else
        snprintf(out, size, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static int mode_icon(void)
{
    static const int icons[MP_MODE_COUNT] = { ICON_REPEAT_ALL, ICON_REPEAT_ONE, ICON_SHUFFLE, ICON_ONCE };
    return icons[mode];
}

static void draw_header(const char *title)
{
    mp_fill(0, 0, MP_WIDTH, 15, C_PANEL);
    mp_hline(0, 15, MP_WIDTH, C_LINE);
    mp_text(6, 4, title, C_TEXT, 240);
    mp_icon(MP_WIDTH - 14, 4, mode_icon(), C_ACCENT);
    if (mp_player_is_open())
        mp_icon(MP_WIDTH - 28, 4, paused ? ICON_PAUSE : ICON_PLAY, C_GREEN);
}

static void draw_bar(int x, int y, int w, int h, uint32_t pos, uint32_t total)
{
    mp_fill(x, y, w, h, C_LINE);
    if (total) {
        uint32_t fill = pos >= total ? (uint32_t)w : (uint32_t)((uint64_t)pos * (uint32_t)w / total);
        mp_fill(x, y, (int)fill, h, C_ACCENT);
    }
}

static void draw_footer(void)
{
    mp_fill(0, FOOTER_Y, MP_WIDTH, MP_HEIGHT - FOOTER_Y, C_PANEL);
    mp_hline(0, FOOTER_Y, MP_WIDTH, C_LINE);
    if (!mp_player_is_open()) {
        mp_text(6, FOOTER_Y + 9, T(T_HELP_BROWSER), C_DIM, 308);
        return;
    }
    const mp_track_info_t *ti = mp_player_info();
    char t[24];
    fmt_time(t, sizeof(t), mp_player_position_ms());
    mp_icon(6, FOOTER_Y + 5, paused ? ICON_PAUSE : ICON_PLAY, C_GREEN);
    int tx = MP_WIDTH - 6 - mp_text_width(t);
    mp_text(18, FOOTER_Y + 5, ti->title, C_TEXT, tx - 26);
    mp_text(tx, FOOTER_Y + 5, t, C_DIM, 0);
    draw_bar(6, FOOTER_Y + 17, MP_WIDTH - 12, 3, mp_player_position_ms(), ti->duration_ms);
}

static void draw_browser(void)
{
    mp_fill(0, 0, MP_WIDTH, MP_HEIGHT, C_BG);
    /* the folder, shown from the music root */
    const char *rel = cwd + strlen(mp_plat_music_root());
    char head[PATH_MAX_LEN + 8];
    snprintf(head, sizeof(head), "%s%s", "/music", rel);
    draw_header(head);
    if (root_missing || list.count == 0) {
        const char *msg = root_missing ? T(T_NO_FOLDER) : T(T_EMPTY);
        mp_text((MP_WIDTH - mp_text_width(msg)) / 2, 100, msg, C_DIM, 316);
    }
    if (sel < top)
        top = sel;
    if (sel >= top + LIST_ROWS)
        top = sel - LIST_ROWS + 1;
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = top + r;
        if (i >= list.count)
            break;
        int y = LIST_Y + r * ROW_H;
        bool is_sel = i == sel;
        if (is_sel)
            mp_fill(0, y, MP_WIDTH - 6, ROW_H, C_SELECT);
        const entry_t *e = &list.e[i];
        const char *name = ent_name(&list, i);
        bool playing = !e->is_dir && mp_player_is_open() && pl_index >= 0 &&
                       !strcmp(pl_dir, cwd) && !strcmp(ent_name(&pl, pl_index), name);
        mp_icon(6, y + 2, e->is_dir ? ICON_FOLDER : (playing ? ICON_PLAY : ICON_NOTE),
                e->is_dir ? C_YELLOW : (playing ? C_GREEN : C_DIM));
        mp_text(18, y + 2, name, playing ? C_ACCENT : C_TEXT, MP_WIDTH - 30);
    }
    /* scroll bar */
    if (list.count > LIST_ROWS) {
        int h = LIST_ROWS * ROW_H;
        int bh = h * LIST_ROWS / list.count;
        if (bh < 6)
            bh = 6;
        int by = LIST_Y + (h - bh) * top / (list.count - LIST_ROWS);
        mp_fill(MP_WIDTH - 4, LIST_Y, 3, h, C_PANEL);
        mp_fill(MP_WIDTH - 4, by, 3, bh, C_DIM);
    }
    draw_footer();
}

static void draw_scope(int x, int y, int w, int h)
{
    mp_fill(x, y, w, h, C_BLACK);
    mp_hline(x, y + h / 2, w, C_LINE);
    int prev = -1;
    int start = scope_pos;
    for (int i = 0; i < w; i++) {
        int s = scope[(start + i * SCOPE_LEN / w) % SCOPE_LEN];
        int v = y + h / 2 - s * (h / 2 - 1) / 32768;
        if (prev >= 0) {
            int a = prev < v ? prev : v, b = prev < v ? v : prev;
            mp_vline(x + i, a, b - a + 1, C_SCOPE);
        } else {
            mp_pset(x + i, v, C_SCOPE);
        }
        prev = v;
    }
}

static void draw_playing(void)
{
    mp_fill(0, 0, MP_WIDTH, MP_HEIGHT, C_BG);
    char head[64];
    if (pl_index >= 0)
        snprintf(head, sizeof(head), "%s  %d/%d", T(T_NOW_PLAYING), pl_index + 1, pl.count);
    else
        snprintf(head, sizeof(head), "%s", T(T_NOW_PLAYING));
    draw_header(head);
    if (!mp_player_is_open()) {
        mp_text((MP_WIDTH - mp_text_width(T(T_NOTHING))) / 2, 110, T(T_NOTHING), C_DIM, 0);
        return;
    }
    const mp_track_info_t *ti = mp_player_info();
    mp_text2(8, 24, ti->title, C_TEXT, 304);
    int y = 44;
    if (ti->artist[0]) {
        mp_text(8, y, ti->artist, C_ACCENT, 304);
        y += 11;
    }
    int fx = mp_text(8, y, ti->format, C_DIM, 290);
    if (ti->in_flash)
        mp_icon(fx + 4, y, ICON_FLASH, C_YELLOW);

    mp_module_status_t st;
    bool mod = mp_player_module_status(&st);
    int scope_h = mod ? 62 : 104;
    draw_scope(8, 70, MP_WIDTH - 16, scope_h);
    mp_rect(7, 69, MP_WIDTH - 14, scope_h + 2, C_LINE);

    if (mod) {
        /* VU meters */
        int vy = 138, vh = 40;
        int n = st.nch ? st.nch : 1;
        int bw = (MP_WIDTH - 16) / n;
        if (bw > 24)
            bw = 24;
        int x0 = (MP_WIDTH - bw * n) / 2;
        for (int i = 0; i < st.nch; i++) {
            /* falls slowly, rises at once */
            static uint8_t shown[32];
            int v = st.vu[i] * 7 / 8;
            shown[i] = v >= shown[i] ? (uint8_t)v : (uint8_t)(shown[i] > 3 ? shown[i] - 3 : 0);
            int lvl = shown[i] * vh / 64;
            int x = x0 + i * bw;
            mp_fill(x, vy, bw - 1, vh, C_PANEL);
            for (int k = 0; k < lvl; k++) {
                int c = k > vh * 3 / 4 ? C_RED : (k > vh / 2 ? C_YELLOW : C_GREEN);
                mp_hline(x, vy + vh - 1 - k, bw - 1, c);
            }
        }
        char s[96];
        snprintf(s, sizeof(s), "%s %d/%d  %s %d/%d  %s %d/%d", T(T_POS), st.pos + 1, st.npos,
                 T(T_ROW), st.row, st.nrows, T(T_TEMPO), st.speed, st.bpm);
        mp_text(8, 184, s, C_DIM, 304);
    }

    char a[24], b[24];
    uint32_t pos = mp_player_position_ms();
    fmt_time(a, sizeof(a), pos);
    if (ti->duration_ms)
        fmt_time(b, sizeof(b), ti->duration_ms);
    else
        snprintf(b, sizeof(b), "--:--");
    draw_bar(8, 198, MP_WIDTH - 16, 4, pos, ti->duration_ms);
    mp_text(8, 206, a, C_TEXT, 0);
    mp_text(MP_WIDTH - 8 - mp_text_width(b), 206, b, C_DIM, 0);
    if (paused) {
        const char *p = T(T_PAUSED);
        mp_text((MP_WIDTH - mp_text_width(p)) / 2, 206, p, C_ACCENT, 0);
    }
    mp_hline(0, 221, MP_WIDTH, C_LINE);
    mp_text(6, 227, T(T_HELP_PLAYING), C_DIM, 308);
}

static void draw_toast(void)
{
    if (!toast[0])
        return;
    if ((int32_t)(mp_plat_time_ms() - toast_until) > 0) {
        toast[0] = '\0';
        return;
    }
    int w = mp_text_width(toast) + 16;
    if (w > MP_WIDTH - 8)
        w = MP_WIDTH - 8;
    int x = (MP_WIDTH - w) / 2, y = 96;
    mp_fill(x, y, w, 22, C_PANEL);
    mp_rect(x, y, w, 22, C_ACCENT);
    mp_text(x + 8, y + 7, toast, C_TEXT, w - 16);
}

static void present(void)
{
    mp_plat_present(canvas, mp_palette, C_COUNT);
}

static void redraw(void)
{
    if (screen == SCR_PLAYING)
        draw_playing();
    else
        draw_browser();
    draw_toast();
    present();
    dirty = false;
    last_draw_ms = mp_plat_time_ms();
}

/* harness: plays a file (path from the music root) */
bool mp_core_play_path(const char *path)
{
    snprintf(pending.path, sizeof(pending.path), "%s", path);
    pending.locator = 0;
    pending.mode = (uint8_t)mode;
    pending.paused = 0;
    pending.screen = 1;
    apply_pending();
    return mp_player_is_open();
}

const uint8_t *mp_core_canvas(const uint32_t **palette, int *ncolors)
{
    if (screen == SCR_PLAYING)
        draw_playing();
    else
        draw_browser();
    *palette = mp_palette;
    *ncolors = C_COUNT;
    return canvas;
}

/* the copy of the samples of a big module to the flash */
static void draw_progress(uint32_t done, uint32_t total)
{
    mp_fill(0, 0, MP_WIDTH, MP_HEIGHT, C_BG);
    draw_header(T(T_TITLE));
    const char *a = T(T_COPY), *b = T(T_COPY2);
    mp_text((MP_WIDTH - mp_text_width(a)) / 2, 90, a, C_TEXT, 316);
    mp_text((MP_WIDTH - mp_text_width(b)) / 2, 104, b, C_DIM, 316);
    mp_rect(39, 124, MP_WIDTH - 78, 12, C_LINE);
    draw_bar(41, 126, MP_WIDTH - 82, 8, done, total);
    char s[32];
    snprintf(s, sizeof(s), "%lu%%  %lu KB", (unsigned long)(total ? (uint64_t)done * 100 / total : 0),
             (unsigned long)(total / 1024));
    mp_text((MP_WIDTH - mp_text_width(s)) / 2, 144, s, C_DIM, 0);
    present();
}

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */
static uint32_t held_prev;
static uint32_t repeat_btn, repeat_next;

/* pressed now, or repeating while held (dirs given) */
static uint32_t pressed_buttons(uint32_t now, uint32_t repeat_mask, uint32_t rate)
{
    uint32_t held = mp_plat_buttons();
    uint32_t pressed = held & ~held_prev;
    held_prev = held;
    if (pressed & repeat_mask) {
        repeat_btn = pressed & repeat_mask;
        repeat_next = now + REPEAT_DELAY;
    } else if (repeat_btn && (held & repeat_btn) == repeat_btn && (int32_t)(now - repeat_next) >= 0) {
        pressed |= repeat_btn;
        repeat_next = now + rate;
    } else if (!(held & repeat_btn)) {
        repeat_btn = 0;
    }
    return pressed;
}

static void cycle_mode(void)
{
    mp_core_set_mode((mode + 1) % MP_MODE_COUNT);
    show_toast(T(T_MODE_ALL + mode));
}

static void browser_input(uint32_t p)
{
    if (p & MP_BTN_UP) {
        if (list.count)
            sel = sel > 0 ? sel - 1 : list.count - 1;
        dirty = true;
    }
    if (p & MP_BTN_DOWN) {
        if (list.count)
            sel = sel < list.count - 1 ? sel + 1 : 0;
        dirty = true;
    }
    if (p & MP_BTN_LEFT) {
        sel = sel - LIST_ROWS < 0 ? 0 : sel - LIST_ROWS;
        dirty = true;
    }
    if (p & MP_BTN_RIGHT) {
        sel = sel + LIST_ROWS >= list.count ? (list.count ? list.count - 1 : 0) : sel + LIST_ROWS;
        dirty = true;
    }
    if ((p & MP_BTN_A) && sel < list.count) {
        const entry_t *e = &list.e[sel];
        char path[PATH_MAX_LEN * 2];
        join(path, sizeof(path), cwd, ent_name(&list, sel));
        if (e->is_dir) {
            if (strlen(path) < PATH_MAX_LEN - 1)
                open_dir(path, NULL);
        } else {
            const char *name = ent_name(&list, sel);
            pl.count = strcmp(pl_dir, cwd) ? 0 : pl.count;
            if (load_playlist(cwd)) {
                for (int i = 0; i < pl.count; i++)
                    if (!strcmp(ent_name(&pl, i), name)) {
                        if (play_index(i))
                            screen = SCR_PLAYING;
                        break;
                    }
            }
        }
        dirty = true;
    }
    if (p & MP_BTN_B)
        go_parent();
    if ((p & MP_BTN_GAME) && mp_player_is_open()) {
        screen = SCR_PLAYING;
        dirty = true;
    }
    if (p & MP_BTN_TIME)
        cycle_mode();
}

static void playing_input(uint32_t p)
{
    if ((p & MP_BTN_A) && mp_player_is_open()) {
        paused = !paused;
        dirty = true;
    }
    if (p & MP_BTN_RIGHT)
        step_track(1, true);
    if (p & MP_BTN_LEFT) {
        if (mp_player_is_open() && mp_player_position_ms() > 3000) {
            play_index(pl_index); /* the start of this track */
        } else {
            step_track(-1, true);
        }
    }
    if ((p & MP_BTN_UP) && mp_player_is_open()) {
        mp_player_seek(1);
        dirty = true;
    }
    if ((p & MP_BTN_DOWN) && mp_player_is_open()) {
        mp_player_seek(-1);
        dirty = true;
    }
    if (p & (MP_BTN_B | MP_BTN_GAME)) {
        screen = SCR_BROWSER;
        /* show the track playing */
        if (pl_index >= 0 && !strcmp(pl_dir, cwd)) {
            for (int i = 0; i < list.count; i++)
                if (!strcmp(ent_name(&list, i), ent_name(&pl, pl_index)))
                    sel = i;
        }
        dirty = true;
    }
    if (p & MP_BTN_TIME)
        cycle_mode();
}

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */
void mp_core_run(void)
{
    lang = mp_plat_language() == 1 ? 1 : 0;
    canvas = mp_malloc(MP_WIDTH * MP_HEIGHT);
    if (!canvas) {
        mp_plat_log("mp: no memory for the screen\n");
        for (;;)
            mp_plat_service(true);
    }
    mp_draw_set_canvas(canvas);
    settings_load();
    rng ^= mp_plat_time_ms() * 2654435761u;
    open_dir(mp_plat_music_root(), NULL);
    last_input_ms = mp_plat_time_ms();
    held_prev = mp_plat_buttons();

    for (;;) {
        mp_plat_service(!dirty && !track_ended && !have_pending);
        uint32_t now = mp_plat_time_ms();

        if (have_pending)
            apply_pending();

        if (track_ended) {
            track_ended = false;
            if (!step_track(1, false))
                stop_playing();
        }

        uint32_t repeat = screen == SCR_BROWSER ? (MP_BTN_UP | MP_BTN_DOWN | MP_BTN_LEFT | MP_BTN_RIGHT)
                                                : (MP_BTN_UP | MP_BTN_DOWN);
        uint32_t p = pressed_buttons(now, repeat, screen == SCR_BROWSER ? REPEAT_RATE : SEEK_RATE);
        if (mp_plat_buttons())
            last_input_ms = now;
        if (p) {
            if (!screen_on) {
                mp_core_wake(); /* this press only turns the screen on */
                continue;
            }
            if (screen == SCR_BROWSER)
                browser_input(p);
            else
                playing_input(p);
        }

        /* the screen turns off while the music plays */
        if (screen_on && screen_off_s && mp_player_is_open() && !paused &&
            now - last_input_ms > (uint32_t)screen_off_s * 1000u) {
            screen_on = false;
            mp_plat_screen(false);
        }
        if (!screen_on)
            continue;

        bool live = mp_player_is_open() && !paused;
        uint32_t period = live ? (screen == SCR_PLAYING ? 40 : 250) : 500;
        if (dirty || now - last_draw_ms >= period || (toast[0] && (int32_t)(now - toast_until) > 0))
            redraw();
    }
}
