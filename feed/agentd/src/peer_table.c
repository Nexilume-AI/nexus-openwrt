#include "peer_table.h"
#include "agent_peer_transport_contract.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL) {
        return capacity;
    }
    for (length = 0U; length < capacity; length++) {
        if (text[length] == '\0') {
            return length;
        }
    }
    return capacity;
}

static void increment_generation(struct peer_table *table)
{
    if (table->generation != UINT64_MAX) {
        table->generation++;
    }
}

static bool identifier_character(unsigned char value)
{
    return islower(value) != 0 || isdigit(value) != 0 ||
           value == '.' || value == '-' || value == '_';
}

bool agent_peer_id_valid(const char *identifier, size_t capacity)
{
    size_t length;
    size_t i;

    if (capacity < 2U) {
        return false;
    }
    length = bounded_length(identifier, capacity);
    if (length == 0U || length >= capacity || length > 64U) {
        return false;
    }
    if ((!islower((unsigned char)identifier[0]) &&
         !isdigit((unsigned char)identifier[0])) ||
        (!islower((unsigned char)identifier[length - 1U]) &&
         !isdigit((unsigned char)identifier[length - 1U]))) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        if (!identifier_character((unsigned char)identifier[i])) {
            return false;
        }
    }
    return true;
}

static bool domain_range_valid(const char *text, size_t length)
{
    size_t label_start = 0U;
    size_t i;

    if (length == 0U || length > 127U) {
        return false;
    }
    for (i = 0U; i <= length; i++) {
        if (i == length || text[i] == '.') {
            if (i == label_start || i - label_start > 63U ||
                text[label_start] == '-' || text[i - 1U] == '-') {
                return false;
            }
            label_start = i + 1U;
            continue;
        }
        if ((!islower((unsigned char)text[i]) &&
             !isdigit((unsigned char)text[i])) && text[i] != '-') {
            return false;
        }
    }
    return true;
}

bool agent_peer_domain_valid(const char *domain_id)
{
    size_t length = bounded_length(domain_id, AGENT_DOMAIN_ID_LEN);

    return length < AGENT_DOMAIN_ID_LEN &&
           domain_range_valid(domain_id, length);
}

bool agent_peer_endpoint_valid(const char *endpoint)
{
    static const char prefix[] = "https://";
    static const char path[] = "/arpx/v1";
    const char *authority;
    const char *slash;
    const char *colon = NULL;
    const char *cursor;
    unsigned long port = 0UL;
    size_t endpoint_length;
    size_t host_length;

    endpoint_length = bounded_length(endpoint, AGENT_PEER_ENDPOINT_LEN);
    if (endpoint_length >= AGENT_PEER_ENDPOINT_LEN ||
        endpoint_length <= sizeof(prefix) - 1U + sizeof(path) - 1U ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }

    authority = endpoint + sizeof(prefix) - 1U;
    slash = strchr(authority, '/');
    if (slash == NULL || strcmp(slash, path) != 0) {
        return false;
    }
    for (cursor = authority; cursor < slash; cursor++) {
        if (*cursor == ':') {
            colon = cursor;
        } else if (*cursor == '@' || *cursor == '?' || *cursor == '#' ||
                   *cursor == '[' || *cursor == ']') {
            return false;
        }
    }
    if (colon == NULL || colon == authority || colon + 1 == slash) {
        return false;
    }

    host_length = (size_t)(colon - authority);
    if (!domain_range_valid(authority, host_length)) {
        return false;
    }
    for (cursor = colon + 1; cursor < slash; cursor++) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
        if (port > 6553UL ||
            (port == 6553UL && (unsigned long)(*cursor - '0') > 5UL)) {
            return false;
        }
        port = (port * 10UL) + (unsigned long)(*cursor - '0');
    }
    return port > 0UL;
}

