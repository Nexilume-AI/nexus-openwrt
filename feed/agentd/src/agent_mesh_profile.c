#include "agent_mesh_profile.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <json-c/json.h>
#include <mbedtls/sha256.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

bool agent_mesh_host_valid(const char *host)
{
    struct in_addr v4;
    struct in6_addr v6;
    size_t length, label = 0;
    if (!host || !(length = strlen(host)) || length >= AGENT_MESH_HOST_LEN) return false;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        uint32_t n = ntohl(v4.s_addr);
        return (n >> 24) != 0 && (n >> 24) != 127 && (n >> 24) < 224;
    }
    if (inet_pton(AF_INET6, host, &v6) == 1)
        return !IN6_IS_ADDR_UNSPECIFIED(&v6) && !IN6_IS_ADDR_LOOPBACK(&v6) &&
               !IN6_IS_ADDR_MULTICAST(&v6) && !IN6_IS_ADDR_LINKLOCAL(&v6) &&
               !IN6_IS_ADDR_V4MAPPED(&v6);
    if (!strcmp(host, "localhost")) return false;
    for (size_t i = 0; i <= length; ++i) {
        if (i == length || host[i] == '.') {
            if (i == label || i - label > 63 || host[label] == '-' || host[i-1] == '-') return false;
            label = i + 1;
        } else if (!(host[i] >= 'a' && host[i] <= 'z') &&
                   !(host[i] >= '0' && host[i] <= '9') && host[i] != '-') return false;
    }
    /* Reject ambiguous numeric host spellings (inet_aton/octal/short IPv4). */
    for (size_t i = 0; i < length; ++i) if (isalpha((unsigned char)host[i])) return true;
    return false;
}

static bool digest(struct json_object *object, const char *key, char output[65])
{
    struct json_object *value;
    const char *text;
    if (!json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string) ||
        json_object_get_string_len(value) != 64) return false;
    text = json_object_get_string(value);
    for (size_t i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    memcpy(output, text, 65);
    return true;
}

static bool path_host(struct json_object *object, const char *key, char *output)
{
    struct json_object *value;
    if (!json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) return false;
    const char *text = json_object_get_string(value);
    if ((size_t)json_object_get_string_len(value) != strlen(text) || !agent_mesh_host_valid(text)) return false;
    strcpy(output, text);
    return true;
}

static bool path_port(struct json_object *object, const char *key, uint16_t *output)
{
    struct json_object *value;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_int)) return false;
    int64_t port = json_object_get_int64(value);
    if (port < 1 || port > 65535) return false;
    *output = (uint16_t)port;
    return true;
}

bool agent_mesh_profile_parse(const char *text, struct agent_mesh_profile *profile)
{
    struct json_tokener *tokener;
    struct json_object *object = NULL, *version, *paths;
    struct agent_mesh_profile parsed = {0};
    bool ok = false;
    size_t length;
    if (!text || !profile || !(length = strnlen(text, AGENT_MESH_PROFILE_MAX + 1)) || length > AGENT_MESH_PROFILE_MAX) return false;
    tokener = json_tokener_new_ex(8);
    if (!tokener) return false;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    object = json_tokener_parse_ex(tokener, text, (int)length);
    if (json_tokener_get_error(tokener) != json_tokener_success ||
        json_tokener_get_parse_end(tokener) != length ||
        !json_object_is_type(object, json_type_object) || json_object_object_length(object) != 5 ||
        !json_object_object_get_ex(object, "v", &version) || !json_object_is_type(version, json_type_int) ||
        json_object_get_int(version) != 2 ||
        !digest(object, "seed_id", parsed.seed_id) ||
        !digest(object, "directory_sha256", parsed.directory_sha256) ||
        !digest(object, "relay_sha256", parsed.relay_sha256) ||
        !json_object_object_get_ex(object, "paths", &paths) || !json_object_is_type(paths, json_type_array)) goto done;
    parsed.count = json_object_array_length(paths);
    if (!parsed.count || parsed.count > AGENT_MESH_PATHS_MAX) goto done;
    for (size_t i = 0; i < parsed.count; ++i) {
        struct json_object *path = json_object_array_get_idx(paths, i);
        if (!json_object_is_type(path, json_type_object) || json_object_object_length(path) != 4 ||
            !path_host(path, "directory_host", parsed.paths[i].directory_host) ||
            !path_host(path, "relay_host", parsed.paths[i].relay_host) ||
            !path_port(path, "directory_port", &parsed.paths[i].directory_port) ||
            !path_port(path, "relay_port", &parsed.paths[i].relay_port)) goto done;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(&parsed.paths[j], &parsed.paths[i], sizeof(parsed.paths[i]))) goto done;
    }
    *profile = parsed;
    ok = true;
done:
    json_object_put(object);
    json_tokener_free(tokener);
    return ok;
}

bool agent_mesh_resolve(const char *host, uint16_t port, uint64_t attempt,
                        struct sockaddr_storage *output, socklen_t *length)
{
    struct addrinfo hints = {0}, *addresses = NULL, *item;
    struct addrinfo *candidates[8];
    size_t count = 0;
    char service[6], literal[INET6_ADDRSTRLEN];
    if (!agent_mesh_host_valid(host)) return false;
    snprintf(service, sizeof(service), "%u", port);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP; hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, service, &hints, &addresses)) return false;
    for (item = addresses; item && count < 8; item = item->ai_next) {
        const void *ip;
        if (item->ai_family == AF_INET) ip = &((struct sockaddr_in *)item->ai_addr)->sin_addr;
        else if (item->ai_family == AF_INET6) ip = &((struct sockaddr_in6 *)item->ai_addr)->sin6_addr;
        else continue;
        if (item->ai_addrlen > sizeof(*output) ||
            !inet_ntop(item->ai_family, ip, literal, sizeof(literal)) || !agent_mesh_host_valid(literal)) continue;
        candidates[count++] = item;
    }
    if (count) {
        item = candidates[attempt % count];
        memset(output, 0, sizeof(*output));
        memcpy(output, item->ai_addr, item->ai_addrlen); *length = item->ai_addrlen;
    }
    freeaddrinfo(addresses);
    return count != 0;
}

bool agent_mesh_certificate_matches(const mbedtls_x509_crt *certificate, const char *fingerprint)
{
    unsigned char hash[32]; unsigned int different = 0;
    static const char hex[] = "0123456789abcdef";
    if (!certificate || !fingerprint || strlen(fingerprint) != 64 ||
        mbedtls_sha256(certificate->raw.p, certificate->raw.len, hash, 0)) return false;
    for (size_t i = 0; i < 32; ++i) {
        different |= (unsigned char)fingerprint[2*i] ^ hex[hash[i] >> 4];
        different |= (unsigned char)fingerprint[2*i+1] ^ hex[hash[i] & 15];
    }
    return different == 0;
}

#ifdef AGENT_MESH_PROFILE_CLI
int main(void)
{
    char input[AGENT_MESH_PROFILE_MAX + 2]; struct agent_mesh_profile profile;
    size_t length = fread(input, 1, sizeof(input)-1, stdin);
    input[length] = 0;
    if (length > AGENT_MESH_PROFILE_MAX || memchr(input, 0, length) || !agent_mesh_profile_parse(input, &profile)) return 1;
    /* Echo only validated public routing information, never credentials. */
    fputs(input, stdout);
    return 0;
}
#endif
