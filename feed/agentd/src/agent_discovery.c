#include "agent_discovery.h"
#include "agent_peer_transport_contract.h"
#include "peer_table.h"

#include <stdlib.h>
#include <string.h>

#define TXT_VERSION 0x01U
#define TXT_ROUTER 0x02U
#define TXT_DOMAIN 0x04U
#define TXT_PATH 0x08U
#define TXT_REQUIRED (TXT_VERSION | TXT_ROUTER | TXT_DOMAIN | TXT_PATH)

static bool copy_text(char *target, size_t capacity, const char *source)
{
    size_t length;

    if (target == NULL || source == NULL || capacity == 0U) {
        return false;
    }
    length = strlen(source);
    if (length == 0U || length >= capacity) {
        return false;
    }
    memcpy(target, source, length + 1U);
    return true;
}

static bool hostname_valid(const char *hostname)
{
    size_t length;
    size_t i;

    if (hostname == NULL) {
        return false;
    }
    length = strlen(hostname);
    if (length == 0U || length >= AGENT_DISCOVERY_HOSTNAME_LEN) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)hostname[i];

        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' ||
              value == '.')) {
            return false;
        }
    }
    return hostname[0] != '.' && hostname[length - 1U] != '.';
}

static bool iface_valid(const char *iface)
{
    size_t length;
    size_t i;

    if (iface == NULL) {
        return false;
    }
    length = strlen(iface);
    if (length == 0U || length >= AGENT_DISCOVERY_IFACE_LEN) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)iface[i];

        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '-' ||
              value == '_' || value == '.')) {
            return false;
        }
    }
    return true;
}

static bool last_update_valid(const char *last_update)
{
    size_t length;
    size_t i;

    if (last_update == NULL) {
        return false;
    }
    length = strlen(last_update);
    if (length < 20U || length >= AGENT_DISCOVERY_LAST_UPDATE_LEN ||
        last_update[length - 1U] != 'Z') {
        return false;
    }
    for (i = 0U; i + 1U < length; i++) {
        unsigned char value = (unsigned char)last_update[i];

        if (!((value >= '0' && value <= '9') || value == '-' ||
              value == ':' || value == 'T')) {
            return false;
        }
    }
    return true;
}

static void increment_generation(struct agent_discovery_table *table)
{
    if (table->generation != UINT64_MAX) {
        table->generation++;
    }
}

void agent_discovery_table_init(
    struct agent_discovery_table *table,
    size_t max_candidates
)
{
    if (table == NULL) {
        return;
    }
    memset(table, 0, sizeof(*table));
    table->max_candidates = max_candidates;
}

void agent_discovery_table_destroy(struct agent_discovery_table *table)
{
    struct agent_discovery_candidate *candidate;
    struct agent_discovery_candidate *next;

    if (table == NULL) {
        return;
    }
    for (candidate = table->head; candidate != NULL; candidate = next) {
        next = candidate->next;
        free(candidate);
    }
    memset(table, 0, sizeof(*table));
}

void agent_discovery_observation_init(
    struct agent_discovery_observation *observation
)
{
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
}

bool agent_discovery_observation_add_txt(
    struct agent_discovery_observation *observation,
    const char *txt
)
{
    const char *value;
    uint8_t field;

    if (observation == NULL || txt == NULL) {
        return false;
    }
    if (strncmp(txt, "ver=", 4U) == 0) {
        field = TXT_VERSION;
        value = txt + 4U;
        if (strcmp(value, "1") != 0) {
            return false;
        }
        observation->protocol_version = AGENT_DISCOVERY_PROTOCOL_VERSION;
    } else if (strncmp(txt, "router=", 7U) == 0) {
        field = TXT_ROUTER;
        value = txt + 7U;
        if (!agent_peer_id_valid(value, AGENT_DISCOVERY_ROUTER_ID_LEN) ||
            !copy_text(observation->router_id,
                       sizeof(observation->router_id), value)) {
            return false;
        }
    } else if (strncmp(txt, "domain=", 7U) == 0) {
        field = TXT_DOMAIN;
        value = txt + 7U;
        if (!agent_peer_domain_valid(value) ||
            !copy_text(observation->domain_id,
                       sizeof(observation->domain_id), value)) {
            return false;
        }
    } else if (strncmp(txt, "path=", 5U) == 0) {
        field = TXT_PATH;
        value = txt + 5U;
        if (strcmp(value, "/arpx/v1") != 0 ||
            !copy_text(observation->registration_path,
                       sizeof(observation->registration_path), value)) {
            return false;
        }
    } else {
        return false;
    }

    if ((observation->txt_fields & field) != 0U) {
        return false;
    }
    observation->txt_fields = (uint8_t)(observation->txt_fields | field);
    return true;
}

