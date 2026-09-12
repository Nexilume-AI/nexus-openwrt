#include "agent_invoke_contract.h"
#include "agent_ipc_protocol.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool copy_part(
    char *target,
    size_t capacity,
    const char *start,
    size_t length
)
{
    if (target == NULL || start == NULL || length == 0U ||
        length >= capacity) {
        return false;
    }
    memcpy(target, start, length);
    target[length] = '\0';
    return true;
}

static bool parse_port(const char *start, const char *end, uint16_t *port)
{
    uint32_t value = 0U;
    const char *cursor;

    if (start == NULL || end == NULL || port == NULL || start == end) {
        return false;
    }
    for (cursor = start; cursor < end; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        if (value > (65535U - (uint32_t)(*cursor - '0')) / 10U) {
            return false;
        }
        value = value * 10U + (uint32_t)(*cursor - '0');
    }
    if (value == 0U) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool ipv4_is_loopback(const char *host)
{
    uint32_t octets[4] = {0U};
    size_t index = 0U;
    const char *cursor;
    bool digit = false;

    if (host == NULL || host[0] == '\0') {
        return false;
    }
    for (cursor = host; ; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            digit = true;
            octets[index] = octets[index] * 10U +
                (uint32_t)(*cursor - '0');
            if (octets[index] > 255U) {
                return false;
            }
        } else if (*cursor == '.' && digit && index < 3U) {
            index++;
            digit = false;
        } else if (*cursor == '\0' && digit && index == 3U) {
            break;
        } else {
            return false;
        }
    }
    return octets[0] == 127U;
}

static bool dns_identity_is_safe(
    const char *start,
    size_t length
)
{
    size_t index;
    size_t label_length = 0U;
    bool has_dot = false;
    bool has_alpha = false;

    if (start == NULL || length == 0U ||
        length >= AGENT_INVOKE_HOST_LEN) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        char current = start[index];

        if (current >= 'a' && current <= 'z') {
            has_alpha = true;
            label_length++;
        } else if (current >= '0' && current <= '9') {
            label_length++;
        } else if (current == '-') {
            if (label_length == 0U || index + 1U == length ||
                start[index + 1U] == '.') {
                return false;
            }
            label_length++;
        } else if (current == '.') {
            if (label_length == 0U || label_length > 63U) return false;
            has_dot = true;
            label_length = 0U;
        } else {
            return false;
        }
        if (label_length > 63U) return false;
    }
    return has_dot && has_alpha && label_length > 0U &&
           label_length <= 63U;
}

static bool parse_remote_ipv4(
    const char *text,
    char *canonical,
    size_t canonical_capacity
)
{
    uint32_t octets[4] = {0U};
    size_t octet = 0U;
    size_t digits = 0U;
    const char *cursor;
    int written;

    if (text == NULL || canonical == NULL || text[0] == '\0') return false;
    for (cursor = text; ; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            if (digits == 0U && *cursor == '0' &&
                cursor[1] >= '0' && cursor[1] <= '9') return false;
            digits++;
            if (digits > 3U) return false;
            octets[octet] = octets[octet] * 10U +
                (uint32_t)(*cursor - '0');
            if (octets[octet] > 255U) return false;
        } else if (*cursor == '.' && digits > 0U && octet < 3U) {
            octet++;
            digits = 0U;
        } else if (*cursor == '\0' && digits > 0U && octet == 3U) {
            break;
        } else {
            return false;
        }
    }
    if (octets[0] == 0U || octets[0] == 127U || octets[0] >= 224U ||
        (octets[0] == 169U && octets[1] == 254U) ||
        (octets[0] == 255U && octets[1] == 255U &&
         octets[2] == 255U && octets[3] == 255U)) {
        return false;
    }
    written = snprintf(
        canonical, canonical_capacity, "%u.%u.%u.%u",
        (unsigned int)octets[0], (unsigned int)octets[1],
        (unsigned int)octets[2], (unsigned int)octets[3]);
    return written > 0 && (size_t)written < canonical_capacity;
}

static int ipv6_hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_ipv6_hextet(
    const char *start,
    size_t length,
    uint16_t *value
)
{
    size_t index;
    uint16_t result = 0U;

    if (start == NULL || value == NULL || length == 0U || length > 4U)
        return false;
    for (index = 0U; index < length; index++) {
        int digit = ipv6_hex_value(start[index]);
        if (digit < 0) return false;
        result = (uint16_t)((result << 4U) | (uint16_t)digit);
    }
    *value = result;
    return true;
}

static bool parse_ipv6_side(
    const char *start,
    size_t length,
    uint16_t words[8],
    size_t *count
)
{
    size_t position = 0U;

    *count = 0U;
    if (length == 0U) return true;
    while (position < length) {
        size_t end = position;
        if (*count >= 8U) return false;
        while (end < length && start[end] != ':') end++;
        if (!parse_ipv6_hextet(start + position, end - position,
                               &words[*count])) return false;
        (*count)++;
        if (end == length) break;
        position = end + 1U;
        if (position == length) return false;
    }
    return true;
}

