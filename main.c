// Pico W "wakeup" device.
//
// Stays plugged into the ThinkPad. Connects to home Wi-Fi and to a cloud MQTT
// broker over TLS, listens on TOPIC_CMD, and on "wakeup" wakes the ThinkPad
// via USB remote wakeup (it presents itself as a USB keyboard).
//
// Everything runs from one loop in main(): TinyUSB (tud_task), the Wi-Fi
// driver + lwIP (cyw43_arch_poll), and the connection state machine
// (net_task). Nothing runs in the background, so no locking is needed.
//
// Reference: pico-examples/pico_w/wifi/mqtt/mqtt_client.c

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"    // mqtt_client_t fields: needed to set TLS SNI
#include "lwip/dns.h"
#include "mbedtls/ssl.h"

#include "ca_cert.h"
#include "secrets.h"
#include "usb_keyboard.h"

#define TOPIC_CMD               "pico-wakeup/pico/cmd"
#define TOPIC_STATUS            "pico-wakeup/pico/status"

#define MQTT_KEEP_ALIVE_S       30      // ping the broker if idle this long
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define DNS_TIMEOUT_MS          10000
#define MQTT_CONNECT_TIMEOUT_MS 20000   // TCP + TLS handshake + CONNACK
#define BACKOFF_MIN_MS          1000
#define BACKOFF_MAX_MS          60000
#define WATCHDOG_TIMEOUT_MS     8000    // reboot if the main loop hangs

#define MSG_MAX_LEN             64      // longer payloads are truncated

// While the laptop is asleep the USB serial port can't drain, and printf()
// would block until its timeout. Skip debug output while suspended.
#define LOG(...) do { if (!tud_suspended()) printf(__VA_ARGS__); } while (0)

// ---- Connection state machine --------------------------------------------------

typedef enum {
    NET_WIFI_START,     // begin joining the Wi-Fi network
    NET_WIFI_WAIT,      // waiting for association + DHCP
    NET_DNS_WAIT,       // waiting for the broker's IP address
    NET_MQTT_WAIT,      // waiting for TCP + TLS + MQTT CONNACK
    NET_MQTT_UP,        // connected and subscribed: normal operation
    NET_BACKOFF,        // waiting before the next retry
} net_state_t;

typedef enum { DNS_PENDING, DNS_DONE, DNS_FAILED } dns_result_t;

static net_state_t net_state = NET_WIFI_START;
static net_state_t retry_state;         // where NET_BACKOFF goes next
static absolute_time_t deadline;        // timeout / backoff end for the current state
static uint32_t backoff_ms = BACKOFF_MIN_MS;

static ip_addr_t broker_ip;
static dns_result_t dns_result;

static mqtt_client_t *mqtt_client;
static struct mqtt_connect_client_info_t client_info;
static char client_id[24];
static bool mqtt_event_pending;
static mqtt_connection_status_t mqtt_event_status;
static bool subscribe_failed;

static bool led_on;

// Incoming message being assembled from the two lwIP callbacks
static char rx_topic[64];
static char rx_payload[MSG_MAX_LEN + 1];
static size_t rx_len;

static const char *wifi_status_str(int status) {
    switch (status) {
        case CYW43_LINK_DOWN:    return "down";
        case CYW43_LINK_JOIN:    return "joining";
        case CYW43_LINK_NOIP:    return "joined, waiting for DHCP";
        case CYW43_LINK_UP:      return "up";
        case CYW43_LINK_FAIL:    return "failed";
        case CYW43_LINK_NONET:   return "SSID not found";
        case CYW43_LINK_BADAUTH: return "wrong password";
        default:                 return "unknown";
    }
}

static bool wifi_is_up(void) {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

// Retry after an exponentially growing delay (1 s, 2 s, 4 s ... 60 s), so a
// broker outage doesn't turn into a reconnect storm.
static void retry_later(net_state_t next, const char *why) {
    LOG("[net] %s, retrying in %lu ms\n", why, (unsigned long)backoff_ms);
    retry_state = next;
    deadline = make_timeout_time_ms(backoff_ms);
    backoff_ms = MIN(backoff_ms * 2, BACKOFF_MAX_MS);
    net_state = NET_BACKOFF;
}

// ---- Publishing status ---------------------------------------------------------

static void pub_request_cb(void *arg, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        LOG("[mqtt] publish not acknowledged (err %d)\n", err);
    }
}