bool agent_peer_valid(const struct agent_peer *peer)
{
    if (peer == NULL ||
        !agent_peer_id_valid(peer->peer_id, sizeof(peer->peer_id)) ||
        !agent_peer_id_valid(peer->router_id, sizeof(peer->router_id)) ||
        !agent_peer_domain_valid(peer->domain_id) ||
        !agent_peer_endpoint_valid(peer->endpoint) ||
        (peer->connect_ipv4[0] != '\0' &&
         !agent_peer_transport_ipv4_valid(peer->connect_ipv4)) ||
        peer->role < AGENT_PEER_ROLE_DIRECT ||
        peer->role > AGENT_PEER_ROLE_RELAY ||
        peer->state < AGENT_PEER_STATE_CONFIGURED ||
        peer->state > AGENT_PEER_STATE_DOWN ||
        peer->graceful_restart_seconds < 5U ||
        peer->graceful_restart_seconds > 300U) {
        return false;
    }
    return true;
}

void peer_table_init(struct peer_table *table, size_t max_peers)
{
    if (table == NULL) {
        return;
    }
    memset(table, 0, sizeof(*table));
    table->max_peers = max_peers;
}

void peer_table_destroy(struct peer_table *table)
{
    struct agent_peer *peer;
    struct agent_peer *next;

    if (table == NULL) {
        return;
    }
    for (peer = table->head; peer != NULL; peer = next) {
        next = peer->next;
        free(peer);
    }
    memset(table, 0, sizeof(*table));
}

const struct agent_peer *peer_table_find(
    const struct peer_table *table,
    const char *peer_id
)
{
    const struct agent_peer *peer;

    if (table == NULL || peer_id == NULL) {
        return NULL;
    }
    for (peer = table->head; peer != NULL; peer = peer->next) {
        if (strcmp(peer->peer_id, peer_id) == 0) {
            return peer;
        }
    }
    return NULL;
}

enum peer_table_result peer_table_upsert(
    struct peer_table *table,
    const struct agent_peer *peer
)
{
    struct agent_peer *current;
    struct agent_peer *copy;
    struct agent_peer *next;

    if (table == NULL || !agent_peer_valid(peer)) {
        return PEER_TABLE_INVALID;
    }
    for (current = table->head; current != NULL; current = current->next) {
        if (strcmp(current->peer_id, peer->peer_id) != 0 &&
            (strcmp(current->router_id, peer->router_id) == 0 ||
             strcmp(current->endpoint, peer->endpoint) == 0)) {
            return PEER_TABLE_DUPLICATE;
        }
        if (strcmp(current->peer_id, peer->peer_id) == 0) {
            next = current->next;
            *current = *peer;
            current->next = next;
            increment_generation(table);
            return PEER_TABLE_OK;
        }
    }
    if (table->count >= table->max_peers) {
        return PEER_TABLE_FULL;
    }
    copy = calloc(1U, sizeof(*copy));
    if (copy == NULL) {
        return PEER_TABLE_NO_MEMORY;
    }
    *copy = *peer;
    copy->next = table->head;
    table->head = copy;
    table->count++;
    increment_generation(table);
    return PEER_TABLE_OK;
}

enum peer_table_result peer_table_set_relay_ticket(
    struct peer_table *table,
    const char *peer_id,
    const char *session_ticket
)
{
    struct agent_peer *peer;
    size_t length;

    if (table == NULL || peer_id == NULL || session_ticket == NULL) {
        return PEER_TABLE_INVALID;
    }
    length = bounded_length(session_ticket, AGENT_PEER_RELAY_TICKET_LEN);
    if (length == 0U || length >= AGENT_PEER_RELAY_TICKET_LEN) {
        return PEER_TABLE_INVALID;
    }
    for (peer = table->head; peer != NULL; peer = peer->next) {
        if (strcmp(peer->peer_id, peer_id) != 0) continue;
        if (peer->role != AGENT_PEER_ROLE_RELAY) {
            return PEER_TABLE_INVALID;
        }
        memcpy(peer->relay_session_ticket, session_ticket, length + 1U);
        return PEER_TABLE_OK;
    }
    return PEER_TABLE_NOT_FOUND;
}

