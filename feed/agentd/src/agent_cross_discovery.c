#include "agent_cross_discovery.h"
#include "peer_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DNS_HEADER_LEN 12U
#define DNS_FLAG_QR 0x8000U
#define DNS_FLAG_TC 0x0200U
#define DNS_FLAG_RD 0x0100U
#define DNS_FLAG_AD 0x0020U
#define DNS_CLASS_IN 1U
#define DNS_TYPE_OPT 41U
#define DNS_MAX_NAME_JUMPS 32U
#define DNS_MAX_MANDATORY_KEYS 16U

struct svcb_record {
    char target[AGENT_CROSS_HOSTNAME_LEN];
    char router_id[AGENT_CROSS_ROUTER_ID_LEN];
    char domain_id[AGENT_CROSS_DOMAIN_ID_LEN];
    char agent_card_uri[AGENT_CROSS_CARD_URI_LEN];
    char ipv4_hint[AGENT_CROSS_IPV4_LEN];
    uint16_t priority;
    uint16_t port;
    uint32_t unknown_optional_keys;
};

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static bool copy_bytes_text(
    char *target,
    size_t capacity,
    const uint8_t *source,
    size_t length
)
{
    size_t i;

    if (target == NULL || source == NULL || length == 0U ||
        length >= capacity) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        if (source[i] < 0x21U || source[i] > 0x7eU) {
            return false;
        }
        target[i] = (char)source[i];
    }
    target[length] = '\0';
    return true;
}

static bool dns_name_valid(const char *name, bool allow_service_label)
{
    size_t length;
    size_t label_length = 0U;
    size_t i;

    if (name == NULL) {
        return false;
    }
    length = strlen(name);
    if (length == 0U || length >= AGENT_CROSS_HOSTNAME_LEN ||
        name[0] == '.' || name[length - 1U] == '.') {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)name[i];

        if (value == '.') {
            if (label_length == 0U || label_length > 63U) {
                return false;
            }
            label_length = 0U;
            continue;
        }
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' ||
              (allow_service_label && value == '_'))) {
            return false;
        }
        label_length++;
    }
    return label_length > 0U && label_length <= 63U;
}

bool agent_cross_owner_from_domain(
    const char *domain,
    char *owner,
    size_t owner_capacity
)
{
    int written;

    if (!agent_peer_domain_valid(domain) || owner == NULL) {
        return false;
    }
    written = snprintf(owner, owner_capacity, "_agents.%s", domain);
    return written > 0 && (size_t)written < owner_capacity;
}

