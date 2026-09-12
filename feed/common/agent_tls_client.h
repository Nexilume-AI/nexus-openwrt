#ifndef NEXUS_AGENT_TLS_CLIENT_H
#define NEXUS_AGENT_TLS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

enum agent_tls_io_result {
    AGENT_TLS_IO_OK = 0,
    AGENT_TLS_IO_WANT_READ,
    AGENT_TLS_IO_WANT_WRITE,
    AGENT_TLS_IO_CLOSED,
    AGENT_TLS_IO_ERROR
};

struct agent_tls_client_global {
    bool initialized;
    mbedtls_x509_crt ca_chain;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config config;
};

struct agent_tls_client_connection {
    bool initialized;
    int fd;
    mbedtls_ssl_context ssl;
};

bool agent_tls_client_global_init(
    struct agent_tls_client_global *global,
    const char *ca_file
);

void agent_tls_client_global_free(
    struct agent_tls_client_global *global
);

bool agent_tls_client_connection_init(
    const struct agent_tls_client_global *global,
    struct agent_tls_client_connection *connection,
    int fd,
    const char *server_identity
);

void agent_tls_client_connection_free(
    struct agent_tls_client_connection *connection
);

enum agent_tls_io_result agent_tls_client_handshake(
    struct agent_tls_client_connection *connection
);

bool agent_tls_client_peer_sha256_matches(
    const struct agent_tls_client_connection *connection,
    const char *expected_hex
);

enum agent_tls_io_result agent_tls_client_write(
    struct agent_tls_client_connection *connection,
    const unsigned char *buffer,
    size_t length,
    size_t *written
);

enum agent_tls_io_result agent_tls_client_read(
    struct agent_tls_client_connection *connection,
    unsigned char *buffer,
    size_t capacity,
    size_t *received
);

#endif