// Publishes e.g. "event=wakeup:remote-wakeup-sent led=off usb=suspended uptime=812"
// Retained, so a client that subscribes later still sees the latest status.
static void publish_status(const char *event) {
    if (net_state != NET_MQTT_UP) {
        return;
    }
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "event=%s led=%s usb=%s uptime=%lu",
                       event, led_on ? "on" : "off",
                       usb_host_state_str(usb_keyboard_host_state()),
                       (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000));
    err_t err = mqtt_publish(mqtt_client, TOPIC_STATUS, msg, (u16_t)len,
                             1 /* QoS */, 1 /* retain */, pub_request_cb, NULL);
    if (err == ERR_OK) {
        LOG("[mqtt] -> %s: %s\n", TOPIC_STATUS, msg);
    } else {
        LOG("[mqtt] publish failed (err %d)\n", err);
    }
}

// ---- Incoming messages -----------------------------------------------------------

static void set_led(bool on) {
    led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

// Called once per complete message. topic and payload are NUL-terminated.
// This is the place to add new commands.
static void handle_message(const char *topic, char *payload) {
    // Strip trailing whitespace/newlines (e.g. from "echo wakeup | mosquitto_pub -s")
    size_t len = strlen(payload);
    while (len > 0 && isspace((unsigned char)payload[len - 1])) {
        payload[--len] = '\0';
    }

    LOG("[mqtt] <- %s: \"%s\"\n", topic, payload);

    if (strcmp(topic, TOPIC_CMD) != 0) {
        return;     // not our command topic
    }

    if (strcmp(payload, "wakeup") == 0) {
        usb_wake_result_t result = usb_keyboard_wake();
        LOG("[wake] %s\n", usb_wake_result_str(result));
        char event[64];
        snprintf(event, sizeof(event), "wakeup:%s", usb_wake_result_str(result));
        publish_status(event);
    } else if (strcmp(payload, "toggle") == 0) {
        set_led(!led_on);
        LOG("[led] %s\n", led_on ? "on" : "off");
        publish_status("toggle");
    } else if (strcmp(payload, "status") == 0) {
        publish_status("status");
    } else {
        LOG("[mqtt] unknown command \"%s\"\n", payload);
        publish_status("unknown-command");
    }
}

// lwIP callback 1 of 2: a PUBLISH arrived. Gives the topic and the total
// payload length; the payload itself follows in mqtt_incoming_data_cb().
// The topic pointer is only valid during this call, so copy it.
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    (void)arg;
    strlcpy(rx_topic, topic, sizeof(rx_topic));
    rx_len = 0;
    if (tot_len > MSG_MAX_LEN) {
        LOG("[mqtt] payload of %lu bytes will be truncated to %d\n",
            (unsigned long)tot_len, MSG_MAX_LEN);
    }
}

// lwIP callback 2 of 2: one chunk of the payload. Called one or more times;
// MQTT_DATA_FLAG_LAST marks the final chunk.
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    size_t room = MSG_MAX_LEN - rx_len;
    size_t n = len < room ? len : room;
    memcpy(rx_payload + rx_len, data, n);
    rx_len += n;

    if (flags & MQTT_DATA_FLAG_LAST) {
        rx_payload[rx_len] = '\0';
        handle_message(rx_topic, rx_payload);
    }
}

// ---- MQTT connection ------------------------------------------------------------

// Called by lwIP when the broker accepts/refuses us, or the connection drops.
// Only record the event here; net_task() acts on it.
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    (void)client; (void)arg;
    mqtt_event_status = status;
    mqtt_event_pending = true;
}

static void sub_request_cb(void *arg, err_t err) {
    (void)arg;
    if (err == ERR_OK) {
        LOG("[mqtt] subscribed to %s\n", TOPIC_CMD);
    } else {
        // Broker refused or timed out. Don't disconnect from inside an lwIP
        // callback; net_task() reconnects, which retries the subscribe.
        subscribe_failed = true;
    }
}