static size_t encode_dns_name(
    uint8_t *packet,
    size_t capacity,
    const char *name
)
{
    const char *label = name;
    const char *dot;
    size_t offset = 0U;
    size_t length;

    if (packet == NULL || !dns_name_valid(name, true)) {
        return 0U;
    }
    while (*label != '\0') {
        dot = strchr(label, '.');
        length = dot == NULL ? strlen(label) : (size_t)(dot - label);
        if (length == 0U || length > 63U || offset + 1U + length >= capacity) {
            return 0U;
        }
        packet[offset++] = (uint8_t)length;
        memcpy(packet + offset, label, length);
        offset += length;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    if (offset >= capacity) {
        return 0U;
    }
    packet[offset++] = 0U;
    return offset;
}

size_t agent_cross_build_query(
    uint8_t *packet,
    size_t capacity,
    uint16_t query_id,
    const char *owner
)
{
    size_t name_length;
    size_t offset;

    if (packet == NULL || capacity < DNS_HEADER_LEN + 16U) {
        return 0U;
    }
    memset(packet, 0, capacity);
    write_u16(packet, query_id);
    write_u16(packet + 2U, DNS_FLAG_RD);
    write_u16(packet + 4U, 1U);
    write_u16(packet + 10U, 1U);
    name_length = encode_dns_name(packet + DNS_HEADER_LEN,
                                  capacity - DNS_HEADER_LEN, owner);
    if (name_length == 0U) {
        return 0U;
    }
    offset = DNS_HEADER_LEN + name_length;
    if (offset + 15U > capacity) {
        return 0U;
    }
    write_u16(packet + offset, AGENT_CROSS_SVCB_TYPE);
    write_u16(packet + offset + 2U, DNS_CLASS_IN);
    offset += 4U;

    /* EDNS(0), 1232-byte UDP payload and DO=1. */
    packet[offset++] = 0U;
    write_u16(packet + offset, DNS_TYPE_OPT);
    write_u16(packet + offset + 2U, 1232U);
    packet[offset + 4U] = 0U;
    packet[offset + 5U] = 0U;
    write_u16(packet + offset + 6U, 0x8000U);
    write_u16(packet + offset + 8U, 0U);
    return offset + 10U;
}

static bool read_dns_name(
    const uint8_t *packet,
    size_t packet_length,
    size_t offset,
    size_t *consumed,
    char *output,
    size_t output_capacity
)
{
    size_t cursor = offset;
    size_t output_length = 0U;
    size_t original_consumed = 0U;
    unsigned int jumps = 0U;
    bool jumped = false;

    if (packet == NULL || consumed == NULL || output == NULL ||
        output_capacity == 0U) {
        return false;
    }
    while (cursor < packet_length && jumps <= DNS_MAX_NAME_JUMPS) {
        uint8_t label_length = packet[cursor];

        if ((label_length & 0xc0U) == 0xc0U) {
            uint16_t pointer;

            if (cursor + 1U >= packet_length) {
                return false;
            }
            pointer = (uint16_t)(((uint16_t)(label_length & 0x3fU) << 8U) |
                                 packet[cursor + 1U]);
            if (pointer >= packet_length) {
                return false;
            }
            if (!jumped) {
                original_consumed += 2U;
                jumped = true;
            }
            cursor = pointer;
            jumps++;
            continue;
        }
        if ((label_length & 0xc0U) != 0U || label_length > 63U) {
            return false;
        }
        cursor++;
        if (!jumped) {
            original_consumed++;
        }
        if (label_length == 0U) {
            if (output_length == 0U) {
                if (output_capacity < 2U) {
                    return false;
                }
                output[output_length++] = '.';
            }
            output[output_length] = '\0';
            *consumed = original_consumed;
            return true;
        }
        if (cursor + label_length > packet_length ||
            output_length + label_length + 1U >= output_capacity) {
            return false;
        }
        if (output_length > 0U) {
            output[output_length++] = '.';
        }
        memcpy(output + output_length, packet + cursor, label_length);
        output_length += label_length;
        cursor += label_length;
        if (!jumped) {
            original_consumed += label_length;
        }
    }
    return false;
}

static bool mandatory_key_supported(uint16_t key)
{
    return key == 1U || key == 2U || key == 3U || key == 4U ||
           key == 6U || key == AGENT_CROSS_KEY_ROUTER_ID ||
           key == AGENT_CROSS_KEY_DOMAIN_ID ||
           key == AGENT_CROSS_KEY_AGENT_CARD;
}

static bool mandatory_contains(
    const uint16_t *keys,
    size_t count,
    uint16_t wanted
)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        if (keys[i] == wanted) {
            return true;
        }
    }
    return false;
}

static bool alpn_has_h2(const uint8_t *value, size_t length)
{
    size_t offset = 0U;
    bool found = false;

    while (offset < length) {
        uint8_t item_length = value[offset++];

        if (item_length == 0U || offset + item_length > length) {
            return false;
        }
        if (item_length == 2U && memcmp(value + offset, "h2", 2U) == 0) {
            found = true;
        }
        offset += item_length;
    }
    return found;
}

