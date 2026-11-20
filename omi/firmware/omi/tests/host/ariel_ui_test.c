/* Host tests for ariel_ui.c (gesture state machine + LED policy).
 * cc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined \
 *   -iquote omi/firmware/omi/src/lib/core omi/firmware/omi/src/lib/core/ariel_ui.c \
 *   omi/firmware/omi/tests/host/ariel_ui_test.c -o /tmp/ariel-ui-test && /tmp/ariel-ui-test
 */
#include <stdio.h>
#include <stdlib.h>

#include "ariel_ui.h"

static int failures;
static int checks;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                      \
        }                                                                                                              \
    } while (0)

/* Simulated driver: 40 ms polling like button.c, plus edge samples. */
#define TICK 40u
static struct ariel_gesture g;
static uint32_t now;
static ariel_evt_t log_type[256];
static uint8_t log_clicks[256];
static int nlog;

static void drain(bool pressed)
{
    for (;;) {
        struct ariel_event e = ariel_gesture_step(&g, pressed, now);
        if (e.type == ARIEL_EVT_NONE) {
            break;
        }
        if (e.type == ARIEL_EVT_PRESS || e.type == ARIEL_EVT_RELEASE) {
            continue; /* raw edges are checked separately */
        }
        if (nlog < 256) {
            log_type[nlog] = e.type;
            log_clicks[nlog] = e.clicks;
            nlog++;
        }
    }
}

static void level_for(bool pressed, uint32_t ms)
{
    uint32_t end = now + ms;
    drain(pressed); /* edge */
    while ((int32_t) (end - now) > 0) {
        now += TICK;
        drain(pressed);
    }
}

static void reset(uint32_t start)
{
    ariel_gesture_init(&g);
    now = start;
    nlog = 0;
    level_for(false, 100); /* arm */
}

static void clicks(int n, uint32_t press_ms, uint32_t gap_ms)
{
    for (int i = 0; i < n; i++) {
        level_for(true, press_ms);
        level_for(false, gap_ms);
    }
}

static void settle(void)
{
    level_for(false, 1000);
}

static void test_clicks(void)
{
    for (int n = 1; n <= 7; n++) {
        reset(1000);
        clicks(n, 120, 200);
        settle();
        CHECK(nlog == 1);
        if (n == 4) {
            CHECK(log_type[0] == ARIEL_EVT_CAPTURE_ON);
        } else if (n == 5) {
            CHECK(log_type[0] == ARIEL_EVT_CAPTURE_OFF);
        } else {
            CHECK(log_type[0] == ARIEL_EVT_CLICKS);
            CHECK(log_clicks[0] == n);
        }
        CHECK(!ariel_gesture_busy(&g));
    }
}

static void test_gap_splits_series(void)
{
    reset(5000);
    clicks(2, 100, 200);
    level_for(false, 600); /* > 400 ms gap closes the series */
    clicks(2, 100, 200);
    settle();
    CHECK(nlog == 2);
    CHECK(log_type[0] == ARIEL_EVT_CLICKS && log_clicks[0] == 2);
    CHECK(log_type[1] == ARIEL_EVT_CLICKS && log_clicks[1] == 2);
}

static void test_medium_hold_breaks_series_and_never_powers_off(void)
{
    reset(0);
    clicks(3, 100, 200);
    level_for(true, 3000); /* stock firmware powered off here */
    level_for(false, 100);
    clicks(1, 100, 200);
    settle();
    /* 3 clicks dropped by the hold; then 1 click. No power-off, no pairing. */
    CHECK(nlog == 1);
    CHECK(log_type[0] == ARIEL_EVT_CLICKS && log_clicks[0] == 1);
    for (uint32_t hold = 1000; hold < ARIEL_HOLD_PAIRING_MS; hold += 1000) {
        reset(0);
        level_for(true, hold);
        settle();
        CHECK(nlog == 0);
    }
}

static void test_pairing_hold(void)
{
    uint32_t holds[] = {10000, 12000, 19900};
    for (unsigned i = 0; i < sizeof(holds) / sizeof(holds[0]); i++) {
        reset(100);
        level_for(true, holds[i] + TICK);
        settle();
        CHECK(nlog == 2);
        CHECK(log_type[0] == ARIEL_EVT_HOLD_PAIRING);
        CHECK(log_type[1] == ARIEL_EVT_PAIRING);
    }
}

