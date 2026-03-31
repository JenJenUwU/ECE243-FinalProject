#ifndef PS2_H
#define PS2_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the PS/2 Keyboard by clearing any stale data in the FIFO.
 */
void ps2_init(void);

/**
 * @brief Polls the PS/2 keyboard data register.
 * @param scan_code Pointer to store the received raw scan code.
 * @return True if a new byte was read.
 */
bool ps2_poll_raw(uint8_t *scan_code);

/**
 * @brief Polls and decodes a complete key event handling extended and break prefixes.
 * @param is_pressed Returns true if the key was pressed, false if released.
 * @param key_code Returns the key code (includes 0xE000 prefix for extended keys).
 * @return True if a complete key event was decoded.
 */
bool ps2_poll_key(bool *is_pressed, uint16_t *key_code);

#endif // PS2_H
