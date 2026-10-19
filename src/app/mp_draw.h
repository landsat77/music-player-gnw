/* Drawing on the 8 bit canvas of the player (mp_draw.c) */
#ifndef MP_DRAW_H
#define MP_DRAW_H

#include <stdint.h>
#include <stdbool.h>

/* the palette */
enum {
    C_BG, C_PANEL, C_TEXT, C_DIM, C_ACCENT, C_SELECT, C_GREEN, C_YELLOW,
    C_RED, C_SCOPE, C_BLACK, C_LINE, C_COUNT
};
extern const uint32_t mp_palette[C_COUNT];

/* icons */
enum { ICON_FOLDER, ICON_NOTE, ICON_PLAY, ICON_PAUSE, ICON_UP, ICON_REPEAT_ALL,
       ICON_REPEAT_ONE, ICON_SHUFFLE, ICON_ONCE, ICON_FLASH, ICON_COUNT };

void mp_draw_set_canvas(uint8_t *canvas);
uint8_t *mp_draw_canvas(void);
void mp_fill(int x, int y, int w, int h, int c);
void mp_hline(int x, int y, int w, int c);
void mp_vline(int x, int y, int h, int c);
void mp_rect(int x, int y, int w, int h, int c);
/* Latin-1 text, 8x8 (scale 2: 16x16); returns the x after the text.
 * maxw > 0: a longer text is shown in maxw pixels and scrolls (still for
 * 1.5 s at each end); mp_text_cut() only cuts it. */
int mp_text(int x, int y, const char *s, int c, int maxw);
int mp_text2(int x, int y, const char *s, int c, int maxw);
int mp_text_cut(int x, int y, const char *s, int c, int maxw);
/* before drawing a frame: the time, for the scrolling texts */
void mp_draw_begin(uint32_t now_ms);
/* a text scrolled in this frame or the last one: redraw often */
bool mp_draw_scrolling(void);
int mp_text_width(const char *s);
void mp_icon(int x, int y, int icon, int c);
void mp_pset(int x, int y, int c);

#endif
