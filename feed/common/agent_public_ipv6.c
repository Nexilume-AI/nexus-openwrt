#include "agent_public_ipv6.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool copy_text(char *target, size_t capacity, const char *source)
{
    size_t length;

    if (target == NULL || source == NULL || capacity == 0U) return false;
    length = strlen(source);
    if (length >= capacity) return false;
    memcpy(target, source, length + 1U);
    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static bool parse_hextet(const char *start, size_t length, uint16_t *value)
{
    size_t index;
    uint16_t result = 0U;

    if (length == 0U || length > 4U || value == NULL) return false;
    for (index = 0U; index < length; index++) {
        int digit = hex_value(start[index]);
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
        if (!parse_hextet(start + position, end - position,
                           &words[*count])) return false;
        (*count)++;
        if (end == length) break;
        position = end + 1U;
        if (position == length) return false;
    }
    return true;
}

static bool parse_ipv6(const char *text, uint8_t address[16])
{
    const char *compression;
    uint16_t left[8] = {0};
    uint16_t right[8] = {0};
    uint16_t words[8] = {0};
    size_t left_count = 0U;
    size_t right_count = 0U;
    size_t index;
    size_t length;

    if (text == NULL || address == NULL || strchr(text, '.') != NULL)
        return false;
    length = strlen(text);
    if (length == 0U || length >= AGENT_PUBLIC_IPV6_TEXT_LEN) return false;
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
        address[index * 2U] = (uint8_t)(words[index] >> 8U);
        address[index * 2U + 1U] = (uint8_t)words[index];
    }
    return true;
}

static bool format_ipv6(
    const uint8_t address[16],
    char output[AGENT_PUBLIC_IPV6_TEXT_LEN]
)
{
    uint16_t words[8];
    size_t best_start = 8U;
    size_t best_length = 0U;
    size_t index;
    size_t used = 0U;

    for (index = 0U; index < 8U; index++)
        words[index] = (uint16_t)(((uint16_t)address[index * 2U] << 8U) |
                                 address[index * 2U + 1U]);
    for (index = 0U; index < 8U;) {
        size_t end = index;
        if (words[index] != 0U) {
            index++;
            continue;
        }
        while (end < 8U && words[end] == 0U) end++;
        if (end - index > best_length) {
            best_start = index;
            best_length = end - index;
        }
        index = end;
    }
    if (best_length < 2U) best_start = 8U;
    for (index = 0U; index < 8U;) {
        int written;
        if (index == best_start) {
            if (used + 2U >= AGENT_PUBLIC_IPV6_TEXT_LEN) return false;
            output[used++] = ':';
            output[used++] = ':';
            index += best_length;
            if (index == 8U) break;
            continue;
        }
        if (used > 0U && output[used - 1U] != ':') output[used++] = ':';
        written = snprintf(output + used, AGENT_PUBLIC_IPV6_TEXT_LEN - used,
                           "%x", words[index]);
        if (written <= 0 || (size_t)written >=
                AGENT_PUBLIC_IPV6_TEXT_LEN - used) return false;
        used += (size_t)written;
        index++;
    }
    output[used] = '\0';
    return true;
}

static void apply_prefix_mask(uint8_t address[16], uint8_t prefix_length)
{
    size_t byte = prefix_length / 8U;
    uint8_t remaining = (uint8_t)(prefix_length % 8U);
    size_t index;

    if (remaining != 0U) {
        address[byte] &= (uint8_t)(0xffU << (8U - remaining));
        byte++;
    }
    for (index = byte; index < 16U; index++) address[index] = 0U;
}

static bool parse_prefix(
    const char *text,
    uint8_t address[16],
    uint8_t *prefix_length,
    char canonical[AGENT_PUBLIC_IPV6_TEXT_LEN + 4U]
)
{
    const char *slash;
    char address_text[AGENT_PUBLIC_IPV6_TEXT_LEN];
    char *end = NULL;
    unsigned long length;
    char formatted[AGENT_PUBLIC_IPV6_TEXT_LEN];
    int written;
    size_t address_length;

    if (text == NULL || prefix_length == NULL || canonical == NULL)
        return false;
    slash = strrchr(text, '/');
    if (slash == NULL) return false;
    address_length = (size_t)(slash - text);
    if (address_length == 0U || address_length >= sizeof(address_text))
        return false;
    memcpy(address_text, text, address_length);
    address_text[address_length] = '\0';
    length = strtoul(slash + 1, &end, 10);
    if (end == slash + 1 || *end != '\0' || length < 48UL || length > 64UL ||
        !parse_ipv6(address_text, address)) return false;
    *prefix_length = (uint8_t)length;
    apply_prefix_mask(address, *prefix_length);
    if (!format_ipv6(address, formatted)) return false;
    written = snprintf(canonical, AGENT_PUBLIC_IPV6_TEXT_LEN + 4U,
                       "%s/%u", formatted, (unsigned)*prefix_length);
    return written > 0 && (size_t)written < AGENT_PUBLIC_IPV6_TEXT_LEN + 4U;
}

