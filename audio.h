/**
 * @file audio.h
 * @brief Audio synthesis configuration and interface definitions.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define SAMPLE_RATE 8000U
#define MAX_ACTIVE_NOTES 16

/**
 * @brief Represents a single polyphonic synthesizer voice.
 * Keeps track of its playback state and frequency stepping logic.
 */
typedef struct {
    bool     active;      /* Flag indicating if the voice is currently sounding */
    uint8_t  note;        /* MIDI Note number being played */
    int32_t  amplitude;         /* Pre-calculated initial 200M scaled amplitude */
    int32_t  current_amplitude; /* Real-time amplitude dropping toward 0 (Envelope) */
    int32_t  decay_step;        /* Amount of amplitude lost per hardware sample */
    uint32_t phase;       /* Q32 fixed-point phase accumulator, wraps at 2^32 */
    uint32_t step;        /* Q32 fixed-point phase increment per audio sample */
    uint32_t end_sample;  /* The absolute sample count when this note ends */
} active_note_t;

/** @brief Initializes the DE1-SoC audio hardware interfaces */
void audio_init(void);

/**
 * @brief Spawns a new audio sound / voice.
 * Allocates an available active_note_t struct and sets up its parameters.
 * 
 * @param voices       Array of available slots
 * @param note         The MIDI note to be played
 * @param velocity     MIDI velocity (loudness, 0-127)
 * @param start_sample The absolute sample count when this sound begins
 * @param duration_ms  How long the sound lasts in milliseconds
 */
void voice_start(active_note_t *voices,
                 uint8_t note,
                 uint8_t velocity,
                 uint32_t start_sample,
                 uint32_t duration_ms);

/**
 * @brief Mixes all active voices together for a single sample frame.
 * @param voices Array of active synths to read
 * @param now_sample the current sample index
 * @return A 32-bit signed audio mix result safely clipped to audio DAC bounds
 */
int32_t synth_mix(active_note_t *voices, uint32_t now_sample);

/**
 * @brief Writes the final processed sample to the hardware codec (L & R channels)
 * @param s Output sample magnitude
 */
void audio_write_sample(int32_t s);

/**
 * @brief Checks the hardware codec FIFO to see if it is ready to accept a sample.
 * @return Non-zero if there is space, zero otherwise
 */
int audio_fifo_has_space(void);

#endif