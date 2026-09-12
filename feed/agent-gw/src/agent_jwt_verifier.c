#include "agent_jwt_verifier.h"

#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENT_JWT_HEADER_MAX 1024U
#define AGENT_JWT_PAYLOAD_MAX 4096U
#define AGENT_JWT_SIGNATURE_MAX 1024U

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static int base64url_value(unsigned char character)
{
    if (character >= 'A' && character <= 'Z') {
        return (int)(character - 'A');
    }
    if (character >= 'a' && character <= 'z') {
        return (int)(character - 'a') + 26;
    }
    if (character >= '0' && character <= '9') {
        return (int)(character - '0') + 52;
    }
    if (character == '-') {
        return 62;
    }
    if (character == '_') {
        return 63;
    }
    return -1;
}

static bool decode_base64url(
    const char *encoded,
    size_t encoded_length,
    uint8_t *decoded,
    size_t decoded_capacity,
    size_t *decoded_length
)
{
    uint32_t accumulator = 0U;
    unsigned int bits = 0U;
    size_t output = 0U;
    size_t index;
    int value;

    if (encoded == NULL || decoded == NULL || decoded_length == NULL ||
        encoded_length == 0U || encoded_length % 4U == 1U) {
        return false;
    }
    for (index = 0U; index < encoded_length; index++) {
        value = base64url_value((unsigned char)encoded[index]);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6U) | (uint32_t)value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (output >= decoded_capacity) {
                return false;
            }
            decoded[output++] =
                (uint8_t)((accumulator >> bits) & 0xffU);
            if (bits == 0U) {
                accumulator = 0U;
            } else {
                accumulator &= (1U << bits) - 1U;
            }
        }
    }
    if (accumulator != 0U) {
        return false;
    }
    *decoded_length = output;
    return true;
}

static struct json_object *parse_json_object(
    const uint8_t *data,
    size_t length
)
{
    struct json_tokener *tokener;
    struct json_object *object;
    enum json_tokener_error error;

    if (data == NULL || length == 0U || length > (size_t)INT32_MAX) {
        return NULL;
    }
    tokener = json_tokener_new_ex(16);
    if (tokener == NULL) {
        return NULL;
    }
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT |
                           JSON_TOKENER_VALIDATE_UTF8);
    object = json_tokener_parse_ex(tokener, (const char *)data,
                                   (int)length);
    error = json_tokener_get_error(tokener);
    if (error != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != length ||
        object == NULL ||
        !json_object_is_type(object, json_type_object)) {
        if (object != NULL) {
            json_object_put(object);
        }
        object = NULL;
    }
    json_tokener_free(tokener);
    return object;
}