static bool card_uri_valid(
    const char *uri,
    const char *target
)
{
    static const char path[] = "/.well-known/agent-card.json";
    char prefix[AGENT_CROSS_HOSTNAME_LEN + 10U];
    const char *cursor;
    unsigned int port = 0U;
    size_t digits = 0U;
    int written;

    if (uri == NULL || target == NULL) return false;
    written = snprintf(prefix, sizeof(prefix), "https://%s:", target);
    if (written <= 0 || (size_t)written >= sizeof(prefix) ||
        strncmp(uri, prefix, (size_t)written) != 0) return false;
    cursor = uri + (size_t)written;
    while (*cursor >= '0' && *cursor <= '9') {
        if (digits >= 5U) return false;
        port = port * 10U + (unsigned int)(*cursor - '0');
        digits++;
        cursor++;
    }
    return digits > 0U && port > 0U && port <= UINT16_MAX &&
           strcmp(cursor, path) == 0;
}

static bool parse_svcb_rdata(
    const uint8_t *packet,
    size_t packet_length,
    size_t rdata_offset,
    size_t rdata_length,
    struct svcb_record *record,
    bool *incompatible
)
{
    uint16_t mandatory[DNS_MAX_MANDATORY_KEYS];
    size_t mandatory_count = 0U;
    size_t target_consumed;
    size_t offset;
    size_t end;
    uint16_t previous_key = 0U;
    bool have_previous = false;
    bool have_mandatory = false;
    bool have_alpn = false;
    bool have_port = false;
    bool have_router = false;
    bool have_domain = false;
    bool have_card = false;
    bool no_default_alpn = false;
    bool have_ipv6_hint = false;

    if (packet == NULL || record == NULL || incompatible == NULL ||
        rdata_length < 4U || rdata_offset > packet_length ||
        rdata_length > packet_length - rdata_offset) {
        return false;
    }
    memset(record, 0, sizeof(*record));
    *incompatible = false;
    end = rdata_offset + rdata_length;
    record->priority = read_u16(packet + rdata_offset);
    if (record->priority == 0U ||
        !read_dns_name(packet, packet_length, rdata_offset + 2U,
                       &target_consumed, record->target,
                       sizeof(record->target)) ||
        strcmp(record->target, ".") == 0 ||
        !dns_name_valid(record->target, false)) {
        return false;
    }
    offset = rdata_offset + 2U + target_consumed;
    if (offset > end) {
        return false;
    }
    while (offset < end) {
        uint16_t key;
        uint16_t value_length;
        const uint8_t *value;

        if (end - offset < 4U) {
            return false;
        }
        key = read_u16(packet + offset);
        value_length = read_u16(packet + offset + 2U);
        offset += 4U;
        if ((have_previous && key <= previous_key) ||
            value_length > end - offset) {
            return false;
        }
        have_previous = true;
        previous_key = key;
        value = packet + offset;

        if (key == 0U) {
            size_t i;

            if (value_length < 2U || (value_length % 2U) != 0U ||
                value_length / 2U > DNS_MAX_MANDATORY_KEYS) {
                return false;
            }
            have_mandatory = true;
            for (i = 0U; i < value_length; i += 2U) {
                uint16_t mandatory_key = read_u16(value + i);

                if (mandatory_key == 0U ||
                    (mandatory_count > 0U &&
                     mandatory_key <= mandatory[mandatory_count - 1U])) {
                    return false;
                }
                if (!mandatory_key_supported(mandatory_key)) {
                    *incompatible = true;
                }
                mandatory[mandatory_count++] = mandatory_key;
            }
        } else if (key == 1U) {
            if (!alpn_has_h2(value, value_length)) {
                return false;
            }
            have_alpn = true;
        } else if (key == 2U) {
            if (value_length != 0U) {
                return false;
            }
            no_default_alpn = true;
        } else if (key == 3U) {
            if (value_length != 2U || read_u16(value) == 0U) {
                return false;
            }
            record->port = read_u16(value);
            have_port = true;
        } else if (key == 4U) {
            if (value_length == 0U || (value_length % 4U) != 0U) {
                return false;
            }
            snprintf(record->ipv4_hint, sizeof(record->ipv4_hint),
                     "%u.%u.%u.%u", (unsigned int)value[0],
                     (unsigned int)value[1], (unsigned int)value[2],
                     (unsigned int)value[3]);
        } else if (key == 6U) {
            if (value_length == 0U || (value_length % 16U) != 0U) {
                return false;
            }
            have_ipv6_hint = true;
        } else if (key == AGENT_CROSS_KEY_ROUTER_ID) {
            if (!copy_bytes_text(record->router_id,
                                 sizeof(record->router_id), value,
                                 value_length) ||
                !agent_peer_id_valid(record->router_id,
                                     sizeof(record->router_id))) {
                return false;
            }
            have_router = true;
        } else if (key == AGENT_CROSS_KEY_DOMAIN_ID) {
            if (!copy_bytes_text(record->domain_id,
                                 sizeof(record->domain_id), value,
                                 value_length) ||
                !agent_peer_domain_valid(record->domain_id)) {
                return false;
            }
            have_domain = true;
        } else if (key == AGENT_CROSS_KEY_AGENT_CARD) {
            if (!copy_bytes_text(record->agent_card_uri,
                                 sizeof(record->agent_card_uri), value,
                                 value_length)) {
                return false;
            }
            have_card = true;
        } else if (record->unknown_optional_keys != UINT32_MAX) {
            record->unknown_optional_keys++;
        }
        offset += value_length;
    }

    if (!have_mandatory || !have_alpn || !have_port || !have_router ||
        !have_domain || !have_card ||
        !mandatory_contains(mandatory, mandatory_count, 1U) ||
        !mandatory_contains(mandatory, mandatory_count, 3U) ||
        !mandatory_contains(mandatory, mandatory_count,
                            AGENT_CROSS_KEY_ROUTER_ID) ||
        !mandatory_contains(mandatory, mandatory_count,
                            AGENT_CROSS_KEY_DOMAIN_ID) ||
        !mandatory_contains(mandatory, mandatory_count,
                            AGENT_CROSS_KEY_AGENT_CARD) ||
        (no_default_alpn && !have_alpn)) {
        return false;
    }
    {
        size_t i;

        for (i = 0U; i < mandatory_count; i++) {
            uint16_t key = mandatory[i];
            bool present = key == 1U ? have_alpn :
                           key == 2U ? no_default_alpn :
                           key == 3U ? have_port :
                           key == 4U ? record->ipv4_hint[0] != '\0' :
                           key == 6U ? have_ipv6_hint :
                           key == AGENT_CROSS_KEY_ROUTER_ID ? have_router :
                           key == AGENT_CROSS_KEY_DOMAIN_ID ? have_domain :
                           key == AGENT_CROSS_KEY_AGENT_CARD ? have_card :
                           false;
            if (!present) {
                return false;
            }
        }
    }
    return !*incompatible &&
           card_uri_valid(record->agent_card_uri, record->target);
}

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        (*value)++;
    }
}