static bool valid_route_id(const char *route_id, uint8_t bytes[16])
{
    size_t index;

    if (route_id == NULL || strlen(route_id) != 32U) return false;
    for (index = 0U; index < 16U; index++) {
        int high = hex_value(route_id[index * 2U]);
        int low = hex_value(route_id[index * 2U + 1U]);
        if (high < 0 || low < 0 || isupper((unsigned char)route_id[index * 2U]) ||
            isupper((unsigned char)route_id[index * 2U + 1U])) return false;
        bytes[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void address_for_route(
    const struct agent_public_ipv6_pool *pool,
    const uint8_t route_bytes[16],
    uint8_t address[16]
)
{
    size_t index;
    uint8_t full_bytes = (uint8_t)(pool->prefix_length / 8U);
    uint8_t remaining = (uint8_t)(pool->prefix_length % 8U);

    memcpy(address, route_bytes, 16U);
    for (index = 0U; index < full_bytes; index++) address[index] = pool->prefix[index];
    if (remaining != 0U) {
        uint8_t mask = (uint8_t)(0xffU << (8U - remaining));
        address[full_bytes] = (uint8_t)((pool->prefix[full_bytes] & mask) |
            (route_bytes[full_bytes] & (uint8_t)~mask));
    }
}

bool agent_public_ipv6_pool_init(
    struct agent_public_ipv6_pool *pool,
    const char *prefix,
    size_t capacity
)
{
    if (pool == NULL || capacity == 0U) return false;
    memset(pool, 0, sizeof(*pool));
    if (!parse_prefix(prefix, pool->prefix, &pool->prefix_length,
                      pool->prefix_text)) return false;
    pool->leases = calloc(capacity, sizeof(*pool->leases));
    if (pool->leases == NULL) {
        memset(pool, 0, sizeof(*pool));
        return false;
    }
    pool->capacity = capacity;
    pool->configured = true;
    return true;
}

void agent_public_ipv6_pool_free(struct agent_public_ipv6_pool *pool)
{
    if (pool == NULL) return;
    free(pool->leases);
    memset(pool, 0, sizeof(*pool));
}

const struct agent_public_ipv6_lease *agent_public_ipv6_find(
    const struct agent_public_ipv6_pool *pool,
    const char *route_id
)
{
    size_t index;
    if (pool == NULL || route_id == NULL) return NULL;
    for (index = 0U; index < pool->count; index++)
        if (strcmp(pool->leases[index].route_id, route_id) == 0)
            return &pool->leases[index];
    return NULL;
}

const struct agent_public_ipv6_lease *agent_public_ipv6_find_address(
    const struct agent_public_ipv6_pool *pool,
    const char *address
)
{
    uint8_t parsed[16];
    size_t index;

    if (pool == NULL || !pool->configured || address == NULL ||
        !parse_ipv6(address, parsed)) return NULL;
    for (index = 0U; index < pool->count; index++) {
        uint8_t lease_address[16];

        if (parse_ipv6(pool->leases[index].address, lease_address) &&
            memcmp(parsed, lease_address, sizeof(parsed)) == 0) {
            return &pool->leases[index];
        }
    }
    return NULL;
}

enum agent_public_ipv6_result agent_public_ipv6_allocate(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    const char *origin,
    uint64_t expires_ms,
    struct agent_public_ipv6_lease *lease
)
{
    uint8_t route_bytes[16];
    uint8_t address[16];
    char address_text[AGENT_PUBLIC_IPV6_TEXT_LEN];
    size_t index;
    struct agent_public_ipv6_lease *entry;

    if (pool == NULL || !pool->configured || origin == NULL || origin[0] == '\0' ||
        expires_ms == 0U || !valid_route_id(route_id, route_bytes))
        return AGENT_PUBLIC_IPV6_INVALID;
    entry = (struct agent_public_ipv6_lease *)agent_public_ipv6_find(pool, route_id);
    if (entry != NULL) {
        if (strcmp(entry->origin, origin) != 0) return AGENT_PUBLIC_IPV6_COLLISION;
        entry->expires_ms = expires_ms;
        if (lease != NULL) *lease = *entry;
        return AGENT_PUBLIC_IPV6_OK;
    }
    if (pool->count >= pool->capacity) return AGENT_PUBLIC_IPV6_FULL;
    address_for_route(pool, route_bytes, address);
    if (!format_ipv6(address, address_text)) return AGENT_PUBLIC_IPV6_INVALID;
    for (index = 0U; index < pool->count; index++)
        if (strcmp(pool->leases[index].address, address_text) == 0)
            return AGENT_PUBLIC_IPV6_COLLISION;
    entry = &pool->leases[pool->count];
    memset(entry, 0, sizeof(*entry));
    if (!copy_text(entry->route_id, sizeof(entry->route_id), route_id) ||
        !copy_text(entry->origin, sizeof(entry->origin), origin) ||
        !copy_text(entry->address, sizeof(entry->address), address_text))
        return AGENT_PUBLIC_IPV6_INVALID;
    entry->expires_ms = expires_ms;
    pool->count++;
    if (lease != NULL) *lease = *entry;
    return AGENT_PUBLIC_IPV6_OK;
}

enum agent_public_ipv6_result agent_public_ipv6_renew(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    const char *origin,
    uint64_t expires_ms,
    struct agent_public_ipv6_lease *lease
)
{
    struct agent_public_ipv6_lease *entry;
    if (pool == NULL || route_id == NULL || origin == NULL || expires_ms == 0U)
        return AGENT_PUBLIC_IPV6_INVALID;
    entry = (struct agent_public_ipv6_lease *)agent_public_ipv6_find(pool, route_id);
    if (entry == NULL) return AGENT_PUBLIC_IPV6_NOT_FOUND;
    if (strcmp(entry->origin, origin) != 0) return AGENT_PUBLIC_IPV6_COLLISION;
    entry->expires_ms = expires_ms;
    if (lease != NULL) *lease = *entry;
    return AGENT_PUBLIC_IPV6_OK;
}

enum agent_public_ipv6_result agent_public_ipv6_release(
    struct agent_public_ipv6_pool *pool,
    const char *route_id,
    struct agent_public_ipv6_lease *lease
)
{
    size_t index;
    if (pool == NULL || route_id == NULL) return AGENT_PUBLIC_IPV6_INVALID;
    for (index = 0U; index < pool->count; index++) {
        if (strcmp(pool->leases[index].route_id, route_id) != 0) continue;
        if (lease != NULL) *lease = pool->leases[index];
        if (index + 1U < pool->count)
            pool->leases[index] = pool->leases[pool->count - 1U];
        memset(&pool->leases[pool->count - 1U], 0, sizeof(pool->leases[0]));
        pool->count--;
        return AGENT_PUBLIC_IPV6_OK;
    }
    return AGENT_PUBLIC_IPV6_NOT_FOUND;
}

size_t agent_public_ipv6_prune(
    struct agent_public_ipv6_pool *pool,
    uint64_t now_ms,
    agent_public_ipv6_release_cb release_cb,
    void *context
)
{
    size_t index = 0U;
    size_t removed = 0U;
    if (pool == NULL) return 0U;
    while (index < pool->count) {
        if (pool->leases[index].expires_ms > now_ms ||
            (release_cb != NULL && !release_cb(&pool->leases[index], context))) {
            index++;
            continue;
        }
        if (index + 1U < pool->count)
            pool->leases[index] = pool->leases[pool->count - 1U];
        memset(&pool->leases[pool->count - 1U], 0, sizeof(pool->leases[0]));
        pool->count--;
        removed++;
    }
    return removed;
}

bool agent_public_ipv6_contains(
    const struct agent_public_ipv6_pool *pool,
    const char *address
)
{
    uint8_t bytes[16];
    uint8_t masked[16];
    if (pool == NULL || !pool->configured || !parse_ipv6(address, bytes))
        return false;
    memcpy(masked, bytes, sizeof(masked));
    apply_prefix_mask(masked, pool->prefix_length);
    return memcmp(masked, pool->prefix, sizeof(masked)) == 0;
}
