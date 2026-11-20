#include "ariel_ui.h"

#include <string.h>

static void push(struct ariel_gesture *g, ariel_evt_t type, uint8_t clicks)
{
    if (g->q_len >= sizeof(g->q_type)) {
        return; /* cannot happen: at most 3 events per step */
    }
    uint8_t slot = (uint8_t) ((g->q_head + g->q_len) % sizeof(g->q_type));
    g->q_type[slot] = (uint8_t) type;
    g->q_clicks[slot] = clicks;
    g->q_len++;
}

static struct ariel_event pop(struct ariel_gesture *g)
{
    struct ariel_event e = {ARIEL_EVT_NONE, 0};
    if (g->q_len == 0) {
        return e;
    }
    e.type = (ariel_evt_t) g->q_type[g->q_head];
    e.clicks = g->q_clicks[g->q_head];
    g->q_head = (uint8_t) ((g->q_head + 1) % sizeof(g->q_type));
    g->q_len--;
    return e;
}

static void close_series(struct ariel_gesture *g)
{
    if (!g->series_open) {
        return;
    }
    if (g->clicks == 4) {
        push(g, ARIEL_EVT_CAPTURE_ON, 4);
    } else if (g->clicks == 5) {
        push(g, ARIEL_EVT_CAPTURE_OFF, 5);
    } else {
        push(g, ARIEL_EVT_CLICKS, g->clicks);
    }
    g->series_open = false;
    g->clicks = 0;
}

static void drop_series(struct ariel_gesture *g)
{
    g->series_open = false;
    g->clicks = 0;
}

void ariel_gesture_init(struct ariel_gesture *g)
{
    memset(g, 0, sizeof(*g));
}

struct ariel_event ariel_gesture_step(struct ariel_gesture *g, bool pressed, uint32_t now_ms)
{
    if (!g->armed) {
        /* Ignore the press that woke the pendant until it is released. */
        if (!pressed) {
            g->armed = true;
            g->down = false;
        }
        return pop(g);
    }

    /* A series ends when no new press came within the gap. Checked before
     * edges so a late press starts a new series instead of extending this one. */
    if (g->series_open && !g->down && (uint32_t) (now_ms - g->release_ms) > ARIEL_CLICK_GAP_MS) {
        close_series(g);
    }

    if (pressed && !g->down) {
        g->down = true;
        g->press_ms = now_ms;
        g->hold_fired = false;
        g->off_fired = false;
        push(g, ARIEL_EVT_PRESS, 0);
    } else if (pressed && g->down) {
        uint32_t held = (uint32_t) (now_ms - g->press_ms);
        if (held >= ARIEL_HOLD_PAIRING_MS && !g->hold_fired) {
            g->hold_fired = true;
            drop_series(g);
            push(g, ARIEL_EVT_HOLD_PAIRING, 0);
        }
        if (held >= ARIEL_HOLD_OFF_MS && !g->off_fired) {
            g->off_fired = true;
            push(g, ARIEL_EVT_POWER_OFF, 0);
        }
    } else if (!pressed && g->down) {
        uint32_t held = (uint32_t) (now_ms - g->press_ms);
        g->down = false;
        g->release_ms = now_ms;
        push(g, ARIEL_EVT_RELEASE, 0);
        if (g->off_fired || held >= ARIEL_HOLD_OFF_MS) {
            /* Power-off already reported (or a late sample skipped it):
             * a release after 20 s never means pairing. */
            if (!g->off_fired) {
                g->off_fired = true;
                push(g, ARIEL_EVT_POWER_OFF, 0);
            }
            drop_series(g);
        } else if (held >= ARIEL_HOLD_PAIRING_MS) {
            drop_series(g);
            push(g, ARIEL_EVT_PAIRING, 0);
        } else if (held < ARIEL_CLICK_MAX_MS) {
            if (g->clicks < 255) {
                g->clicks++;
            }
            g->series_open = true;
        } else {
            /* A medium hold is not a click and breaks any series. */
            drop_series(g);
        }
    }

    return pop(g);
}

bool ariel_gesture_busy(const struct ariel_gesture *g)
{
    return g->down || g->series_open || g->q_len > 0 || !g->armed;
}

struct ariel_led_out ariel_led_policy(const struct ariel_led_in *in)
{
    struct ariel_led_out out = {false, false, false, false};

    if (in->off) {
        return out;
    }
    if (in->powering_off) {
        bool glow = (uint32_t) (in->now_ms - in->power_off_ms) < ARIEL_GLOW_MS;
        out.red = out.green = out.blue = glow;
        return out;
    }
    if (in->fail_active) {
        uint32_t idx = (uint32_t) (in->now_ms - in->fail_ms) / ARIEL_FAIL_FLASH_MS;
        if (idx < 2u * ARIEL_FAIL_FLASHES) {
            out.red = (idx % 2u) == 0u;
            return out;
        }
        out.fail_done = true;
    }
    if ((uint32_t) (in->now_ms - in->boot_ms) < ARIEL_GLOW_MS) {
        out.red = out.green = out.blue = true;
        return out;
    }
    if (in->pairing_open) {
        out.blue = ((in->now_ms / 500u) % 2u) == 0u;
        return out;
    }
    return out;
}
