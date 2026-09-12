#include "agent_forwarding_assertion.h"

#include <mbedtls/md.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define FORWARDING_PREFIX "nfa1."
#define FORWARDING_PAYLOAD_VERSION 1U
#define FORWARDING_PAYLOAD_FIXED 68U
#define FORWARDING_PAYLOAD_MAX 1024U
#define FORWARDING_SIGNATURE_MAX 512U

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL) return capacity;
    for (length = 0U; length < capacity; length++) {
        if (text[length] == '\0') return length;
    }
    return capacity;
}

static bool copy_text(char *output, size_t capacity, const char *text)
{
    size_t length = bounded_length(text, capacity);

    if (length == 0U || length >= capacity) return false;
    memcpy(output, text, length + 1U);
    return true;
}

static bool key_id_valid(const char *text)
{
    size_t length = bounded_length(text, AGENT_FORWARDING_KID_LEN);
    size_t index;

    if (length == 0U || length >= AGENT_FORWARDING_KID_LEN) return false;
    for (index = 0U; index < length; index++) {
        unsigned char value = (unsigned char)text[index];
        if (!isalnum(value) && value != '-' && value != '_') return false;
    }
    return true;
}

static bool router_id_valid(const char *text)
{
    size_t length = bounded_length(text, AGENT_IPC_ROUTER_ID_LEN);
    size_t index;

    if (length == 0U || length >= AGENT_IPC_ROUTER_ID_LEN) return false;
    for (index = 0U; index < length; index++) {
        unsigned char value = (unsigned char)text[index];
        if (!isalnum(value) && value != '.' && value != '-' && value != '_' &&
            value != ':') return false;
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

static bool put_text(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *text,
    size_t maximum
)
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

static bool get_text(
    const uint8_t *input,
    size_t input_length,
    size_t *offset,
    char *output,
    size_t capacity
)
{
    size_t length;

    if (*offset > input_length || input_length - *offset < 2U) return false;
    length = get_u16(input + *offset);
    *offset += 2U;
    if (length == 0U || length >= capacity ||
        input_length - *offset < length) return false;
    memcpy(output, input + *offset, length);
    output[length] = '\0';
    *offset += length;
    return true;
}

static bool sha256(const uint8_t *data, size_t length, uint8_t output[32])
{
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    return info != NULL && mbedtls_md(info, data, length, output) == 0;
}

static const char base64url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static bool encode_base64url(
    const uint8_t *input,
    size_t input_length,
    char *output,
    size_t capacity,
    size_t *written
)
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

static bool decode_base64url(
    const char *input,
    size_t input_length,
    uint8_t *output,
    size_t capacity,
    size_t *written
)
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

static bool context_fields_valid(
    const struct agent_forwarding_context *context
)
{
    return context != NULL && context->source_router_id != NULL &&
        context->target_router_id != NULL && context->source_agent != NULL &&
        context->tenant != NULL && context->intent != NULL &&
        context->task_id != NULL && context->hop_limit > 0U;
}

enum agent_forwarding_result agent_forwarding_signer_init(
    struct agent_forwarding_signer *signer,
    const char *private_key_file,
    const char *key_id,
    const char *issuer,
    uint32_t ttl_seconds
)
{
    static const unsigned char personalization[] =
        "nexus-forwarding-assertion-p61";

    if (signer == NULL || private_key_file == NULL ||
        !key_id_valid(key_id) || !router_id_valid(issuer) ||
        ttl_seconds == 0U || ttl_seconds > AGENT_FORWARDING_MAX_TTL_SECONDS) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    memset(signer, 0, sizeof(*signer));
    mbedtls_pk_init(&signer->private_key);
    mbedtls_entropy_init(&signer->entropy);
    mbedtls_ctr_drbg_init(&signer->drbg);
    if (!copy_text(signer->key_id, sizeof(signer->key_id), key_id) ||
        !copy_text(signer->issuer, sizeof(signer->issuer), issuer) ||
        mbedtls_ctr_drbg_seed(
            &signer->drbg, mbedtls_entropy_func, &signer->entropy,
            personalization, sizeof(personalization) - 1U) != 0 ||
        mbedtls_pk_parse_keyfile(
            &signer->private_key, private_key_file, NULL,
            mbedtls_ctr_drbg_random, &signer->drbg) != 0 ||
        !mbedtls_pk_can_do(&signer->private_key, MBEDTLS_PK_RSA)) {
        agent_forwarding_signer_free(signer);
        return AGENT_FORWARDING_KEY_LOAD_FAILED;
    }
    signer->ttl_seconds = ttl_seconds;
    signer->initialized = true;
    return AGENT_FORWARDING_OK;
}

void agent_forwarding_signer_free(struct agent_forwarding_signer *signer)
{
    if (signer == NULL) return;
    mbedtls_pk_free(&signer->private_key);
    mbedtls_ctr_drbg_free(&signer->drbg);
    mbedtls_entropy_free(&signer->entropy);
    memset(signer, 0, sizeof(*signer));
}

enum agent_forwarding_result agent_forwarding_issue(
    struct agent_forwarding_signer *signer,
    const struct agent_forwarding_context *context,
    uint64_t now_seconds,
    char *output,
    size_t capacity
)
{
    uint8_t payload[FORWARDING_PAYLOAD_MAX];
    uint8_t signature[FORWARDING_SIGNATURE_MAX];
    uint8_t digest[AGENT_FORWARDING_DIGEST_LEN];
    size_t payload_length = FORWARDING_PAYLOAD_FIXED;
    size_t encoded_length;
    size_t signature_length = 0U;
    size_t offset;

    if (signer == NULL || !signer->initialized ||
        !context_fields_valid(context) || context->body == NULL ||
        context->body_length == 0U ||
        output == NULL || capacity < 8U ||
        now_seconds == 0U ||
        now_seconds > UINT64_MAX - signer->ttl_seconds) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    memset(payload, 0, sizeof(payload));
    payload[0] = FORWARDING_PAYLOAD_VERSION;
    payload[1] = context->hop_limit;
    put_u64(payload + 4U, now_seconds);
    put_u64(payload + 12U, now_seconds + signer->ttl_seconds);
    if (mbedtls_ctr_drbg_random(
            &signer->drbg, payload + 20U, AGENT_FORWARDING_NONCE_LEN) != 0 ||
        !sha256(context->body, context->body_length, payload + 36U) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  signer->issuer, sizeof(signer->issuer)) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->source_router_id, AGENT_IPC_ROUTER_ID_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->target_router_id, AGENT_IPC_ROUTER_ID_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->source_agent, AGENT_IPC_AGENT_ID_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->tenant, AGENT_IPC_TENANT_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->intent, AGENT_IPC_INTENT_LEN) ||
        !put_text(payload, sizeof(payload), &payload_length,
                  context->task_id, AGENT_IPC_TASK_ID_LEN)) {
        return AGENT_FORWARDING_CRYPTO_FAILED;
    }
    offset = (size_t)snprintf(output, capacity, "%s%s.",
                              FORWARDING_PREFIX, signer->key_id);
    if (offset >= capacity ||
        !encode_base64url(payload, payload_length, output + offset,
                          capacity - offset, &encoded_length)) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    offset += encoded_length;
    if (offset + 1U >= capacity) return AGENT_FORWARDING_INVALID_ARGUMENT;
    output[offset++] = '.';
    output[offset] = '\0';
    if (!sha256((const uint8_t *)output, offset - 1U, digest) ||
        mbedtls_pk_sign(
            &signer->private_key, MBEDTLS_MD_SHA256,
            digest, sizeof(digest), signature, sizeof(signature),
            &signature_length, mbedtls_ctr_drbg_random, &signer->drbg) != 0 ||
        !encode_base64url(signature, signature_length, output + offset,
                          capacity - offset, &encoded_length)) {
        memset(output, 0, capacity);
        return AGENT_FORWARDING_CRYPTO_FAILED;
    }
    memset(payload, 0, sizeof(payload));
    memset(signature, 0, sizeof(signature));
    memset(digest, 0, sizeof(digest));
    return AGENT_FORWARDING_OK;
}