void agent_cross_table_init(
    struct agent_cross_table *table,
    size_t max_candidates
)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
        table->max_candidates = max_candidates;
    }
}

void agent_cross_table_destroy(struct agent_cross_table *table)
{
    struct agent_cross_candidate *candidate;
    struct agent_cross_candidate *next;

    if (table == NULL) {
        return;
    }
    for (candidate = table->head; candidate != NULL; candidate = next) {
        next = candidate->next;
        free(candidate);
    }
    memset(table, 0, sizeof(*table));
}

static bool table_observe(
    struct agent_cross_table *table,
    const char *owner,
    const struct svcb_record *record,
    uint32_t ttl_seconds,
    uint64_t now_ms
)
{
    struct agent_cross_candidate *candidate;
    uint64_t lifetime_ms = (uint64_t)ttl_seconds * 1000U;
    uint64_t expires_at = UINT64_MAX - now_ms < lifetime_ms
        ? UINT64_MAX : now_ms + lifetime_ms;

    for (candidate = table->head; candidate != NULL;
         candidate = candidate->next) {
        if (strcmp(candidate->router_id, record->router_id) != 0) {
            continue;
        }
        if (record->priority > candidate->priority &&
            candidate->expires_at_ms > now_ms) {
            return true;
        }
        snprintf(candidate->owner, sizeof(candidate->owner), "%s", owner);
        snprintf(candidate->target, sizeof(candidate->target), "%s",
                 record->target);
        snprintf(candidate->domain_id, sizeof(candidate->domain_id), "%s",
                 record->domain_id);
        snprintf(candidate->agent_card_uri,
                 sizeof(candidate->agent_card_uri), "%s",
                 record->agent_card_uri);
        snprintf(candidate->ipv4_hint, sizeof(candidate->ipv4_hint), "%s",
                 record->ipv4_hint);
        candidate->priority = record->priority;
        candidate->port = record->port;
        candidate->ttl_seconds = ttl_seconds;
        candidate->unknown_optional_keys = record->unknown_optional_keys;
        candidate->last_seen_ms = now_ms;
        candidate->expires_at_ms = expires_at;
        increment(&table->records_accepted);
        increment(&table->generation);
        return true;
    }
    if (table->count >= table->max_candidates) {
        increment(&table->records_rejected);
        return false;
    }
    candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) {
        return false;
    }
    snprintf(candidate->owner, sizeof(candidate->owner), "%s", owner);
    snprintf(candidate->target, sizeof(candidate->target), "%s",
             record->target);
    snprintf(candidate->router_id, sizeof(candidate->router_id), "%s",
             record->router_id);
    snprintf(candidate->domain_id, sizeof(candidate->domain_id), "%s",
             record->domain_id);
    snprintf(candidate->agent_card_uri, sizeof(candidate->agent_card_uri),
             "%s", record->agent_card_uri);
    snprintf(candidate->ipv4_hint, sizeof(candidate->ipv4_hint), "%s",
             record->ipv4_hint);
    candidate->priority = record->priority;
    candidate->port = record->port;
    candidate->ttl_seconds = ttl_seconds;
    candidate->unknown_optional_keys = record->unknown_optional_keys;
    candidate->first_seen_ms = now_ms;
    candidate->last_seen_ms = now_ms;
    candidate->expires_at_ms = expires_at;
    candidate->next = table->head;
    table->head = candidate;
    table->count++;
    increment(&table->records_accepted);
    increment(&table->generation);
    return true;
}

