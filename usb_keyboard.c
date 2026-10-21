#include "usb_keyboard.h"

#include "tusb.h"

// Tapping a key needs two reports: "Shift down", then "nothing pressed".
// Each report can only be sent once the previous one has left the endpoint
// (tud_hid_ready()), so this is a tiny state machine driven by the main loop.
typedef enum {
    TAP_IDLE,
    TAP_SEND_PRESS,
    TAP_SEND_RELEASE,
} tap_state_t;

static tap_state_t tap_state = TAP_IDLE;
// After a remote wakeup the host needs a moment to resume the bus; we queue
// a key tap and send it once tud_resume_cb() reports the bus is active.
static bool tap_after_resume = false;

static usb_host_state_t host_state = USB_HOST_UNMOUNTED;
static bool host_state_dirty = false;

static void set_host_state(usb_host_state_t state) {
    if (state != host_state) {
        host_state = state;
        host_state_dirty = true;
    }
}

// ---- TinyUSB device callbacks (called from tud_task() in the main loop) ----

void tud_mount_cb(void) {
    set_host_state(USB_HOST_ACTIVE);
}

void tud_umount_cb(void) {
    set_host_state(USB_HOST_UNMOUNTED);
}

// remote_wakeup_en: whether the host gave us permission to wake it.
void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    set_host_state(USB_HOST_SUSPENDED);
}

void tud_resume_cb(void) {
    set_host_state(tud_mounted() ? USB_HOST_ACTIVE : USB_HOST_UNMOUNTED);
    if (tap_after_resume) {
        tap_after_resume = false;
        tap_state = TAP_SEND_PRESS;
    }
}

// ---- TinyUSB HID callbacks (required, but we have nothing to report) ----

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// The host sends keyboard LED state (Caps Lock etc.) here; we ignore it.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, const uint8_t *buffer,
                           uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// ---- Public API ----

void usb_keyboard_task(void) {
    if (tap_state == TAP_IDLE || !tud_hid_ready()) {
        return;
    }
    if (tap_state == TAP_SEND_PRESS) {
        // Left Shift alone: wakes the screen without typing anything.
        tud_hid_keyboard_report(0, KEYBOARD_MODIFIER_LEFTSHIFT, NULL);
        tap_state = TAP_SEND_RELEASE;
    } else {
        tud_hid_keyboard_report(0, 0, NULL);
        tap_state = TAP_IDLE;
    }
}

usb_wake_result_t usb_keyboard_wake(void) {
    if (!tud_mounted()) {
        return USB_WAKE_NOT_MOUNTED;
    }
    if (tud_suspended()) {
        // Drives the "resume" K-state on the bus. Fails if the host did not
        // enable remote wakeup when it suspended us.
        if (!tud_remote_wakeup()) {
            return USB_WAKE_NOT_ALLOWED;
        }
        tap_after_resume = true;
        return USB_WAKE_SENT_REMOTE_WAKEUP;
    }
    tap_state = TAP_SEND_PRESS;
    return USB_WAKE_SENT_KEY_TAP;
}

usb_host_state_t usb_keyboard_host_state(void) {
    return host_state;
}

bool usb_keyboard_state_changed(void) {
    bool changed = host_state_dirty;
    host_state_dirty = false;
    return changed;
}

const char *usb_host_state_str(usb_host_state_t state) {
    switch (state) {
        case USB_HOST_ACTIVE:    return "active";
        case USB_HOST_SUSPENDED: return "suspended";
        default:                 return "unmounted";
    }
}

const char *usb_wake_result_str(usb_wake_result_t result) {
    switch (result) {
        case USB_WAKE_SENT_REMOTE_WAKEUP: return "remote-wakeup-sent";
        case USB_WAKE_SENT_KEY_TAP:       return "key-tap-sent";
        case USB_WAKE_NOT_ALLOWED:        return "remote-wakeup-not-enabled-by-host";
        default:                          return "usb-not-mounted";
    }
}