enum agent_forwarding_result agent_forwarding_verifier_init(
    struct agent_forwarding_verifier *verifier,
    const char *public_key_file,
    const char *key_id,
    const char *issuer,
    uint32_t clock_skew_seconds,
    uint32_t max_ttl_seconds
)
{
    if (verifier == NULL || public_key_file == NULL || max_ttl_seconds == 0U ||
        !key_id_valid(key_id) || !router_id_valid(issuer) ||
        max_ttl_seconds > AGENT_FORWARDING_MAX_TTL_SECONDS) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    memset(verifier, 0, sizeof(*verifier));
    mbedtls_pk_init(&verifier->public_key);
    if (!copy_text(verifier->key_id, sizeof(verifier->key_id), key_id) ||
        !copy_text(verifier->issuer, sizeof(verifier->issuer), issuer) ||
        mbedtls_pk_parse_public_keyfile(
            &verifier->public_key, public_key_file) != 0 ||
        !mbedtls_pk_can_do(&verifier->public_key, MBEDTLS_PK_RSA)) {
        agent_forwarding_verifier_free(verifier);
        return AGENT_FORWARDING_KEY_LOAD_FAILED;
    }
    verifier->clock_skew_seconds = clock_skew_seconds;
    verifier->max_ttl_seconds = max_ttl_seconds;
    verifier->initialized = true;
    return AGENT_FORWARDING_OK;
}