static bool get_json_string(
    struct json_object *object,
    const char *name,
    char *target,
    size_t capacity
)
{
    struct json_object *value;
    const char *text;
    size_t length;

    if (!json_object_object_get_ex(object, name, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return false;
    }
    text = json_object_get_string(value);
    length = (size_t)json_object_get_string_len(value);
    return text != NULL && length > 0U && length < capacity &&
           strlen(text) == length &&
           copy_text(target, capacity, text);
}

static bool get_json_u64(
    struct json_object *object,
    const char *name,
    bool required,
    uint64_t *target
)
{
    struct json_object *value;
    int64_t number;

    if (!json_object_object_get_ex(object, name, &value)) {
        return !required;
    }
    if (!json_object_is_type(value, json_type_int)) {
        return false;
    }
    number = json_object_get_int64(value);
    if (number < 0) {
        return false;
    }
    *target = (uint64_t)number;
    return true;
}

static bool audience_matches(
    struct json_object *payload,
    const char *expected
)
{
    struct json_object *audience;
    struct json_object *item;
    const char *text;
    size_t index;
    size_t count;
    bool matched = false;

    if (!json_object_object_get_ex(payload, "aud", &audience)) {
        return false;
    }
    if (json_object_is_type(audience, json_type_string)) {
        return strcmp(json_object_get_string(audience), expected) == 0;
    }
    if (!json_object_is_type(audience, json_type_array)) {
        return false;
    }
    count = json_object_array_length(audience);
    for (index = 0U; index < count; index++) {
        item = json_object_array_get_idx(audience, index);
        if (item == NULL ||
            !json_object_is_type(item, json_type_string)) {
            return false;
        }
        text = json_object_get_string(item);
        if (text != NULL && strcmp(text, expected) == 0) {
            matched = true;
        }
    }
    return matched;
}

static enum agent_jwt_result parse_header(
    struct json_object *header,
    char *key_id,
    size_t key_id_capacity,
    bool *transaction_token
)
{
    struct json_object *type_value;
    char algorithm[16];
    const char *type;

    if (key_id == NULL || key_id_capacity == 0U ||
        transaction_token == NULL) {
        return AGENT_JWT_INVALID_ARGUMENT;
    }
    *transaction_token = false;
    if (!get_json_string(header, "alg", algorithm, sizeof(algorithm)) ||
        strcmp(algorithm, "RS256") != 0) {
        return AGENT_JWT_UNSUPPORTED_ALGORITHM;
    }
    if (json_object_object_get_ex(header, "typ", &type_value)) {
        if (!json_object_is_type(type_value, json_type_string)) {
            return AGENT_JWT_UNSUPPORTED_ALGORITHM;
        }
        type = json_object_get_string(type_value);
        if (type != NULL && strcmp(type, "txntoken+jwt") == 0) {
            *transaction_token = true;
        } else if (type == NULL ||
                   (strcmp(type, "at+jwt") != 0 &&
                    strcmp(type, "JWT") != 0)) {
            return AGENT_JWT_UNSUPPORTED_ALGORITHM;
        }
    }
    if (!get_json_string(header, "kid", key_id, key_id_capacity)) {
        return AGENT_JWT_KEY_ID_MISMATCH;
    }
    return AGENT_JWT_OK;
}

static bool parse_claims(
    struct json_object *payload,
    const struct agent_auth_policy *policy,
    bool transaction_token,
    struct agent_auth_claims *claims
)
{
    struct json_object *target_agent;

    memset(claims, 0, sizeof(*claims));
    claims->transaction_token = transaction_token;
    if (!get_json_string(payload, "iss", claims->issuer,
                         sizeof(claims->issuer)) ||
        !audience_matches(payload, policy->audience) ||
        !copy_text(claims->audience, sizeof(claims->audience),
                   policy->audience) ||
        !get_json_string(payload, "sub", claims->subject,
                         sizeof(claims->subject)) ||
        !get_json_string(payload, "tenant", claims->tenant,
                         sizeof(claims->tenant)) ||
        !get_json_string(payload, "scope", claims->scope,
                         sizeof(claims->scope)) ||
        !get_json_u64(payload, "iat", true, &claims->issued_at) ||
        !get_json_u64(payload, "nbf", false, &claims->not_before) ||
        !get_json_u64(payload, "exp", true, &claims->expires_at)) {
        return false;
    }
    if (json_object_object_get_ex(payload, "target_agent", &target_agent) &&
        (!json_object_is_type(target_agent, json_type_string) ||
         !copy_text(claims->target_agent,
                    sizeof(claims->target_agent),
                    json_object_get_string(target_agent)))) {
        return false;
    }
    if (transaction_token) {
        if (!get_json_string(payload, "txn", claims->transaction_id,
                             sizeof(claims->transaction_id))) {
            return false;
        }
    } else if (!get_json_string(payload, "jti", claims->jti,
                                sizeof(claims->jti))) {
        return false;
    }
    if (claims->not_before == 0U) {
        claims->not_before = claims->issued_at;
    }
    return true;
}

enum agent_jwt_result agent_jwt_verifier_init(
    struct agent_jwt_verifier *verifier,
    const char *public_key_file,
    const char *key_id,
    const struct agent_auth_policy *policy
)
{
    struct agent_jwt_key *key;

    if (verifier == NULL || public_key_file == NULL ||
        policy == NULL || key_id == NULL) {
        return AGENT_JWT_INVALID_ARGUMENT;
    }
    memset(verifier, 0, sizeof(*verifier));
    key = &verifier->keys[0];
    verifier->key_count = 1U;
    mbedtls_pk_init(&key->public_key);
    key->initialized = true;
    if (!copy_text(key->key_id, sizeof(key->key_id), key_id) ||
        mbedtls_pk_parse_public_keyfile(
            &key->public_key, public_key_file) != 0 ||
        !mbedtls_pk_can_do(&key->public_key, MBEDTLS_PK_RSA) ||
        mbedtls_pk_get_bitlen(&key->public_key) < 2048U ||
        mbedtls_pk_get_bitlen(&key->public_key) > 4096U) {
        agent_jwt_verifier_free(verifier);
        return AGENT_JWT_KEY_LOAD_FAILED;
    }
    verifier->policy = *policy;
    verifier->initialized = true;
    return AGENT_JWT_OK;
}

void agent_jwt_verifier_free(struct agent_jwt_verifier *verifier)
{
    size_t index;

    if (verifier == NULL) {
        return;
    }
    for (index = 0U; index < verifier->key_count; index++) {
        if (verifier->keys[index].initialized) {
            mbedtls_pk_free(&verifier->keys[index].public_key);
        }
    }
    memset(verifier, 0, sizeof(*verifier));
}

static struct agent_jwt_key *find_key(
    struct agent_jwt_verifier *verifier,
    const char *key_id
)
{
    size_t index;

    for (index = 0U; index < verifier->key_count; index++) {
        if (verifier->keys[index].initialized &&
            strcmp(verifier->keys[index].key_id, key_id) == 0) {
            return &verifier->keys[index];
        }
    }
    return NULL;
}

enum agent_jwt_result agent_jwt_verify(
    struct agent_jwt_verifier *verifier,
    const char *token,
    size_t token_length,
    uint64_t now_seconds,
    struct agent_auth_claims *claims,
    enum agent_auth_result *claim_result
)
{
    const mbedtls_md_info_t *sha256;
    const char *first_dot;
    const char *second_dot;
    struct json_object *header = NULL;
    struct json_object *payload = NULL;
    uint8_t header_data[AGENT_JWT_HEADER_MAX];
    uint8_t payload_data[AGENT_JWT_PAYLOAD_MAX];
    uint8_t signature[AGENT_JWT_SIGNATURE_MAX];
    unsigned char digest[32];
    size_t header_length;
    size_t payload_length;
    size_t signature_length;
    size_t signing_length;
    enum agent_jwt_result result = AGENT_JWT_MALFORMED;
    bool transaction_token = false;
    char key_id[AGENT_JWT_KID_LEN];
    struct agent_jwt_key *key;

    if (verifier == NULL || !verifier->initialized ||
        token == NULL || claims == NULL || claim_result == NULL ||
        token_length == 0U || token_length > AGENT_JWT_MAX_TOKEN_LEN) {
        return AGENT_JWT_INVALID_ARGUMENT;
    }
    *claim_result = AGENT_AUTH_INVALID_ARGUMENT;
    first_dot = memchr(token, '.', token_length);
    if (first_dot == NULL) {
        return AGENT_JWT_MALFORMED;
    }
    second_dot = memchr(first_dot + 1, '.',
                        token_length - (size_t)(first_dot + 1 - token));
    if (second_dot == NULL ||
        memchr(second_dot + 1, '.',
               token_length - (size_t)(second_dot + 1 - token)) != NULL) {
        return AGENT_JWT_MALFORMED;
    }

    if (!decode_base64url(token, (size_t)(first_dot - token),
                          header_data, sizeof(header_data),
                          &header_length) ||
        !decode_base64url(first_dot + 1,
                          (size_t)(second_dot - first_dot - 1),
                          payload_data, sizeof(payload_data),
                          &payload_length) ||
        !decode_base64url(second_dot + 1,
                          token_length -
                              (size_t)(second_dot + 1 - token),
                          signature, sizeof(signature),
                          &signature_length)) {
        return AGENT_JWT_MALFORMED;
    }

    header = parse_json_object(header_data, header_length);
    payload = parse_json_object(payload_data, payload_length);
    if (header == NULL || payload == NULL) {
        goto done;
    }
    result = parse_header(header, key_id, sizeof(key_id),
                          &transaction_token);
    if (result != AGENT_JWT_OK) {
        goto done;
    }

    key = find_key(verifier, key_id);
    if (key == NULL) {
        result = AGENT_JWT_KEY_ID_MISMATCH;
        goto done;
    }
    signing_length = (size_t)(second_dot - token);
    sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (sha256 == NULL ||
        mbedtls_md(sha256, (const unsigned char *)token,
                   signing_length, digest) != 0 ||
        mbedtls_pk_verify(&key->public_key, MBEDTLS_MD_SHA256,
                          digest, sizeof(digest),
                          signature, signature_length) != 0) {
        result = AGENT_JWT_SIGNATURE_INVALID;
        goto done;
    }
    if (!parse_claims(payload, &verifier->policy,
                      transaction_token, claims)) {
        result = AGENT_JWT_CLAIMS_INVALID;
        goto done;
    }
    *claim_result = agent_auth_validate_claims(
        &verifier->policy, claims, now_seconds);
    result = *claim_result == AGENT_AUTH_OK ?
        AGENT_JWT_OK : AGENT_JWT_CLAIMS_INVALID;

done:
    if (header != NULL) {
        json_object_put(header);
    }
    if (payload != NULL) {
        json_object_put(payload);
    }
    memset(digest, 0, sizeof(digest));
    memset(signature, 0, sizeof(signature));
    return result;
}

const char *agent_jwt_result_name(enum agent_jwt_result result)
{
    switch (result) {
    case AGENT_JWT_OK:
        return "ok";
    case AGENT_JWT_INVALID_ARGUMENT:
        return "invalid_argument";
    case AGENT_JWT_MALFORMED:
        return "malformed";
    case AGENT_JWT_UNSUPPORTED_ALGORITHM:
        return "unsupported_algorithm_or_key";
    case AGENT_JWT_KEY_ID_MISMATCH:
        return "key_id_mismatch";
    case AGENT_JWT_SIGNATURE_INVALID:
        return "signature_invalid";
    case AGENT_JWT_CLAIMS_INVALID:
        return "claims_invalid";
    case AGENT_JWT_KEY_LOAD_FAILED:
        return "key_load_failed";
    default:
        return "unknown";
    }
}
