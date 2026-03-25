#include <stdint.h>
#include <string.h>

#include "piano.h"
#include "midi_data.h"

// Screen / hardware layout dimensions
#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240

// Base address for the VGA pixel buffer DMA controller
#define PIXEL_BUF_CTRL 0xFF203020

// Standard RGB565 16-bit color definitions
#define BLACK       0x0000
#define WHITE       0xFFFF
#define IVORY       0xF79E
#define GRAY        0x8410
#define DARK_BG     0x0861

// Colors when keys are actively pressed
#define KEY_LIT     0x07FF
#define BLACK_LIT   0x07FF

// 4 distinct colors used cyclically for the falling notes
#define NOTE_COL_0  0x001F
#define NOTE_COL_1  0xF81F
#define NOTE_COL_2  0x07E0
#define NOTE_COL_3  0xFFE0

// 88-key standard piano physical layout tracking
#define TOTAL_WHITE_KEYS  52
#define TOTAL_BLACK_KEYS  36
#define TOTAL_PIANO_KEYS  88

// Bottom area reserved for static keys
#define KEY_HEIGHT   40
#define KEY_Y_START  (SCREEN_HEIGHT - KEY_HEIGHT)

// Tile boundary so tiles slide behind the 4px piano housing
#define NOTE_BOTTOM  (KEY_Y_START - 4)

// Black key visual dimensions
#define BLACK_W      4
#define BLACK_H      ((KEY_HEIGHT * 6) / 10)

// Standard 88-key piano MIDI bounds (A0 to C8)
#define MIDI_PIANO_MIN_NOTE 21
#define MIDI_PIANO_MAX_NOTE 108

// Visual falling note pool settings
#define MAX_NOTES 64
#define MIN_NOTE_HEIGHT 8
#define FALL_TIME_MS 1800U

/* Arrays holding the pre-calculated X-coordinates and widths of every white key */
static int KEY_X[TOTAL_WHITE_KEYS + 1];
static int KEY_W[TOTAL_WHITE_KEYS];

/* The standard pattern of black keys across an 88-key piano keyboard */
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

/** @brief Hardware positioning struct for a single black key */
typedef struct {
    int bx;           /* X coordinate starting pixel */
    int white_left;   /* The index of the white key immediately to its left */
} black_key_t;

/* List mapping visual black keys to their positions */
static black_key_t BLACK_KEYS[TOTAL_BLACK_KEYS];
static int NUM_BLACK_KEYS = 0;

/** @brief Active falling visual note rect */
typedef struct {
    int active;
    int x;
    int y;
    int prev_y[2];   /* Cached erase position per hardware frame-buffer (double-buffer safe) */
    int w;
    int h;
    int key_index;
    int is_black;
    short color;
    uint32_t spawn_ms; /* Absolute ms timestamp when this should appear at the top entirely */
} visual_note_t;

/* Pool of on-screen visual blocks */
static visual_note_t notes[MAX_NOTES];

/* Keyboard active literal UI states (pressed vs unpressed tracking) per Framebuffer */
static int active_keys[TOTAL_WHITE_KEYS];
static int prev_active_keys[2][TOTAL_WHITE_KEYS];
static int active_black_keys[TOTAL_BLACK_KEYS];
static int prev_active_black_keys[2][TOTAL_BLACK_KEYS];

/* Pre-calculated lookup tables to convert MIDI Notes (0-127) to UI Key indices */
static int midi_is_black[128];
static int midi_to_white_index[128];
static int midi_to_black_index[128];

/* Active hardware pixel buffer base memory address and flip tracker */
static volatile int pixel_buffer_start;
static int color_cycle = 0;
static int buf_idx = 0;   /* toggles 0/1 — tracks which buffer we're drawing into */

static const short NOTE_COLORS[4] = {
    NOTE_COL_0, NOTE_COL_1, NOTE_COL_2, NOTE_COL_3
};

/** @brief Precalculates exact widths and bounds for all white keys to fit 320px screen */
static void init_key_positions(void) {
    const int WIDE[8] = {3, 10, 16, 23, 29, 36, 42, 49};
    int wi = 0;
    int x = 0;

    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        int w = 6;
        KEY_X[i] = x;
        if (wi < 8 && i == WIDE[wi]) {
            w = 7;
            wi++;
        }
        KEY_W[i] = w;
        x += w;
    }
    KEY_X[TOTAL_WHITE_KEYS] = x;
}

static void init_black_keys(void) {
    NUM_BLACK_KEYS = 0;
    for (int i = 0; i < TOTAL_WHITE_KEYS - 1; i++) {
        if (!BLACK_PATTERN[i]) {
            continue;
        }
        BLACK_KEYS[NUM_BLACK_KEYS].bx = KEY_X[i + 1] - (BLACK_W / 2);
        BLACK_KEYS[NUM_BLACK_KEYS].white_left = i;
        NUM_BLACK_KEYS++;
    }
}

