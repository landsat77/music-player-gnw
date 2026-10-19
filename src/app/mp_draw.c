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

void mp_pset(int x, int y, int c)
{
    if ((unsigned)x < MP_WIDTH && (unsigned)y < MP_HEIGHT)
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

static int text_scaled(int x, int y, const char *s, int c, int maxw, int scale)
{
    int cw = 8 * scale;
    int n = (int)strlen(s);
    int fit = maxw > 0 ? maxw / cw : n;
    bool cut = n > fit;
    int shown = cut ? (fit >= 2 ? fit - 2 : 0) : n;
    for (int i = 0; i < shown; i++)
        glyph(x + i * cw, y, (unsigned char)s[i], c, scale);
    x += shown * cw;
    if (cut && fit >= 2) {
        glyph(x, y, '.', c, scale);
        glyph(x + cw, y, '.', c, scale);
        x += 2 * cw;
    }
    return x;
}

int mp_text(int x, int y, const char *s, int c, int maxw) { return text_scaled(x, y, s, c, maxw, 1); }
int mp_text2(int x, int y, const char *s, int c, int maxw) { return text_scaled(x, y, s, c, maxw, 2); }

void mp_icon(int x, int y, int icon, int c)
{
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (icons[icon][r] & (0x80 >> b))
                mp_pset(x + b, y + r, c);
}
