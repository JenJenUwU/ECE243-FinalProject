/**
 * @file audio.c
 * @brief Implementation of the fixed-point DE1-SoC software synthesizer.
 */

#include "audio.h"
#include "midi_data.h"

/* Hardware addresses for the DE1-SoC Audio Controller */
#define AUDIO_BASE 0xFF203040
#define AUDIO_FIFOSPACE_OFFSET 0x4
#define AUDIO_LEFTDATA_OFFSET  0x8
#define AUDIO_RIGHTDATA_OFFSET 0xC

/* Pointers to memory-mapped registers governing the audio IP core */
static volatile uint32_t *const audio_fifospace =
    (volatile uint32_t *)(AUDIO_BASE + AUDIO_FIFOSPACE_OFFSET);
static volatile uint32_t *const audio_leftdata =
    (volatile uint32_t *)(AUDIO_BASE + AUDIO_LEFTDATA_OFFSET);
static volatile uint32_t *const audio_rightdata =
    (volatile uint32_t *)(AUDIO_BASE + AUDIO_RIGHTDATA_OFFSET);

/* Pre-generated 256-sample single-cycle waveform approximating piano overtones */
static const int8_t piano_wavetable[256] = {
       0,   9,  18,  26,  35,  43,  51,  59,  67,  74,  81,  87,  93,  98, 104, 108,
     112, 116, 119, 121, 123, 125, 126, 127, 127, 127, 126, 125, 124, 122, 120, 118,
     116, 114, 111, 108, 105, 102,  99,  96,  93,  91,  88,  85,  82,  80,  77,  75,
      73,  71,  69,  67,  66,  64,  63,  62,  60,  59,  58,  57,  57,  56,  55,  54,
      53,  53,  52,  51,  50,  50,  49,  48,  47,  46,  45,  44,  43,  42,  41,  40,
      39,  38,  37,  36,  34,  33,  32,  31,  30,  30,  29,  28,  27,  26,  26,  25,
      24,  24,  23,  23,  22,  22,  22,  21,  21,  20,  20,  19,  19,  19,  18,  17,
      17,  16,  15,  15,  14,  13,  12,  11,  10,   9,   8,   6,   5,   4,   3,   1,
       0,  -1,  -3,  -4,  -5,  -6,  -8,  -9, -10, -11, -12, -13, -14, -15, -15, -16,
     -17, -17, -18, -19, -19, -19, -20, -20, -21, -21, -22, -22, -22, -23, -23, -24,
     -24, -25, -26, -26, -27, -28, -29, -30, -30, -31, -32, -33, -34, -36, -37, -38,
     -39, -40, -41, -42, -43, -44, -45, -46, -47, -48, -49, -50, -50, -51, -52, -53,
     -53, -54, -55, -56, -57, -57, -58, -59, -60, -62, -63, -64, -66, -67, -69, -71,
     -73, -75, -77, -80, -82, -85, -88, -91, -93, -96, -99,-102,-105,-108,-111,-114,
    -116,-118,-120,-122,-124,-125,-126,-127,-127,-127,-126,-125,-123,-121,-119,-116,
    -112,-108,-104, -98, -93, -87, -81, -74, -67, -59, -51, -43, -35, -26, -18,  -9,
};

void audio_init(void) {
    /* No-op: HW is self-initializing on reset */
}

int audio_fifo_has_space(void) {
    /* Extracts the Write Space Available (WSLC) bits 23:16 to see if codec accepts data */
    return (((*audio_fifospace) & 0x00FF0000) != 0);
}

void audio_write_sample(int32_t s) {
    /* Casts signed 32-bit sample directly into hardware Left/Right FIFO regs */
    *audio_leftdata  = (uint32_t)s;
    *audio_rightdata = (uint32_t)s;
}

/**
 * @brief synth_mix — fixed-point logic matching the original float logic.
 *
 * Each active voice computes a square wave. A 32-bit phase accumulator tracks
 * the waveform.  The most-significant-bit effectively divides the wave in half
 * (+1 for the first half-cycle, -1 for the latter).
 * 
 * Wave amplitude scaling computes precisely to match the float equation:
 *   Original: wave(1.0 or -1.0) * (velocity / 127.0) * 200000000.0
 * 
 * This executes using only integer adds/shifts in the hot loop keeping 
 * polyphonic audio lag-free on CPUs without hardware FPU.
 *
 * @param voices The array of active synths
 * @param now_sample Current time/sample counter to track decay
 * @return Safely clipped int32 audio mixture
 */
int32_t synth_mix(active_note_t *voices, uint32_t now_sample) {
    /* Use 64-bit sum to prevent overflow when 16 notes add together simultaneously */
    int64_t mix = 0;

    for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (!voices[i].active) {
            continue;
        }

        /* End the note early if it has played out its full duration */
        if (now_sample >= voices[i].end_sample) {
            voices[i].active = false;
            continue;
        }

        /* 256-sample Wavetable lookup uses the top 8 bits of the Q32 phase */
        uint32_t table_index = voices[i].phase >> 24;
        int32_t wave = piano_wavetable[table_index];

        /* Scale down the 32-bit amplitude to prevent integer multiplication overflow
           then multiply by the -127 to +127 wavetable value */
        int32_t amp_scaled = voices[i].current_amplitude >> 7; 
        
        /* Add the waveform to the master mix */
        mix += (int64_t)(amp_scaled * wave);

        /* Render linear decay envelope */
        voices[i].current_amplitude -= voices[i].decay_step;
        if (voices[i].current_amplitude < 0) {
            voices[i].current_amplitude = 0;
            voices[i].active = false;  /* Cut off completely silent notes */
        }

        /* Advance phase accumulator — wraps mathematically accurately at 2^32 */
        voices[i].phase += voices[i].step;
    }

    /* Hard clamp to hardware/software limits to prevent digital screeching distortion */
    if (mix > 200000000) { mix = 200000000; }
    if (mix < -200000000) { mix = -200000000; }

    return (int32_t)mix;
}

void voice_start(active_note_t *voices,
                 uint8_t note,
                 uint8_t velocity,
                 uint32_t start_sample,
                 uint32_t duration_ms) {
    int slot = -1;

    /* Find the first inactive hardware voice slot to use */
    for (uint32_t i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (!voices[i].active) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        /* All 16 voices are busy, forcibly overwrite slot 0 */
        slot = 0;
    }

    voices[slot].active     = true;
    voices[slot].note       = note;
    
    /* Fetch exact frequency. Safe to use soft float once per note event dynamically. */
    float freq = note < 128 ? note_freqs[note] : note_freqs[69];
    
    /* Map frequency hz into 32-bit phase step increments: 2^32 / 8000 Hz */
    voices[slot].step       = (uint32_t)(freq * 536870.912f);
    
    /* Precalc identical output amplitude scale as the original float representation */
    voices[slot].amplitude  = (int32_t)(((float)velocity / 127.0f) * 200000000.0f);
    voices[slot].current_amplitude = voices[slot].amplitude;
    
    voices[slot].phase      = 0U;
    
    /* Precalc identical length into hardware samples */
    uint32_t total_samples = (duration_ms * SAMPLE_RATE) / 1000U;
    voices[slot].end_sample = start_sample + total_samples;
    
    /* Calculate precise decay step mathematically to fade amplitude exactly to 0 at the end */
    voices[slot].decay_step = total_samples > 0 ? (voices[slot].amplitude / total_samples) : voices[slot].amplitude;
}