void agent_forwarding_verifier_free(
    struct agent_forwarding_verifier *verifier
)
{
    if (verifier == NULL) return;
    mbedtls_pk_free(&verifier->public_key);
    memset(verifier, 0, sizeof(*verifier));
}

enum agent_forwarding_result agent_forwarding_verify(
    struct agent_forwarding_verifier *verifier,
    const char *assertion,
    const struct agent_forwarding_context *expected,
    uint64_t now_seconds,
    struct agent_forwarding_verified *verified
)
{
    uint8_t payload[FORWARDING_PAYLOAD_MAX];
    uint8_t signature[FORWARDING_SIGNATURE_MAX];
    uint8_t digest[AGENT_FORWARDING_DIGEST_LEN];
    const char *kid;
    const char *payload_part;
    const char *signature_part;
    const char *dot;
    size_t payload_length;
    size_t signature_length;
    size_t offset = FORWARDING_PAYLOAD_FIXED;
    size_t signing_length;
    char issuer[AGENT_FORWARDING_ISSUER_LEN];
    char source_router[AGENT_IPC_ROUTER_ID_LEN];
    char target_router[AGENT_IPC_ROUTER_ID_LEN];
    char source_agent[AGENT_IPC_AGENT_ID_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char intent[AGENT_IPC_INTENT_LEN];
    char task_id[AGENT_IPC_TASK_ID_LEN];
    uint64_t issued_at;
    uint64_t expires_at;
    size_t index;

    if (verifier == NULL || !verifier->initialized || assertion == NULL ||
        !context_fields_valid(expected) || verified == NULL ||
        strncmp(assertion, FORWARDING_PREFIX,
                sizeof(FORWARDING_PREFIX) - 1U) != 0) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    kid = assertion + sizeof(FORWARDING_PREFIX) - 1U;
    dot = strchr(kid, '.');
    if (dot == NULL || (size_t)(dot - kid) != strlen(verifier->key_id) ||
        memcmp(kid, verifier->key_id, (size_t)(dot - kid)) != 0) {
        return AGENT_FORWARDING_KEY_ID_MISMATCH;
    }
    payload_part = dot + 1U;
    dot = strchr(payload_part, '.');
    if (dot == NULL || strchr(dot + 1U, '.') != NULL) {
        return AGENT_FORWARDING_MALFORMED;
    }
    signature_part = dot + 1U;
    signing_length = (size_t)(dot - assertion);
    if (!decode_base64url(payload_part, (size_t)(dot - payload_part),
                          payload, sizeof(payload), &payload_length) ||
        !decode_base64url(signature_part, strlen(signature_part), signature,
                          sizeof(signature), &signature_length) ||
        payload_length < FORWARDING_PAYLOAD_FIXED ||
        payload[0] != FORWARDING_PAYLOAD_VERSION || payload[1] == 0U ||
        payload[2] != 0U || payload[3] != 0U) {
        return AGENT_FORWARDING_MALFORMED;
    }
    if (!sha256((const uint8_t *)assertion, signing_length, digest) ||
        mbedtls_pk_verify(
            &verifier->public_key, MBEDTLS_MD_SHA256,
            digest, sizeof(digest), signature, signature_length) != 0) {
        return AGENT_FORWARDING_SIGNATURE_INVALID;
    }
    issued_at = get_u64(payload + 4U);
    expires_at = get_u64(payload + 12U);
    if (!get_text(payload, payload_length, &offset,
                  issuer, sizeof(issuer)) ||
        !get_text(payload, payload_length, &offset,
                  source_router, sizeof(source_router)) ||
        !get_text(payload, payload_length, &offset,
                  target_router, sizeof(target_router)) ||
        !get_text(payload, payload_length, &offset,
                  source_agent, sizeof(source_agent)) ||
        !get_text(payload, payload_length, &offset,
                  tenant, sizeof(tenant)) ||
        !get_text(payload, payload_length, &offset,
                  intent, sizeof(intent)) ||
        !get_text(payload, payload_length, &offset,
                  task_id, sizeof(task_id)) || offset != payload_length) {
        return AGENT_FORWARDING_MALFORMED;
    }
    if (strcmp(issuer, verifier->issuer) != 0) {
        return AGENT_FORWARDING_ISSUER_MISMATCH;
    }
    if (expires_at <= issued_at ||
        expires_at - issued_at > verifier->max_ttl_seconds) {
        return AGENT_FORWARDING_LIFETIME_EXCEEDED;
    }
    if ((issued_at > now_seconds &&
         issued_at - now_seconds > verifier->clock_skew_seconds) ||
        (now_seconds > expires_at &&
         now_seconds - expires_at > verifier->clock_skew_seconds)) {
        return AGENT_FORWARDING_EXPIRED;
    }
    if (strcmp(source_router, expected->source_router_id) != 0 ||
        strcmp(target_router, expected->target_router_id) != 0 ||
        strcmp(source_agent, expected->source_agent) != 0 ||
        strcmp(tenant, expected->tenant) != 0 ||
        strcmp(intent, expected->intent) != 0 ||
        strcmp(task_id, expected->task_id) != 0 ||
        expected->hop_limit > payload[1]) {
        return AGENT_FORWARDING_CONTEXT_MISMATCH;
    }
    memset(verified, 0, sizeof(*verified));
    (void)copy_text(verified->issuer, sizeof(verified->issuer), issuer);
    verified->issued_at = issued_at;
    verified->expires_at = expires_at;
    verified->initial_hop_limit = payload[1];
    memcpy(verified->body_sha256, payload + 36U,
           sizeof(verified->body_sha256));
    for (index = 0U; index < AGENT_FORWARDING_NONCE_LEN; index++) {
        (void)snprintf(verified->nonce + index * 2U, 3U,
                       "%02x", payload[20U + index]);
    }
    memset(payload, 0, sizeof(payload));
    memset(signature, 0, sizeof(signature));
    memset(digest, 0, sizeof(digest));
    return AGENT_FORWARDING_OK;
}

enum agent_forwarding_result agent_forwarding_verify_body(
    const struct agent_forwarding_verified *verified,
    const uint8_t *body,
    size_t body_length
)
{
    uint8_t digest[AGENT_FORWARDING_DIGEST_LEN];

    if (verified == NULL || body == NULL || body_length == 0U ||
        !sha256(body, body_length, digest)) {
        return AGENT_FORWARDING_INVALID_ARGUMENT;
    }
    if (memcmp(digest, verified->body_sha256, sizeof(digest)) != 0) {
        return AGENT_FORWARDING_BODY_MISMATCH;
    }
    return AGENT_FORWARDING_OK;
}

const char *agent_forwarding_result_name(enum agent_forwarding_result result)
{
    switch (result) {
    case AGENT_FORWARDING_OK: return "ok";
    case AGENT_FORWARDING_INVALID_ARGUMENT: return "invalid_argument";
    case AGENT_FORWARDING_KEY_LOAD_FAILED: return "key_load_failed";
    case AGENT_FORWARDING_CRYPTO_FAILED: return "crypto_failed";
    case AGENT_FORWARDING_MALFORMED: return "malformed";
    case AGENT_FORWARDING_KEY_ID_MISMATCH: return "key_id_mismatch";
    case AGENT_FORWARDING_SIGNATURE_INVALID: return "signature_invalid";
    case AGENT_FORWARDING_ISSUER_MISMATCH: return "issuer_mismatch";
    case AGENT_FORWARDING_EXPIRED: return "expired";
    case AGENT_FORWARDING_LIFETIME_EXCEEDED: return "lifetime_exceeded";
    case AGENT_FORWARDING_CONTEXT_MISMATCH: return "context_mismatch";
    case AGENT_FORWARDING_BODY_MISMATCH: return "body_mismatch";
    default: return "unknown";
    }
}
