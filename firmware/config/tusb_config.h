// TinyUSB configuration: USB device with one CDC (serial) and one HID
// (keyboard) interface. Based on pico-examples/usb/device/dev_hid_composite.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (the Pico SDK sets it)
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_PICO
#endif

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE // the RP2040's one USB port is a device...
#define CFG_TUD_ENABLED         1               // ...not a host
#define CFG_TUD_ENDPOINT0_SIZE  64

// Classes
#define CFG_TUD_CDC             1       // serial port for printf()
#define CFG_TUD_HID             2       // 0: boot keyboard (wake), 1: system control (sleep)
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// CDC FIFO sizes (bytes buffered between printf() and the USB endpoint)
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  256
#define CFG_TUD_CDC_EP_BUFSIZE  64

// A boot keyboard report is 8 bytes, a system control report 1 byte
#define CFG_TUD_HID_EP_BUFSIZE  16

#endif