static void mqtt_start_connect(void) {
    LOG("[mqtt] connecting to %s (%s:%d, TLS) as %s\n", MQTT_BROKER_HOST,
        ipaddr_ntoa(&broker_ip), MQTT_TLS_PORT, client_id);

    err_t err = mqtt_client_connect(mqtt_client, &broker_ip, MQTT_TLS_PORT,
                                    mqtt_connection_cb, NULL, &client_info);
    if (err != ERR_OK) {
        retry_later(NET_DNS_WAIT, "mqtt_client_connect failed");
        return;
    }
    // SNI: tell the TLS server which hostname we want. EMQX Cloud serves many
    // deployments from shared IPs and rejects connections without it;
    // mbedTLS also checks the certificate against this name.
    mbedtls_ssl_set_hostname(altcp_tls_context(mqtt_client->conn), MQTT_BROKER_HOST);
    mqtt_set_inpub_callback(mqtt_client, mqtt_incoming_publish_cb,
                            mqtt_incoming_data_cb, NULL);

    mqtt_event_pending = false;
    subscribe_failed = false;
    deadline = make_timeout_time_ms(MQTT_CONNECT_TIMEOUT_MS);
    net_state = NET_MQTT_WAIT;
}

// ---- DNS ---------------------------------------------------------------------------

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    if (ipaddr) {
        broker_ip = *ipaddr;
        dns_result = DNS_DONE;
    } else {
        dns_result = DNS_FAILED;
    }
}

static void dns_start(void) {
    LOG("[dns] resolving %s\n", MQTT_BROKER_HOST);
    dns_result = DNS_PENDING;
    err_t err = dns_gethostbyname(MQTT_BROKER_HOST, &broker_ip, dns_found_cb, NULL);
    if (err == ERR_OK) {
        dns_result = DNS_DONE;      // answer was cached, no callback will come
    } else if (err != ERR_INPROGRESS) {
        dns_result = DNS_FAILED;
    }
    deadline = make_timeout_time_ms(DNS_TIMEOUT_MS);
    net_state = NET_DNS_WAIT;
}

// ---- Wi-Fi ------------------------------------------------------------------------

static void wifi_start(void) {
    LOG("[wifi] connecting to \"%s\"\n", WIFI_SSID);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);  // drop any half-open association
    // _async: returns immediately; progress is visible via cyw43_tcpip_link_status().
    // The blocking variant would stall USB servicing for up to the timeout.
    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        retry_later(NET_WIFI_START, "wifi connect request failed");
        return;
    }
    deadline = make_timeout_time_ms(WIFI_CONNECT_TIMEOUT_MS);
    net_state = NET_WIFI_WAIT;
}

// ---- The state machine, run every main-loop iteration --------------------------------

static void net_task(void) {
    // Wi-Fi loss is detected here for every state that needs the network.
    if (net_state == NET_DNS_WAIT || net_state == NET_MQTT_WAIT || net_state == NET_MQTT_UP) {
        if (!wifi_is_up()) {
            LOG("[wifi] link lost (%s)\n",
                wifi_status_str(cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA)));
            mqtt_disconnect(mqtt_client);   // no-op if not connected
            retry_later(NET_WIFI_START, "wifi down");
            return;
        }
    }

    switch (net_state) {
    case NET_WIFI_START:
        wifi_start();
        break;

    case NET_WIFI_WAIT: {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status == CYW43_LINK_UP) {
            LOG("[wifi] connected, IP %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
            dns_start();
        } else if (status < 0) {    // FAIL, NONET, BADAUTH
            retry_later(NET_WIFI_START, wifi_status_str(status));
        } else if (time_reached(deadline)) {
            retry_later(NET_WIFI_START, "wifi connect timeout");
        }
        break;
    }

    case NET_DNS_WAIT:
        // NET_DNS_WAIT is also the retry target for MQTT failures: if no
        // lookup is in progress, start one (the broker IP may have changed).
        if (dns_result == DNS_DONE) {
            LOG("[dns] %s -> %s\n", MQTT_BROKER_HOST, ipaddr_ntoa(&broker_ip));
            dns_result = DNS_PENDING;
            mqtt_start_connect();
        } else if (dns_result == DNS_FAILED) {
            dns_result = DNS_PENDING;
            retry_later(NET_DNS_WAIT, "dns lookup failed");
        } else if (time_reached(deadline)) {
            dns_start();
        }
        break;

    case NET_MQTT_WAIT:
        if (mqtt_event_pending) {
            mqtt_event_pending = false;
            if (mqtt_event_status == MQTT_CONNECT_ACCEPTED) {
                LOG("[mqtt] connected\n");
                backoff_ms = BACKOFF_MIN_MS;    // success: reset the backoff
                net_state = NET_MQTT_UP;
                mqtt_subscribe(mqtt_client, TOPIC_CMD, 1, sub_request_cb, NULL);
                publish_status("online");
            } else {
                // e.g. MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_ (5) = bad credentials,
                // MQTT_CONNECT_DISCONNECTED (256) = TCP/TLS failed
                LOG("[mqtt] connection failed, status %d\n", mqtt_event_status);
                retry_later(NET_DNS_WAIT, "mqtt connect failed");
            }
        } else if (time_reached(deadline)) {
            mqtt_disconnect(mqtt_client);
            retry_later(NET_DNS_WAIT, "mqtt connect timeout");
        }
        break;

    case NET_MQTT_UP:
        if (mqtt_event_pending) {
            mqtt_event_pending = false;
            // DISCONNECTED: TCP closed; TIMEOUT: broker stopped answering pings
            LOG("[mqtt] connection lost, status %d\n", mqtt_event_status);
            retry_later(NET_DNS_WAIT, "mqtt disconnected");
        } else if (subscribe_failed) {
            subscribe_failed = false;
            mqtt_disconnect(mqtt_client);
            retry_later(NET_DNS_WAIT, "subscribe failed");
        }
        break;

    case NET_BACKOFF:
        if (time_reached(deadline)) {
            if (retry_state == NET_WIFI_START) {
                net_state = NET_WIFI_START;
            } else {
                dns_start();
            }
        }
        break;
    }
}

