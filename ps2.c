#include "ps2.h"

// Hardware addresses for the DE1-SoC PS/2 Controller
#define PS2_BASE 0xFF200100

static volatile uint32_t *const ps2_data = (volatile uint32_t *)PS2_BASE;

void ps2_init(void) {
    // Clear the FIFO by reading until empty
    while (*ps2_data & 0x8000) {
        // read and discard
    }
}

bool ps2_poll_raw(uint8_t *scan_code) {
    uint32_t data_reg = *ps2_data;
    
    // Check RVALID bit (bit 15)
    if (data_reg & 0x8000) {
        // RVALID is high, bits 7:0 contain data
        *scan_code = (uint8_t)(data_reg & 0xFF);
        return true;
    }
    
    return false;
}

static bool is_extended = false;
static bool is_break = false;

bool ps2_poll_key(bool *is_pressed, uint16_t *key_code) {
    uint8_t code;
    while (ps2_poll_raw(&code)) {
        if (code == 0xE0) {
            is_extended = true;
            continue;
        }
        if (code == 0xF0) {
            is_break = true;
            continue;
        }
        
        *is_pressed = !is_break;
        *key_code = (is_extended ? 0xE000 : 0) | code;
        
        // Reset state for next key event
        is_extended = false;
        is_break = false;
        return true;
    }
    return false;
}
