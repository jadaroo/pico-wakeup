// lwIP configuration for this project.
// Reference: https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html
// Based on pico-examples/pico_w/wifi/lwipopts_examples_common.h and the
// pico-examples MQTT example, trimmed to what this program uses.
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// ---- Operating mode --------------------------------------------------------

// No RTOS: lwIP has no threads of its own. It runs only when cyw43_arch_poll()
// is called, and talks to us through callbacks ("raw API").
#define NO_SYS                      1
// The BSD socket and netconn APIs need threads (NO_SYS 0), so they are off.
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// ---- Memory ----------------------------------------------------------------

// Use the C library malloc() for lwIP's heap. Only safe with the *poll* arch,
// where lwIP never runs from an interrupt; the background arch must use 0.
#define MEM_LIBC_MALLOC             1
#define MEM_ALIGNMENT               4
// Fixed pools: TCP segments in flight, packets queued while ARP resolves,
// and receive buffers handed over by the Wi-Fi driver.
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
// MQTT: the client runs a 1-second cyclic timer (keepalive pings, request
// timeouts). Timers come from a fixed pool, so reserve one extra slot.
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)

// ---- Network interface (the CYW43 Wi-Fi driver) -----------------------------

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1   // Wi-Fi frames look like Ethernet to lwIP
#define LWIP_NETIF_STATUS_CALLBACK  1   // used by cyw43 to report link changes
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1   // DHCP sends a hostname to the router
#define LWIP_NETIF_TX_SINGLE_PBUF   1   // cyw43 driver needs each frame in one buffer
// Compute checksums in C (algorithm 3 is the fastest portable one on M0+).
#define LWIP_CHKSUM_ALGORITHM       3

// ---- Protocols ---------------------------------------------------------------

#define LWIP_IPV4                   1
#define LWIP_ICMP                   1   // answer pings, handy for debugging
#define LWIP_RAW                    1
#define LWIP_UDP                    1   // DHCP and DNS run over UDP
#define LWIP_DHCP                   1   // get an IP address from the router
#define DHCP_DOES_ARP_CHECK         0   // skip the "is this IP taken" probe:
#define LWIP_DHCP_DOES_ACD_CHECK    0   //   faster connect, router is trusted
// MQTT: we connect to the broker by hostname, so we need DNS.
#define LWIP_DNS                    1
// MQTT: the protocol itself runs over a TCP connection.
#define LWIP_TCP                    1
#define TCP_MSS                     1460
// Receive window. MQTT: TLS records can be 16 KB and mbedTLS only hands data
// to MQTT once a whole record has arrived, so the window must be larger than
// one record or the connection stalls (see pico-examples mqtt/lwipopts.h).
#define TCP_WND                     32768
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1

// ---- TLS (MQTT over port 8883) -----------------------------------------------

// altcp = "application layered TCP": a TCP-like API that can be backed by
// plain TCP or by TLS. lwIP's MQTT client uses altcp, so turning on the
// mbedTLS backend gives us MQTT over TLS without changing MQTT code.
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
// Reject the connection if the broker's certificate does not verify against
// our CA. lwIP's default is VERIFY_OPTIONAL, which *ignores* failures.
#define ALTCP_MBEDTLS_AUTHMODE      MBEDTLS_SSL_VERIFY_REQUIRED

// ---- MQTT client (lwip/apps/mqtt.h) -------------------------------------------

// Requests (subscribe, QoS 1 publishes) waiting for the broker's ack. We may
// have a subscribe plus a few status publishes outstanding at once.
#define MQTT_REQ_MAX_IN_FLIGHT      8
// Outgoing buffer for encoded MQTT packets; our status messages are short.
#define MQTT_OUTPUT_RINGBUF_SIZE    512
// Incoming buffer: holds the topic + first payload chunk of each message.
// Longer payloads arrive as multiple data callbacks (see main.c).
#define MQTT_VAR_HEADER_BUFFER_LEN  128

// ---- Debug / statistics --------------------------------------------------------

#define LWIP_STATS                  0
#ifndef NDEBUG
#define LWIP_DEBUG                  1   // enables LWIP_ASSERT checks
#endif
#define MQTT_DEBUG                  LWIP_DBG_OFF

#endif
