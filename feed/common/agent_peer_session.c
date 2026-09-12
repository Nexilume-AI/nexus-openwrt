#include "agent_peer_session.h"

#include <stdio.h>
#include <string.h>

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static bool terminated_text(const char *text, size_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static void schedule_backoff(
    struct agent_peer_session *session,
    uint64_t now_ms
)
{
    uint32_t next_backoff;

    session->state = AGENT_PEER_SESSION_BACKOFF;
    session->deadline_ms = 0U;
    session->next_action_ms = saturating_add(
        now_ms, session->current_backoff_ms);
    next_backoff = session->current_backoff_ms > UINT32_MAX / 2U
        ? UINT32_MAX
        : session->current_backoff_ms * 2U;
    session->current_backoff_ms = next_backoff > session->config.max_backoff_ms
        ? session->config.max_backoff_ms
        : next_backoff;
}

bool agent_peer_session_init(
    struct agent_peer_session *session,
    const struct agent_peer_session_config *config
)
{
    struct agent_arpx_message probe;

    if (session == NULL || config == NULL ||
        !terminated_text(config->expected_router_id,
                         sizeof(config->expected_router_id)) ||
        !terminated_text(config->expected_domain_id,
                         sizeof(config->expected_domain_id)) ||
        config->open_timeout_ms < 100U ||
        config->open_timeout_ms > 30000U ||
        config->heartbeat_miss_limit < 2U ||
        config->heartbeat_miss_limit > 10U ||
        config->initial_backoff_ms < 100U ||
        config->initial_backoff_ms > config->max_backoff_ms ||
        config->max_backoff_ms > 300000U) {
        return false;
    }
    memset(&probe, 0, sizeof(probe));
    probe.version = AGENT_ARPX_VERSION;
    probe.type = AGENT_ARPX_OPEN;
    snprintf(probe.router_id, sizeof(probe.router_id), "%s",
             config->expected_router_id);
    snprintf(probe.domain_id, sizeof(probe.domain_id), "%s",
             config->expected_domain_id);
    probe.boot_epoch = 1U;
    probe.sequence = 1U;
    probe.heartbeat_ms = AGENT_ARPX_MIN_HEARTBEAT_MS;
    if (agent_arpx_message_validate(&probe) != AGENT_ARPX_OK) {
        return false;
    }

    memset(session, 0, sizeof(*session));
    session->config = *config;
    session->state = AGENT_PEER_SESSION_CONFIGURED;
    session->current_backoff_ms = config->initial_backoff_ms;
    return true;
}

enum agent_peer_session_result agent_peer_session_begin(
    struct agent_peer_session *session,
    uint64_t now_ms
)
{
    if (session == NULL) {
        return AGENT_PEER_SESSION_INVALID;
    }
    if (session->state != AGENT_PEER_SESSION_CONFIGURED) {
        return AGENT_PEER_SESSION_INVALID_STATE;
    }
    session->state = AGENT_PEER_SESSION_CONNECTING;
    session->deadline_ms = saturating_add(
        now_ms, session->config.open_timeout_ms);
    session->next_action_ms = 0U;
    return AGENT_PEER_SESSION_OK;
}

enum agent_peer_session_result agent_peer_session_transport_ready(
    struct agent_peer_session *session,
    uint64_t now_ms
)
{
    if (session == NULL) {
        return AGENT_PEER_SESSION_INVALID;
    }
    if (session->state != AGENT_PEER_SESSION_CONNECTING) {
        return AGENT_PEER_SESSION_INVALID_STATE;
    }
    session->state = AGENT_PEER_SESSION_WAIT_OPEN;
    session->deadline_ms = saturating_add(
        now_ms, session->config.open_timeout_ms);
    return AGENT_PEER_SESSION_OK;
}

static enum agent_peer_session_result fail_protocol(
    struct agent_peer_session *session,
    uint64_t now_ms,
    enum agent_peer_session_result result
)
{
    session->protocol_errors++;
    schedule_backoff(session, now_ms);
    return result;
}

enum agent_peer_session_result agent_peer_session_on_message(
    struct agent_peer_session *session,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    uint64_t heartbeat_window;

    if (session == NULL || message == NULL) {
        return AGENT_PEER_SESSION_INVALID;
    }
    if (session->state != AGENT_PEER_SESSION_WAIT_OPEN &&
        session->state != AGENT_PEER_SESSION_ESTABLISHED) {
        return AGENT_PEER_SESSION_INVALID_STATE;
    }
    if (agent_arpx_message_validate(message) != AGENT_ARPX_OK) {
        return fail_protocol(session, now_ms,
                             AGENT_PEER_SESSION_PROTOCOL_ERROR);
    }
    if (strcmp(message->router_id, session->config.expected_router_id) != 0 ||
        strcmp(message->domain_id, session->config.expected_domain_id) != 0) {
        return fail_protocol(session, now_ms,
                             AGENT_PEER_SESSION_IDENTITY_MISMATCH);
    }
    if (session->state == AGENT_PEER_SESSION_WAIT_OPEN) {
        if (message->type != AGENT_ARPX_OPEN) {
            return fail_protocol(session, now_ms,
                                 AGENT_PEER_SESSION_PROTOCOL_ERROR);
        }
        session->remote_boot_epoch = message->boot_epoch;
        session->last_sequence = message->sequence;
        session->remote_heartbeat_ms = message->heartbeat_ms;
        heartbeat_window = (uint64_t)message->heartbeat_ms *
                           session->config.heartbeat_miss_limit;
        session->deadline_ms = saturating_add(now_ms, heartbeat_window);
        session->state = AGENT_PEER_SESSION_ESTABLISHED;
        session->current_backoff_ms = session->config.initial_backoff_ms;
        session->opens_accepted++;
        return AGENT_PEER_SESSION_OK;
    }
    if ((message->type != AGENT_ARPX_HEARTBEAT &&
         message->type != AGENT_ARPX_CAPABILITY_UPDATE &&
         message->type != AGENT_ARPX_CAPABILITY_WITHDRAW &&
         message->type != AGENT_ARPX_SNAPSHOT_REQUEST &&
         message->type != AGENT_ARPX_SNAPSHOT_END) ||
        message->boot_epoch != session->remote_boot_epoch) {
        return fail_protocol(session, now_ms,
                             AGENT_PEER_SESSION_PROTOCOL_ERROR);
    }
    if (session->last_sequence == UINT64_MAX ||
        message->sequence != session->last_sequence + 1U) {
        return fail_protocol(session, now_ms,
                             AGENT_PEER_SESSION_SEQUENCE_ERROR);
    }
    session->last_sequence = message->sequence;
    session->deadline_ms = saturating_add(
        now_ms,
        (uint64_t)session->config.heartbeat_miss_limit *
            session->remote_heartbeat_ms);
    if (message->type == AGENT_ARPX_HEARTBEAT) {
        session->heartbeats_accepted++;
    } else if (message->type == AGENT_ARPX_CAPABILITY_UPDATE) {
        session->updates_accepted++;
    } else if (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW) {
        session->withdrawals_accepted++;
    } else if (message->type == AGENT_ARPX_SNAPSHOT_REQUEST) {
        session->snapshot_requests_accepted++;
    } else {
        session->snapshot_ends_accepted++;
    }
    return AGENT_PEER_SESSION_OK;
}

void agent_peer_session_transport_down(
    struct agent_peer_session *session,
    uint64_t now_ms
)
{
    if (session == NULL || session->state == AGENT_PEER_SESSION_CONFIGURED ||
        session->state == AGENT_PEER_SESSION_BACKOFF) {
        return;
    }
    schedule_backoff(session, now_ms);
}

bool agent_peer_session_tick(
    struct agent_peer_session *session,
    uint64_t now_ms
)
{
    if (session == NULL) {
        return false;
    }
    if ((session->state == AGENT_PEER_SESSION_CONNECTING ||
         session->state == AGENT_PEER_SESSION_WAIT_OPEN ||
         session->state == AGENT_PEER_SESSION_ESTABLISHED) &&
        session->deadline_ms != 0U && now_ms >= session->deadline_ms) {
        session->timeouts++;
        schedule_backoff(session, now_ms);
        return true;
    }
    if (session->state == AGENT_PEER_SESSION_BACKOFF &&
        now_ms >= session->next_action_ms) {
        session->state = AGENT_PEER_SESSION_CONNECTING;
        session->deadline_ms = saturating_add(
            now_ms, session->config.open_timeout_ms);
        session->next_action_ms = 0U;
        session->reconnects++;
        return true;
    }
    return false;
}

const char *agent_peer_session_state_name(enum agent_peer_session_state state)
{
    switch (state) {
    case AGENT_PEER_SESSION_CONFIGURED:
        return "configured";
    case AGENT_PEER_SESSION_CONNECTING:
        return "connecting";
    case AGENT_PEER_SESSION_WAIT_OPEN:
        return "wait-open";
    case AGENT_PEER_SESSION_ESTABLISHED:
        return "established";
    case AGENT_PEER_SESSION_BACKOFF:
        return "backoff";
    default:
        return "unknown";
    }
}