enum peer_table_result peer_table_remove(
    struct peer_table *table,
    const char *peer_id
)
{
    struct agent_peer **link;
    struct agent_peer *removed;

    if (table == NULL || peer_id == NULL) {
        return PEER_TABLE_INVALID;
    }
    for (link = &table->head; *link != NULL; link = &(*link)->next) {
        if (strcmp((*link)->peer_id, peer_id) == 0) {
            removed = *link;
            *link = removed->next;
            free(removed);
            table->count--;
            increment_generation(table);
            return PEER_TABLE_OK;
        }
    }
    return PEER_TABLE_NOT_FOUND;
}

static bool transition_allowed(
    enum agent_peer_state from,
    enum agent_peer_state to
)
{
    if (from == to) {
        return true;
    }
    switch (from) {
    case AGENT_PEER_STATE_CONFIGURED:
        return to == AGENT_PEER_STATE_CONNECTING ||
               to == AGENT_PEER_STATE_DOWN;
    case AGENT_PEER_STATE_CONNECTING:
        return to == AGENT_PEER_STATE_ESTABLISHED ||
               to == AGENT_PEER_STATE_DOWN;
    case AGENT_PEER_STATE_ESTABLISHED:
        return to == AGENT_PEER_STATE_STALE ||
               to == AGENT_PEER_STATE_DOWN;
    case AGENT_PEER_STATE_STALE:
        return to == AGENT_PEER_STATE_ESTABLISHED ||
               to == AGENT_PEER_STATE_DOWN;
    case AGENT_PEER_STATE_DOWN:
        return to == AGENT_PEER_STATE_CONFIGURED ||
               to == AGENT_PEER_STATE_CONNECTING;
    default:
        return false;
    }
}

enum peer_table_result peer_table_set_state(
    struct peer_table *table,
    const char *peer_id,
    enum agent_peer_state state
)
{
    struct agent_peer *peer;

    if (table == NULL || peer_id == NULL ||
        state < AGENT_PEER_STATE_CONFIGURED || state > AGENT_PEER_STATE_DOWN) {
        return PEER_TABLE_INVALID;
    }
    for (peer = table->head; peer != NULL; peer = peer->next) {
        if (strcmp(peer->peer_id, peer_id) != 0) {
            continue;
        }
        if (!transition_allowed(peer->state, state)) {
            return PEER_TABLE_INVALID_TRANSITION;
        }
        if (peer->state != state) {
            peer->state = state;
            increment_generation(table);
        }
        return PEER_TABLE_OK;
    }
    return PEER_TABLE_NOT_FOUND;
}

void peer_table_swap(struct peer_table *left, struct peer_table *right)
{
    struct peer_table temporary;

    if (left == NULL || right == NULL) {
        return;
    }
    temporary = *left;
    *left = *right;
    *right = temporary;
}

const struct agent_peer *peer_table_first(const struct peer_table *table)
{
    return table == NULL ? NULL : table->head;
}

const char *agent_peer_role_name(enum agent_peer_role role)
{
    switch (role) {
    case AGENT_PEER_ROLE_DIRECT:
        return "direct";
    case AGENT_PEER_ROLE_REFLECTOR:
        return "reflector";
    case AGENT_PEER_ROLE_RELAY:
        return "relay";
    default:
        return "unknown";
    }
}

const char *agent_peer_state_name(enum agent_peer_state state)
{
    switch (state) {
    case AGENT_PEER_STATE_CONFIGURED:
        return "configured";
    case AGENT_PEER_STATE_CONNECTING:
        return "connecting";
    case AGENT_PEER_STATE_ESTABLISHED:
        return "established";
    case AGENT_PEER_STATE_STALE:
        return "stale";
    case AGENT_PEER_STATE_DOWN:
        return "down";
    default:
        return "unknown";
    }
}
