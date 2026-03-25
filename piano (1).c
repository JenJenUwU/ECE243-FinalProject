#include <stdlib.h>

// Screen / hardware
#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240
#define PIXEL_BUF_CTRL 0xFF203020

// Colors (RGB565)
#define BLACK       0x0000
#define WHITE       0xFFFF
#define IVORY       0xF79E
#define GRAY        0x8410
#define DARK_BG     0x0861

#define KEY_LIT     0x07FF   // cyan (white key lit)
#define BLACK_LIT   0x07FF   // cyan (black key lit)

#define NOTE_COL_0  0x001F   // blue
#define NOTE_COL_1  0xF81F   // magenta
#define NOTE_COL_2  0x07E0   // green
#define NOTE_COL_3  0xFFE0   // yellow

// 88-key piano layout
#define TOTAL_WHITE_KEYS  52
#define KEY_HEIGHT   40
#define KEY_Y_START  (SCREEN_HEIGHT - KEY_HEIGHT)   // 200

#define BLACK_W      4
#define BLACK_H      ((KEY_HEIGHT * 6) / 10)        // 24

static int KEY_X[TOTAL_WHITE_KEYS + 1];
static int KEY_W[TOTAL_WHITE_KEYS];

void init_key_positions() {
    const int WIDE[8] = {3, 10, 16, 23, 29, 36, 42, 49};
    int wi = 0, x = 0;
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        KEY_X[i] = x;
        int w = 6;
        if (wi < 8 && i == WIDE[wi]) { w = 7; wi++; }
        KEY_W[i] = w;
        x += w;
    }
    KEY_X[TOTAL_WHITE_KEYS] = x;
}

static const int BLACK_PATTERN[TOTAL_WHITE_KEYS] = {
    1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 0,
    0
};

// Black key data
#define TOTAL_BLACK_KEYS  36

typedef struct {
    int bx;           // left pixel of the black key
    int white_left;   // white key index to its left
} BlackKey;

static BlackKey BLACK_KEYS[TOTAL_BLACK_KEYS];
static int      NUM_BLACK_KEYS = 0;

void init_black_keys() {
    NUM_BLACK_KEYS = 0;
    for (int i = 0; i < TOTAL_WHITE_KEYS - 1; i++) {
        if (!BLACK_PATTERN[i]) continue;
        BLACK_KEYS[NUM_BLACK_KEYS].bx         = KEY_X[i + 1] - BLACK_W / 2;
        BLACK_KEYS[NUM_BLACK_KEYS].white_left  = i;
        NUM_BLACK_KEYS++;
    }
}

// Note pool
#define MAX_NOTES   30
#define NOTE_SPEED   2
#define SPAWN_GAP   12
#define NOTE_MIN_H  10
#define NOTE_MAX_H  35

// is_black==0 → white key note, key_index = white key index
// is_black==1 → black key note, key_index = black key index
typedef struct {
    int   active;
    int   x, y;
    int   prev_y;
    int   w, h;
    int   key_index;
    int   is_black;
    short color;
} Note;

Note notes[MAX_NOTES];

int active_keys[TOTAL_WHITE_KEYS];
int prev_active_keys[TOTAL_WHITE_KEYS];
int active_black_keys[TOTAL_BLACK_KEYS];
int prev_active_black_keys[TOTAL_BLACK_KEYS];

// LCG random
static unsigned int rng = 12345;
unsigned int rng_next() {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}
int rng_range(int lo, int hi) {
    return lo + (int)((rng_next() >> 16) % (unsigned)(hi - lo + 1));
}

// Double-buffer
volatile int pixel_buffer_start;

void wait_for_vsync() {
    volatile int *pixel_ctrl_ptr = (volatile int *) PIXEL_BUF_CTRL;
    *pixel_ctrl_ptr = 1;
    volatile int *status_reg = pixel_ctrl_ptr + 3;
    while ((*status_reg & 0x01) != 0);
    pixel_buffer_start = *(pixel_ctrl_ptr + 1);
}

