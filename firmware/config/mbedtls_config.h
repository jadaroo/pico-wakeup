// mbedTLS configuration: a TLS 1.2 *client* able to talk to EMQX Cloud.
// Based on pico-examples/pico_w/wifi/mbedtls_config_examples_common.h.
// EMQX uses DigiCert RSA certificate chains signed with SHA-256, which the
// RSA / ECDHE-RSA + AES-GCM + SHA-256/384 set below covers.
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// Some mbedTLS sources use INT_MAX without including limits.h.
#include <limits.h>

// ---- Platform integration (provided by the Pico SDK) ----
#define MBEDTLS_NO_PLATFORM_ENTROPY     // no /dev/urandom on a microcontroller...
#define MBEDTLS_ENTROPY_HARDWARE_ALT    // ...SDK supplies entropy from the RP2040 ROSC
#define MBEDTLS_HAVE_TIME               // millisecond clock for handshake timeouts
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_ALLOW_PRIVATE_ACCESS    // lwIP's glue touches internal fields
// No wall-clock date (no MBEDTLS_HAVE_TIME_DATE): certificate expiry dates
// are NOT checked, only the signature chain and hostname.

// ---- Protocol ----
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  // SNI: shared cloud hosts need the hostname
#define MBEDTLS_SSL_OUT_CONTENT_LEN     2048 // we only send small packets: save RAM
// (Incoming records may be 16 KB; that buffer size stays at the default.)

// ---- Key exchange and ciphers ----
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED  // preferred: forward secrecy
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED        // fallback
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM          // much faster P-256/P-384 on Cortex-M0+
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES        // smaller flash/RAM, slightly slower
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C

// ---- Certificates (RSA, X.509, PEM) ----
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C             // ca_cert.h is PEM text...
#define MBEDTLS_BASE64_C                // ...which is base64
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

// ---- Hashes and randomness ----
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ERROR_C                 // readable error strings

#endif
