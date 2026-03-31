/**
 * @file main.c
 * @brief Main entry point for the MIDI Piano visualizer and synthesizer.
 *
 * This program reads a pre-compiled array of MIDI notes and coordinates
 * playing them through the DE1-SoC audio hardware while simultaneously
 * visualizing them as falling blocks on a VGA display. It carefully
 * manages the timing relationship between the audio samples and visual frames.
 */

#include <stdbool.h>
#include <stdint.h>

#include "audio.h"
#include "piano.h"
#include "midi_data.h"
#include "ps2.h"

/**
 * @brief Processes MIDI events up to a specific time and spawns notes.
 *
 * This checks the upcoming MIDI notes and sees if their start time has
 * arrived (or if it's time to spawn them visually).
 *
 * @param now_ms           The current time in milliseconds based on the audio sample count
 * @param next_audio_note  Pointer to the index of the next unplayed note for audio
 * @param next_visual_note Pointer to the index of the next unspawned note for the screen
 * @param voices           Array of active audio synthesizers
 * @param sample           The current absolute audio sample number
 * @param lead_ms          How early (in ms) a note must visually spawn so it hits the piano 
 *                         key exactly when the audio starts playing
 */
static void process_midi_until(uint32_t now_ms,
                               uint32_t *next_audio_note,
                               uint32_t *next_visual_note,
                               active_note_t *voices,
                               uint32_t sample,
                               uint32_t lead_ms,
                               uint32_t speed_factor_q16) {
    
    /* 1. Audio: Start playing notes if their start time has passed (delayed by lead_ms) */
    while (*next_audio_note < MIDI_NOTE_COUNT &&
           (MIDI_NOTES[*next_audio_note].start_ms + lead_ms) <= now_ms) {
        
        uint32_t duration_ms = MIDI_NOTES[*next_audio_note].duration_ms;
        uint32_t adj_duration_ms = (uint32_t)(((uint64_t)duration_ms * 65536ULL) / speed_factor_q16);

        voice_start(voices,
                    MIDI_NOTES[*next_audio_note].note,
                    MIDI_NOTES[*next_audio_note].velocity,
                    sample,
                    adj_duration_ms);
        (*next_audio_note)++;
    }

    /* 2. Visuals: Spawn falling blocks early so they reach the key in time */
    while (*next_visual_note < MIDI_NOTE_COUNT) {
        /* The block hits the keys exactly at (start_ms + lead_ms).
           So it needs to spawn exactly lead_ms before that: which is precisely start_ms! */
        uint32_t spawn_ms = MIDI_NOTES[*next_visual_note].start_ms;

        if (spawn_ms > now_ms) {
            /* Note shouldn't be spawned yet */
            break;
        }

        /* Span the visual block above the keys */
        piano_spawn_note(&MIDI_NOTES[*next_visual_note]);
        (*next_visual_note)++;
    }
}

/**
 * @brief Checks if any audio voices are currently playing.
 *
 * @param voices The array of active audio synthesizers
 * @return true if at least one voice is active, false otherwise
 */
static bool any_voice_active(const active_note_t *voices) {
    for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (voices[i].active) {
            return true;
        }
    }
    return false;
}

int main(void) {
    /* Array to hold the state of all concurrent polyphonic sounds */
    active_note_t voices[MAX_ACTIVE_NOTES] = {0};

    /* Initialize hardware modules */
    audio_init();
    piano_init();
    ps2_init();

    uint32_t hw_sample         = 0;
    uint32_t next_audio_note   = 0;
    uint32_t next_visual_note  = 0;
    uint32_t next_frame_sample = 0;
    
    uint64_t logic_time_frac_ms = 0;
    bool is_paused = false;
    uint32_t speed_factor_q16 = 65536; // 1.0x

    /* Number of audio samples that transpire during one 30 FPS video frame */
    const uint32_t samples_per_frame = SAMPLE_RATE / FPS;
    
    /* How long a block takes to fall from top of screen to the keys */
    const uint32_t lead_ms           = piano_fall_time_ms();

    /* Main game loop */
    while (1) {
        /*
         * AUDIO PHASE — Drains the audio FIFO buffer.
         * Runs as many samples as the FIFO can currently accept up to the
         * boundary of the next video frame. This ensures the audio stream
         * never stutters, even if visual rendering takes heavily varying time.
         */
        while (audio_fifo_has_space() && hw_sample < next_frame_sample) {
            
            /* Poll Keyboard events */
            bool is_pressed;
            uint16_t key_code;
            while (ps2_poll_key(&is_pressed, &key_code)) {
                if (is_pressed) {
                    if (key_code == 0x29) { // Spacebar
                        is_paused = !is_paused;
                    } else if (key_code == 0xE075) { // Up Arrow
                        speed_factor_q16 += 6553; // Increase by ~10%
                        if (speed_factor_q16 > 131072) { // Max 2.0x
                            speed_factor_q16 = 131072;
                        }
                    } else if (key_code == 0xE072) { // Down Arrow
                        if (speed_factor_q16 > 16384 + 6553) { // Min 0.25x (16384)
                            speed_factor_q16 -= 6553;
                        } else {
                            speed_factor_q16 = 16384; 
                        }
                    }
                }
            }

            if (!is_paused) {
                logic_time_frac_ms += (1000ULL * speed_factor_q16) / SAMPLE_RATE;
            }

            uint32_t now_ms = (uint32_t)(logic_time_frac_ms >> 16);

            process_midi_until(now_ms,
                               &next_audio_note,
                               &next_visual_note,
                               voices,
                               hw_sample,
                               lead_ms,
                               speed_factor_q16);

            /* Generate next wave sample and write to the audio codec */
            audio_write_sample(synth_mix(voices, hw_sample));
            hw_sample++;
        }

        /*
         * RENDER PHASE — Updates the screen once per frame boundary.
         * The render waits for a vertical sync (VSync), drawing falling 
         * blocks and highlighting keys. The audio FIFO handles music playback
         * smoothly in the background while the processor blocks on VSync.
         */
        if (hw_sample >= next_frame_sample) {
            uint32_t now_ms = (uint32_t)(logic_time_frac_ms >> 16);
            piano_update(now_ms);
            piano_render();
            next_frame_sample += samples_per_frame;
        }

        /* End condition: No more notes in song & trailing audio decays completed */
        if (next_audio_note >= MIDI_NOTE_COUNT && !any_voice_active(voices)) {
            break;
        }
    }

    piano_draw_end_screen();

    /* Program done; infinite halt */
    while (1) {
    }

    return 0;
}