// Drawing primitives
void plot_pixel(int x, int y, short color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    volatile short *addr =
        (volatile short *)(pixel_buffer_start + (y << 10) + (x << 1));
    *addr = color;
}
void draw_rect(int x, int y, int w, int h, short color) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            plot_pixel(i, j, color);
}
void draw_vline(int x, int y, int h, short color) {
    for (int j = y; j < y + h; j++) plot_pixel(x, j, color);
}
void draw_hline(int x, int y, int w, short color) {
    for (int i = x; i < x + w; i++) plot_pixel(i, y, color);
}

// Background helpers
static short bg_color(int y) {
    return (y & 1) ? (short)DARK_BG : (short)(DARK_BG + 0x0020);
}
void erase_rect_bg(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= KEY_Y_START) continue;
        short c = bg_color(j);
        for (int i = x; i < x + w; i++)
            plot_pixel(i, j, c);
    }
}

// Piano drawing
void draw_background() {
    for (int y = 0; y < KEY_Y_START - 4; y++)
        draw_hline(0, y, SCREEN_WIDTH, bg_color(y));
}
void draw_piano_housing() {
    draw_rect(0, KEY_Y_START - 4, SCREEN_WIDTH, 4, BLACK);
}
void draw_white_keys_full() {
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        short color = active_keys[i] ? KEY_LIT : IVORY;
        draw_rect(KEY_X[i] + 1, KEY_Y_START,
                  KEY_W[i] - 1, KEY_HEIGHT - 2, color);
    }
    for (int i = 0; i <= TOTAL_WHITE_KEYS; i++)
        draw_vline(KEY_X[i], KEY_Y_START, KEY_HEIGHT, BLACK);
    draw_hline(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH, BLACK);
}
void draw_black_keys() {
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        short color = active_black_keys[i] ? BLACK_LIT : BLACK;
        int bx = BLACK_KEYS[i].bx;
        draw_rect(bx, KEY_Y_START, BLACK_W, BLACK_H, color);
        if (!active_black_keys[i] && BLACK_W >= 2)
            draw_vline(bx + 1, KEY_Y_START + 1, 2, GRAY);
    }
}
void draw_white_key(int i, short color) {
    draw_rect(KEY_X[i] + 1, KEY_Y_START,
              KEY_W[i] - 1, KEY_HEIGHT - 2, color);
}
void redraw_black_key(int bi) {
    if (bi < 0 || bi >= NUM_BLACK_KEYS) return;
    short color = active_black_keys[bi] ? BLACK_LIT : BLACK;
    int bx = BLACK_KEYS[bi].bx;
    draw_rect(bx, KEY_Y_START, BLACK_W, BLACK_H, color);
    if (!active_black_keys[bi] && BLACK_W >= 2)
        draw_vline(bx + 1, KEY_Y_START + 1, 2, GRAY);
}

// Find black key index by white_left
int find_black_key(int white_left) {
    for (int i = 0; i < NUM_BLACK_KEYS; i++)
        if (BLACK_KEYS[i].white_left == white_left) return i;
    return -1;
}

void draw_static_scene() {
    draw_background();
    draw_piano_housing();
    draw_white_keys_full();
    draw_black_keys();
}

// Note bars
static const short NOTE_COLORS[4] = {
    NOTE_COL_0, NOTE_COL_1, NOTE_COL_2, NOTE_COL_3
};
static int color_cycle = 0;

// Total keys: 52 white + 36 black = 88
#define TOTAL_KEYS  88

void spawn_note() {
    int slot = -1;
    for (int i = 0; i < MAX_NOTES; i++)
        if (!notes[i].active) { slot = i; break; }
    if (slot < 0) return;

    // Pick from all 88 keys: 0..51 = white, 52..87 = black
    int pick = rng_range(0, TOTAL_KEYS - 1);
    int h    = rng_range(NOTE_MIN_H, NOTE_MAX_H);

    notes[slot].active = 1;
    notes[slot].h      = h;
    notes[slot].y      = -h;
    notes[slot].prev_y = -h;
    notes[slot].color  = NOTE_COLORS[color_cycle & 3];
    color_cycle++;

    if (pick < TOTAL_WHITE_KEYS) {
        // White key note — fits strictly inside dividers
        notes[slot].is_black  = 0;
        notes[slot].key_index = pick;
        notes[slot].x         = KEY_X[pick] + 1;
        notes[slot].w         = KEY_W[pick] - 2;
    } else {
        // Black key note
        int bi = pick - TOTAL_WHITE_KEYS;
        notes[slot].is_black  = 1;
        notes[slot].key_index = bi;
        notes[slot].x         = BLACK_KEYS[bi].bx;
        notes[slot].w         = BLACK_W;
    }
}

