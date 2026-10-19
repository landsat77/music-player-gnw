/*
 * Drawing of the player: an 8 bit canvas (palette indexes, MP_WIDTH x
 * MP_HEIGHT), the 8x8 font (Latin-1), a few 8x8 icons.
 */
#include <string.h>

#include "mp_platform.h"
#include "mp_draw.h"

extern const uint8_t mp_font8x8[256][8];

const uint32_t mp_palette[C_COUNT] = {
    [C_BG]     = 0x0f1418,
    [C_PANEL]  = 0x1d252e,
    [C_TEXT]   = 0xe9eef2,
    [C_DIM]    = 0x8996a3,
    [C_ACCENT] = 0xffb347,
    [C_SELECT] = 0x2c4a68,
    [C_GREEN]  = 0x58c45c,
    [C_YELLOW] = 0xffd54f,
    [C_RED]    = 0xef5350,
    [C_SCOPE]  = 0x6fd3ff,
    [C_BLACK]  = 0x000000,
    [C_LINE]   = 0x33404d,
};

/* 8x8 icons, row by row, bit 7 = leftmost pixel */
static const uint8_t icons[ICON_COUNT][8] = {
    [ICON_FOLDER]     = { 0x00, 0x70, 0x8f, 0x81, 0x81, 0x81, 0xff, 0x00 },
    [ICON_NOTE]       = { 0x0f, 0x09, 0x09, 0x09, 0x09, 0x69, 0xf6, 0x60 },
    [ICON_PLAY]       = { 0x40, 0x60, 0x70, 0x78, 0x78, 0x70, 0x60, 0x40 },
    [ICON_PAUSE]      = { 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00 },
    [ICON_UP]         = { 0x10, 0x38, 0x7c, 0xfe, 0x38, 0x38, 0x38, 0x00 },
    [ICON_REPEAT_ALL] = { 0x00, 0x7e, 0x81, 0x85, 0x8e, 0x84, 0x7c, 0x00 },
    [ICON_REPEAT_ONE] = { 0x08, 0x76, 0x8a, 0x82, 0x82, 0x82, 0x7c, 0x00 },
    [ICON_SHUFFLE]    = { 0x00, 0xc6, 0x2e, 0x10, 0x10, 0x2e, 0xc6, 0x00 },
    [ICON_ONCE]       = { 0x00, 0x00, 0x08, 0x0c, 0xfe, 0x0c, 0x08, 0x00 },
    [ICON_FLASH]      = { 0x0c, 0x18, 0x30, 0x7c, 0x18, 0x30, 0x60, 0x00 },
};

static uint8_t *cv;

void mp_draw_set_canvas(uint8_t *canvas) { cv = canvas; }
uint8_t *mp_draw_canvas(void) { return cv; }

/* horizontal clip of the text (scrolling) */
static int clip_x0 = 0, clip_x1 = MP_WIDTH;

void mp_pset(int x, int y, int c)
{
    if (x >= clip_x0 && x < clip_x1 && (unsigned)x < MP_WIDTH && (unsigned)y < MP_HEIGHT)
        cv[y * MP_WIDTH + x] = (uint8_t)c;
}

void mp_fill(int x, int y, int w, int h, int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > MP_WIDTH) w = MP_WIDTH - x;
    if (y + h > MP_HEIGHT) h = MP_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;
    for (int j = 0; j < h; j++)
        memset(cv + (y + j) * MP_WIDTH + x, c, (size_t)w);
}

void mp_hline(int x, int y, int w, int c) { mp_fill(x, y, w, 1, c); }
void mp_vline(int x, int y, int h, int c) { mp_fill(x, y, 1, h, c); }

void mp_rect(int x, int y, int w, int h, int c)
{
    mp_hline(x, y, w, c);
    mp_hline(x, y + h - 1, w, c);
    mp_vline(x, y, h, c);
    mp_vline(x + w - 1, y, h, c);
}

static void glyph(int x, int y, unsigned ch, int c, int scale)
{
    const uint8_t *g = mp_font8x8[ch & 0xff];
    if (x + 8 * scale <= clip_x0 || x >= clip_x1)
        return;
    if (scale > 1 && (x < clip_x0 || x + 8 * scale > clip_x1)) {
        /* partly visible: pixel by pixel through the clip */
        for (int r = 0; r < 8 * scale; r++)
            for (int b = 0; b < 8 * scale; b++)
                if (g[r / scale] & (1u << (b / scale)))
                    mp_pset(x + b, y + r, c);
        return;
    }
    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        if (!bits)
            continue;
        for (int b = 0; b < 8; b++) {
            if (bits & (1u << b)) {
                if (scale == 1)
                    mp_pset(x + b, y + r, c);
                else
                    mp_fill(x + b * scale, y + r * scale, scale, scale, c);
            }
        }
    }
}

