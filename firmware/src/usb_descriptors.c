// USB descriptors: what the Pico tells the laptop it is.
//   Interface 0+1: CDC ACM serial port (/dev/ttyACM0) for debug printf()
//   Interface 2:   HID boot keyboard, used to wake the laptop
//   Interface 3:   HID system control (the "Sleep" key), used to suspend it
//   Interface 4:   Raspberry Pi reset interface (picotool load -f, reboot)
// Based on pico-sdk/src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c and
// pico-examples/usb/device/dev_hid_composite/usb_descriptors.c.
#include <string.h>

#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "tusb.h"

// 0xCafe is TinyUSB's test vendor ID: fine for a personal device, not for
// products you sell.
#define USBD_VID            0xCafe
#define USBD_PID            0x4012

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_HID_SYS,
    ITF_NUM_RESET,
    ITF_NUM_TOTAL
};

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_HID,
    STR_HID_SYS,
    STR_RESET,
};

#define EPNUM_CDC_NOTIF     0x81
#define EPNUM_CDC_OUT       0x02
#define EPNUM_CDC_IN        0x82
#define EPNUM_HID           0x83
#define EPNUM_HID_SYS       0x84

// ---- Device descriptor --------------------------------------------------------

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // "Miscellaneous / IAD": required because CDC uses two interfaces that
    // must be grouped by an Interface Association Descriptor.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// ---- HID report descriptors ---------------------------------------------------

// HID instance numbers follow the order of the HID interfaces below.
// Instance 0: standard 8-byte boot keyboard report (modifiers, reserved,
// 6 keycodes). No report ID, as the boot protocol requires.
static const uint8_t desc_hid_keyboard_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

// Instance 1: 1-byte system control report. Value 1 = Power Down,
// 2 = Sleep, 3 = Wake Up, 0 = nothing pressed. Linux maps Sleep to
// KEY_SLEEP, which logind handles with HandleSuspendKey=suspend.
// Kept on its own interface so the keyboard stays a plain boot keyboard.
static const uint8_t desc_hid_system_report[] = {
    TUD_HID_REPORT_DESC_SYSTEM_CONTROL()
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance == 0 ? desc_hid_keyboard_report : desc_hid_system_report;
}

// ---- Configuration descriptor -------------------------------------------------

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

static const uint8_t desc_configuration[] = {
    // REMOTE_WAKEUP tells the host this device may wake it from suspend.
    // Without this bit the host never grants permission and
    // tud_remote_wakeup() will always fail.
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STR_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // Boot-protocol keyboard. Linux enables wakeup by default for these
    // (usbhid sets power/wakeup=enabled for boot keyboards).
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_keyboard_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID_SYS, STR_HID_SYS, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_system_report), EPNUM_HID_SYS,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STR_RESET),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---- String descriptors -------------------------------------------------------

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const string_desc[] = {
    [STR_MANUFACTURER] = "pico-wakeup",
    [STR_PRODUCT]      = "Pico W Wakeup",
    [STR_SERIAL]       = serial_str,
    [STR_CDC]          = "Pico W Wakeup Debug",
    [STR_HID]          = "Pico W Wakeup Keyboard",
    [STR_HID_SYS]      = "Pico W Wakeup System Control",
    [STR_RESET]        = "Reset",
};

// USB strings are UTF-16; this buffer must outlive the control transfer.
static uint16_t desc_str_buf[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t len;

    if (index == STR_LANGID) {
        desc_str_buf[1] = 0x0409; // English (US)
        len = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0]) || !string_desc[index]) {
            return NULL;
        }
        if (index == STR_SERIAL && !serial_str[0]) {
            pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
        }
        const char *str = string_desc[index];
        len = (uint8_t)strlen(str);
        if (len > 31) {
            len = 31;
        }
        for (uint8_t i = 0; i < len; i++) {
            desc_str_buf[1 + i] = (uint8_t)str[i];
        }
    }

    // First word: descriptor type and total length in bytes
    desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str_buf;
}
