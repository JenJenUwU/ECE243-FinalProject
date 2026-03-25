#include <stdbool.h>
#include <stdint.h>

#include "midi_data.h"

#define AUDIO_BASE 0xFF203040
#define SAMPLE_RATE 8000U
#define MAX_ACTIVE_NOTES 16

#define AUDIO_FIFOSPACE_OFFSET 0x4
#define AUDIO_LEFTDATA_OFFSET  0x8
#define AUDIO_RIGHTDATA_OFFSET 0xC

static volatile uint32_t *const audio_fifospace = (volatile uint32_t *)(AUDIO_BASE + AUDIO_FIFOSPACE_OFFSET);
static volatile uint32_t *const audio_leftdata = (volatile uint32_t *)(AUDIO_BASE + AUDIO_LEFTDATA_OFFSET);
static volatile uint32_t *const audio_rightdata = (volatile uint32_t *)(AUDIO_BASE + AUDIO_RIGHTDATA_OFFSET);

typedef struct {
    bool active;
    uint8_t note;
    uint8_t velocity;
    float phase;
    float step;
    uint32_t end_sample;
} active_note_t;

static float note_to_hz(uint8_t note) {
    return note_freqs[note];
}

static int32_t synth_mix(active_note_t *voices, uint32_t now_sample) {
    float mix = 0.0f;

    for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (!voices[i].active) {
            continue;
        }

        if (now_sample >= voices[i].end_sample) {
            voices[i].active = false;
            continue;
        }

        float wave = (voices[i].phase < 0.5f) ? 1.0f : -1.0f;
        float amp = (float)voices[i].velocity / 127.0f;
        mix += wave * amp;

        voices[i].phase += voices[i].step;
        if (voices[i].phase >= 1.0f) {
            voices[i].phase -= 1.0f;
        }
    }

    if (mix > 1.0f) {
        mix = 1.0f;
    } else if (mix < -1.0f) {
        mix = -1.0f;
    }

    return (int32_t)(mix * 200000000.0f);
}

static void voice_start(active_note_t *voices,
                        uint8_t note,
                        uint8_t velocity,
                        uint32_t start_sample,
                        uint32_t duration_ms) {
    uint32_t slot = 0;

    for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (!voices[i].active) {
            slot = i;
            break;
        }
    }

    voices[slot].active = true;
    voices[slot].note = note;
    voices[slot].velocity = velocity;
    voices[slot].phase = 0.0f;
    voices[slot].step = note_to_hz(note) / (float)SAMPLE_RATE;
    voices[slot].end_sample = start_sample + (duration_ms * SAMPLE_RATE) / 1000U;
}

int main(void) {
    active_note_t voices[MAX_ACTIVE_NOTES] = {0};
    uint32_t next_note = 0;

    for (uint32_t sample = 0;; sample++) {
        uint32_t now_ms = (sample * 1000U) / SAMPLE_RATE;

        while (next_note < MIDI_NOTE_COUNT && MIDI_NOTES[next_note].start_ms <= now_ms) {
            voice_start(voices,
                        MIDI_NOTES[next_note].note,
                        MIDI_NOTES[next_note].velocity,
                        sample,
                        MIDI_NOTES[next_note].duration_ms);
            next_note++;
        }

        while (((*audio_fifospace) & 0x00FF0000) == 0) {
        }

        int32_t s = synth_mix(voices, sample);
        *audio_leftdata = (uint32_t)s;
        *audio_rightdata = (uint32_t)s;

        if (next_note >= MIDI_NOTE_COUNT) {
            bool any_active = false;
            for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
                if (voices[i].active) {
                    any_active = true;
                    break;
                }
            }
            if (!any_active) {
                break;
            }
        }
    }

    while (1) {
    }

    return 0;
}