static bool parse_remote_ipv6(
    const char *start,
    size_t length,
    char *canonical,
    size_t canonical_capacity
)
{
    const char *compression;
    uint16_t left[8] = {0};
    uint16_t right[8] = {0};
    uint16_t words[8] = {0};
    uint8_t bytes[16] = {0};
    char text[46U];
    size_t left_count = 0U;
    size_t right_count = 0U;
    size_t index;

    if (start == NULL || canonical == NULL || length == 0U ||
        length >= sizeof(text) || length >= canonical_capacity ||
        memchr(start, '.', length) != NULL || memchr(start, '%', length) != NULL)
        return false;
    memcpy(text, start, length);
    text[length] = '\0';
    compression = strstr(text, "::");
    if (compression != NULL && strstr(compression + 2, "::") != NULL)
        return false;
    if (compression == NULL) {
        if (!parse_ipv6_side(text, length, words, &left_count) ||
            left_count != 8U) return false;
    } else {
        size_t left_length = (size_t)(compression - text);
        size_t right_offset = left_length + 2U;
        if (!parse_ipv6_side(text, left_length, left, &left_count) ||
            !parse_ipv6_side(text + right_offset, length - right_offset,
                             right, &right_count) ||
            left_count + right_count >= 8U) return false;
        for (index = 0U; index < left_count; index++) words[index] = left[index];
        for (index = 0U; index < right_count; index++)
            words[8U - right_count + index] = right[index];
    }
    for (index = 0U; index < 8U; index++) {
        bytes[index * 2U] = (uint8_t)(words[index] >> 8U);
        bytes[index * 2U + 1U] = (uint8_t)words[index];
    }
    if ((bytes[0] == 0U && bytes[1] == 0U &&
         memcmp(bytes, (uint8_t[16]){0}, 16U) == 0) ||
        (memcmp(bytes, (uint8_t[15]){0}, 15U) == 0 && bytes[15] == 1U) ||
        bytes[0] == 0xffU ||
        (bytes[0] == 0xfeU && (bytes[1] & 0xc0U) == 0x80U) ||
        (memcmp(bytes, (uint8_t[10]){0}, 10U) == 0 &&
         bytes[10] == 0xffU && bytes[11] == 0xffU)) {
        return false;
    }
    memcpy(canonical, text, length + 1U);
    return true;
}

static bool path_is_safe(const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;

    if (path == NULL || path[0] != '/') {
        return false;
    }
    for (; *cursor != '\0'; cursor++) {
        if (*cursor <= 0x20U || *cursor == 0x7fU ||
            *cursor == '\\' || *cursor == '#') {
            return false;
        }
    }
    return true;
}