static void test_power_off_hold(void)
{
    reset(100);
    level_for(true, 20000 + TICK);
    /* fires while still held */
    CHECK(nlog == 2);
    CHECK(log_type[0] == ARIEL_EVT_HOLD_PAIRING);
    CHECK(log_type[1] == ARIEL_EVT_POWER_OFF);
    level_for(true, 5000);
    settle();
    CHECK(nlog == 2); /* no pairing on release, no second power-off */
}

static void test_wake_press_ignored(void)
{
    ariel_gesture_init(&g);
    now = 0;
    nlog = 0;
    level_for(true, 25000); /* button held from boot */
    level_for(false, 1000);
    CHECK(nlog == 0);
    clicks(4, 100, 200);
    settle();
    CHECK(nlog == 1 && log_type[0] == ARIEL_EVT_CAPTURE_ON);
}

static void test_clock_wrap(void)
{
    reset(0xFFFFFFFFu - 300u);
    clicks(5, 100, 200);
    settle();
    CHECK(nlog == 1 && log_type[0] == ARIEL_EVT_CAPTURE_OFF);
    reset(0xFFFFFFFFu - 5000u);
    level_for(true, 20000 + TICK);
    CHECK(nlog == 2 && log_type[1] == ARIEL_EVT_POWER_OFF);
}

static void test_late_sample_release_after_20s(void)
{
    /* Caller missed samples: press edge then a release 21 s later with no
     * samples in between. Must be power-off, never pairing. */
    reset(0);
    drain(true);
    now += 21000;
    drain(false);
    CHECK(nlog == 1 && log_type[0] == ARIEL_EVT_POWER_OFF);
}

static void test_raw_edges(void)
{
    reset(0);
    struct ariel_event e = ariel_gesture_step(&g, true, now);
    CHECK(e.type == ARIEL_EVT_PRESS);
    now += 50;
    e = ariel_gesture_step(&g, false, now);
    CHECK(e.type == ARIEL_EVT_RELEASE);
}

static struct ariel_led_out led(uint32_t t, struct ariel_led_in in)
{
    in.now_ms = t;
    return ariel_led_policy(&in);
}

static void test_led(void)
{
    struct ariel_led_in in = {0};
    in.boot_ms = 1000;
    struct ariel_led_out o = led(1000, in);
    CHECK(o.red && o.green && o.blue);
    o = led(3999, in);
    CHECK(o.red && o.green && o.blue);
    o = led(4000, in);
    CHECK(!o.red && !o.green && !o.blue); /* dark when idle/recording */
    o = led(500000, in);
    CHECK(!o.red && !o.green && !o.blue);

    in.pairing_open = true;
    int on = 0, off = 0;
    for (uint32_t t = 10000; t < 20000; t += 250) {
        o = led(t, in);
        CHECK(!o.red && !o.green);
        if (o.blue) {
            on++;
        } else {
            off++;
        }
    }
    CHECK(on == 20 && off == 20); /* 1 Hz blink, 50 % duty */

    in.fail_active = true;
    in.fail_ms = 30000;
    int flashes = 0;
    bool prev = false;
    for (uint32_t t = 30000; t < 31000; t += 10) {
        o = led(t, in);
        if (o.red && !prev) {
            flashes++;
        }
        prev = o.red;
        CHECK(!o.green);
        if (t >= 30000 + 6 * ARIEL_FAIL_FLASH_MS) {
            CHECK(o.fail_done);
        } else {
            CHECK(!o.fail_done && !o.blue);
        }
    }
    CHECK(flashes == 3);

    struct ariel_led_in off_in = {0};
    off_in.powering_off = true;
    off_in.power_off_ms = 70000;
    off_in.boot_ms = 0;
    o = led(70000, off_in);
    CHECK(o.red && o.green && o.blue);
    o = led(72999, off_in);
    CHECK(o.red && o.green && o.blue);
    o = led(73000, off_in);
    CHECK(!o.red && !o.green && !o.blue);
    off_in.off = true;
    o = led(70000, off_in);
    CHECK(!o.red && !o.green && !o.blue);
}

int main(void)
{
    test_clicks();
    test_gap_splits_series();
    test_medium_hold_breaks_series_and_never_powers_off();
    test_pairing_hold();
    test_power_off_hold();
    test_wake_press_ignored();
    test_clock_wrap();
    test_late_sample_release_after_20s();
    test_raw_edges();
    test_led();
    printf("ariel_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
