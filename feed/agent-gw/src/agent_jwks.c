#include "agent_jwt_verifier.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENT_JWKS_RSA_BYTES_MAX 512U
#define AGENT_JWKS_EXPONENT_BYTES_MAX 8U

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

static bool get_string(
    struct json_object *object,
    const char *name,
    bool required,
    const char **value
)
{
    struct json_object *member;

    *value = NULL;
    if (!json_object_object_get_ex(object, name, &member)) {
        return !required;
    }
    if (!json_object_is_type(member, json_type_string) ||
        json_object_get_string_len(member) <= 0) {
        return false;
    }
    *value = json_object_get_string(member);
    return *value != NULL &&
           strlen(*value) ==
               (size_t)json_object_get_string_len(member);
}

static struct json_object *read_jwks(const char *path)
{
    struct json_tokener *tokener = NULL;
    struct json_object *root = NULL;
    struct stat status;
    uint8_t *data = NULL;
    ssize_t received;
    size_t offset = 0U;
    enum json_tokener_error error;
    int descriptor = -1;

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (uint64_t)status.st_size > AGENT_JWT_MAX_JWKS_BYTES) {
        goto done;
    }
    data = malloc((size_t)status.st_size);
    if (data == NULL) {
        goto done;
    }
    while (offset < (size_t)status.st_size) {
        received = read(descriptor, data + offset,
                        (size_t)status.st_size - offset);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            goto done;
        }
        offset += (size_t)received;
    }

    tokener = json_tokener_new_ex(16);
    if (tokener == NULL) {
        goto done;
    }
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT |
                           JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, (const char *)data,
                                 (int)offset);
    error = json_tokener_get_error(tokener);
    if (error != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != offset ||
        root == NULL ||
        !json_object_is_type(root, json_type_object)) {
        if (root != NULL) {
            json_object_put(root);
        }
        root = NULL;
    }

done:
    if (descriptor >= 0) {
        close(descriptor);
    }
    if (tokener != NULL) {
        json_tokener_free(tokener);
    }
    free(data);
    return root;
}

static bool key_id_is_unique(
    const struct agent_jwt_verifier *verifier,
    const char *key_id
)
{
    size_t index;

    for (index = 0U; index < verifier->key_count; index++) {
        if (strcmp(verifier->keys[index].key_id, key_id) == 0) {
            return false;
        }
    }
    return true;
}

static bool load_rsa_key(
    struct agent_jwt_key *key,
    const char *key_id,
    const char *modulus,
    const char *exponent
)
{
    uint8_t modulus_data[AGENT_JWKS_RSA_BYTES_MAX];
    uint8_t exponent_data[AGENT_JWKS_EXPONENT_BYTES_MAX];
    mbedtls_rsa_context *rsa;
    size_t modulus_length;
    size_t exponent_length;
    size_t key_id_length = strlen(key_id);

    if (key_id_length == 0U ||
        key_id_length >= sizeof(key->key_id) ||
        !decode_base64url(modulus, strlen(modulus),
                          modulus_data, sizeof(modulus_data),
                          &modulus_length) ||
        !decode_base64url(exponent, strlen(exponent),
                          exponent_data, sizeof(exponent_data),
                          &exponent_length)) {
        return false;
    }

    memset(key, 0, sizeof(*key));
    mbedtls_pk_init(&key->public_key);
    key->initialized = true;
    if (mbedtls_pk_setup(
            &key->public_key,
            mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) {
        return false;
    }
    rsa = mbedtls_pk_rsa(key->public_key);
    if (rsa == NULL ||
        mbedtls_rsa_import_raw(
            rsa,
            modulus_data, modulus_length,
            NULL, 0U, NULL, 0U, NULL, 0U,
            exponent_data, exponent_length) != 0 ||
        mbedtls_rsa_complete(rsa) != 0 ||
        mbedtls_rsa_check_pubkey(rsa) != 0 ||
        mbedtls_pk_get_bitlen(&key->public_key) < 2048U ||
        mbedtls_pk_get_bitlen(&key->public_key) > 4096U) {
        return false;
    }
    mbedtls_rsa_set_padding(
        rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);
    memcpy(key->key_id, key_id, key_id_length + 1U);
    memset(modulus_data, 0, sizeof(modulus_data));
    memset(exponent_data, 0, sizeof(exponent_data));
    return true;
}

enum agent_jwt_result agent_jwt_verifier_init_jwks(
    struct agent_jwt_verifier *verifier,
    const char *jwks_file,
    const struct agent_auth_policy *policy
)
{
    struct json_object *root = NULL;
    struct json_object *keys;
    struct json_object *candidate;
    const char *key_type;
    const char *key_use;
    const char *algorithm;
    const char *key_id;
    const char *modulus;
    const char *exponent;
    size_t index;
    size_t count;
    enum agent_jwt_result result = AGENT_JWT_KEY_LOAD_FAILED;

    if (verifier == NULL || jwks_file == NULL || policy == NULL) {
        return AGENT_JWT_INVALID_ARGUMENT;
    }
    memset(verifier, 0, sizeof(*verifier));
    root = read_jwks(jwks_file);
    if (root == NULL ||
        !json_object_object_get_ex(root, "keys", &keys) ||
        !json_object_is_type(keys, json_type_array)) {
        goto done;
    }
    count = json_object_array_length(keys);
    for (index = 0U; index < count; index++) {
        candidate = json_object_array_get_idx(keys, index);
        if (candidate == NULL ||
            !json_object_is_type(candidate, json_type_object) ||
            !get_string(candidate, "kty", true, &key_type) ||
            !get_string(candidate, "use", false, &key_use) ||
            !get_string(candidate, "alg", false, &algorithm)) {
            goto done;
        }
        if (strcmp(key_type, "RSA") != 0 ||
            (key_use != NULL && strcmp(key_use, "sig") != 0) ||
            (algorithm != NULL && strcmp(algorithm, "RS256") != 0)) {
            continue;
        }
        if (verifier->key_count >= AGENT_JWT_MAX_KEYS ||
            !get_string(candidate, "kid", true, &key_id) ||
            !get_string(candidate, "n", true, &modulus) ||
            !get_string(candidate, "e", true, &exponent) ||
            !key_id_is_unique(verifier, key_id)) {
            goto done;
        }
        verifier->key_count++;
        if (!load_rsa_key(
                &verifier->keys[verifier->key_count - 1U],
                key_id, modulus, exponent)) {
            goto done;
        }
    }
    if (verifier->key_count == 0U) {
        goto done;
    }
    verifier->policy = *policy;
    verifier->initialized = true;
    result = AGENT_JWT_OK;

done:
    if (root != NULL) {
        json_object_put(root);
    }
    if (result != AGENT_JWT_OK) {
        agent_jwt_verifier_free(verifier);
    }
    return result;
}