enum agent_cross_parse_result agent_cross_parse_response(
    struct agent_cross_table *table,
    const uint8_t *packet,
    size_t packet_length,
    uint16_t expected_query_id,
    const char *expected_owner,
    uint64_t now_ms,
    uint32_t ttl_cap_seconds
)
{
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    size_t offset = DNS_HEADER_LEN;
    size_t consumed;
    char name[AGENT_CROSS_HOSTNAME_LEN];
    uint16_t i;
    size_t accepted_before;

    if (table == NULL || packet == NULL || packet_length < DNS_HEADER_LEN ||
        packet_length > AGENT_CROSS_MAX_DNS_PACKET ||
        expected_owner == NULL || read_u16(packet) != expected_query_id) {
        return AGENT_CROSS_PARSE_MALFORMED;
    }
    flags = read_u16(packet + 2U);
    if ((flags & DNS_FLAG_QR) == 0U) {
        return AGENT_CROSS_PARSE_MALFORMED;
    }
    if ((flags & DNS_FLAG_TC) != 0U) {
        return AGENT_CROSS_PARSE_TRUNCATED;
    }
    if ((flags & 0x000fU) != 0U) {
        return AGENT_CROSS_PARSE_RCODE;
    }
    if ((flags & DNS_FLAG_AD) == 0U) {
        increment(&table->dnssec_rejected);
        return AGENT_CROSS_PARSE_INSECURE;
    }
    questions = read_u16(packet + 4U);
    answers = read_u16(packet + 6U);
    if (questions != 1U || answers > 64U) {
        return AGENT_CROSS_PARSE_MALFORMED;
    }
    if (!read_dns_name(packet, packet_length, offset, &consumed,
                       name, sizeof(name))) {
        return AGENT_CROSS_PARSE_MALFORMED;
    }
    offset += consumed;
    if (offset + 4U > packet_length || strcmp(name, expected_owner) != 0 ||
        read_u16(packet + offset) != AGENT_CROSS_SVCB_TYPE ||
        read_u16(packet + offset + 2U) != DNS_CLASS_IN) {
        return AGENT_CROSS_PARSE_MALFORMED;
    }
    offset += 4U;
    accepted_before = table->count;
    for (i = 0U; i < answers; i++) {
        uint16_t type;
        uint16_t rr_class;
        uint32_t ttl;
        uint16_t rdlength;
        struct svcb_record record;
        bool incompatible = false;

        if (!read_dns_name(packet, packet_length, offset, &consumed,
                           name, sizeof(name))) {
            return AGENT_CROSS_PARSE_MALFORMED;
        }
        offset += consumed;
        if (offset + 10U > packet_length) {
            return AGENT_CROSS_PARSE_MALFORMED;
        }
        type = read_u16(packet + offset);
        rr_class = read_u16(packet + offset + 2U);
        ttl = read_u32(packet + offset + 4U);
        rdlength = read_u16(packet + offset + 8U);
        offset += 10U;
        if (rdlength > packet_length - offset) {
            return AGENT_CROSS_PARSE_MALFORMED;
        }
        if (type == AGENT_CROSS_SVCB_TYPE && rr_class == DNS_CLASS_IN &&
            strcmp(name, expected_owner) == 0) {
            if (ttl < AGENT_CROSS_MIN_TTL_SECONDS) {
                ttl = AGENT_CROSS_MIN_TTL_SECONDS;
            } else if (ttl > ttl_cap_seconds) {
                ttl = ttl_cap_seconds;
            }
            if (parse_svcb_rdata(packet, packet_length, offset, rdlength,
                                 &record, &incompatible)) {
                const char *expected_domain =
                    strncmp(expected_owner, "_agents.", 8U) == 0
                    ? expected_owner + 8U : NULL;

                if (expected_domain == NULL ||
                    strcmp(record.domain_id, expected_domain) != 0) {
                    increment(&table->records_rejected);
                } else {
                    (void)table_observe(table, expected_owner, &record,
                                        ttl, now_ms);
                }
            } else if (incompatible) {
                increment(&table->records_incompatible);
            } else {
                increment(&table->records_rejected);
            }
        }
        offset += rdlength;
    }
    return table->count > accepted_before || answers > 0U
        ? AGENT_CROSS_PARSE_OK : AGENT_CROSS_PARSE_NO_DATA;
}

size_t agent_cross_table_prune(
    struct agent_cross_table *table,
    uint64_t now_ms
)
{
    struct agent_cross_candidate **link;
    struct agent_cross_candidate *removed;
    size_t count = 0U;

    if (table == NULL) {
        return 0U;
    }
    link = &table->head;
    while (*link != NULL) {
        if ((*link)->expires_at_ms > now_ms) {
            link = &(*link)->next;
            continue;
        }
        removed = *link;
        *link = removed->next;
        free(removed);
        table->count--;
        count++;
        increment(&table->candidates_expired);
        increment(&table->generation);
    }
    return count;
}

const struct agent_cross_candidate *agent_cross_table_first(
    const struct agent_cross_table *table
)
{
    return table == NULL ? NULL : table->head;
}
