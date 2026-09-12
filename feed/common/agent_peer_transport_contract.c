#include "agent_peer_transport_contract.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool decimal_octet(const char *start, size_t length)
{
    unsigned int value = 0U;
    size_t index;

    if (length == 0U || length > 3U ||
        (length > 1U && start[0] == '0')) {
        return false;
    }
    for (index = 0U; index < length; index++) {
        if (!isdigit((unsigned char)start[index])) {
            return false;
        }
        value = (value * 10U) + (unsigned int)(start[index] - '0');
    }
    return value <= 255U;
}

bool agent_peer_transport_ipv4_valid(const char *address)
{
    const char *part;
    const char *cursor;
    size_t parts = 0U;
    size_t length;

    if (address == NULL) {
        return false;
    }
    length = strlen(address);
    if (length < 7U || length >= AGENT_PEER_TRANSPORT_IPV4_LEN) {
        return false;
    }
    part = address;
    for (cursor = address; ; cursor++) {
        if (*cursor != '.' && *cursor != '\0') {
            continue;
        }
        if (!decimal_octet(part, (size_t)(cursor - part))) {
            return false;
        }
        parts++;
        if (*cursor == '\0') {
            break;
        }
        part = cursor + 1;
    }
    return parts == 4U;
}

enum agent_peer_transport_role agent_peer_transport_role_select(
    const char *local_router_id,
    const char *remote_router_id
)
{
    int order;

    if (local_router_id == NULL || remote_router_id == NULL ||
        local_router_id[0] == '\0' || remote_router_id[0] == '\0') {
        return AGENT_PEER_TRANSPORT_ROLE_INVALID;
    }
    order = strcmp(local_router_id, remote_router_id);
    if (order == 0) {
        return AGENT_PEER_TRANSPORT_ROLE_INVALID;
    }
    return order < 0 ? AGENT_PEER_TRANSPORT_ROLE_DIAL :
                       AGENT_PEER_TRANSPORT_ROLE_ACCEPT;
}

static bool host_valid(const char *host, size_t length)
{
    size_t label_start = 0U;
    size_t index;

    if (length == 0U || length >= AGENT_PEER_TRANSPORT_HOST_LEN) {
        return false;
    }
    for (index = 0U; index <= length; index++) {
        if (index == length || host[index] == '.') {
            if (index == label_start || index - label_start > 63U ||
                host[label_start] == '-' || host[index - 1U] == '-') {
                return false;
            }
            label_start = index + 1U;
            continue;
        }
        if (!islower((unsigned char)host[index]) &&
            !isdigit((unsigned char)host[index]) && host[index] != '-') {
            return false;
        }
    }
    return true;
}

bool agent_peer_transport_endpoint_parse(
    const char *endpoint,
    const char *connect_ipv4,
    struct agent_peer_transport_endpoint *parsed
)
{
    static const char prefix[] = "https://";
    const char *authority;
    const char *colon;
    const char *slash;
    const char *cursor;
    unsigned long port = 0UL;
    size_t host_length;
    size_t authority_length;
    int written;

    if (endpoint == NULL || connect_ipv4 == NULL || parsed == NULL ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0 ||
        !agent_peer_transport_ipv4_valid(connect_ipv4)) {
        return false;
    }
    authority = endpoint + sizeof(prefix) - 1U;
    slash = strchr(authority, '/');
    colon = slash == NULL ? NULL : memchr(authority, ':',
                                           (size_t)(slash - authority));
    if (slash == NULL || strcmp(slash, AGENT_PEER_TRANSPORT_PATH) != 0 ||
        colon == NULL || colon == authority || colon + 1 == slash) {
        return false;
    }
    host_length = (size_t)(colon - authority);
    authority_length = (size_t)(slash - authority);
    if (!host_valid(authority, host_length) ||
        authority_length >= AGENT_PEER_TRANSPORT_AUTHORITY_LEN) {
        return false;
    }
    for (cursor = colon + 1; cursor < slash; cursor++) {
        if (!isdigit((unsigned char)*cursor) || port > 6553UL ||
            (port == 6553UL && (unsigned long)(*cursor - '0') > 5UL)) {
            return false;
        }
        port = (port * 10UL) + (unsigned long)(*cursor - '0');
    }
    if (port == 0UL) {
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));
    memcpy(parsed->server_identity, authority, host_length);
    parsed->server_identity[host_length] = '\0';
    memcpy(parsed->authority, authority, authority_length);
    parsed->authority[authority_length] = '\0';
    written = snprintf(parsed->connect_ipv4, sizeof(parsed->connect_ipv4),
                       "%s", connect_ipv4);
    if (written < 0 || (size_t)written >= sizeof(parsed->connect_ipv4)) {
        memset(parsed, 0, sizeof(*parsed));
        return false;
    }
    parsed->port = (uint16_t)port;
    return true;
}

void agent_peer_transport_ingress_init(
    struct agent_peer_transport_ingress *ingress
)
{
    if (ingress != NULL) {
        memset(ingress, 0, sizeof(*ingress));
    }
}

static size_t frame_size_from_header(const uint8_t header[4])
{
    uint32_t payload = ((uint32_t)header[0] << 24U) |
                       ((uint32_t)header[1] << 16U) |
                       ((uint32_t)header[2] << 8U) | header[3];

    if (payload == 0U || payload >
        AGENT_ARPX_MAX_FRAME_SIZE - AGENT_ARPX_FRAME_HEADER_SIZE) {
        return 0U;
    }
    return (size_t)payload + AGENT_ARPX_FRAME_HEADER_SIZE;
}

enum agent_peer_transport_contract_result agent_peer_transport_ingress_feed(
    struct agent_peer_transport_ingress *ingress,
    const uint8_t *data,
    size_t size,
    agent_peer_transport_frame_callback callback,
    void *context,
    size_t *frames_emitted
)
{
    size_t emitted = 0U;
    size_t copied;
    size_t remaining;

    if (frames_emitted != NULL) {
        *frames_emitted = 0U;
    }
    if (ingress == NULL || (data == NULL && size != 0U) ||
        callback == NULL || frames_emitted == NULL) {
        return AGENT_PEER_TRANSPORT_CONTRACT_INVALID;
    }
    while (size > 0U) {
        if (ingress->expected == 0U && ingress->used < 4U) {
            copied = 4U - ingress->used;
            if (copied > size) {
                copied = size;
            }
            memcpy(ingress->frame + ingress->used, data, copied);
            ingress->used += copied;
            data += copied;
            size -= copied;
            if (ingress->used < 4U) {
                continue;
            }
            ingress->expected = frame_size_from_header(ingress->frame);
            if (ingress->expected == 0U) {
                agent_peer_transport_ingress_init(ingress);
                return AGENT_PEER_TRANSPORT_CONTRACT_FRAME_TOO_LARGE;
            }
        }
        remaining = ingress->expected - ingress->used;
        copied = remaining < size ? remaining : size;
        memcpy(ingress->frame + ingress->used, data, copied);
        ingress->used += copied;
        data += copied;
        size -= copied;
        if (ingress->used == ingress->expected) {
            if (!callback(ingress->frame, ingress->expected, context)) {
                agent_peer_transport_ingress_init(ingress);
                return AGENT_PEER_TRANSPORT_CONTRACT_CALLBACK_ERROR;
            }
            emitted++;
            agent_peer_transport_ingress_init(ingress);
        }
    }
    *frames_emitted = emitted;
    return AGENT_PEER_TRANSPORT_CONTRACT_OK;
}