int mp_text_width(const char *s)
{
    return (int)strlen(s) * 8;
}

/* Scrolling texts: one slot per place (x, y, scale) on the screen; its
 * clock starts again when the text there changes, or when it was not drawn
 * for a while (back to the same row of the browser). */
#define SCROLL_SLOTS 12
#define SCROLL_WAIT_MS 1500  /* still at each end */
#define SCROLL_FORGET_MS 300 /* not drawn for that long: start again */
static struct {
    int16_t x, y;
    uint8_t scale, used;
    uint32_t hash, t0, seen;
} slots[SCROLL_SLOTS];
static uint32_t draw_now;
static bool scrolled, scrolled_last;

void mp_draw_begin(uint32_t now_ms)
{
    draw_now = now_ms;
    scrolled_last = scrolled;
    scrolled = false;
}

bool mp_draw_scrolling(void) { return scrolled || scrolled_last; }

static uint32_t text_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* how far the text is scrolled now (pixels), `over`: what does not fit */
static int scroll_offset(int x, int y, int scale, const char *s, int over)
{
    uint32_t h = text_hash(s);
    int k = -1, oldest = 0;
    for (int i = 0; i < SCROLL_SLOTS; i++) {
        if (slots[i].used && slots[i].x == x && slots[i].y == y && slots[i].scale == scale) {
            k = i;
            break;
        }
        if (!slots[i].used || slots[i].seen < slots[oldest].seen)
            oldest = i;
    }
    if (k < 0) {
        k = oldest;
        slots[k].used = 1;
        slots[k].x = (int16_t)x;
        slots[k].y = (int16_t)y;
        slots[k].scale = (uint8_t)scale;
        slots[k].hash = h + 1; /* below: a new start */
    }
    if (slots[k].hash != h || draw_now - slots[k].seen > SCROLL_FORGET_MS) {
        slots[k].hash = h;
        slots[k].t0 = draw_now;
    }
    slots[k].seen = draw_now;
    scrolled = true;

    int speed = 30 * scale; /* pixels per second */
    uint32_t run = (uint32_t)over * 1000u / (uint32_t)speed;
    uint32_t cycle = SCROLL_WAIT_MS + run + SCROLL_WAIT_MS;
    uint32_t t = (draw_now - slots[k].t0) % cycle;
    if (t < SCROLL_WAIT_MS)
        return 0;
    t -= SCROLL_WAIT_MS;
    if (t >= run)
        return over;
    return (int)(t * (uint32_t)speed / 1000u);
}

static int text_scaled(int x, int y, const char *s, int c, int maxw, int scale, bool scroll)
{
    int cw = 8 * scale;
    int n = (int)strlen(s);
    int w = n * cw;
    if (maxw <= 0 || w <= maxw) {
        for (int i = 0; i < n; i++)
            glyph(x + i * cw, y, (unsigned char)s[i], c, scale);
        return x + w;
    }
    /* too long: cut at maxw, scrolled from right to left */
    int off = scroll ? scroll_offset(x, y, scale, s, w - maxw) : 0;
    int x0 = clip_x0, x1 = clip_x1;
    clip_x0 = x > x0 ? x : x0;
    clip_x1 = x + maxw < x1 ? x + maxw : x1;
    int first = off / cw;
    for (int i = first; i < n; i++) {
        int gx = x - off + i * cw;
        if (gx >= clip_x1)
            break;
        glyph(gx, y, (unsigned char)s[i], c, scale);
    }
    clip_x0 = x0;
    clip_x1 = x1;
    return x + maxw;
}

int mp_text(int x, int y, const char *s, int c, int maxw) { return text_scaled(x, y, s, c, maxw, 1, true); }
int mp_text2(int x, int y, const char *s, int c, int maxw) { return text_scaled(x, y, s, c, maxw, 2, true); }
int mp_text_cut(int x, int y, const char *s, int c, int maxw) { return text_scaled(x, y, s, c, maxw, 1, false); }

void mp_icon(int x, int y, int icon, int c)
{
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (icons[icon][r] & (0x80 >> b))
                mp_pset(x + b, y + r, c);
}