// Update
void update_notes() {
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        prev_active_keys[i] = active_keys[i];
        active_keys[i]      = 0;
    }
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        prev_active_black_keys[i] = active_black_keys[i];
        active_black_keys[i]      = 0;
    }

    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].active) continue;

        notes[i].prev_y = notes[i].y;
        notes[i].y     += NOTE_SPEED;

        if (notes[i].y >= KEY_Y_START) {
            notes[i].active = 0;
            continue;
        }
        if (notes[i].y + notes[i].h >= KEY_Y_START) {
            if (notes[i].is_black)
                active_black_keys[notes[i].key_index] = 1;
            else
                active_keys[notes[i].key_index] = 1;
        }
    }
}

// Dirty-rect render
void render_dirty() {
    // 1. Erase notes at old positions
    for (int i = 0; i < MAX_NOTES; i++) {
        if (notes[i].prev_y == notes[i].y) continue;

        int old_top = notes[i].prev_y;
        int old_bot = notes[i].prev_y + notes[i].h;
        if (old_top < 0)           old_top = 0;
        if (old_bot > KEY_Y_START) old_bot = KEY_Y_START;
        if (old_top < old_bot)
            erase_rect_bg(notes[i].x, old_top, notes[i].w, old_bot - old_top);
    }

    // 2. Draw notes at new positions
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].active) continue;

        int y_top = notes[i].y;
        int y_bot = notes[i].y + notes[i].h;
        if (y_top < 0)           y_top = 0;
        if (y_bot > KEY_Y_START) y_bot = KEY_Y_START;
        if (y_top >= y_bot)      continue;

        draw_rect(notes[i].x, y_top, notes[i].w, y_bot - y_top, notes[i].color);
    }

    // 3. Redraw white keys whose lit state changed
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        if (active_keys[i] == prev_active_keys[i]) continue;
        short color = active_keys[i] ? KEY_LIT : IVORY;
        draw_white_key(i, color);
        // Restore black keys adjacent to this white key
        int bi_right = find_black_key(i);
        int bi_left  = find_black_key(i - 1);
        if (bi_right >= 0) redraw_black_key(bi_right);
        if (bi_left  >= 0) redraw_black_key(bi_left);
    }

    // 4. Redraw black keys whose lit state changed
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        if (active_black_keys[i] == prev_active_black_keys[i]) continue;
        redraw_black_key(i);
    }
}

// Main
int main(void) {
    volatile int *pixel_ctrl_ptr = (volatile int *) PIXEL_BUF_CTRL;

    *(pixel_ctrl_ptr + 1) = 0x08000000;
    pixel_buffer_start    = *(pixel_ctrl_ptr + 1);

    init_key_positions();
    init_black_keys();

    for (int i = 0; i < MAX_NOTES; i++) {
        notes[i].active   = 0;
        notes[i].y        = 0;
        notes[i].prev_y   = 0;
        notes[i].is_black = 0;
    }
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        active_keys[i]      = 0;
        prev_active_keys[i] = 0;
    }
    for (int i = 0; i < TOTAL_BLACK_KEYS; i++) {
        active_black_keys[i]      = 0;
        prev_active_black_keys[i] = 0;
    }

    draw_static_scene();
    wait_for_vsync();
    draw_static_scene();

    int frame = 0;
    while (1) {
        if (frame % SPAWN_GAP == 0)
            spawn_note();

        update_notes();
        render_dirty();
        wait_for_vsync();

        frame++;
        if (frame >= 10000) frame = 0;
    }

    return 0;
}
	