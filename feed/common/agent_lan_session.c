#include "agent_lan_session.h"

#ifdef NEXUS_LAN_SESSION_USE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include <mbedtls/md.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SESSION_PREFIX "nls1."
#define SESSION_PAYLOAD_VERSION 1U
#define SESSION_PAYLOAD_FIXED 40U
#define SESSION_PAYLOAD_MAX 512U
#define SESSION_SIGNATURE_LEN 32U

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL) return capacity;
    for (length = 0U; length < capacity; length++) {
        if (text[length] == '\0') return length;
    }
    return capacity;
}

bool agent_lan_session_secret_is_valid(const char *secret)
{
    size_t index;

    if (bounded_length(secret, AGENT_LAN_SESSION_SECRET_LEN + 1U) !=
        AGENT_LAN_SESSION_SECRET_LEN) return false;
    for (index = 0U; index < AGENT_LAN_SESSION_SECRET_LEN; index++) {
        if (!isdigit((unsigned char)secret[index]) &&
            (secret[index] < 'a' || secret[index] > 'f')) return false;
    }
    return true;
}

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static void put_u64(uint8_t *output, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; index++) {
        output[7U - index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; index++) value = (value << 8U) | input[index];
    return value;
}

static bool put_text(uint8_t *output, size_t capacity, size_t *offset,
                     const char *text, size_t maximum)
{
    size_t length = bounded_length(text, maximum);

    if (length == 0U || length >= maximum || length > UINT16_MAX ||
        *offset > capacity || capacity - *offset < length + 2U) return false;
    put_u16(output + *offset, (uint16_t)length);
    *offset += 2U;
    memcpy(output + *offset, text, length);
    *offset += length;
    return true;
}

static bool get_text(const uint8_t *input, size_t input_length, size_t *offset,
                     char *output, size_t capacity)
{
    size_t length;

    if (*offset > input_length || input_length - *offset < 2U) return false;
    length = get_u16(input + *offset);
    *offset += 2U;
    if (length == 0U || length >= capacity || input_length - *offset < length) {
        return false;
    }
    memcpy(output, input + *offset, length);
    output[length] = '\0';
    *offset += length;
    return true;
}

static const char base64url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static bool encode_base64url(const uint8_t *input, size_t input_length,
                             char *output, size_t capacity, size_t *written)
{
    size_t required = (input_length / 3U) * 4U +
        (input_length % 3U == 0U ? 0U : input_length % 3U + 1U);
    size_t source = 0U;
    size_t target = 0U;

    if (capacity <= required) return false;
    while (input_length - source >= 3U) {
        uint32_t value = ((uint32_t)input[source] << 16U) |
            ((uint32_t)input[source + 1U] << 8U) | input[source + 2U];
        output[target++] = base64url[(value >> 18U) & 63U];
        output[target++] = base64url[(value >> 12U) & 63U];
        output[target++] = base64url[(value >> 6U) & 63U];
        output[target++] = base64url[value & 63U];
        source += 3U;
    }
    if (input_length - source == 1U) {
        uint32_t value = (uint32_t)input[source] << 16U;
        output[target++] = base64url[(value >> 18U) & 63U];
        output[target++] = base64url[(value >> 12U) & 63U];
    } else if (input_length - source == 2U) {
        uint32_t value = ((uint32_t)input[source] << 16U) |
            ((uint32_t)input[source + 1U] << 8U);
        output[target++] = base64url[(value >> 18U) & 63U];
        output[target++] = base64url[(value >> 12U) & 63U];
        output[target++] = base64url[(value >> 6U) & 63U];
    }
    output[target] = '\0';
    *written = target;
    return target == required;
}

static int base64url_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
}

static bool decode_base64url(const char *input, size_t input_length,
                             uint8_t *output, size_t capacity, size_t *written)
{
    uint32_t accumulator = 0U;
    unsigned int bits = 0U;
    size_t index;
    size_t target = 0U;

    if (input_length == 0U || input_length % 4U == 1U) return false;
    for (index = 0U; index < input_length; index++) {
        int value = base64url_value((unsigned char)input[index]);
        if (value < 0) return false;
        accumulator = (accumulator << 6U) | (uint32_t)value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (target >= capacity) return false;
            output[target++] = (uint8_t)(accumulator >> bits);
            accumulator &= bits == 0U ? 0U : ((1U << bits) - 1U);
        }
    }
    if (accumulator != 0U) return false;
    *written = target;
    return true;
}