bool agent_discovery_observation_valid(
    const struct agent_discovery_observation *observation
)
{
    return observation != NULL &&
           agent_peer_id_valid(observation->instance,
                               sizeof(observation->instance)) &&
           agent_peer_id_valid(observation->router_id,
                               sizeof(observation->router_id)) &&
           strcmp(observation->instance, observation->router_id) == 0 &&
           agent_peer_domain_valid(observation->domain_id) &&
           hostname_valid(observation->hostname) &&
           agent_peer_transport_ipv4_valid(observation->ipv4) &&
           iface_valid(observation->iface) &&
           last_update_valid(observation->last_update) &&
           strcmp(observation->registration_path, "/arpx/v1") == 0 &&
           observation->protocol_version == AGENT_DISCOVERY_PROTOCOL_VERSION &&
           observation->port > 0U &&
           observation->ttl_seconds >= AGENT_DISCOVERY_MIN_TTL_SECONDS &&
           observation->ttl_seconds <= AGENT_DISCOVERY_MAX_TTL_SECONDS &&
           observation->txt_fields == TXT_REQUIRED;
}

const struct agent_discovery_candidate *agent_discovery_table_find(
    const struct agent_discovery_table *table,
    const char *router_id
)
{
    const struct agent_discovery_candidate *candidate;

    if (table == NULL || router_id == NULL) {
        return NULL;
    }
    for (candidate = table->head; candidate != NULL;
         candidate = candidate->next) {
        if (strcmp(candidate->observation.router_id, router_id) == 0) {
            return candidate;
        }
    }
    return NULL;
}

enum agent_discovery_result agent_discovery_table_observe(
    struct agent_discovery_table *table,
    const struct agent_discovery_observation *observation,
    const char *local_router_id,
    uint64_t now_ms
)
{
    struct agent_discovery_candidate *candidate;
    uint64_t lifetime_ms;
    uint64_t expires_at_ms;

    if (table == NULL || local_router_id == NULL ||
        !agent_discovery_observation_valid(observation)) {
        if (table != NULL && table->rejected != UINT64_MAX) {
            table->rejected++;
        }
        return AGENT_DISCOVERY_INVALID;
    }
    if (strcmp(observation->router_id, local_router_id) == 0) {
        if (table->self_suppressed != UINT64_MAX) {
            table->self_suppressed++;
        }
        return AGENT_DISCOVERY_OK;
    }

    lifetime_ms = (uint64_t)observation->ttl_seconds * 1000U;
    expires_at_ms = UINT64_MAX - now_ms < lifetime_ms
        ? UINT64_MAX : now_ms + lifetime_ms;

    for (candidate = table->head; candidate != NULL;
         candidate = candidate->next) {
        if (strcmp(candidate->observation.router_id,
                   observation->router_id) == 0) {
            if (strcmp(candidate->observation.last_update,
                       observation->last_update) == 0) {
                return AGENT_DISCOVERY_OK;
            }
            candidate->observation = *observation;
            candidate->last_seen_ms = now_ms;
            candidate->expires_at_ms = expires_at_ms;
            if (table->accepted != UINT64_MAX) {
                table->accepted++;
            }
            increment_generation(table);
            return AGENT_DISCOVERY_OK;
        }
    }
    if (table->count >= table->max_candidates) {
        if (table->rejected != UINT64_MAX) {
            table->rejected++;
        }
        return AGENT_DISCOVERY_FULL;
    }
    candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) {
        return AGENT_DISCOVERY_NO_MEMORY;
    }
    candidate->observation = *observation;
    candidate->first_seen_ms = now_ms;
    candidate->last_seen_ms = now_ms;
    candidate->expires_at_ms = expires_at_ms;
    candidate->next = table->head;
    table->head = candidate;
    table->count++;
    if (table->accepted != UINT64_MAX) {
        table->accepted++;
    }
    increment_generation(table);
    return AGENT_DISCOVERY_OK;
}

size_t agent_discovery_table_prune(
    struct agent_discovery_table *table,
    uint64_t now_ms
)
{
    struct agent_discovery_candidate **link;
    struct agent_discovery_candidate *removed;
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
        if (table->expired != UINT64_MAX) {
            table->expired++;
        }
        increment_generation(table);
    }
    return count;
}

const struct agent_discovery_candidate *agent_discovery_table_first(
    const struct agent_discovery_table *table
)
{
    return table == NULL ? NULL : table->head;
}
