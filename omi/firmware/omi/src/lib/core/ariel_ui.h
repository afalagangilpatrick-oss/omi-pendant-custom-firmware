#ifndef ARIEL_UI_H
#define ARIEL_UI_H

/*
 * Ariel's pendant behaviour (pendant-app CONTRACT v5), as pure logic with no
 * Zephyr dependency so it can be unit-tested on a host.
 *
 * Gestures (one button):
 *   4 quick clicks                -> ARIEL_EVT_CAPTURE_ON
 *   5 quick clicks                -> ARIEL_EVT_CAPTURE_OFF
 *   hold >= 10 s, release < 20 s  -> ARIEL_EVT_PAIRING
 *   hold >= 20 s                  -> ARIEL_EVT_POWER_OFF (fires while still held)
 *   any other hold                -> nothing (a hold never powers off before 20 s)
 * A press already down when the machine starts (the press that woke the
 * pendant) is ignored until it is released.
 *
 * LED: 3 s glow after power-on and before power-off, slow blink while the
 * pairing window is open, 3 quick flashes when pairing fails, otherwise dark.
 */

#include <stdbool.h>
#include <stdint.h>

#define ARIEL_CLICK_MAX_MS 600u      /* a press shorter than this is a click */
#define ARIEL_CLICK_GAP_MS 400u      /* max release -> next press gap inside a series */
#define ARIEL_HOLD_PAIRING_MS 10000u /* hold this long, then release -> pairing */
#define ARIEL_HOLD_OFF_MS 20000u     /* hold this long -> power off */
#define ARIEL_GLOW_MS 3000u
#define ARIEL_PAIRING_WINDOW_MS 60000u
#define ARIEL_FAIL_FLASHES 3u
#define ARIEL_FAIL_FLASH_MS 150u

typedef enum {
    ARIEL_EVT_NONE = 0,
    ARIEL_EVT_PRESS,        /* raw press edge (forwarded to the app as before) */
    ARIEL_EVT_RELEASE,      /* raw release edge */
    ARIEL_EVT_CLICKS,       /* a finished click series that is not 4 or 5; see clicks */
    ARIEL_EVT_CAPTURE_ON,   /* 4 clicks */
    ARIEL_EVT_CAPTURE_OFF,  /* 5 clicks */
    ARIEL_EVT_HOLD_PAIRING, /* hold just reached 10 s (feedback only) */
    ARIEL_EVT_PAIRING,      /* released between 10 s and 20 s */
    ARIEL_EVT_POWER_OFF,    /* hold reached 20 s */
} ariel_evt_t;

struct ariel_gesture {
    bool armed;          /* false until the button has been seen released */
    bool down;           /* current level */
    bool hold_fired;     /* HOLD_PAIRING already reported for this press */
    bool off_fired;      /* POWER_OFF already reported for this press */
    bool series_open;    /* a click series is waiting for more clicks */
    uint8_t clicks;      /* clicks in the current series */
    uint8_t q_head;      /* small FIFO of events not yet returned */
    uint8_t q_len;
    uint8_t q_type[4];
    uint8_t q_clicks[4];
    uint32_t press_ms;   /* time of the last press edge */
    uint32_t release_ms; /* time of the last release edge */
};

struct ariel_event {
    ariel_evt_t type;
    uint8_t clicks; /* for ARIEL_EVT_CLICKS */
};

void ariel_gesture_init(struct ariel_gesture *g);

/*
 * Feed one sample. Call on every edge and at least every 50 ms while
 * ariel_gesture_busy() is true. Returns at most one event per call; keep
 * calling with the same sample until it returns ARIEL_EVT_NONE.
 * now_ms is a free-running millisecond clock (wraps safely).
 */
struct ariel_event ariel_gesture_step(struct ariel_gesture *g, bool pressed, uint32_t now_ms);

/* True while a series or a hold is open and the caller should keep sampling. */
bool ariel_gesture_busy(const struct ariel_gesture *g);

struct ariel_led_in {
    uint32_t now_ms;
    uint32_t boot_ms;      /* time the firmware started */
    bool powering_off;     /* power-off glow in progress */
    uint32_t power_off_ms; /* when powering_off started */
    bool pairing_open;     /* pairing window active */
    bool fail_active;      /* pairing failed, flashes in progress */
    uint32_t fail_ms;      /* when the failure was reported */
    bool off;              /* pendant has gone to System OFF */
};

struct ariel_led_out {
    bool red;
    bool green;
    bool blue;
    bool fail_done; /* fail flashes finished: caller clears fail_active */
};

/*
 * Colours: power glow = white (r+g+b), pairing blink = blue, fail = red.
 * Pairing blink: on for the first 500 ms of every second since the window
 * opened is approximated from now_ms (callers sample at >= 4 Hz while open).
 */
struct ariel_led_out ariel_led_policy(const struct ariel_led_in *in);

#endif /* ARIEL_UI_H */