static int semitone_is_black(uint8_t midi_note) {
    switch (midi_note % 12U) {
        case 1:  // C#
        case 3:  // D#
        case 6:  // F#
        case 8:  // G#
        case 10: // A#
            return 1;
        default:
            return 0;
    }
}

static void init_note_maps(void) {
    int white_idx = 0;
    int black_idx = 0;

    for (int i = 0; i < 128; i++) {
        midi_is_black[i] = 0;
        midi_to_white_index[i] = -1;
        midi_to_black_index[i] = -1;
    }

    for (int note = MIDI_PIANO_MIN_NOTE; note <= MIDI_PIANO_MAX_NOTE; note++) {
        if (semitone_is_black((uint8_t)note)) {
            midi_is_black[note] = 1;
            midi_to_black_index[note] = black_idx;
            black_idx++;
        } else {
            midi_is_black[note] = 0;
            midi_to_white_index[note] = white_idx;
            white_idx++;
        }
    }
}

/** 
 * @brief Sends a request to the VGA controller to swap double buffers on the next
 * Vertical Synchronization pulse. Blocks processor until visual swap is complete
 * to prevent screen-tearing drawing artifacts.
 */
static void wait_for_vsync(void) {
    volatile int *pixel_ctrl_ptr = (volatile int *)PIXEL_BUF_CTRL;
    *pixel_ctrl_ptr = 1;

    volatile int *status_reg = pixel_ctrl_ptr + 3;
    while ((*status_reg & 0x01) != 0) {
    }

    pixel_buffer_start = *(pixel_ctrl_ptr + 1);
    buf_idx ^= 1;   /* we are now drawing into the other buffer */
}

static void plot_pixel(int x, int y, short color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) {
        return;
    }

    volatile short *addr =
        (volatile short *)(pixel_buffer_start + (y << 10) + (x << 1));
    *addr = color;
}

static void draw_rect(int x, int y, int w, int h, short color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            plot_pixel(i, j, color);
        }
    }
}

static void draw_vline(int x, int y, int h, short color) {
    for (int j = y; j < y + h; j++) {
        plot_pixel(x, j, color);
    }
}

static void draw_hline(int x, int y, int w, short color) {
    for (int i = x; i < x + w; i++) {
        plot_pixel(i, y, color);
    }
}

static short bg_color(int y) {
    return (y & 1) ? (short)DARK_BG : (short)(DARK_BG + 0x0020);
}

static void erase_rect_bg(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= NOTE_BOTTOM) {
            continue;
        }
        short c = bg_color(j);
        for (int i = x; i < x + w; i++) {
            plot_pixel(i, j, c);
        }
    }
}


static void draw_background(void) {
    for (int y = 0; y < NOTE_BOTTOM; y++) {
        draw_hline(0, y, SCREEN_WIDTH, bg_color(y));
    }
}

static void draw_piano_housing(void) {
    draw_rect(0, NOTE_BOTTOM, SCREEN_WIDTH, 4, BLACK);
}

static void draw_white_key(int i, short color) {
    draw_rect(KEY_X[i] + 1, KEY_Y_START, KEY_W[i] - 1, KEY_HEIGHT - 2, color);
}

static void draw_white_keys_full(void) {
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        short color = active_keys[i] ? KEY_LIT : IVORY;
        draw_white_key(i, color);
    }

    for (int i = 0; i <= TOTAL_WHITE_KEYS; i++) {
        draw_vline(KEY_X[i], KEY_Y_START, KEY_HEIGHT, BLACK);
    }
    draw_hline(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH, BLACK);
}

static void redraw_black_key(int bi) {
    if (bi < 0 || bi >= NUM_BLACK_KEYS) {
        return;
    }

    short color = active_black_keys[bi] ? BLACK_LIT : BLACK;
    int bx = BLACK_KEYS[bi].bx;

    draw_rect(bx, KEY_Y_START, BLACK_W, BLACK_H, color);
    if (!active_black_keys[bi] && BLACK_W >= 2) {
        draw_vline(bx + 1, KEY_Y_START + 1, 2, GRAY);
    }
}

static void draw_black_keys(void) {
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        redraw_black_key(i);
    }
}

static void draw_static_scene(void) {
    draw_background();
    draw_piano_housing();
    draw_white_keys_full();
    draw_black_keys();
}

static int duration_to_height(uint32_t duration_ms) {
    uint32_t h = (duration_ms * NOTE_BOTTOM) / FALL_TIME_MS;
    if (h < MIN_NOTE_HEIGHT) {
        h = MIN_NOTE_HEIGHT;
    }
    if (h > (uint32_t)(SCREEN_HEIGHT * 2)) {
        h = (uint32_t)(SCREEN_HEIGHT * 2);
    }
    return (int)h;
}

