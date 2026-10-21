// USB keyboard used to wake the laptop, the "Sleep" system key used to
// suspend it, and tracking of the USB link state.
// All functions must be called from the main loop (same context as tud_task()).
#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <stdbool.h>

typedef enum {
    USB_HOST_UNMOUNTED,     // not plugged in / not enumerated yet
    USB_HOST_ACTIVE,        // host awake and talking to us
    USB_HOST_SUSPENDED,     // host suspended the bus (laptop asleep)
} usb_host_state_t;

typedef enum {
    USB_WAKE_SENT_REMOTE_WAKEUP,    // bus was suspended; resume signal sent
    USB_WAKE_SENT_KEY_TAP,          // host was awake; tapped Shift to wake the screen
    USB_WAKE_NOT_ALLOWED,           // suspended, but host did not enable remote wakeup
    USB_WAKE_NOT_MOUNTED,           // USB not enumerated: nothing to wake
} usb_wake_result_t;

typedef enum {
    USB_SLEEP_SENT_KEY,             // "Sleep" key pressed; host should suspend
    USB_SLEEP_ALREADY_ASLEEP,       // bus already suspended: nothing to do
    USB_SLEEP_NOT_MOUNTED,          // USB not enumerated
} usb_sleep_result_t;

// Call every main-loop iteration: sends pending key press/release reports.
void usb_keyboard_task(void);

// Try to wake the host.
usb_wake_result_t usb_keyboard_wake(void);

// Press the HID "System Sleep" key, which Linux handles as KEY_SLEEP.
usb_sleep_result_t usb_keyboard_sleep(void);

usb_host_state_t usb_keyboard_host_state(void);

// Returns true once after each change of usb_keyboard_host_state().
bool usb_keyboard_state_changed(void);

const char *usb_host_state_str(usb_host_state_t state);
const char *usb_wake_result_str(usb_wake_result_t result);
const char *usb_sleep_result_str(usb_sleep_result_t result);

#endif
