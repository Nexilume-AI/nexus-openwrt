#include "agent_tls_client.h"

#include <errno.h>
#include <limits.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static int socket_send(
    void *context,
    const unsigned char *buffer,
    size_t length
)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) length = (size_t)INT_MAX;
    result = send(fd, buffer, length, MSG_NOSIGNAL);
    if (result >= 0) return (int)result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int socket_receive(
    void *context,
    unsigned char *buffer,
    size_t length
)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) length = (size_t)INT_MAX;
    result = recv(fd, buffer, length, 0);
    if (result >= 0) return (int)result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static enum agent_tls_io_result translate_result(int result)
{
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return AGENT_TLS_IO_WANT_READ;
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return AGENT_TLS_IO_WANT_WRITE;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == 0) {
        return AGENT_TLS_IO_CLOSED;
    }
    return result > 0 ? AGENT_TLS_IO_OK : AGENT_TLS_IO_ERROR;
}

void agent_tls_client_global_free(
    struct agent_tls_client_global *global
)
{
    if (global == NULL) return;
    mbedtls_ssl_config_free(&global->config);
    mbedtls_ctr_drbg_free(&global->drbg);
    mbedtls_entropy_free(&global->entropy);
    mbedtls_x509_crt_free(&global->ca_chain);
    memset(global, 0, sizeof(*global));
}

bool agent_tls_client_global_init(
    struct agent_tls_client_global *global,
    const char *ca_file
)
{
    static const unsigned char personalization[] =
        "nexus-agent-gw-p274";

    if (global == NULL || ca_file == NULL || ca_file[0] == '\0') return false;
    memset(global, 0, sizeof(*global));
    mbedtls_x509_crt_init(&global->ca_chain);
    mbedtls_entropy_init(&global->entropy);
    mbedtls_ctr_drbg_init(&global->drbg);
    mbedtls_ssl_config_init(&global->config);
    if (mbedtls_x509_crt_parse_file(&global->ca_chain, ca_file) != 0 ||
        mbedtls_ctr_drbg_seed(
            &global->drbg, mbedtls_entropy_func, &global->entropy,
            personalization, sizeof(personalization) - 1U) != 0 ||
        mbedtls_ssl_config_defaults(
            &global->config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        agent_tls_client_global_free(global);
        return false;
    }
    mbedtls_ssl_conf_authmode(
        &global->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&global->config, &global->ca_chain, NULL);
    mbedtls_ssl_conf_rng(
        &global->config, mbedtls_ctr_drbg_random, &global->drbg);
    mbedtls_ssl_conf_min_tls_version(
        &global->config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(
        &global->config, MBEDTLS_SSL_VERSION_TLS1_3);
    global->initialized = true;
    return true;
}

void agent_tls_client_connection_free(
    struct agent_tls_client_connection *connection
)
{
    if (connection == NULL) return;
    mbedtls_ssl_free(&connection->ssl);
    memset(connection, 0, sizeof(*connection));
    connection->fd = -1;
}

bool agent_tls_client_connection_init(
    const struct agent_tls_client_global *global,
    struct agent_tls_client_connection *connection,
    int fd,
    const char *server_identity
)
{
    if (global == NULL || !global->initialized || connection == NULL ||
        fd < 0 || server_identity == NULL || server_identity[0] == '\0') {
        return false;
    }
    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
    mbedtls_ssl_init(&connection->ssl);
    if (mbedtls_ssl_setup(
            &connection->ssl, &global->config) != 0 ||
        mbedtls_ssl_set_hostname(
            &connection->ssl, server_identity) != 0) {
        agent_tls_client_connection_free(connection);
        return false;
    }
    mbedtls_ssl_set_bio(
        &connection->ssl, &connection->fd,
        socket_send, socket_receive, NULL);
    connection->initialized = true;
    return true;
}

enum agent_tls_io_result agent_tls_client_handshake(
    struct agent_tls_client_connection *connection
)
{
    int result;

    if (connection == NULL || !connection->initialized) {
        return AGENT_TLS_IO_ERROR;
    }
    result = mbedtls_ssl_handshake(&connection->ssl);
    if (result == 0) {
        return mbedtls_ssl_get_verify_result(&connection->ssl) == 0U
            ? AGENT_TLS_IO_OK : AGENT_TLS_IO_ERROR;
    }
    return translate_result(result);
}

bool agent_tls_client_peer_sha256_matches(
    const struct agent_tls_client_connection *connection,
    const char *expected_hex
)
{
    static const char hexadecimal[] = "0123456789abcdef";
    const mbedtls_x509_crt *certificate;
    unsigned char digest[32];
    unsigned char difference = 0U;
    size_t index;

    if (expected_hex == NULL || expected_hex[0] == '\0') return true;
    if (connection == NULL || !connection->initialized ||
        strlen(expected_hex) != sizeof(digest) * 2U) return false;
    certificate = mbedtls_ssl_get_peer_cert(&connection->ssl);
    if (certificate == NULL || certificate->raw.p == NULL ||
        certificate->raw.len == 0U ||
        mbedtls_sha256(certificate->raw.p, certificate->raw.len,
                       digest, 0) != 0) return false;
    for (index = 0U; index < sizeof(digest); index++) {
        const unsigned char high = digest[index] >> 4U;
        const unsigned char low = digest[index] & 0x0fU;
        const char high_hex = hexadecimal[high];
        const char low_hex = hexadecimal[low];
        difference |= (unsigned char)(high_hex ^ expected_hex[index * 2U]);
        difference |= (unsigned char)(low_hex ^ expected_hex[index * 2U + 1U]);
    }
    return difference == 0U;
}

enum agent_tls_io_result agent_tls_client_write(
    struct agent_tls_client_connection *connection,
    const unsigned char *buffer,
    size_t length,
    size_t *written
)
{
    int result;

    if (written != NULL) *written = 0U;
    if (connection == NULL || !connection->initialized || buffer == NULL ||
        length == 0U || written == NULL) return AGENT_TLS_IO_ERROR;
    if (length > (size_t)INT_MAX) length = (size_t)INT_MAX;
    result = mbedtls_ssl_write(&connection->ssl, buffer, length);
    if (result > 0) *written = (size_t)result;
    return translate_result(result);
}

enum agent_tls_io_result agent_tls_client_read(
    struct agent_tls_client_connection *connection,
    unsigned char *buffer,
    size_t capacity,
    size_t *received
)
{
    int result;

    if (received != NULL) *received = 0U;
    if (connection == NULL || !connection->initialized || buffer == NULL ||
        capacity == 0U || received == NULL) return AGENT_TLS_IO_ERROR;
    if (capacity > (size_t)INT_MAX) capacity = (size_t)INT_MAX;
    result = mbedtls_ssl_read(&connection->ssl, buffer, capacity);
    if (result > 0) *received = (size_t)result;
    return translate_result(result);
}