/** @brief Fully initializes the piano UI system, memory addresses and clears both hardware VGA buffers */
void piano_init(void) {
    volatile int *pixel_ctrl_ptr = (volatile int *)PIXEL_BUF_CTRL;

    init_key_positions();
    init_black_keys();
    init_note_maps();

    memset(notes, 0, sizeof(notes));
    memset(active_keys, 0, sizeof(active_keys));
    memset(prev_active_keys, 0, sizeof(prev_active_keys));
    memset(active_black_keys, 0, sizeof(active_black_keys));
    memset(prev_active_black_keys, 0, sizeof(prev_active_black_keys));

    *(pixel_ctrl_ptr + 1) = 0x08000000;
    pixel_buffer_start = *(pixel_ctrl_ptr + 1);
    draw_static_scene();

    wait_for_vsync();

    *(pixel_ctrl_ptr + 1) = 0x08400000;
    pixel_buffer_start = *(pixel_ctrl_ptr + 1);
    draw_static_scene();
}

uint32_t piano_fall_time_ms(void) {
    return FALL_TIME_MS;
}

void piano_spawn_note(const midi_note_event_t *event) {
    if (event == 0) {
        return;
    }
    if (event->note < MIDI_PIANO_MIN_NOTE || event->note > MIDI_PIANO_MAX_NOTE) {
        return;
    }

    /* Gap threshold: if the silence between the end of an existing note and
     * the start of this one is <= this many ms, treat them as one continuous
     * bar rather than spawning a separate block.  66 ms ≈ 2 frames @30 Hz. */
    #define MERGE_GAP_MS 66U

    uint32_t new_spawn_ms = event->start_ms;
    int new_h = duration_to_height(event->duration_ms);

    /* Determine the key identity of this event up front so we can match. */
    int new_is_black  = midi_is_black[event->note];
    int new_key_index = new_is_black
                        ? midi_to_black_index[event->note]
                        : midi_to_white_index[event->note];

    /* Try to find an existing active note on the same key that this event
     * can be merged into (i.e. their spawn windows are adjacent/overlapping). */
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].active) {
            continue;
        }
        if (notes[i].is_black != new_is_black) {
            continue;
        }
        if (notes[i].key_index != new_key_index) {
            continue;
        }

        /* The existing note's spawn window ends at spawn_ms + duration of
         * the visual bar (inverse of duration_to_height).  We stored h, so
         * reverse: end_spawn_ms = spawn_ms + (h * FALL_TIME_MS) / NOTE_BOTTOM */
        uint32_t existing_end_ms = notes[i].spawn_ms
            + (uint32_t)((uint32_t)notes[i].h * FALL_TIME_MS) / (uint32_t)NOTE_BOTTOM;

        if (new_spawn_ms <= existing_end_ms + MERGE_GAP_MS) {
            /* Extend the existing bar to cover this note too. */
            uint32_t merged_end_ms = new_spawn_ms + (uint32_t)new_h
                                     * FALL_TIME_MS / (uint32_t)NOTE_BOTTOM;
            if (merged_end_ms > existing_end_ms) {
                uint32_t merged_duration_ms = merged_end_ms - notes[i].spawn_ms;
                int merged_h = (int)((merged_duration_ms * (uint32_t)NOTE_BOTTOM)
                               / FALL_TIME_MS);
                if (merged_h < MIN_NOTE_HEIGHT) {
                    merged_h = MIN_NOTE_HEIGHT;
                }
                notes[i].h = merged_h;
            }
            return; /* merged — no new slot needed */
        }
    }

    /* No merge candidate found — allocate a fresh slot. */
    int slot = -1;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    visual_note_t *n = &notes[slot];
    n->active   = 1;
    n->spawn_ms = new_spawn_ms;
    n->h        = new_h;
    n->y           = -n->h;
    n->prev_y[0]   = n->y;
    n->prev_y[1]   = n->y;
    n->color    = NOTE_COLORS[color_cycle & 3];
    color_cycle++;

    if (midi_is_black[event->note]) {
        int bi = midi_to_black_index[event->note];
        if (bi < 0 || bi >= NUM_BLACK_KEYS) {
            n->active = 0;
            return;
        }
        n->is_black = 1;
        n->key_index = bi;
        n->x = BLACK_KEYS[bi].bx;
        n->w = BLACK_W;
    } else {
        int wi = midi_to_white_index[event->note];
        if (wi < 0 || wi >= TOTAL_WHITE_KEYS) {
            n->active = 0;
            return;
        }
        n->is_black = 0;
        n->key_index = wi;
        n->x = KEY_X[wi] + 1;
        n->w = KEY_W[wi] - 2;
    }
}

