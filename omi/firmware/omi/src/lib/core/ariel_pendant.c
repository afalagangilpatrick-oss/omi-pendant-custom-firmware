#include "ariel_pendant.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "button.h"
#include "capture_mute.h"
#include "haptic.h"
#include "led.h"
#include "transport.h"
#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
#include "sd_card.h"
#endif

LOG_MODULE_REGISTER(ariel, CONFIG_LOG_DEFAULT_LEVEL);

extern bool is_off;

#define ARIEL_LED_LEVEL 30            /* fixed brightness, independent of the app's dim setting */
#define ARIEL_LED_TICK_MS 50
#define ARIEL_ENFORCE_BONDED_MS 20000 /* an unencrypted link is dropped after this */
#define ARIEL_ENFORCE_PAIRING_MS 60000
#define ARIEL_WIPE_DELAY_MS 15000
#define ARIEL_WIPE_KEY "ariel/ring_wiped"

/* ---------------------------------------------------------------- state */

static atomic_t pairing_open;   /* explicit window from the 10 s hold */
static atomic_t pairing_paired; /* a bond completed during the current window */
static atomic_t fail_active;
static atomic_t powering_off;
static atomic_val_t fail_ms;
static atomic_val_t power_off_ms;
static uint32_t boot_ms;
static bool boot_marked;

static struct k_work_delayable led_work;
static struct k_work_delayable pairing_timeout_work;
static struct k_work_delayable power_off_work;
static struct k_work_delayable enforce_work;
static struct bt_conn *enforce_conn;
static K_MUTEX_DEFINE(enforce_lock);
static bool works_ready;

static void ensure_works(void);

/* ---------------------------------------------------------------- LED */

static void led_apply(const struct ariel_led_out *o)
{
    set_led_pwm(LED_RED, o->red ? ARIEL_LED_LEVEL : 0);
    set_led_pwm(LED_GREEN, o->green ? ARIEL_LED_LEVEL : 0);
    set_led_pwm(LED_BLUE, o->blue ? ARIEL_LED_LEVEL : 0);
}

static void led_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    uint32_t now = k_uptime_get_32();
    struct ariel_led_in in = {
        .now_ms = now,
        .boot_ms = boot_ms,
        .powering_off = atomic_get(&powering_off) != 0,
        .power_off_ms = (uint32_t) atomic_get(&power_off_ms),
        .pairing_open = atomic_get(&pairing_open) != 0,
        .fail_active = atomic_get(&fail_active) != 0,
        .fail_ms = (uint32_t) atomic_get(&fail_ms),
        .off = is_off,
    };
    if (!boot_marked) {
        in.boot_ms = now - ARIEL_GLOW_MS; /* no boot glow before ariel_boot_mark() */
    }
    struct ariel_led_out out = ariel_led_policy(&in);
    if (out.fail_done) {
        atomic_clear(&fail_active);
    }
    if (is_off) {
        return; /* turnoff_all() owns the LEDs now */
    }
    led_apply(&out);

    bool animating = in.pairing_open || (in.fail_active && !out.fail_done) ||
                     (in.powering_off && (uint32_t) (now - in.power_off_ms) < ARIEL_GLOW_MS) ||
                     (boot_marked && (uint32_t) (now - boot_ms) < ARIEL_GLOW_MS);
    if (animating) {
        k_work_reschedule(&led_work, K_MSEC(ARIEL_LED_TICK_MS));
    }
}

void ariel_led_kick(void)
{
    ensure_works();
    k_work_reschedule(&led_work, K_NO_WAIT);
}

void ariel_boot_mark(void)
{
    ensure_works();
    boot_ms = k_uptime_get_32();
    boot_marked = true;
    ariel_led_kick();
}

static void fail_flash(void)
{
    atomic_set(&fail_ms, (atomic_val_t) k_uptime_get_32());
    atomic_set(&fail_active, 1);
    ariel_led_kick();
}

/* ---------------------------------------------------------------- bonds + pairing */

static void count_bond(const struct bt_bond_info *info, void *data)
{
    ARG_UNUSED(info);
    (*(int *) data)++;
}

static int bond_count(void)
{
    int n = 0;
    bt_foreach_bond(BT_ID_DEFAULT, count_bond, &n);
    return n;
}

bool ariel_pairing_is_open(void)
{
    return atomic_get(&pairing_open) != 0;
}

