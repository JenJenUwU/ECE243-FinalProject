/**
 * @file piano.h
 * @brief Interfaces for drawing and animating the piano UI.
 */
#ifndef PIANO_H
#define PIANO_H

#include <stdint.h>
#include "midi_data.h"

#define FPS 30

/** @brief Initializes the VGA pixel buffer, colors, and key maps */
void piano_init(void);

/** @brief Provides the time in ms it takes for a note to reach the keys */
uint32_t piano_fall_time_ms(void);

/** @brief Spawns a new visually falling bar mapped to the correct key */
void piano_spawn_note(const midi_note_event_t *event);

/** @brief Advances all falling tile positions and resolves key press active states */
void piano_update(uint32_t now_ms);

/** @brief Clears and redraws notes safely using double-buffered pixel memory */
void piano_render(void);

/** @brief Displays a simple end screen when the song is over */
void piano_draw_end_screen(void);

#endif