/**
 * @brief Applies logic updates per-frame. Moves visual boxes down the screen
 * based on elapsed system ms. Updates piano key state collisions.
 */
void piano_update(uint32_t now_ms) {
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        active_keys[i] = 0;
    }
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        active_black_keys[i] = 0;
    }

    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes[i].active) {
            continue;
        }

        if (now_ms <= notes[i].spawn_ms) {
            notes[i].y = -notes[i].h;
        } else {
            uint32_t elapsed_ms = now_ms - notes[i].spawn_ms;
            notes[i].y = -notes[i].h + (int)((elapsed_ms * NOTE_BOTTOM) / FALL_TIME_MS);
        }

        if (notes[i].y >= NOTE_BOTTOM) {
            /* 
             * Cool down state to let double-buffered erasers finish cleaning ghosts.
             * active = 1 (alive) -> -1 (erase buf A) -> -2 (erase buf B) -> 0 (free slot)
             */
            if (notes[i].active == 1) {
                notes[i].active = -1;
            } else if (notes[i].active == -1) {
                notes[i].active = -2;
            } else if (notes[i].active == -2) {
                notes[i].active = 0;
            }
            continue;
        }

        if ((notes[i].y + notes[i].h) >= NOTE_BOTTOM) {
            if (notes[i].is_black) {
                active_black_keys[notes[i].key_index] = 1;
            } else {
                active_keys[notes[i].key_index] = 1;
            }
        }
    }
}

/**
 * @brief Commits all visual state changes cleanly into the un-displayed "back" pixel buffer via VGA.
 * The buffer is swapped exactly at VSync.
 */
void piano_render(void) {
    /* --- 1. Erase notes at their OLD positions for THIS buffer ---
     * prev_y[buf_idx] holds where this note was drawn the last time we
     * rendered into this buffer (two frames ago).  Erasing that position
     * cleans this buffer correctly without touching the other buffer. */
    for (int i = 0; i < MAX_NOTES; i++) {
        int old_y = notes[i].prev_y[buf_idx];
        if (old_y == notes[i].y) {
            continue;
        }

        int old_top = old_y;
        int old_bot = old_y + notes[i].h;
        if (old_top < 0) {
            old_top = 0;
        }
        if (old_bot > NOTE_BOTTOM) {
            old_bot = NOTE_BOTTOM;
        }
        if (old_top < old_bot) {
            erase_rect_bg(notes[i].x, old_top, notes[i].w, old_bot - old_top);
        }
    }

    /* --- 2. Draw notes at their NEW positions & record for this buffer --- */
    for (int i = 0; i < MAX_NOTES; i++) {
        if (notes[i].active <= 0) {
            /* Note is marked for erase or completely inactive. Skip drawing. */
            notes[i].prev_y[buf_idx] = notes[i].y;
            continue;
        }

        int y_top = notes[i].y;
        int y_bot = notes[i].y + notes[i].h;
        if (y_top < 0) {
            y_top = 0;
        }
        if (y_bot > NOTE_BOTTOM) {
            y_bot = NOTE_BOTTOM;
        }
        if (y_top >= y_bot) {
            /* Save position even if fully off-screen so erase stays correct */
            notes[i].prev_y[buf_idx] = notes[i].y;
            continue;
        }

        draw_rect(notes[i].x, y_top, notes[i].w, y_bot - y_top, notes[i].color);

        /* Remember where we drew into THIS buffer for next erase pass */
        notes[i].prev_y[buf_idx] = notes[i].y;
    }

    /* --- 3. Redraw white keys whose lit state changed --- */
    for (int i = 0; i < TOTAL_WHITE_KEYS; i++) {
        if (active_keys[i] == prev_active_keys[buf_idx][i]) {
            continue;
        }

        draw_white_key(i, active_keys[i] ? KEY_LIT : IVORY);
        prev_active_keys[buf_idx][i] = active_keys[i];

        /* Restore any black keys that overlap this white key */
        int bi_right = -1;
        int bi_left  = -1;
        for (int b = 0; b < NUM_BLACK_KEYS; b++) {
            if (BLACK_KEYS[b].white_left == i) {
                bi_right = b;
            }
            if (BLACK_KEYS[b].white_left == (i - 1)) {
                bi_left = b;
            }
        }
        if (bi_right >= 0) {
            redraw_black_key(bi_right);
        }
        if (bi_left >= 0) {
            redraw_black_key(bi_left);
        }
    }

    /* --- 4. Redraw black keys whose lit state changed --- */
    for (int i = 0; i < NUM_BLACK_KEYS; i++) {
        if (active_black_keys[i] != prev_active_black_keys[buf_idx][i]) {
            redraw_black_key(i);
            prev_active_black_keys[buf_idx][i] = active_black_keys[i];
        }
    }

    wait_for_vsync();
}