bool agent_invoke_parse_loopback_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
)
{
    static const char prefix[] = "http://";
    const char *host_start;
    const char *host_end;
    const char *port_start;
    const char *port_end;
    const char *path;
    size_t path_length;

    if (endpoint == NULL || parsed == NULL ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    parsed->transport = AGENT_INVOKE_HTTP;
    host_start = endpoint + sizeof(prefix) - 1U;
    if (*host_start == '[') {
        host_start++;
        host_end = strchr(host_start, ']');
        if (host_end == NULL || host_end[1] != ':' ||
            (size_t)(host_end - host_start) != 3U ||
            strncmp(host_start, "::1", 3U) != 0) {
            return false;
        }
        parsed->family = AGENT_INVOKE_IPV6;
        port_start = host_end + 2;
    } else {
        host_end = strchr(host_start, ':');
        if (host_end == NULL ||
            !copy_part(parsed->host, sizeof(parsed->host), host_start,
                       (size_t)(host_end - host_start)) ||
            !ipv4_is_loopback(parsed->host)) {
            return false;
        }
        parsed->family = AGENT_INVOKE_IPV4;
        port_start = host_end + 1;
    }
    path = strchr(port_start, '/');
    port_end = path != NULL ? path : endpoint + strlen(endpoint);
    if (!parse_port(port_start, port_end, &parsed->port)) {
        return false;
    }
    if (parsed->family == AGENT_INVOKE_IPV6 &&
        !copy_part(parsed->host, sizeof(parsed->host), host_start,
                   (size_t)(host_end - host_start))) {
        return false;
    }
    if (path == NULL) {
        return copy_part(parsed->path, sizeof(parsed->path), "/", 1U);
    }
    path_length = strlen(path);
    return path_length < sizeof(parsed->path) &&
           copy_part(parsed->path, sizeof(parsed->path), path, path_length) &&
           path_is_safe(parsed->path);
}

static bool ipv4_is_private_lan(const char *host)
{
    char canonical[16U];
    unsigned int first;
    unsigned int second;
    unsigned int third;
    unsigned int fourth;

    if (!parse_remote_ipv4(host, canonical, sizeof(canonical)) ||
        strcmp(host, canonical) != 0 ||
        sscanf(canonical, "%u.%u.%u.%u",
               &first, &second, &third, &fourth) != 4) {
        return false;
    }
    (void)third;
    (void)fourth;
    return first == 10U ||
           (first == 172U && second >= 16U && second <= 31U) ||
           (first == 192U && second == 168U);
}

static bool ipv6_is_ula(
    const char *start,
    size_t length,
    char *canonical,
    size_t canonical_capacity
)
{
    char first;
    char second;

    if (!parse_remote_ipv6(
            start, length, canonical, canonical_capacity) || length < 2U) {
        return false;
    }
    first = start[0] >= 'A' && start[0] <= 'F'
        ? (char)(start[0] - 'A' + 'a') : start[0];
    second = start[1] >= 'A' && start[1] <= 'F'
        ? (char)(start[1] - 'A' + 'a') : start[1];
    return first == 'f' && (second == 'c' || second == 'd');
}

bool agent_invoke_parse_lan_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
)
{
    static const char prefix[] = "http://";
    const char *host_start;
    const char *host_end;
    const char *port_start;
    const char *port_end;
    const char *path;
    size_t host_length;
    size_t path_length;

    if (endpoint == NULL || parsed == NULL ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    memset(parsed, 0, sizeof(*parsed));
    parsed->transport = AGENT_INVOKE_HTTP;
    host_start = endpoint + sizeof(prefix) - 1U;
    if (*host_start == '[') {
        host_start++;
        host_end = strchr(host_start, ']');
        if (host_end == NULL || host_end[1] != ':') return false;
        host_length = (size_t)(host_end - host_start);
        if (!ipv6_is_ula(host_start, host_length, parsed->host,
                         sizeof(parsed->host))) return false;
        parsed->family = AGENT_INVOKE_IPV6;
        port_start = host_end + 2;
    } else {
        host_end = strchr(host_start, ':');
        if (host_end == NULL) return false;
        host_length = (size_t)(host_end - host_start);
        if (!copy_part(parsed->host, sizeof(parsed->host),
                       host_start, host_length) ||
            !ipv4_is_private_lan(parsed->host)) return false;
        parsed->family = AGENT_INVOKE_IPV4;
        port_start = host_end + 1;
    }
    path = strchr(port_start, '/');
    port_end = path != NULL ? path : endpoint + strlen(endpoint);
    if (!parse_port(port_start, port_end, &parsed->port)) return false;
    if (path == NULL) {
        return copy_part(parsed->path, sizeof(parsed->path), "/", 1U);
    }
    path_length = strlen(path);
    return path_length < sizeof(parsed->path) &&
           copy_part(parsed->path, sizeof(parsed->path), path, path_length) &&
           path_is_safe(parsed->path);
}

bool agent_invoke_parse_remote_tls_endpoint(
    const char *endpoint,
    struct agent_invoke_endpoint *parsed
)
{
    static const char prefix[] = "https://";
    const char *host_start;
    const char *host_end;
    const char *port_start;
    const char *port_end;
    const char *path;
    size_t host_length;
    size_t path_length;

    if (endpoint == NULL || parsed == NULL ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0) return false;
    memset(parsed, 0, sizeof(*parsed));
    parsed->transport = AGENT_INVOKE_HTTPS;
    parsed->family = AGENT_INVOKE_DNS;
    host_start = endpoint + sizeof(prefix) - 1U;
    host_end = strchr(host_start, ':');
    if (host_end == NULL || strchr(host_end + 1, ':') != NULL) return false;
    host_length = (size_t)(host_end - host_start);
    if (!dns_identity_is_safe(host_start, host_length) ||
        !copy_part(parsed->host, sizeof(parsed->host),
                   host_start, host_length)) return false;
    port_start = host_end + 1;
    path = strchr(port_start, '/');
    port_end = path != NULL ? path : endpoint + strlen(endpoint);
    if (!parse_port(port_start, port_end, &parsed->port)) return false;
    if (path == NULL) {
        return copy_part(parsed->path, sizeof(parsed->path), "/", 1U);
    }
    path_length = strlen(path);
    return path_length < sizeof(parsed->path) &&
           copy_part(parsed->path, sizeof(parsed->path), path, path_length) &&
           path_is_safe(parsed->path);
}

bool agent_invoke_parse_remote_map(
    const char *mapping,
    struct agent_invoke_remote_map *parsed
)
{
    const char *equals;
    const char *colon;
    size_t identity_length;

    if (mapping == NULL || parsed == NULL ||
        strlen(mapping) >= AGENT_INVOKE_REMOTE_MAP_LEN) return false;
    equals = strchr(mapping, '=');
    if (equals == NULL || equals[1] == '\0' ||
        strchr(equals + 1, '=') != NULL) return false;
    colon = memchr(mapping, ':', (size_t)(equals - mapping));
    if (colon == NULL ||
        memchr(colon + 1, ':', (size_t)(equals - colon - 1)) != NULL) {
        return false;
    }
    identity_length = (size_t)(colon - mapping);
    memset(parsed, 0, sizeof(*parsed));
    if (!dns_identity_is_safe(mapping, identity_length) ||
        !copy_part(parsed->identity, sizeof(parsed->identity),
                   mapping, identity_length) ||
        !parse_port(colon + 1, equals, &parsed->port)) {
        memset(parsed, 0, sizeof(*parsed));
        return false;
    }
    if (equals[1] == '[') {
        const char *close = strchr(equals + 2, ']');
        if (close == NULL || close[1] != '\0' ||
            !parse_remote_ipv6(
                equals + 2, (size_t)(close - equals - 2),
                parsed->address, sizeof(parsed->address))) {
            memset(parsed, 0, sizeof(*parsed));
            return false;
        }
        parsed->family = AGENT_INVOKE_IPV6;
    } else {
        if (!parse_remote_ipv4(equals + 1, parsed->address,
                               sizeof(parsed->address))) {
            memset(parsed, 0, sizeof(*parsed));
            return false;
        }
        parsed->family = AGENT_INVOKE_IPV4;
    }
    return true;
}

bool agent_invoke_remote_map_matches(
    const struct agent_invoke_endpoint *endpoint,
    const struct agent_invoke_remote_map *mapping
)
{
    return endpoint != NULL && mapping != NULL &&
           endpoint->transport == AGENT_INVOKE_HTTPS &&
           endpoint->family == AGENT_INVOKE_DNS &&
           endpoint->port == mapping->port &&
           strcmp(endpoint->host, mapping->identity) == 0;
}

static bool endpoint_matches(
    const struct agent_invoke_endpoint *left,
    const struct agent_invoke_endpoint *right
)
{
    return left != NULL && right != NULL &&
           left->transport == right->transport &&
           left->family == right->family &&
           left->port == right->port &&
           strcmp(left->host, right->host) == 0 &&
           strcmp(left->path, right->path) == 0;
}

static bool bounded_dynamic_text(const char *value, size_t capacity)
{
    return value != NULL && value[0] != '\0' &&
           strnlen(value, capacity) < capacity;
}

static bool ca_bundle_id_is_safe(const char *value)
{
    size_t index;
    size_t length;

    if (!bounded_dynamic_text(value, AGENT_INVOKE_CA_BUNDLE_ID_LEN)) {
        return false;
    }
    length = strlen(value);
    for (index = 0U; index < length; index++) {
        const char current = value[index];
        if (!((current >= 'a' && current <= 'z') ||
              (current >= 'A' && current <= 'Z') ||
              (current >= '0' && current <= '9') ||
              current == '.' || current == '_' || current == '-')) {
            return false;
        }
    }
    return true;
}

static bool certificate_sha256_is_safe(const char *value)
{
    size_t index;

    if (value == NULL || value[0] == '\0') return true;
    if (strlen(value) != AGENT_INVOKE_CERT_SHA256_LEN - 1U) return false;
    for (index = 0U; index < AGENT_INVOKE_CERT_SHA256_LEN - 1U; index++) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

void agent_invoke_dynamic_map_table_init(
    struct agent_invoke_dynamic_map_table *table
)
{
    if (table != NULL) memset(table, 0, sizeof(*table));
}

size_t agent_invoke_dynamic_map_prune(
    struct agent_invoke_dynamic_map_table *table,
    uint64_t now_ms
)
{
    size_t index;
    size_t pruned = 0U;

    if (table == NULL) return 0U;
    for (index = 0U; index < AGENT_INVOKE_DYNAMIC_MAP_CAPACITY; index++) {
        struct agent_invoke_dynamic_map *entry = &table->entries[index];
        if (!entry->active || entry->expires_at_ms > now_ms) continue;
        memset(entry, 0, sizeof(*entry));
        table->count--;
        table->expired++;
        pruned++;
    }
    return pruned;
}

bool agent_invoke_dynamic_map_upsert(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    const struct agent_invoke_remote_map *mapping,
    const char *ca_bundle_id,
    const char *certificate_sha256,
    uint64_t expires_at_ms,
    uint64_t now_ms
)
{
    struct agent_invoke_dynamic_map *target = NULL;
    size_t index;
    bool replacing = false;

    if (table == NULL ||
        !bounded_dynamic_text(route_id, AGENT_INVOKE_ROUTE_ID_LEN) ||
        endpoint == NULL || mapping == NULL ||
        !agent_invoke_remote_map_matches(endpoint, mapping) ||
        !ca_bundle_id_is_safe(ca_bundle_id) ||
        !certificate_sha256_is_safe(certificate_sha256) ||
        expires_at_ms <= now_ms) {
        if (table != NULL) table->rejected++;
        return false;
    }
    (void)agent_invoke_dynamic_map_prune(table, now_ms);
    for (index = 0U; index < AGENT_INVOKE_DYNAMIC_MAP_CAPACITY; index++) {
        struct agent_invoke_dynamic_map *entry = &table->entries[index];
        if (entry->active && strcmp(entry->route_id, route_id) == 0) {
            target = entry;
            replacing = true;
            break;
        }
        if (!entry->active && target == NULL) target = entry;
    }
    if (target == NULL) {
        table->rejected++;
        return false;
    }
    memset(target, 0, sizeof(*target));
    target->active = true;
    (void)snprintf(target->route_id, sizeof(target->route_id), "%s", route_id);
    target->endpoint = *endpoint;
    target->mapping = *mapping;
    (void)snprintf(target->ca_bundle_id, sizeof(target->ca_bundle_id), "%s",
                   ca_bundle_id);
    if (certificate_sha256 != NULL) {
        (void)snprintf(target->certificate_sha256,
                       sizeof(target->certificate_sha256), "%s",
                       certificate_sha256);
    }
    target->expires_at_ms = expires_at_ms;
    if (replacing) {
        table->renewed++;
    } else {
        table->count++;
        table->accepted++;
    }
    return true;
}

bool agent_invoke_dynamic_map_renew(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    uint64_t expires_at_ms,
    uint64_t now_ms
)
{
    size_t index;

    if (table == NULL ||
        !bounded_dynamic_text(route_id, AGENT_INVOKE_ROUTE_ID_LEN) ||
        expires_at_ms <= now_ms) return false;
    (void)agent_invoke_dynamic_map_prune(table, now_ms);
    for (index = 0U; index < AGENT_INVOKE_DYNAMIC_MAP_CAPACITY; index++) {
        struct agent_invoke_dynamic_map *entry = &table->entries[index];
        if (entry->active && strcmp(entry->route_id, route_id) == 0) {
            entry->expires_at_ms = expires_at_ms;
            table->renewed++;
            return true;
        }
    }
    return false;
}

bool agent_invoke_dynamic_map_remove(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id
)
{
    size_t index;

    if (table == NULL || route_id == NULL) return false;
    for (index = 0U; index < AGENT_INVOKE_DYNAMIC_MAP_CAPACITY; index++) {
        struct agent_invoke_dynamic_map *entry = &table->entries[index];
        if (entry->active && strcmp(entry->route_id, route_id) == 0) {
            memset(entry, 0, sizeof(*entry));
            table->count--;
            table->removed++;
            return true;
        }
    }
    return false;
}

const struct agent_invoke_dynamic_map *agent_invoke_dynamic_map_find(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    uint64_t now_ms
)
{
    size_t index;

    if (table == NULL || route_id == NULL || endpoint == NULL) return NULL;
    (void)agent_invoke_dynamic_map_prune(table, now_ms);
    for (index = 0U; index < AGENT_INVOKE_DYNAMIC_MAP_CAPACITY; index++) {
        const struct agent_invoke_dynamic_map *entry = &table->entries[index];
        if (agent_invoke_dynamic_map_matches(
                entry, route_id, endpoint, now_ms)) {
            return entry;
        }
    }
    return NULL;
}

bool agent_invoke_dynamic_map_matches(
    const struct agent_invoke_dynamic_map *entry,
    const char *route_id,
    const struct agent_invoke_endpoint *endpoint,
    uint64_t now_ms
)
{
    return entry != NULL && route_id != NULL && endpoint != NULL &&
           entry->active && entry->expires_at_ms > now_ms &&
           strcmp(entry->route_id, route_id) == 0 &&
           endpoint_matches(&entry->endpoint, endpoint) &&
           agent_invoke_remote_map_matches(endpoint, &entry->mapping);
}

bool agent_invoke_decrement_hop_limit(
    uint32_t incoming,
    uint8_t *forwarded
)
{
    if (forwarded == NULL || incoming < 2U || incoming > 255U) {
        return false;
    }
    *forwarded = (uint8_t)(incoming - 1U);
    return true;
}

bool agent_invoke_retry_declaration_valid(
    bool idempotent,
    bool allow_retry,
    const char *idempotency_key
)
{
    if (!allow_retry) {
        return true;
    }
    return idempotent && idempotency_key != NULL &&
           idempotency_key[0] != '\0' && strlen(idempotency_key) <= 256U;
}

static bool parse_digits(const char *text, size_t offset, size_t width, int *out)
{
    size_t index;
    int value = 0;

    for (index = 0U; index < width; index++) {
        char current = text[offset + index];
        if (current < '0' || current > '9') {
            return false;
        }
        value = value * 10 + (current - '0');
    }
    *out = value;
    return true;
}

static bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int64_t days_before_year(int year)
{
    int64_t previous = (int64_t)year - 1;
    return previous * 365 + previous / 4 - previous / 100 + previous / 400;
}

static bool parse_utc_deadline_ms(const char *text, uint64_t *epoch_ms)
{
    static const int month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int max_day;
    int index;
    int64_t days;
    uint64_t seconds;

    if (text == NULL || epoch_ms == NULL || strlen(text) != 20U ||
        text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z' ||
        !parse_digits(text, 0U, 4U, &year) ||
        !parse_digits(text, 5U, 2U, &month) ||
        !parse_digits(text, 8U, 2U, &day) ||
        !parse_digits(text, 11U, 2U, &hour) ||
        !parse_digits(text, 14U, 2U, &minute) ||
        !parse_digits(text, 17U, 2U, &second) ||
        year < 1970 || month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    max_day = month_days[month - 1];
    if (month == 2 && leap_year(year)) {
        max_day++;
    }
    if (day < 1 || day > max_day) {
        return false;
    }
    days = days_before_year(year) - days_before_year(1970);
    for (index = 1; index < month; index++) {
        days += month_days[index - 1];
        if (index == 2 && leap_year(year)) {
            days++;
        }
    }
    days += day - 1;
    seconds = (uint64_t)days * 86400U + (uint64_t)hour * 3600U +
        (uint64_t)minute * 60U + (uint64_t)second;
    if (seconds > UINT64_MAX / 1000U) {
        return false;
    }
    *epoch_ms = seconds * 1000U;
    return true;
}

enum agent_invoke_deadline_result agent_invoke_resolve_timeout_ms(
    uint64_t now_epoch_ms,
    uint32_t configured_timeout_ms,
    const char *deadline,
    uint32_t *effective_timeout_ms
)
{
    uint64_t deadline_ms;
    uint64_t remaining;

    if (configured_timeout_ms == 0U || effective_timeout_ms == NULL) {
        return AGENT_INVOKE_DEADLINE_INVALID;
    }
    if (deadline == NULL || deadline[0] == '\0') {
        *effective_timeout_ms = configured_timeout_ms;
        return AGENT_INVOKE_DEADLINE_OK;
    }
    if (!parse_utc_deadline_ms(deadline, &deadline_ms)) {
        return AGENT_INVOKE_DEADLINE_INVALID;
    }
    if (deadline_ms <= now_epoch_ms) {
        return AGENT_INVOKE_DEADLINE_EXPIRED;
    }
    remaining = deadline_ms - now_epoch_ms;
    *effective_timeout_ms = remaining < configured_timeout_ms
        ? (uint32_t)remaining
        : configured_timeout_ms;
    return *effective_timeout_ms > 0U ? AGENT_INVOKE_DEADLINE_OK :
                                       AGENT_INVOKE_DEADLINE_EXPIRED;
}

bool agent_invoke_effective_timeout_ms(
    uint64_t now_epoch_ms,
    uint32_t configured_timeout_ms,
    const char *deadline,
    uint32_t *effective_timeout_ms
)
{
    return agent_invoke_resolve_timeout_ms(
        now_epoch_ms, configured_timeout_ms, deadline,
        effective_timeout_ms) == AGENT_INVOKE_DEADLINE_OK;
}

static bool route_id_is_safe(const char *route_id)
{
    size_t index;

    if (route_id == NULL || strlen(route_id) != 32U) {
        return false;
    }
    for (index = 0U; index < 32U; index++) {
        if (!((route_id[index] >= '0' && route_id[index] <= '9') ||
              (route_id[index] >= 'a' && route_id[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool build_http_request(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    bool streaming,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    int header_length;

    if (endpoint == NULL || body == NULL || body_length == 0U ||
        output == NULL || output_length == NULL ||
        !route_id_is_safe(route_id) || !path_is_safe(endpoint->path)) {
        return false;
    }
    if (endpoint->family == AGENT_INVOKE_IPV6) {
        header_length = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: [%s]:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json\r\n"
            "Accept: %s\r\n"
            "Content-Length: %" PRIu64 "\r\nConnection: close\r\n"
            "X-Nexus-Route-Id: %s\r\n\r\n",
            endpoint->path, endpoint->host, (unsigned int)endpoint->port,
            streaming ? "text/event-stream" : "application/json",
            (uint64_t)body_length, route_id);
    } else {
        header_length = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json\r\n"
            "Accept: %s\r\n"
            "Content-Length: %" PRIu64 "\r\nConnection: close\r\n"
            "X-Nexus-Route-Id: %s\r\n\r\n",
            endpoint->path, endpoint->host, (unsigned int)endpoint->port,
            streaming ? "text/event-stream" : "application/json",
            (uint64_t)body_length, route_id);
    }
    if (header_length < 0 || (size_t)header_length >= output_capacity ||
        body_length >= output_capacity - (size_t)header_length) {
        return false;
    }
    memcpy(output + (size_t)header_length, body, body_length);
    *output_length = (size_t)header_length + body_length;
    output[*output_length] = '\0';
    return true;
}

bool agent_invoke_build_http_request(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    return build_http_request(
        endpoint, route_id, false, body, body_length,
        output, output_capacity, output_length);
}

bool agent_invoke_build_stream_http_request(
    const struct agent_invoke_endpoint *endpoint,
    const char *route_id,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    return build_http_request(
        endpoint, route_id, true, body, body_length,
        output, output_capacity, output_length);
}

static bool internal_header_value_is_safe(
    const char *value,
    bool allow_empty
)
{
    size_t index;
    size_t length;

    if (value == NULL) return false;
    length = strlen(value);
    if ((!allow_empty && length == 0U) || length >= AGENT_INVOKE_PATH_LEN) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)value[index];

        if (character < 0x21U || character > 0x7eU) return false;
    }
    return true;
}

bool agent_invoke_internal_token_is_valid(
    const char *token,
    size_t token_length
)
{
    size_t index;

    if (token == NULL || token_length != AGENT_INVOKE_INTERNAL_TOKEN_LEN) {
        return false;
    }
    for (index = 0U; index < token_length; index++) {
        if (!((token[index] >= '0' && token[index] <= '9') ||
              (token[index] >= 'a' && token[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool agent_invoke_internal_token_matches(
    const char expected[AGENT_INVOKE_INTERNAL_TOKEN_LEN + 1U],
    const char *presented,
    size_t presented_length
)
{
    volatile unsigned int difference =
        (unsigned int)(presented_length ^ AGENT_INVOKE_INTERNAL_TOKEN_LEN);
    size_t index;

    if (expected == NULL) return false;
    for (index = 0U; index < AGENT_INVOKE_INTERNAL_TOKEN_LEN; index++) {
        unsigned char actual = 0U;

        if (presented != NULL && index < presented_length) {
            actual = (unsigned char)presented[index];
        }
        difference |= (unsigned int)(
            (unsigned char)expected[index] ^ actual);
    }
    return difference == 0U;
}

bool agent_invoke_internal_generation_is_valid(const char *generation)
{
    size_t index;

    if (generation == NULL ||
        strlen(generation) != AGENT_INVOKE_INTERNAL_GENERATION_LEN) {
        return false;
    }
    for (index = 0U; index < AGENT_INVOKE_INTERNAL_GENERATION_LEN; index++) {
        if (!((generation[index] >= '0' && generation[index] <= '9') ||
              (generation[index] >= 'a' && generation[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

size_t agent_invoke_backend_response_limit(
    bool internal_invoke,
    size_t configured_limit
)
{
    if (internal_invoke && configured_limit > AGENT_IPC_MAX_INVOKE_BODY) {
        return AGENT_IPC_MAX_INVOKE_BODY;
    }
    return configured_limit;
}

enum agent_invoke_internal_selection_result
agent_invoke_internal_selection_validate(
    struct agent_invoke_dynamic_map_table *table,
    const char *route_id,
    const char *selected_endpoint,
    const char *selected_origin,
    const char *target_agent,
    const char *envelope_target_agent,
    uint64_t now_ms,
    struct agent_invoke_endpoint *parsed_endpoint,
    struct agent_invoke_dynamic_map *lease_snapshot
)
{
    struct agent_invoke_endpoint endpoint;
    const struct agent_invoke_dynamic_map *lease;

    if (table == NULL || parsed_endpoint == NULL || lease_snapshot == NULL ||
        route_id == NULL ||
        selected_endpoint == NULL || selected_origin == NULL ||
        target_agent == NULL || envelope_target_agent == NULL ||
        selected_origin[0] == '\0' || target_agent[0] == '\0' ||
        envelope_target_agent[0] == '\0' ||
        !agent_invoke_parse_remote_tls_endpoint(
            selected_endpoint, &endpoint)) {
        return AGENT_INVOKE_INTERNAL_SELECTION_INVALID;
    }
    if (strcmp(selected_origin, target_agent) != 0 ||
        strcmp(target_agent, envelope_target_agent) != 0) {
        return AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH;
    }
    lease = agent_invoke_dynamic_map_find(
        table, route_id, &endpoint, now_ms);
    if (lease == NULL) {
        return AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED;
    }
    *parsed_endpoint = endpoint;
    *lease_snapshot = *lease;
    return AGENT_INVOKE_INTERNAL_SELECTION_OK;
}

bool agent_invoke_build_internal_http_request(
    const struct agent_invoke_endpoint *gateway,
    const char *route_id,
    const char *selected_endpoint,
    const char *selected_origin,
    const char *target_agent,
    const char *token,
    uint32_t timeout_ms,
    const char *body,
    size_t body_length,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    struct agent_invoke_endpoint selected;
    char target_header[AGENT_INVOKE_PATH_LEN + 64U];
    int header_length;

    if (gateway == NULL || body == NULL || body_length == 0U ||
        output == NULL || output_length == NULL ||
        gateway->transport != AGENT_INVOKE_HTTP ||
        (gateway->family == AGENT_INVOKE_IPV4 &&
         strcmp(gateway->host, "127.0.0.1") != 0) ||
        (gateway->family == AGENT_INVOKE_IPV6 &&
         strcmp(gateway->host, "::1") != 0) ||
        (gateway->family != AGENT_INVOKE_IPV4 &&
         gateway->family != AGENT_INVOKE_IPV6) ||
        strcmp(gateway->path, AGENT_INVOKE_INTERNAL_PATH) != 0 ||
        !route_id_is_safe(route_id) ||
        !agent_invoke_parse_remote_tls_endpoint(
            selected_endpoint, &selected) ||
        !internal_header_value_is_safe(selected_endpoint, false) ||
        !internal_header_value_is_safe(selected_origin, false) ||
        !internal_header_value_is_safe(target_agent, false) ||
        !agent_invoke_internal_token_is_valid(
            token, token == NULL ? 0U : strlen(token)) ||
        timeout_ms < 10U || timeout_ms > 30000U) {
        return false;
    }
    {
        int target_length = snprintf(
            target_header, sizeof(target_header), "%s: %s\r\n",
            AGENT_INVOKE_INTERNAL_TARGET_HEADER, target_agent);

        if (target_length < 0 ||
            (size_t)target_length >= sizeof(target_header)) return false;
    }
    if (gateway->family == AGENT_INVOKE_IPV6) {
        header_length = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: [%s]:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json\r\n"
            "Accept: application/json\r\n"
            "Content-Length: %" PRIu64 "\r\nConnection: close\r\n"
            "X-Nexus-Route-Id: %s\r\n%s: %s\r\n%s: %s\r\n%s: %s\r\n"
            "%s: %u\r\n%s\r\n",
            gateway->path, gateway->host, (unsigned int)gateway->port,
            (uint64_t)body_length, route_id,
            AGENT_INVOKE_INTERNAL_ENDPOINT_HEADER, selected_endpoint,
            AGENT_INVOKE_INTERNAL_ORIGIN_HEADER, selected_origin,
            AGENT_INVOKE_INTERNAL_TOKEN_HEADER, token,
            AGENT_INVOKE_INTERNAL_TIMEOUT_HEADER, timeout_ms,
            target_header);
    } else {
        header_length = snprintf(
            output, output_capacity,
            "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
            "Content-Type: application/vnd.nexus.agent-envelope+json\r\n"
            "Accept: application/json\r\n"
            "Content-Length: %" PRIu64 "\r\nConnection: close\r\n"
            "X-Nexus-Route-Id: %s\r\n%s: %s\r\n%s: %s\r\n%s: %s\r\n"
            "%s: %u\r\n%s\r\n",
            gateway->path, gateway->host, (unsigned int)gateway->port,
            (uint64_t)body_length, route_id,
            AGENT_INVOKE_INTERNAL_ENDPOINT_HEADER, selected_endpoint,
            AGENT_INVOKE_INTERNAL_ORIGIN_HEADER, selected_origin,
            AGENT_INVOKE_INTERNAL_TOKEN_HEADER, token,
            AGENT_INVOKE_INTERNAL_TIMEOUT_HEADER, timeout_ms,
            target_header);
    }
    if (header_length < 0 || (size_t)header_length >= output_capacity ||
        body_length >= output_capacity - (size_t)header_length) {
        return false;
    }
    memcpy(output + (size_t)header_length, body, body_length);
    *output_length = (size_t)header_length + body_length;
    output[*output_length] = '\0';
    return true;
}

static bool ascii_equal_case(
    const char *left,
    size_t left_length,
    const char *right
)
{
    size_t index;

    if (strlen(right) != left_length) {
        return false;
    }
    for (index = 0U; index < left_length; index++) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static const char *find_header_end(const char *buffer, size_t length)
{
    size_t index;

    if (length < 4U) {
        return NULL;
    }
    for (index = 0U; index + 3U < length; index++) {
        if (memcmp(buffer + index, "\r\n\r\n", 4U) == 0) {
            return buffer + index + 4U;
        }
    }
    return NULL;
}

static bool parse_http_status(
    const char *line,
    size_t line_length,
    int *status
)
{
    int parsed;

    if (line == NULL || status == NULL || line_length < 12U ||
        (memcmp(line, "HTTP/1.1 ", 9U) != 0 &&
         memcmp(line, "HTTP/1.0 ", 9U) != 0) ||
        line[9] < '0' || line[9] > '9' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (line_length > 12U && line[12] != ' ')) {
        return false;
    }
    parsed = (line[9] - '0') * 100 +
        (line[10] - '0') * 10 + (line[11] - '0');
    if (parsed < 200 || parsed > 599) {
        return false;
    }
    *status = parsed;
    return true;
}

static bool parse_decimal_size(
    const char *start,
    const char *end,
    size_t *value
)
{
    size_t parsed = 0U;
    const char *cursor;

    if (start == end) {
        return false;
    }
    for (cursor = start; cursor < end; cursor++) {
        size_t digit;
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        digit = (size_t)(*cursor - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

enum agent_invoke_http_result agent_invoke_parse_http_response(
    const char *buffer,
    size_t buffer_length,
    size_t max_body_length,
    struct agent_invoke_http_response *response
)
{
    const char *header_end;
    const char *line;
    const char *line_end;
    const char *colon;
    const char *value;
    const char *value_end;
    size_t content_length = 0U;
    bool have_content_length = false;
    int status;

    if (buffer == NULL || response == NULL || max_body_length == 0U ||
        max_body_length > SIZE_MAX - AGENT_INVOKE_MAX_HEADER_BYTES) {
        return AGENT_INVOKE_HTTP_INVALID;
    }
    if (buffer_length > max_body_length + AGENT_INVOKE_MAX_HEADER_BYTES) {
        return AGENT_INVOKE_HTTP_TOO_LARGE;
    }
    header_end = find_header_end(buffer, buffer_length);
    if (header_end == NULL) {
        return buffer_length >= AGENT_INVOKE_MAX_HEADER_BYTES
            ? AGENT_INVOKE_HTTP_INVALID : AGENT_INVOKE_HTTP_INCOMPLETE;
    }
    if ((size_t)(header_end - buffer) > AGENT_INVOKE_MAX_HEADER_BYTES) {
        return AGENT_INVOKE_HTTP_INVALID;
    }
    memset(response, 0, sizeof(*response));
    line_end = strstr(buffer, "\r\n");
    if (line_end == NULL || line_end >= header_end ||
        !parse_http_status(buffer, (size_t)(line_end - buffer), &status)) {
        return AGENT_INVOKE_HTTP_INVALID;
    }
    if (status >= 300 && status <= 399) {
        return AGENT_INVOKE_HTTP_REDIRECT;
    }
    response->status = status;
    response->content_type[0] = '\0';
    line = line_end + 2U;
    while (line < header_end - 2U) {
        line_end = strstr(line, "\r\n");
        if (line_end == NULL || line_end >= header_end) {
            return AGENT_INVOKE_HTTP_INVALID;
        }
        colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon == NULL) {
            return AGENT_INVOKE_HTTP_INVALID;
        }
        value = colon + 1U;
        while (value < line_end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        value_end = line_end;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            value_end--;
        }
        if (ascii_equal_case(line, (size_t)(colon - line),
                             "Content-Length")) {
            size_t parsed_length;
            if (!parse_decimal_size(value, value_end, &parsed_length) ||
                (have_content_length && parsed_length != content_length)) {
                return AGENT_INVOKE_HTTP_INVALID;
            }
            content_length = parsed_length;
            have_content_length = true;
        } else if (ascii_equal_case(
                       line, (size_t)(colon - line), "Transfer-Encoding")) {
            return AGENT_INVOKE_HTTP_INVALID;
        } else if (ascii_equal_case(
                       line, (size_t)(colon - line), "Content-Type")) {
            size_t length = (size_t)(value_end - value);
            const unsigned char *cursor;
            if (length == 0U || length >= sizeof(response->content_type)) {
                return AGENT_INVOKE_HTTP_INVALID;
            }
            for (cursor = (const unsigned char *)value;
                 cursor < (const unsigned char *)value_end; cursor++) {
                if (*cursor < 0x20U || *cursor == 0x7fU) {
                    return AGENT_INVOKE_HTTP_INVALID;
                }
            }
            memcpy(response->content_type, value, length);
            response->content_type[length] = '\0';
        }
        line = line_end + 2U;
    }
    if (!have_content_length) {
        return AGENT_INVOKE_HTTP_INVALID;
    }
    if (content_length > max_body_length) {
        return AGENT_INVOKE_HTTP_TOO_LARGE;
    }
    response->header_length = (size_t)(header_end - buffer);
    response->body_length = content_length;
    if (content_length > SIZE_MAX - response->header_length) {
        return AGENT_INVOKE_HTTP_TOO_LARGE;
    }
    if (buffer_length < response->header_length + content_length) {
        return AGENT_INVOKE_HTTP_INCOMPLETE;
    }
    if (buffer_length != response->header_length + content_length) {
        return AGENT_INVOKE_HTTP_INVALID;
    }
    if (response->content_type[0] == '\0') {
        (void)snprintf(response->content_type,
                       sizeof(response->content_type),
                       "%s", "application/octet-stream");
    }
    return AGENT_INVOKE_HTTP_OK;
}

void agent_invoke_backend_machine_init(
    struct agent_invoke_backend_machine *machine
)
{
    if (machine != NULL) {
        machine->phase = AGENT_BACKEND_CONNECTING;
        machine->outcome = AGENT_BACKEND_ACTIVE;
    }
}

bool agent_invoke_backend_machine_transition(
    struct agent_invoke_backend_machine *machine,
    enum agent_invoke_backend_event event
)
{
    if (machine == NULL || machine->outcome != AGENT_BACKEND_ACTIVE) {
        return false;
    }
    if (event == AGENT_BACKEND_DEADLINE_EXPIRED ||
        event == AGENT_BACKEND_CLIENT_DISCONNECTED ||
        event == AGENT_BACKEND_IO_FAILED) {
        machine->phase = AGENT_BACKEND_FINISHED;
        machine->outcome = event == AGENT_BACKEND_DEADLINE_EXPIRED
            ? AGENT_BACKEND_TIMED_OUT
            : (event == AGENT_BACKEND_CLIENT_DISCONNECTED
                ? AGENT_BACKEND_CANCELLED : AGENT_BACKEND_FAILED);
        return true;
    }
    if (machine->phase == AGENT_BACKEND_CONNECTING &&
        event == AGENT_BACKEND_CONNECTED) {
        machine->phase = AGENT_BACKEND_SENDING;
        return true;
    }
    if (machine->phase == AGENT_BACKEND_SENDING &&
        event == AGENT_BACKEND_REQUEST_SENT) {
        machine->phase = AGENT_BACKEND_RECEIVING;
        return true;
    }
    if (machine->phase == AGENT_BACKEND_RECEIVING &&
        event == AGENT_BACKEND_RESPONSE_RECEIVED) {
        machine->phase = AGENT_BACKEND_FINISHED;
        machine->outcome = AGENT_BACKEND_SUCCEEDED;
        return true;
    }
    return false;
}