static bool hmac(const char *secret, const uint8_t *input, size_t input_length,
                 uint8_t output[SESSION_SIGNATURE_LEN])
{
#ifdef NEXUS_LAN_SESSION_USE_OPENSSL
    unsigned int output_length = 0U;

    return HMAC(EVP_sha256(), secret, (int)AGENT_LAN_SESSION_SECRET_LEN,
                input, input_length, output, &output_length) != NULL &&
           output_length == SESSION_SIGNATURE_LEN;
#else
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    return info != NULL && mbedtls_md_hmac(
        info, (const unsigned char *)secret, AGENT_LAN_SESSION_SECRET_LEN,
        input, input_length, output) == 0;
#endif
}

static bool constant_equal(const uint8_t *left, const uint8_t *right,
                           size_t length)
{
    uint8_t difference = 0U;
    size_t index;

    for (index = 0U; index < length; index++) difference |= left[index] ^ right[index];
    return difference == 0U;
}

enum agent_lan_session_result agent_lan_session_issue(
    const char *secret, const char *source_address, const char *tenant,
    const char *origin, uint64_t now_seconds, uint32_t ttl_seconds,
    const uint8_t nonce[AGENT_LAN_SESSION_NONCE_LEN], char *output,
    size_t capacity)
{
    uint8_t payload[SESSION_PAYLOAD_MAX] = {0};
    uint8_t signature[SESSION_SIGNATURE_LEN];
    size_t payload_length = SESSION_PAYLOAD_FIXED;
    size_t encoded_length;
    size_t offset;

    if (!agent_lan_session_secret_is_valid(secret) || source_address == NULL ||
        tenant == NULL || origin == NULL || nonce == NULL || output == NULL ||
        now_seconds == 0U || ttl_seconds == 0U ||
        ttl_seconds > AGENT_LAN_SESSION_MAX_TTL_SECONDS ||
        now_seconds > UINT64_MAX - ttl_seconds) return AGENT_LAN_SESSION_INVALID_ARGUMENT;
    payload[0] = SESSION_PAYLOAD_VERSION;
    put_u64(payload + 8U, now_seconds);
    put_u64(payload + 16U, now_seconds + ttl_seconds);
    memcpy(payload + 24U, nonce, AGENT_LAN_SESSION_NONCE_LEN);
    if (!put_text(payload, sizeof(payload), &payload_length, source_address,
                  AGENT_LAN_SESSION_SOURCE_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length, tenant,
                  AGENT_IPC_TENANT_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length, origin,
                  AGENT_IPC_URI_LEN)) return AGENT_LAN_SESSION_INVALID_ARGUMENT;
    offset = (size_t)snprintf(output, capacity, "%s", SESSION_PREFIX);
    if (offset >= capacity ||
        !encode_base64url(payload, payload_length, output + offset,
                          capacity - offset, &encoded_length)) {
        return AGENT_LAN_SESSION_INVALID_ARGUMENT;
    }
    offset += encoded_length;
    if (offset + 1U >= capacity) return AGENT_LAN_SESSION_INVALID_ARGUMENT;
    output[offset++] = '.';
    output[offset] = '\0';
    if (!hmac(secret, (const uint8_t *)output, offset - 1U, signature) ||
        !encode_base64url(signature, sizeof(signature), output + offset,
                          capacity - offset, &encoded_length)) {
        memset(output, 0, capacity);
        return AGENT_LAN_SESSION_CRYPTO_FAILED;
    }
    memset(payload, 0, sizeof(payload));
    memset(signature, 0, sizeof(signature));
    return AGENT_LAN_SESSION_OK;
}

