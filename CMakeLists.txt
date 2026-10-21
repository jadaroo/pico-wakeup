cmake_minimum_required(VERSION 3.13)

# Must be set before pico_sdk_import so the SDK selects the Pico W board
# (CYW43439 Wi-Fi chip, LED on the Wi-Fi chip instead of GPIO 25).
set(PICO_BOARD pico_w)

include(pico_sdk_import.cmake)

project(wakeup C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

if (NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/secrets.h)
    message(FATAL_ERROR "secrets.h not found: cp secrets.h.example secrets.h and fill it in")
endif()

add_executable(wakeup
    main.c
    usb_keyboard.c
    usb_descriptors.c
)

# lwipopts.h, mbedtls_config.h, tusb_config.h and secrets.h live next to the
# sources; lwIP, mbedTLS and TinyUSB find their config headers via this path.
target_include_directories(wakeup PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(wakeup PRIVATE
    # Keep "picotool load -f" / 1200-baud reset working with our own USB
    # descriptors (the SDK only adds these automatically for its default ones).
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=1
    # The MS OS 2.0 descriptor only matters for driverless access on Windows.
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=0
)

target_link_libraries(wakeup
    pico_stdlib
    pico_unique_id
    hardware_watchdog
    # Wi-Fi + lwIP, everything driven from main() by cyw43_arch_poll().
    pico_cyw43_arch_lwip_poll
    # lwIP's MQTT client (lwip/apps/mqtt.h).
    pico_lwip_mqtt
    # TLS: mbedTLS itself, plus lwIP's altcp_tls glue that runs MQTT over it.
    pico_mbedtls
    pico_lwip_mbedtls
    # We drive TinyUSB ourselves (keyboard + serial), so link it directly.
    # pico_stdio_usb then uses our descriptors instead of its own.
    tinyusb_device
)

# Debug printf() goes to the USB serial port, not the UART pins.
pico_enable_stdio_usb(wakeup 1)
pico_enable_stdio_uart(wakeup 0)

pico_add_extra_outputs(wakeup)

# lwIP's mbedTLS glue ignores some return values; silence that warning.
set_source_files_properties(
    ${PICO_LWIP_PATH}/src/apps/altcp_tls/altcp_tls_mbedtls.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-result"
)
