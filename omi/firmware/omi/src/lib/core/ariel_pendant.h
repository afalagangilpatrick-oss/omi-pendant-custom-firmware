#ifndef ARIEL_PENDANT_H
#define ARIEL_PENDANT_H

/*
 * Ariel's custom pendant behaviour on top of the Omi CV1 firmware:
 * pairing window + bonded-only links, LED rules, button gestures, the 20 s
 * power-off and the one-time wipe of old recordings. Normal mode only: SAFE
 * (recovery) never calls any of this, so phone DFU in SAFE keeps working.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ariel_ui.h"

/* Extra button-service codes (existing: 1 tap, 2 double, 3 long, 4 press, 5 release). */
#define ARIEL_BTN_CAPTURE_ON 6
#define ARIEL_BTN_CAPTURE_OFF 7
#define ARIEL_BTN_PAIRING 8

/* Call once, early in main() (normal mode), before the LEDs are used. */
void ariel_boot_mark(void);

/* After bt_enable() and settings_load() in normal mode. */
int ariel_security_init(void);

/* After the SD card and storage service are up (normal mode). */
void ariel_first_boot_wipe_schedule(void);

/* SAFE mode, right after bt_enable(): pair without storing a bond. */
void ariel_safe_mode_bt_setup(void);

/* Handle one gesture event from button.c (system workqueue context). */
void ariel_handle_gesture(struct ariel_event evt);

void ariel_pairing_open(void);
bool ariel_pairing_is_open(void);

/* Replaces the stock LED state machine (main loop calls this every second). */
void ariel_led_kick(void);

#endif /* ARIEL_PENDANT_H */