// ---- Setup ------------------------------------------------------------------------

static void mqtt_setup(void) {
    // Client ID must be unique per broker: "pico-" + last 4 hex digits of the
    // board's flash ID.
    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(board_id, sizeof(board_id));
    snprintf(client_id, sizeof(client_id), "pico-%s", board_id + strlen(board_id) - 4);

    client_info.client_id   = client_id;
    client_info.client_user = MQTT_USERNAME;
    client_info.client_pass = MQTT_PASSWORD;
    client_info.keep_alive  = MQTT_KEEP_ALIVE_S;
    // Last Will: if we vanish without disconnecting (power loss, Wi-Fi gone),
    // the broker publishes this on our behalf after ~1.5x the keepalive.
    client_info.will_topic  = TOPIC_STATUS;
    client_info.will_msg    = "event=offline";
    client_info.will_qos    = 1;
    client_info.will_retain = 1;
    // Verify the broker's certificate chain against DigiCert Global Root G2.
    // Created once and reused for every reconnect.
    client_info.tls_config  = altcp_tls_create_config_client(
        (const u8_t *)ca_cert_pem, sizeof(ca_cert_pem));
    if (!client_info.tls_config) {
        panic("TLS config failed (bad CA certificate?)");
    }

    // One client object for the whole program; reconnects reuse it.
    mqtt_client = mqtt_client_new();
    if (!mqtt_client) {
        panic("mqtt_client_new failed");
    }
}

int main(void) {
    // Bring the Wi-Fi chip up first: it blocks for a moment to load firmware,
    // and USB enumeration (below) must not be stalled once it starts.
    if (cyw43_arch_init()) {
        panic("cyw43_arch_init failed");
    }
    cyw43_arch_enable_sta_mode();
    set_led(false);

    // We own TinyUSB, so we start it; stdio_init_all() then attaches printf()
    // to the CDC interface.
    tusb_init();
    stdio_init_all();

    // Give a terminal a moment to attach, servicing USB meanwhile, so the
    // first log lines aren't lost.
    absolute_time_t until = make_timeout_time_ms(2000);
    while (!time_reached(until)) {
        tud_task();
    }

    LOG("\n=== Pico W wakeup ===\n");
    if (watchdog_caused_reboot()) {
        LOG("[boot] previous run was reset by the watchdog\n");
    }
    mqtt_setup();
    LOG("[boot] client id %s, command topic %s\n", client_id, TOPIC_CMD);

    watchdog_enable(WATCHDOG_TIMEOUT_MS, true /* pause while debugging */);

    for (;;) {
        watchdog_update();
        tud_task();                 // USB: enumeration, serial, keyboard
        cyw43_arch_poll();          // Wi-Fi driver + lwIP: runs all network callbacks
        usb_keyboard_task();        // send queued key reports
        net_task();                 // connection state machine

        if (usb_keyboard_state_changed()) {
            usb_host_state_t s = usb_keyboard_host_state();
            LOG("[usb] host %s\n", usb_host_state_str(s));
            publish_status(s == USB_HOST_SUSPENDED ? "laptop-asleep" : "laptop-awake");
        }

        // Sleep until an interrupt (USB, Wi-Fi) or 1 ms passes.
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1));
    }
}