static void pairing_timeout_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    bool paired = atomic_get(&pairing_paired) != 0;
    atomic_clear(&pairing_open);
    LOG_INF("Pairing window closed (%s)", paired ? "paired" : "timed out");
    if (!paired) {
        fail_flash();
    } else {
        ariel_led_kick();
    }
}

void ariel_pairing_open(void)
{
    ensure_works();
    atomic_clear(&pairing_paired);
    atomic_set(&pairing_open, 1);
    k_work_reschedule(&pairing_timeout_work, K_MSEC(ARIEL_PAIRING_WINDOW_MS));
    LOG_INF("Pairing window open for %u s", ARIEL_PAIRING_WINDOW_MS / 1000);
    /* One link at a time: free it so the new phone or laptop can connect. */
    struct bt_conn *conn = get_current_connection_ref();
    if (conn != NULL) {
        (void) bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    }
    ariel_led_kick();
}

static bool pairing_allowed(void)
{
    /* First install with no bonds at all: accept, or nobody could ever connect. */
    return ariel_pairing_is_open() || bond_count() == 0;
}

static enum bt_security_err pairing_accept(struct bt_conn *conn, const struct bt_conn_pairing_feat *const feat)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(feat);
    if (pairing_allowed()) {
        LOG_INF("Pairing request accepted");
        return BT_SECURITY_ERR_SUCCESS;
    }
    LOG_WRN("Pairing request refused: hold the button 10 s to open pairing");
    return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    ARG_UNUSED(conn);
    LOG_INF("Pairing complete (bonded %d, bonds now %d)", bonded, bond_count());
    if (bonded && ariel_pairing_is_open()) {
        atomic_set(&pairing_paired, 1);
        /* Close now; the timeout handler sees "paired" and stays quiet. */
        k_work_reschedule(&pairing_timeout_work, K_NO_WAIT);
    }
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    ARG_UNUSED(conn);
    LOG_WRN("Pairing failed (reason %d)", reason);
    if (ariel_pairing_is_open()) {
        fail_flash();
    }
}

static struct bt_conn_auth_cb auth_cb = {
    .pairing_accept = pairing_accept,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

/* ---------------------------------------------------------------- link enforcement */

static void enforce_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    k_mutex_lock(&enforce_lock, K_FOREVER);
    struct bt_conn *conn = enforce_conn;
    enforce_conn = NULL;
    k_mutex_unlock(&enforce_lock);
    if (conn == NULL) {
        return;
    }
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) {
        LOG_WRN("Dropping a link that never encrypted");
        (void) bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
    bt_conn_unref(conn);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        return;
    }
    int rc = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (rc) {
        LOG_WRN("Security request failed (%d)", rc);
    }
    k_mutex_lock(&enforce_lock, K_FOREVER);
    if (enforce_conn != NULL) {
        bt_conn_unref(enforce_conn);
    }
    enforce_conn = bt_conn_ref(conn);
    k_mutex_unlock(&enforce_lock);
    k_work_reschedule(&enforce_work,
                      K_MSEC(pairing_allowed() ? ARIEL_ENFORCE_PAIRING_MS : ARIEL_ENFORCE_BONDED_MS));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(reason);
    k_mutex_lock(&enforce_lock, K_FOREVER);
    if (enforce_conn == conn) {
        bt_conn_unref(enforce_conn);
        enforce_conn = NULL;
    }
    k_mutex_unlock(&enforce_lock);
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    LOG_INF("Security level %d (err %d)", level, err);
    if (err == BT_SECURITY_ERR_SUCCESS && level >= BT_SECURITY_L2) {
        k_mutex_lock(&enforce_lock, K_FOREVER);
        if (enforce_conn == conn) {
            bt_conn_unref(enforce_conn);
            enforce_conn = NULL;
        }
        k_mutex_unlock(&enforce_lock);
    }
}

static struct bt_conn_cb ariel_conn_cb = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .security_changed = on_security_changed,
};

int ariel_security_init(void)
{
    ensure_works();
    int err = bt_conn_auth_cb_register(&auth_cb);
    if (err) {
        LOG_ERR("auth cb register failed (%d)", err);
        return err;
    }
    err = bt_conn_auth_info_cb_register(&auth_info_cb);
    if (err) {
        LOG_ERR("auth info cb register failed (%d)", err);
        return err;
    }
    err = bt_conn_cb_register(&ariel_conn_cb);
    if (err) {
        LOG_ERR("conn cb register failed (%d)", err);
        return err;
    }
    LOG_INF("Bonded-only links on; %d bond(s) stored", bond_count());
    return 0;
}