enum agent_lan_session_result agent_lan_session_verify(
    const char *secret, const char *token, const char *source_address,
    uint64_t now_seconds, struct agent_lan_session_claims *claims)
{
    uint8_t payload[SESSION_PAYLOAD_MAX];
    uint8_t supplied[SESSION_SIGNATURE_LEN];
    uint8_t expected[SESSION_SIGNATURE_LEN];
    const char *signature;
    size_t token_length;
    size_t payload_length;
    size_t signature_length;
    size_t signing_length;
    size_t offset = SESSION_PAYLOAD_FIXED;

    if (!agent_lan_session_secret_is_valid(secret) || token == NULL ||
        source_address == NULL || now_seconds == 0U || claims == NULL) {
        return AGENT_LAN_SESSION_INVALID_ARGUMENT;
    }
    token_length = bounded_length(token, AGENT_LAN_SESSION_TOKEN_MAX);
    if (token_length >= AGENT_LAN_SESSION_TOKEN_MAX ||
        strncmp(token, SESSION_PREFIX, sizeof(SESSION_PREFIX) - 1U) != 0 ||
        (signature = strrchr(token, '.')) == NULL ||
        signature <= token + sizeof(SESSION_PREFIX) - 1U) {
        return AGENT_LAN_SESSION_MALFORMED;
    }
    signing_length = (size_t)(signature - token);
    if (!decode_base64url(token + sizeof(SESSION_PREFIX) - 1U,
                          signing_length - (sizeof(SESSION_PREFIX) - 1U),
                          payload, sizeof(payload), &payload_length) ||
        !decode_base64url(signature + 1U,
                          token_length - signing_length - 1U,
                          supplied, sizeof(supplied), &signature_length) ||
        signature_length != sizeof(supplied) ||
        !hmac(secret, (const uint8_t *)token, signing_length, expected)) {
        return AGENT_LAN_SESSION_MALFORMED;
    }
    if (!constant_equal(supplied, expected, sizeof(expected))) {
        return AGENT_LAN_SESSION_SIGNATURE_INVALID;
    }
    memset(claims, 0, sizeof(*claims));
    if (payload_length < SESSION_PAYLOAD_FIXED ||
        payload[0] != SESSION_PAYLOAD_VERSION ||
        !get_text(payload, payload_length, &offset, claims->source_address,
                  sizeof(claims->source_address)) ||
        !get_text(payload, payload_length, &offset, claims->tenant,
                  sizeof(claims->tenant)) ||
        !get_text(payload, payload_length, &offset, claims->origin,
                  sizeof(claims->origin)) || offset != payload_length) {
        return AGENT_LAN_SESSION_MALFORMED;
    }
    claims->issued_at = get_u64(payload + 8U);
    claims->expires_at = get_u64(payload + 16U);
    if (strcmp(claims->source_address, source_address) != 0) {
        return AGENT_LAN_SESSION_SOURCE_MISMATCH;
    }
    if (claims->expires_at <= claims->issued_at ||
        claims->expires_at - claims->issued_at >
            AGENT_LAN_SESSION_MAX_TTL_SECONDS) {
        return AGENT_LAN_SESSION_LIFETIME_EXCEEDED;
    }
    if (now_seconds < claims->issued_at || now_seconds >= claims->expires_at) {
        return AGENT_LAN_SESSION_EXPIRED;
    }
    return AGENT_LAN_SESSION_OK;
}

const char *agent_lan_session_result_name(enum agent_lan_session_result result)
{
    switch (result) {
    case AGENT_LAN_SESSION_OK: return "ok";
    case AGENT_LAN_SESSION_INVALID_ARGUMENT: return "invalid_argument";
    case AGENT_LAN_SESSION_MALFORMED: return "malformed";
    case AGENT_LAN_SESSION_SIGNATURE_INVALID: return "signature_invalid";
    case AGENT_LAN_SESSION_SOURCE_MISMATCH: return "source_mismatch";
    case AGENT_LAN_SESSION_EXPIRED: return "expired";
    case AGENT_LAN_SESSION_LIFETIME_EXCEEDED: return "lifetime_exceeded";
    case AGENT_LAN_SESSION_CRYPTO_FAILED: return "crypto_failed";
    }
    return "unknown";
}