void ariel_safe_mode_bt_setup(void)
{
    /* Recovery may pair (SMP needs an encrypted link) but must not store a
     * bond: SAFE never loads the real bond list, so a stored key here could
     * push Ariel's phone or laptop out of it. */
    bt_set_bondable(false);
}

/* ---------------------------------------------------------------- power off */

static void power_off_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    turnoff_all(); /* does not return on success */
}

/* ---------------------------------------------------------------- gestures */

void ariel_handle_gesture(struct ariel_event evt)
{
    ensure_works();
    switch (evt.type) {
    case ARIEL_EVT_PRESS:
        button_notify_code(4);
        break;
    case ARIEL_EVT_RELEASE:
        button_notify_code(5);
        break;
    case ARIEL_EVT_CLICKS:
        if (evt.clicks == 1) {
            button_notify_code(1);
        } else if (evt.clicks == 2) {
            button_notify_code(2);
        }
        break;
    case ARIEL_EVT_CAPTURE_ON:
        LOG_INF("4 clicks: capture on");
        if (capture_mute_set(false) == 0) {
            play_haptic_milli(80);
        }
        button_notify_code(ARIEL_BTN_CAPTURE_ON);
        break;
    case ARIEL_EVT_CAPTURE_OFF:
        LOG_INF("5 clicks: capture off");
        (void) capture_mute_set(true); /* fails closed: capture stays off */
        play_haptic_milli(250);
        button_notify_code(ARIEL_BTN_CAPTURE_OFF);
        break;
    case ARIEL_EVT_HOLD_PAIRING:
        play_haptic_milli(40); /* "you can let go now for pairing" */
        break;
    case ARIEL_EVT_PAIRING:
        button_notify_code(ARIEL_BTN_PAIRING);
        ariel_pairing_open();
        break;
    case ARIEL_EVT_POWER_OFF:
        LOG_INF("20 s hold: powering off");
        atomic_set(&power_off_ms, (atomic_val_t) k_uptime_get_32());
        atomic_set(&powering_off, 1);
        ariel_led_kick();
        k_work_reschedule(&power_off_work, K_MSEC(ARIEL_GLOW_MS));
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- first-boot wipe */

static int wipe_flag_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
    ARG_UNUSED(key);
    uint8_t v = 0;
    if (len == sizeof(v) && read_cb(cb_arg, &v, sizeof(v)) == sizeof(v)) {
        *(uint8_t *) param = v;
    }
    return 0;
}

#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
static K_THREAD_STACK_DEFINE(wipe_stack, 1536);
static struct k_thread wipe_thread;

static void wipe_entry(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    k_msleep(ARIEL_WIPE_DELAY_MS);
    uint8_t done = 0;
    (void) settings_load_subtree_direct(ARIEL_WIPE_KEY, wipe_flag_cb, &done);
    if (done) {
        return;
    }
    int err = sd_ring_clear();
    if (err) {
        LOG_WRN("Old recordings not wiped yet (%d); will retry next boot", err);
        return;
    }
    done = 1;
    err = settings_save_one(ARIEL_WIPE_KEY, &done, sizeof(done));
    LOG_INF("Old recordings wiped (flag save %d)", err);
}
#endif

void ariel_first_boot_wipe_schedule(void)
{
#ifdef CONFIG_OMI_ENABLE_OFFLINE_STORAGE
    k_thread_create(&wipe_thread, wipe_stack, K_THREAD_STACK_SIZEOF(wipe_stack), wipe_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    k_thread_name_set(&wipe_thread, "ariel_wipe");
#endif
}

/* ---------------------------------------------------------------- init */

static void ensure_works(void)
{
    unsigned int key = irq_lock();
    if (!works_ready) {
        k_work_init_delayable(&led_work, led_work_fn);
        k_work_init_delayable(&pairing_timeout_work, pairing_timeout_fn);
        k_work_init_delayable(&power_off_work, power_off_fn);
        k_work_init_delayable(&enforce_work, enforce_fn);
        works_ready = true;
    }
    irq_unlock(key);
}
