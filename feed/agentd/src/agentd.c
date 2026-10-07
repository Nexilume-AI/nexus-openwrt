#include "route_table.h"
#include "agent_dynamic_manifest.h"
#include "agent_local_agents.h"
#include "agent_ipc_server.h"
#include "agent_peer_transport.h"
#include "agent_peer_listener.h"
#include "agent_peer_routes.h"
#include "agent_discovery.h"
#include "agent_cross_discovery.h"
#include "agent_discovery_promotion.h"
#include "agent_auto_promotion.h"
#include "agent_card_authorization.h"
#include "agent_card_directory_trust.h"
#include "agent_relay_bootstrap.h"
#include "agent_relay_invoke.h"
#include "agent_invoke_contract.h"
#include "agent_recovery.h"
#include "agent_public_ipv6.h"
#include "peer_table.h"
#include "static_peers.h"
#include "static_policies.h"
#include "static_routes.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define AGENTD_DEFAULT_MAX_ROUTES 10000U
#define AGENTD_DEFAULT_LEASE_SECONDS 30U
#define AGENTD_MIN_LEASE_SECONDS 5U
#define AGENTD_MAX_LEASE_SECONDS 3600U
#define AGENTD_DEFAULT_PRUNE_INTERVAL_MS 1000U
#define AGENTD_MAX_LIST_LIMIT 1000U
#define AGENTD_AGENT_CAPABILITY_LIMIT 64U
#define AGENTD_DEFAULT_IPC_SOCKET "/var/run/agentd/lookup.sock"
#define AGENTD_DEFAULT_IPC_ALLOWED_UID 454U
#define AGENTD_DEFAULT_MAX_PEERS 64U
#define AGENTD_DEFAULT_ROUTER_ID "router-local"
#define AGENTD_DEFAULT_DOMAIN_ID "local.invalid"
#define AGENTD_SNAPSHOT_BATCH_SIZE 16U
#define AGENTD_SNAPSHOT_SCAN_BUDGET 128U
#define AGENTD_SNAPSHOT_PUMP_INTERVAL_MS 25U
#define AGENTD_DEFAULT_PEER_CA_FILE "/etc/agentd/tls/peer-ca.crt"
#define AGENTD_DEFAULT_PEER_CERT_FILE "/etc/agentd/tls/router.crt"
#define AGENTD_DEFAULT_PEER_KEY_FILE "/etc/agentd/tls/router.key"
#define AGENTD_DEFAULT_DISCOVERY_POLL_MS 5000U
#define AGENTD_DEFAULT_DISCOVERY_TTL_CAP_SECONDS 120U
#define AGENTD_DEFAULT_MAX_DISCOVERIES 64U
#define AGENTD_DISCOVERY_UBUS_TIMEOUT_MS 1000
#define AGENTD_DISCOVERY_SERVICE "_agent-router._tcp"
#define AGENTD_DEFAULT_CROSS_DOMAIN "remote.invalid"
#define AGENTD_DEFAULT_CROSS_RESOLVER_IPV4 "127.0.0.1"
#define AGENTD_DEFAULT_CROSS_RESOLVER_PORT 1053U
#define AGENTD_DEFAULT_CROSS_POLL_MS 60000U
#define AGENTD_DEFAULT_CROSS_TIMEOUT_MS 1500U
#define AGENTD_DEFAULT_CROSS_TTL_CAP_SECONDS 300U
#define AGENTD_DEFAULT_MAX_CROSS_DISCOVERIES 32U
#define AGENTD_DEFAULT_MAX_PROMOTIONS 16U
#define AGENTD_DEFAULT_PROMOTION_GRACE_SECONDS 30U
#define AGENTD_MAX_PROMOTION_GRACE_SECONDS 300U
#define AGENTD_AUTO_PROMOTION_MODE_LEN 16U
#define AGENTD_DEFAULT_RELAY_DIRECTORY_ENDPOINT \
    "https://directory.invalid:8443/v1/relay-assignment"
#define AGENTD_DEFAULT_RELAY_DIRECTORY_IPV4 ""
#define AGENTD_DEFAULT_RELAY_POLL_MS 60000U
#define AGENTD_DEFAULT_RELAY_TIMEOUT_MS 5000U
#define AGENTD_FORWARDING_KEY_PATH_LEN 256U
#define AGENTD_CARD_AUTH_MODE_LEN 32U
#define AGENTD_DEFAULT_MAX_PUBLIC_IPV6 256U
#define AGENTD_NETD_UBUS_OBJECT "agent.netd"
#define AGENTD_NETD_TIMEOUT_MS 1500
#define AGENTD_RELAY_CONFIG_GENERATION_LEN 65U
#define AGENTD_INTERNAL_GATEWAY_ENDPOINT_LEN 256U
#define AGENTD_MANIFEST_SNAPSHOT \
    "/var/run/agent-manifests/manifests.json"
#define AGENTD_MANIFEST_SNAPSHOT_TEMP \
    "/var/run/agent-manifests/.manifests.json.tmp"
#define AGENTD_CLOUD_TENANT_FILE \
    "/var/run/nexus-agent-cloud/tenant-id"

struct agentd_config {
    size_t max_routes;
    uint32_t default_lease_seconds;
    uint32_t min_lease_seconds;
    uint32_t max_lease_seconds;
    uint32_t prune_interval_ms;
    uid_t ipc_allowed_uid;
    char ipc_socket[AGENT_IPC_SOCKET_PATH_LEN];
    bool reflector_enabled;
    bool discovery_enabled;
    uint32_t discovery_poll_ms;
    uint32_t discovery_ttl_cap_seconds;
    size_t max_discoveries;
    char auto_promotion_mode[AGENTD_AUTO_PROMOTION_MODE_LEN];
    char auto_promotion_allowlist[AGENT_AUTO_PROMOTION_ALLOWLIST_LEN];
    uint32_t auto_promotion_grace_seconds;
    bool cross_discovery_enabled;
    char cross_discovery_domain[AGENT_CROSS_DOMAIN_ID_LEN];
    char cross_discovery_owner[AGENT_CROSS_OWNER_LEN];
    char cross_resolver_ipv4[AGENT_CROSS_IPV4_LEN];
    uint16_t cross_resolver_port;
    bool cross_resolver_detected;
    uint32_t cross_poll_ms;
    uint32_t cross_timeout_ms;
    uint32_t cross_ttl_cap_seconds;
    size_t max_cross_discoveries;
    char card_authorization_mode[AGENTD_CARD_AUTH_MODE_LEN];
    char card_authorization_allowlist[AGENT_CARD_ALLOWLIST_LEN];
    struct agent_relay_bootstrap_config relay;
    struct agent_relay_bootstrap_config open_mesh_relay;
    struct agent_peer_transport_config peer_transport;
    struct agent_peer_listener_config peer_listener;
    bool forwarding_assertion_required;
    char forwarding_public_key[AGENTD_FORWARDING_KEY_PATH_LEN];
    char forwarding_key_id[AGENT_FORWARDING_KID_LEN];
    char forwarding_issuer[AGENT_FORWARDING_ISSUER_LEN];
    char forwarding_source_router_id[AGENT_IPC_ROUTER_ID_LEN];
    char relay_config_generation[AGENTD_RELAY_CONFIG_GENERATION_LEN];
    bool relay_gateway_internal_enabled;
    char relay_gateway_internal_endpoint[
        AGENTD_INTERNAL_GATEWAY_ENDPOINT_LEN];
    char relay_gateway_token_file[AGENTD_FORWARDING_KEY_PATH_LEN];
    char public_ipv6_prefix[AGENT_PUBLIC_IPV6_TEXT_LEN + 4U];
    size_t max_public_ipv6;
    bool public_ipv6_upstream_relay;
};

struct agent_relay_plane {
    const char *name;
    struct agent_relay_bootstrap *bootstrap;
    bool assignment_active;
    struct agent_relay_assignment assignment;
};

struct snapshot_export_job {
    bool active;
    char peer_id[AGENT_PEER_ID_LEN];
    uint64_t snapshot_id;
    uint64_t route_generation;
    size_t cursor;
};

static struct route_table routes;
static struct agent_dynamic_manifest_table dynamic_manifests;
static struct agent_policy_table policies;
static struct peer_table peers;
static struct agent_discovery_table discoveries;
static struct agent_cross_table cross_discoveries;
static struct agent_discovery_promotion_manager promotions;
static struct agent_auto_promotion_policy auto_promotion_policy;
static struct agent_card_policy card_policy;
static struct agent_card_manager cards;
static struct agent_card_trust_manager card_trust;
static struct agent_recovery_state recovery_state;
static struct agentd_config config;
static struct agent_public_ipv6_pool public_ipv6_pool;
static struct ubus_context *ubus_ctx;
static struct uloop_timeout prune_timer;
static struct uloop_timeout snapshot_timer;
static struct uloop_timeout discovery_timer;
static struct uloop_timeout cross_discovery_timer;
static struct uloop_timeout cross_query_timeout;
static struct uloop_fd cross_query_fd = {.fd = -1};
static struct blob_buf response;
static size_t static_routes_loaded;
static size_t static_policies_loaded;
static size_t static_peers_loaded;
static struct agent_ipc_server ipc_server;
static struct agent_peer_transport_manager *peer_transport;
static struct agent_peer_listener *peer_listener;
static struct agent_relay_invoke_manager *relay_invoke;
static struct agent_peer_route_manager peer_routes;
static struct agent_relay_plane cloud_relay = {.name = "cloud"};
static struct agent_relay_plane open_mesh_relay = {.name = "open_mesh"};
static uint64_t peer_updates_sent;
static uint64_t peer_withdrawals_sent;
static uint64_t peer_send_failures;
static uint64_t snapshot_requests_sent;
static uint64_t snapshot_requests_received;
static uint64_t snapshot_ends_sent;
static uint64_t snapshot_exported_routes;
static uint64_t snapshot_export_restarts;
static uint64_t reflected_updates_sent;
static uint64_t reflected_withdrawals_sent;
static uint64_t split_horizon_suppressed;
static uint64_t next_snapshot_id;
static uint64_t dynamic_manifest_rejected;
static uint64_t dynamic_manifest_snapshot_failures;
static uint64_t dynamic_manifest_snapshot_generation = UINT64_MAX;
static uint64_t discovery_polls;
static uint64_t discovery_poll_failures;
static uint64_t auto_promotion_attempts;
static uint64_t auto_promotion_succeeded;
static uint64_t auto_promotion_failures;
static uint64_t cross_queries_sent;
static uint64_t cross_responses_secure;
static uint64_t cross_query_failures;
static uint64_t cross_query_timeouts;
static uint16_t cross_query_id;
static bool cross_query_pending;
static struct snapshot_export_job snapshot_jobs[AGENTD_DEFAULT_MAX_PEERS];

static bool copy_text(char *target, size_t capacity, const char *source);

static bool relay_invoke_ipc_received(
    void *context,
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_request *request
)
{
    (void)context;
    return agent_relay_invoke_from_ipc(relay_invoke, client, request);
}

static void relay_invoke_ipc_cancelled(
    void *context,
    struct agent_ipc_client *client
)
{
    (void)context;
    agent_relay_invoke_cancel_ipc(relay_invoke, client);
}

static bool relay_tunnel_received(
    void *context,
    const char *peer_id,
    const struct agent_relay_tunnel_message *message,
    uint64_t now_ms
)
{
    (void)context;
    return agent_relay_invoke_on_tunnel(
        relay_invoke, peer_id, message, now_ms);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }

    return ((uint64_t)now.tv_sec * 1000U) +
           ((uint64_t)now.tv_nsec / 1000000U);
}

static uint64_t unix_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
        return 0U;
    }
    return ((uint64_t)now.tv_sec * 1000U) +
           ((uint64_t)now.tv_nsec / 1000000U);
}

static void relay_peer_from_assignment(
    const struct agent_relay_assignment *assignment,
    struct agent_peer *peer
)
{
    memset(peer, 0, sizeof(*peer));
    (void)copy_text(peer->peer_id, sizeof(peer->peer_id),
                    assignment->relay_id);
    (void)copy_text(peer->router_id, sizeof(peer->router_id),
                    assignment->relay_router_id);
    (void)copy_text(peer->domain_id, sizeof(peer->domain_id),
                    assignment->relay_domain_id);
    (void)copy_text(peer->endpoint, sizeof(peer->endpoint),
                    assignment->relay_endpoint);
    (void)copy_text(peer->connect_ipv4, sizeof(peer->connect_ipv4),
                    assignment->connect_ipv4);
    (void)copy_text(peer->relay_session_ticket,
                    sizeof(peer->relay_session_ticket),
                    assignment->session_ticket);
    peer->role = AGENT_PEER_ROLE_RELAY;
    peer->open_mesh = assignment->open_mesh;
    (void)copy_text(peer->mesh_connect_host, sizeof(peer->mesh_connect_host), assignment->mesh_connect_host);
    (void)copy_text(peer->mesh_tls_sha256, sizeof(peer->mesh_tls_sha256), assignment->mesh_tls_sha256);
    peer->state = AGENT_PEER_STATE_CONFIGURED;
    peer->graceful_restart_seconds = 30U;
}

static void broadcast_reflected_withdraw(const struct agent_route *route);

static size_t remove_peer_routes_immediately(const char *peer_id)
{
    const struct agent_route *route;

    for (route = route_table_first(&routes); route != NULL;
         route = route->next) {
        if (route->source == AGENT_ROUTE_SOURCE_PEER && route->healthy &&
            strcmp(route->learned_from_peer, peer_id) == 0) {
            broadcast_reflected_withdraw(route);
        }
    }
    return agent_peer_routes_remove_peer_routes(&peer_routes, peer_id);
}

static bool reload_peer_runtime(void)
{
    char transport_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};
    char listener_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};

    return agent_peer_transport_reload(
               peer_transport, &peers, transport_error,
               sizeof(transport_error)) &&
           agent_peer_listener_reload(
               peer_listener, &peers, listener_error,
               sizeof(listener_error));
}

static bool relay_assignment_changed(
    void *context,
    const struct agent_relay_assignment *assignment
)
{
    struct agent_relay_plane *plane = context;
    struct agent_peer peer;
    const struct agent_peer *existing;
    char previous_peer_id[AGENT_PEER_ID_LEN] = {0};
    char previous_ticket[AGENT_PEER_RELAY_TICKET_LEN] = {0};
    bool relay_id_changed = false;

    if (plane == NULL) return false;
    if (plane->assignment_active) {
        (void)copy_text(previous_peer_id, sizeof(previous_peer_id),
                        plane->assignment.relay_id);
    }
    if (assignment == NULL) {
        if (!plane->assignment_active) return true;
        (void)remove_peer_routes_immediately(previous_peer_id);
        (void)peer_table_remove(&peers, previous_peer_id);
        plane->assignment_active = false;
        memset(&plane->assignment, 0, sizeof(plane->assignment));
        return reload_peer_runtime();
    }
    if (plane->assignment_active &&
        agent_relay_assignment_equal(&plane->assignment, assignment)) {
        if (!agent_relay_assignment_ticket_equal(
                &plane->assignment, assignment)) {
            existing = peer_table_find(&peers, assignment->relay_id);
            if (existing == NULL) return false;
            (void)copy_text(previous_ticket, sizeof(previous_ticket),
                            existing->relay_session_ticket);
            if (peer_table_set_relay_ticket(
                    &peers, assignment->relay_id,
                    assignment->session_ticket) != PEER_TABLE_OK) {
                return false;
            }
            if (!agent_peer_transport_set_relay_ticket(
                    peer_transport, assignment->relay_id,
                    assignment->session_ticket)) {
                (void)peer_table_set_relay_ticket(
                    &peers, assignment->relay_id, previous_ticket);
                return false;
            }
        }
        plane->assignment = *assignment;
        return true;
    }
    existing = peer_table_find(&peers, assignment->relay_id);
    if (existing != NULL &&
        (!plane->assignment_active ||
         strcmp(existing->peer_id, previous_peer_id) != 0)) {
        return false;
    }
    relay_id_changed = plane->assignment_active &&
        strcmp(previous_peer_id, assignment->relay_id) != 0;
    if (plane->assignment_active) {
        if (relay_id_changed) {
            (void)remove_peer_routes_immediately(previous_peer_id);
        } else {
            (void)agent_peer_routes_session_down(
                &peer_routes, previous_peer_id, monotonic_ms());
        }
        (void)peer_table_remove(&peers, previous_peer_id);
    }
    relay_peer_from_assignment(assignment, &peer);
    if (peer_table_upsert(&peers, &peer) != PEER_TABLE_OK) {
        plane->assignment_active = false;
        memset(&plane->assignment, 0, sizeof(plane->assignment));
        (void)reload_peer_runtime();
        return false;
    }
    plane->assignment = *assignment;
    plane->assignment_active = true;
    if (!reload_peer_runtime()) {
        (void)peer_table_remove(&peers, assignment->relay_id);
        plane->assignment_active = false;
        memset(&plane->assignment, 0, sizeof(plane->assignment));
        (void)reload_peer_runtime();
        return false;
    }
    return true;
}

static void relay_plane_report_failure(const char *peer_id)
{
    struct agent_relay_plane *planes[] = {
        &cloud_relay, &open_mesh_relay
    };
    size_t index;

    for (index = 0U; index < sizeof(planes) / sizeof(planes[0]); index++) {
        struct agent_relay_plane *plane = planes[index];

        if (plane->bootstrap != NULL && plane->assignment_active &&
            strcmp(peer_id, plane->assignment.relay_id) == 0) {
            (void)agent_relay_bootstrap_report_relay_failure(
                plane->bootstrap, peer_id);
        }
    }
}
static bool send_peer_payload(
    const char *peer_id,
    const struct agent_arpx_message *message
)
{
    return agent_peer_transport_send(peer_transport, peer_id, message) ||
           agent_peer_listener_send(peer_listener, peer_id, message);
}

static const struct agent_route *route_at_index(size_t index)
{
    const struct agent_route *route = route_table_first(&routes);

    while (route != NULL && index > 0U) {
        route = route->next;
        index--;
    }
    return route;
}

static bool split_horizon_blocks(
    const struct agent_route *route,
    const struct agent_peer *peer
)
{
    size_t i;

    if (route == NULL || peer == NULL ||
        route->source != AGENT_ROUTE_SOURCE_PEER) {
        return false;
    }
    if (strcmp(route->learned_from_peer, peer->peer_id) == 0) {
        return true;
    }
    for (i = 0U; i < route->path_length; i++) {
        if (strcmp(route->path[i], peer->router_id) == 0) {
            return true;
        }
    }
    return false;
}

static void broadcast_route_update(const struct agent_route *route)
{
    const struct agent_peer *peer;
    uint64_t now = monotonic_ms();

    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        struct agent_arpx_message message;

        if (peer->state != AGENT_PEER_STATE_ESTABLISHED) {
            continue;
        }
        if (!agent_peer_routes_export_update(
                route, config.peer_transport.local_router_id, peer,
                config.reflector_enabled, now, &message)) {
            if (split_horizon_blocks(route, peer)) {
                split_horizon_suppressed++;
            }
            continue;
        }
        if (send_peer_payload(peer->peer_id, &message)) {
            peer_updates_sent++;
            if (route->source == AGENT_ROUTE_SOURCE_PEER) {
                reflected_updates_sent++;
            }
        } else {
            peer_send_failures++;
        }
    }
}

static void broadcast_reflected_withdraw(const struct agent_route *route)
{
    const struct agent_peer *peer;
    uint64_t now = monotonic_ms();

    if (!config.reflector_enabled || route == NULL ||
        route->source != AGENT_ROUTE_SOURCE_PEER) {
        return;
    }
    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        struct agent_arpx_message probe;
        struct agent_arpx_message withdraw;

        if (peer->state != AGENT_PEER_STATE_ESTABLISHED) {
            continue;
        }
        if (!agent_peer_routes_export_update(
                route, config.peer_transport.local_router_id, peer, true,
                now, &probe)) {
            if (split_horizon_blocks(route, peer)) {
                split_horizon_suppressed++;
            }
            continue;
        }
        if (!agent_peer_routes_make_withdraw(route->route_id, &withdraw) ||
            !send_peer_payload(peer->peer_id, &withdraw)) {
            peer_send_failures++;
            continue;
        }
        peer_withdrawals_sent++;
        reflected_withdrawals_sent++;
    }
}

static void reflected_route_removed(
    void *context,
    const struct agent_route *route
)
{
    (void)context;
    broadcast_reflected_withdraw(route);
}

static struct snapshot_export_job *snapshot_job_for(
    const char *peer_id,
    bool allocate
)
{
    struct snapshot_export_job *free_job = NULL;
    size_t i;

    for (i = 0U; i < AGENTD_DEFAULT_MAX_PEERS; i++) {
        if (snapshot_jobs[i].active &&
            strcmp(snapshot_jobs[i].peer_id, peer_id) == 0) {
            return &snapshot_jobs[i];
        }
        if (!snapshot_jobs[i].active && free_job == NULL) {
            free_job = &snapshot_jobs[i];
        }
    }
    return allocate ? free_job : NULL;
}

static bool schedule_snapshot_export(
    const char *peer_id,
    uint64_t snapshot_id
)
{
    struct snapshot_export_job *job = snapshot_job_for(peer_id, true);

    if (job == NULL || snapshot_id == 0U ||
        !copy_text(job->peer_id, sizeof(job->peer_id), peer_id)) {
        return false;
    }
    job->active = true;
    job->snapshot_id = snapshot_id;
    job->route_generation = routes.generation;
    job->cursor = 0U;
    return true;
}

static void cancel_snapshot_export(const char *peer_id)
{
    struct snapshot_export_job *job = snapshot_job_for(peer_id, false);

    if (job != NULL) {
        memset(job, 0, sizeof(*job));
    }
}

static void promotion_peer_removing(void *context, const char *peer_id)
{
    (void)context;
    cancel_snapshot_export(peer_id);
    (void)agent_peer_routes_session_down(
        &peer_routes, peer_id, monotonic_ms());
}

static bool card_authorize_peer_update(
    void *context,
    const struct agent_peer *peer,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    const struct agent_discovery_promotion *promotion;
    struct agent_card_manager *manager = context;

    if ((config.peer_transport.open_mesh &&
         (peer->role != AGENT_PEER_ROLE_RELAY || peer->open_mesh)) ||
        card_policy.mode == AGENT_CARD_AUTH_OFF) {
        return true;
    }
    promotion = agent_discovery_promotion_find(
        &promotions, peer->peer_id);
    if (promotion == NULL || promotion->source != AGENT_PROMOTION_SVCB) {
        return true;
    }
    return agent_card_capability_authorized(
        manager, peer->router_id, peer->domain_id, message->intent,
        message->capability_version, message->tenant, now_ms);
}

static void card_peer_removing(void *context, const char *router_id)
{
    const struct agent_discovery_promotion *promotion;

    (void)context;
    promotion = agent_discovery_promotion_find(&promotions, router_id);
    if (promotion == NULL || promotion->source != AGENT_PROMOTION_SVCB ||
        !promotion->automatic ||
        strcmp(promotion->router_id, router_id) != 0) {
        return;
    }
    promotion_peer_removing(NULL, promotion->peer.peer_id);
    (void)agent_discovery_promotion_remove(
        &promotions, &peers, promotion->peer.peer_id);
    if (peer_transport != NULL && peer_listener != NULL) {
        (void)reload_peer_runtime();
    }
}

static size_t reconcile_promotions(uint64_t now_ms)
{
    size_t removed = agent_discovery_promotion_reconcile(
        &promotions, &peers, &discoveries, &cross_discoveries, now_ms,
        promotion_peer_removing, NULL);

    if (removed > 0U) {
        (void)reload_peer_runtime();
    }
    return removed;
}

static bool request_peer_snapshot(const char *peer_id)
{
    struct agent_arpx_message request;

    next_snapshot_id++;
    if (next_snapshot_id == 0U) {
        next_snapshot_id++;
    }
    if (!agent_peer_routes_make_snapshot_request(next_snapshot_id, &request) ||
        !send_peer_payload(peer_id, &request) ||
        !agent_peer_routes_snapshot_begin(
            &peer_routes, peer_id, next_snapshot_id)) {
        peer_send_failures++;
        return false;
    }
    snapshot_requests_sent++;
    return true;
}

static void pump_snapshot_export(struct snapshot_export_job *job)
{
    const struct agent_peer *peer;
    size_t sent = 0U;
    size_t scanned = 0U;
    uint64_t now = monotonic_ms();

    if (job == NULL || !job->active) {
        return;
    }
    peer = peer_table_find(&peers, job->peer_id);
    if (peer == NULL || peer->state != AGENT_PEER_STATE_ESTABLISHED) {
        memset(job, 0, sizeof(*job));
        return;
    }
    if (job->route_generation != routes.generation) {
        job->route_generation = routes.generation;
        job->cursor = 0U;
        snapshot_export_restarts++;
    }
    while (job->cursor < routes.count &&
           sent < AGENTD_SNAPSHOT_BATCH_SIZE &&
           scanned < AGENTD_SNAPSHOT_SCAN_BUDGET) {
        const struct agent_route *route = route_at_index(job->cursor);
        struct agent_arpx_message update;

        if (route == NULL) {
            job->cursor = 0U;
            job->route_generation = routes.generation;
            snapshot_export_restarts++;
            return;
        }
        job->cursor++;
        scanned++;
        if (!agent_peer_routes_export_update(
                route, config.peer_transport.local_router_id, peer,
                config.reflector_enabled, now, &update)) {
            continue;
        }
        if (!send_peer_payload(peer->peer_id, &update)) {
            job->cursor--;
            return;
        }
        peer_updates_sent++;
        snapshot_exported_routes++;
        if (route->source == AGENT_ROUTE_SOURCE_PEER) {
            reflected_updates_sent++;
        }
        sent++;
    }
    if (job->cursor >= routes.count &&
        job->route_generation == routes.generation) {
        struct agent_arpx_message end;

        if (!agent_peer_routes_make_snapshot_end(job->snapshot_id, &end) ||
            !send_peer_payload(peer->peer_id, &end)) {
            return;
        }
        snapshot_ends_sent++;
        memset(job, 0, sizeof(*job));
    }
}

static void pump_snapshot_exports(void)
{
    size_t i;

    for (i = 0U; i < AGENTD_DEFAULT_MAX_PEERS; i++) {
        pump_snapshot_export(&snapshot_jobs[i]);
    }
}

static void broadcast_route_withdraw(const char *route_id)
{
    const struct agent_peer *peer;
    struct agent_arpx_message message;

    if (!agent_peer_routes_make_withdraw(route_id, &message)) {
        return;
    }
    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        if (peer->state != AGENT_PEER_STATE_ESTABLISHED) {
            continue;
        }
        if (send_peer_payload(peer->peer_id, &message)) {
            peer_withdrawals_sent++;
        } else {
            peer_send_failures++;
        }
    }
}

static bool peer_message_received(
    void *context,
    const char *peer_id,
    const struct agent_arpx_message *message,
    uint64_t now_ms
)
{
    enum agent_peer_route_result result;
    struct agent_route previous;
    const struct agent_route *existing = NULL;
    bool had_previous = false;

    if (message->type == AGENT_ARPX_SNAPSHOT_REQUEST) {
        snapshot_requests_received++;
        return schedule_snapshot_export(peer_id, message->snapshot_id);
    }
    if (message->type == AGENT_ARPX_SNAPSHOT_END) {
        result = agent_peer_routes_snapshot_end(
            context, peer_id, message->snapshot_id,
            reflected_route_removed, NULL);
        return result != AGENT_PEER_ROUTE_INVALID;
    }
    if (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW) {
        existing = route_table_find(&routes, message->route_id);
        if (existing != NULL &&
            existing->source == AGENT_ROUTE_SOURCE_PEER &&
            strcmp(existing->learned_from_peer, peer_id) == 0) {
            previous = *existing;
            previous.next = NULL;
            had_previous = true;
        }
    }
    result = agent_peer_routes_on_message(context, peer_id, message, now_ms);
    if (result == AGENT_PEER_ROUTE_OK && config.reflector_enabled) {
        if (message->type == AGENT_ARPX_CAPABILITY_UPDATE) {
            broadcast_route_update(route_table_find(&routes,
                                                    message->route_id));
        } else if (had_previous) {
            broadcast_reflected_withdraw(&previous);
        }
    }

    return result != AGENT_PEER_ROUTE_INVALID;
}

static void peer_session_changed(
    void *context,
    const char *peer_id,
    bool session_up,
    uint64_t now_ms
)
{
    if (session_up) {
        agent_peer_routes_session_up(context, peer_id);
        (void)request_peer_snapshot(peer_id);
    } else {
        const struct agent_route *route;

        relay_plane_report_failure(peer_id);

        if (config.reflector_enabled) {
            for (route = route_table_first(&routes); route != NULL;
                 route = route->next) {
                if (route->source == AGENT_ROUTE_SOURCE_PEER &&
                    route->healthy &&
                    strcmp(route->learned_from_peer, peer_id) == 0) {
                    broadcast_reflected_withdraw(route);
                }
            }
        }
        cancel_snapshot_export(peer_id);
        (void)agent_peer_routes_session_down(context, peer_id, now_ms);
    }
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }

    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool write_all(int descriptor, const char *data, size_t length)
{
    size_t used = 0U;

    while (used < length) {
        ssize_t written = write(descriptor, data + used, length - used);
        if (written > 0) {
            used += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool write_dynamic_manifest_snapshot(void)
{
    struct json_object *root = NULL;
    struct json_object *items = NULL;
    const char *serialized;
    size_t index;
    int descriptor = -1;
    bool success = false;

    if (dynamic_manifest_snapshot_generation == dynamic_manifests.generation)
        return true;
    root = json_object_new_object();
    items = json_object_new_array_ext((int)dynamic_manifests.count);
    if (root == NULL || items == NULL) goto done;
    json_object_object_add(root, "generation",
                           json_object_new_uint64(
                               dynamic_manifests.generation));
    json_object_object_add(root, "route_generation",
                           json_object_new_uint64(routes.generation));
    for (index = 0U; index < dynamic_manifests.count; index++) {
        const struct agent_dynamic_manifest_entry *entry =
            &dynamic_manifests.entries[index];
        const struct agent_adapter_mapping *mapping = &entry->mapping;
        struct json_object *item = json_object_new_object();
        struct json_object *tool = json_object_new_object();

        if (item == NULL || tool == NULL) {
            if (item != NULL) json_object_put(item);
            if (tool != NULL) json_object_put(tool);
            goto done;
        }
        json_object_object_add(item, "route_id",
                               json_object_new_string(entry->route_id));
        json_object_object_add(item, "tenant",
                               json_object_new_string(entry->tenant));
        json_object_object_add(item, "origin",
                               json_object_new_string(entry->origin));
        json_object_object_add(item, "agent_name",
                               json_object_new_string(entry->agent_name));
        json_object_object_add(
            item, "manifest_digest",
            json_object_new_string(entry->manifest_digest));
        json_object_object_add(item, "publish",
                               json_object_new_boolean(entry->publish));
        if (entry->computer_present) {
            static const struct {
                uint16_t bit;
                const char *name;
            } workspace_scopes[] = {
                {AGENT_IPC_WORKSPACE_CONNECTION_LIST, "connection.list"},
                {AGENT_IPC_WORKSPACE_CONNECTION_CREATE, "connection.create"},
                {AGENT_IPC_WORKSPACE_CONNECTION_UPDATE, "connection.update"},
                {AGENT_IPC_WORKSPACE_CONNECTION_DELETE, "connection.delete"},
                {AGENT_IPC_WORKSPACE_CONNECTION_TEST, "connection.test"},
                {AGENT_IPC_WORKSPACE_CONNECTION_BIND, "connection.bind"},
                {AGENT_IPC_WORKSPACE_FILES_LIST, "files.list"},
                {AGENT_IPC_WORKSPACE_FILES_READ, "files.read"},
                {AGENT_IPC_WORKSPACE_FILES_WRITE, "files.write"},
                {AGENT_IPC_WORKSPACE_COMMAND_EXECUTE, "command.execute"},
                {AGENT_IPC_WORKSPACE_BROWSER_CONTROL, "browser.control"},
            };
            struct json_object *computer = json_object_new_object();
            struct json_object *scopes = json_object_new_array();
            const char *requirement = "disabled";
            size_t scope_index;

            if (computer == NULL || scopes == NULL) {
                if (computer != NULL) json_object_put(computer);
                if (scopes != NULL) json_object_put(scopes);
                json_object_put(item);
                json_object_put(tool);
                goto done;
            }
            if (entry->computer_requirement == AGENT_IPC_COMPUTER_OPTIONAL)
                requirement = "optional";
            else if (entry->computer_requirement == AGENT_IPC_COMPUTER_REQUIRED)
                requirement = "required";
            for (scope_index = 0U;
                 scope_index < sizeof(workspace_scopes) /
                     sizeof(workspace_scopes[0]); scope_index++) {
                if ((entry->workspace_capabilities &
                     workspace_scopes[scope_index].bit) == 0U) continue;
                json_object_array_add(
                    scopes,
                    json_object_new_string(workspace_scopes[scope_index].name));
            }
            json_object_object_add(
                computer, "requirement", json_object_new_string(requirement));
            json_object_object_add(
                computer, "workspace_capabilities", scopes);
            json_object_object_add(item, "computer", computer);
        }
        if (entry->mobile_present) {
            static const struct {
                uint16_t bit;
                const char *name;
            } mobile_scopes[] = {
                {AGENT_IPC_MOBILE_OBSERVE, "mobile.observe"},
                {AGENT_IPC_MOBILE_SCREEN_CAPTURE, "mobile.screen.capture"},
                {AGENT_IPC_MOBILE_TAP, "mobile.tap"},
                {AGENT_IPC_MOBILE_TYPE_TEXT, "mobile.type_text"},
                {AGENT_IPC_MOBILE_SWIPE, "mobile.swipe"},
                {AGENT_IPC_MOBILE_PRESS_BACK, "mobile.press_back"},
                {AGENT_IPC_MOBILE_OPEN_APP, "mobile.open_app"},
                {AGENT_IPC_MOBILE_WAIT_FOR_STATE, "mobile.wait_for_state"},
            };
            struct json_object *mobile = json_object_new_object();
            struct json_object *capabilities = json_object_new_array();
            const char *requirement = "disabled";
            size_t scope_index;

            if (mobile == NULL || capabilities == NULL) {
                if (mobile != NULL) json_object_put(mobile);
                if (capabilities != NULL) json_object_put(capabilities);
                json_object_put(item);
                json_object_put(tool);
                goto done;
            }
            if (entry->mobile_requirement == AGENT_IPC_MOBILE_OPTIONAL)
                requirement = "optional";
            else if (entry->mobile_requirement == AGENT_IPC_MOBILE_REQUIRED)
                requirement = "required";
            for (scope_index = 0U;
                 scope_index < sizeof(mobile_scopes) / sizeof(mobile_scopes[0]);
                 scope_index++) {
                if ((entry->mobile_capabilities & mobile_scopes[scope_index].bit) == 0U)
                    continue;
                json_object_array_add(
                    capabilities,
                    json_object_new_string(mobile_scopes[scope_index].name));
            }
            json_object_object_add(
                mobile, "requirement", json_object_new_string(requirement));
            json_object_object_add(
                mobile, "mobile_capabilities", capabilities);
            json_object_object_add(item, "mobile", mobile);

            if (entry->mobile_scopes != 0U) {
                struct json_object *tool_scopes = json_object_new_array();
                if (tool_scopes == NULL) {
                    json_object_put(item);
                    json_object_put(tool);
                    goto done;
                }
                for (scope_index = 0U;
                     scope_index < sizeof(mobile_scopes) / sizeof(mobile_scopes[0]);
                     scope_index++) {
                    if ((entry->mobile_scopes & mobile_scopes[scope_index].bit) == 0U)
                        continue;
                    json_object_array_add(
                        tool_scopes,
                        json_object_new_string(mobile_scopes[scope_index].name));
                }
                json_object_object_add(tool, "mobile_scopes", tool_scopes);
            }
        }
        json_object_object_add(tool, "protocol",
                               json_object_new_string("mcp"));
        json_object_object_add(tool, "authority",
                               json_object_new_string(mapping->authority));
        json_object_object_add(tool, "selector",
                               json_object_new_string(mapping->selector));
        json_object_object_add(tool, "intent",
                               json_object_new_string(mapping->intent));
        json_object_object_add(
            tool, "intent_version",
            json_object_new_uint64(mapping->intent_version));
        if (mapping->title[0] != '\0')
            json_object_object_add(
                tool, "title", json_object_new_string(mapping->title));
        if (mapping->description[0] != '\0')
            json_object_object_add(
                tool, "description",
                json_object_new_string(mapping->description));
        json_object_object_add(
            tool, "input_schema_json",
            json_object_new_string(mapping->input_schema_json));
        json_object_object_add(tool, "task",
                               json_object_new_boolean(mapping->task));
        json_object_object_add(
            tool, "continuable",
            json_object_new_boolean(mapping->continuable));
        json_object_object_add(
            tool, "recovery_protocol",
            json_object_new_int(mapping->recovery_protocol));
        json_object_object_add(tool, "demo",
                               json_object_new_boolean(mapping->demo));
        json_object_object_add(tool, "chat",
                               json_object_new_boolean(mapping->chat));
        json_object_object_add(
            tool, "interactive",
            json_object_new_boolean(mapping->interactive));
        json_object_object_add(item, "tool", tool);
        json_object_array_add(items, item);
    }
    json_object_object_add(root, "manifests", items);
    items = NULL;
    serialized = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PLAIN);
    if (serialized == NULL) goto done;
    descriptor = open(
        AGENTD_MANIFEST_SNAPSHOT_TEMP,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        0644);
    if (descriptor < 0 || fchmod(descriptor, 0644) != 0 ||
        !write_all(descriptor, serialized, strlen(serialized)) ||
        fsync(descriptor) != 0 || close(descriptor) != 0) {
        descriptor = -1;
        goto done;
    }
    descriptor = -1;
    if (rename(AGENTD_MANIFEST_SNAPSHOT_TEMP,
               AGENTD_MANIFEST_SNAPSHOT) != 0) goto done;
    dynamic_manifest_snapshot_generation = dynamic_manifests.generation;
    success = true;

done:
    if (descriptor >= 0) close(descriptor);
    if (items != NULL) json_object_put(items);
    if (root != NULL) json_object_put(root);
    if (!success) dynamic_manifest_snapshot_failures++;
    return success;
}

static bool cloud_tenant_id_valid(const char *value)
{
    size_t index;

    if (value == NULL || strlen(value) != 36U) return false;
    for (index = 0U; index < 36U; index++) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool read_cloud_tenant_id(char output[AGENT_TENANT_LEN])
{
    struct stat metadata;
    ssize_t count;
    size_t length;
    int descriptor = open(AGENTD_CLOUD_TENANT_FILE,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_uid != 0U ||
        (metadata.st_mode & 0022U) != 0U || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size >= AGENT_TENANT_LEN) goto failed;
    do {
        count = read(descriptor, output, (size_t)metadata.st_size);
    } while (count < 0 && errno == EINTR);
    if (count != metadata.st_size || close(descriptor) != 0) {
        descriptor = -1;
        goto failed;
    }
    descriptor = -1;
    length = (size_t)count;
    while (length > 0U && (output[length - 1U] == '\n' ||
                           output[length - 1U] == '\r')) length--;
    output[length] = '\0';
    return cloud_tenant_id_valid(output);

failed:
    if (descriptor >= 0) close(descriptor);
    output[0] = '\0';
    return false;
}

static bool resolve_relay_cloud_tenant_alias(
    const char *cloud_tenant,
    const char *target_agent,
    const char *intent,
    char local_tenant[AGENT_IPC_TENANT_LEN],
    void *context
)
{
    struct agent_dynamic_manifest_table *table = context;
    char enrolled_tenant[AGENT_TENANT_LEN] = {0};
    size_t index;
    bool matched = false;

    if (table == NULL || cloud_tenant == NULL || target_agent == NULL ||
        target_agent[0] == '\0' || intent == NULL || intent[0] == '\0' ||
        !read_cloud_tenant_id(enrolled_tenant) ||
        strcmp(cloud_tenant, enrolled_tenant) != 0) return false;
    for (index = 0U; index < table->count; index++) {
        const struct agent_dynamic_manifest_entry *entry =
            &table->entries[index];

        if (!entry->publish || strcmp(entry->origin, target_agent) != 0 ||
            strcmp(entry->mapping.authority, target_agent) != 0 ||
            strcmp(entry->mapping.intent, intent) != 0) continue;
        if (matched && strcmp(local_tenant, entry->tenant) != 0) return false;
        if (!matched && !copy_text(local_tenant, AGENT_IPC_TENANT_LEN,
                                   entry->tenant)) return false;
        matched = true;
    }
    return matched;
}

static bool forwarding_key_path_safe(const char *path)
{
    static const char prefix[] = "/etc/agentd/";

    return path != NULL &&
           strncmp(path, prefix, sizeof(prefix) - 1U) == 0 &&
           path[sizeof(prefix) - 1U] != '\0' &&
           strstr(path, "..") == NULL;
}

static bool blobmsg_get_compatible_u64(
    struct blob_attr *attribute,
    uint64_t *value
)
{
    if (attribute == NULL || value == NULL) {
        return false;
    }
    if (blobmsg_type(attribute) == BLOBMSG_TYPE_INT32) {
        *value = blobmsg_get_u32(attribute);
        return true;
    }
    if (blobmsg_type(attribute) == BLOBMSG_TYPE_INT64) {
        *value = blobmsg_get_u64(attribute);
        return true;
    }
    return false;
}

static bool valid_route_id(const char *route_id)
{
    size_t i;

    if (route_id == NULL || strlen(route_id) != 32U) {
        return false;
    }

    for (i = 0U; i < 32U; i++) {
        if (!isdigit((unsigned char)route_id[i]) &&
            (route_id[i] < 'a' || route_id[i] > 'f')) {
            return false;
        }
    }

    return true;
}

static bool random_route_id(char output[AGENT_ROUTE_ID_LEN])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random_bytes[16];
    size_t offset = 0U;
    ssize_t count;
    int fd;
    size_t i;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    while (offset < sizeof(random_bytes)) {
        count = read(fd, random_bytes + offset,
                     sizeof(random_bytes) - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    close(fd);

    for (i = 0U; i < sizeof(random_bytes); i++) {
        output[i * 2U] = hex[random_bytes[i] >> 4U];
        output[(i * 2U) + 1U] = hex[random_bytes[i] & 0x0fU];
    }
    output[32] = '\0';
    return true;
}

static int route_result_to_ubus(enum route_table_result result)
{
    switch (result) {
    case ROUTE_TABLE_OK:
        return UBUS_STATUS_OK;
    case ROUTE_TABLE_INVALID:
        return UBUS_STATUS_INVALID_ARGUMENT;
    case ROUTE_TABLE_FULL:
    case ROUTE_TABLE_NO_MEMORY:
        return UBUS_STATUS_UNKNOWN_ERROR;
    case ROUTE_TABLE_NOT_FOUND:
        return UBUS_STATUS_NOT_FOUND;
    default:
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
}

static uint32_t requested_lease_seconds(struct blob_attr *attribute)
{
    if (attribute == NULL) {
        return config.default_lease_seconds;
    }
    return blobmsg_get_u32(attribute);
}

static bool lease_is_valid(uint32_t lease_seconds)
{
    return lease_seconds >= config.min_lease_seconds &&
           lease_seconds <= config.max_lease_seconds;
}

static void add_route_to_blob(
    struct blob_buf *buffer,
    const struct agent_route *route,
    bool include_score
)
{
    void *path;
    size_t i;

    blobmsg_add_string(buffer, "route_id", route->route_id);
    blobmsg_add_string(buffer, "intent", route->intent);
    blobmsg_add_u32(buffer, "version", route->version);
    blobmsg_add_string(buffer, "origin", route->origin);
    blobmsg_add_string(buffer, "endpoint", route->endpoint);
    blobmsg_add_string(buffer, "tenant", route->tenant);
    blobmsg_add_string(buffer, "region", route->region);
    blobmsg_add_u64(buffer, "cost_microunits", route->cost_microunits);
    blobmsg_add_u32(buffer, "latency_ms", route->latency_ms);
    blobmsg_add_u32(buffer, "trust", route->trust_level);
    blobmsg_add_u32(buffer, "load_permille", route->load_permille);
    blobmsg_add_u32(buffer, "hop_count", route->hop_count);
    blobmsg_add_u8(buffer, "healthy", route->healthy);
    if (route->source == AGENT_ROUTE_SOURCE_STATIC) {
        blobmsg_add_string(buffer, "lease", "infinite");
    } else {
        blobmsg_add_u64(buffer, "lease_expires_monotonic_ms",
                        route->lease_expires_ms);
    }
    blobmsg_add_u64(buffer, "sequence", route->sequence);
    blobmsg_add_string(buffer, "source",
                       agent_route_source_name(route->source));
    if (route->source == AGENT_ROUTE_SOURCE_PEER) {
        blobmsg_add_string(buffer, "learned_from_peer",
                           route->learned_from_peer);
        blobmsg_add_u64(buffer, "snapshot_id", route->snapshot_id);
        path = blobmsg_open_array(buffer, "path_vector");
        for (i = 0U; i < route->path_length; i++) {
            blobmsg_add_string(buffer, NULL, route->path[i]);
        }
        blobmsg_close_array(buffer, path);
    }
    if (include_score) {
        blobmsg_add_u64(buffer, "score", agent_route_score(route));
    }
}

static void add_peer_to_blob(
    struct blob_buf *buffer,
    const struct agent_peer *peer
)
{
    const struct agent_discovery_promotion *promotion =
        agent_discovery_promotion_find(&promotions, peer->peer_id);
    uint64_t now_ms = monotonic_ms();
    struct agent_peer_transport_status transport_status;
    struct agent_peer_listener_status listener_status;
    bool has_transport_status = agent_peer_transport_get_status(
        peer_transport, peer->peer_id, &transport_status);
    bool has_listener_status = agent_peer_listener_get_status(
        peer_listener, peer->peer_id, &listener_status);
    bool use_listener = has_listener_status &&
        (listener_status.eligible || listener_status.session_up);

    blobmsg_add_string(buffer, "peer_id", peer->peer_id);
    blobmsg_add_string(buffer, "router_id", peer->router_id);
    blobmsg_add_string(buffer, "domain_id", peer->domain_id);
    blobmsg_add_string(buffer, "endpoint", peer->endpoint);
    if (peer->connect_ipv4[0] != '\0') {
        blobmsg_add_string(buffer, "connect_ipv4", peer->connect_ipv4);
    }
    blobmsg_add_string(buffer, "role", agent_peer_role_name(peer->role));
    blobmsg_add_u8(buffer, "directory_managed",
                   peer->role == AGENT_PEER_ROLE_RELAY);
    blobmsg_add_u8(buffer, "discovery_managed", promotion != NULL);
    if (promotion != NULL) {
        blobmsg_add_u8(buffer, "promotion_automatic",
                       promotion->automatic);
        blobmsg_add_string(buffer, "promotion_source",
                           agent_promotion_source_name(promotion->source));
        blobmsg_add_u64(buffer, "candidate_generation",
                        promotion->candidate_generation);
        blobmsg_add_u64(buffer, "promotion_remaining_ms",
                        promotion->expires_at_ms > now_ms
                            ? promotion->expires_at_ms - now_ms : 0U);
    }
    blobmsg_add_string(buffer, "state", agent_peer_state_name(peer->state));
    blobmsg_add_string(buffer, "protocol", "ARPX/1");
    blobmsg_add_u8(buffer, "wire_protocol_implemented", true);
    blobmsg_add_u8(buffer, "session_engine_implemented", true);
    blobmsg_add_u8(buffer, "session_up",
                   (has_transport_status && transport_status.session_up) ||
                   (has_listener_status && listener_status.session_up));
    blobmsg_add_u8(buffer, "transport_implemented", true);
    blobmsg_add_u8(buffer, "transport_eligible",
                   (has_transport_status && transport_status.eligible) ||
                   (has_listener_status && listener_status.eligible));
    blobmsg_add_string(buffer, "transport_phase",
                       use_listener ? listener_status.phase :
                       has_transport_status ? transport_status.phase :
                                              "unavailable");
    blobmsg_add_string(buffer, "transport_direction",
                       use_listener ? listener_status.direction :
                       has_transport_status ? transport_status.direction :
                                              "unavailable");
    blobmsg_add_u32(buffer, "graceful_restart_seconds",
                    peer->graceful_restart_seconds);
    blobmsg_add_u64(buffer, "boot_epoch", peer->boot_epoch);
    blobmsg_add_u64(buffer, "last_sequence", peer->last_sequence);
    blobmsg_add_u64(buffer, "learned_routes",
                    (uint64_t)peer->learned_routes);
    blobmsg_add_u8(buffer, "snapshot_receiving",
                   peer->snapshot_receiving);
    blobmsg_add_u64(buffer, "snapshot_id", peer->snapshot_id);
    blobmsg_add_u64(buffer, "snapshots_completed",
                    peer->snapshots_completed);
    if (use_listener) {
        blobmsg_add_u64(buffer, "local_sequence",
                        listener_status.local_sequence);
        blobmsg_add_u64(buffer, "tcp_connects", 0U);
        blobmsg_add_u64(buffer, "inbound_accepts",
                        listener_status.accepts);
        blobmsg_add_u64(buffer, "tls_handshakes",
                        listener_status.tls_handshakes);
        blobmsg_add_u64(buffer, "h2_sessions",
                        listener_status.h2_sessions);
        blobmsg_add_u64(buffer, "messages_sent",
                        listener_status.messages_sent);
        blobmsg_add_u64(buffer, "messages_received",
                        listener_status.messages_received);
        blobmsg_add_u64(buffer, "protocol_errors",
                        listener_status.protocol_errors);
        blobmsg_add_u64(buffer, "reconnects", listener_status.reconnects);
        blobmsg_add_u8(buffer, "invoke_tunnel_enabled",
                       listener_status.invoke_tunnel_enabled);
        blobmsg_add_u8(buffer, "invoke_tunnel_up",
                       listener_status.invoke_tunnel_up);
        blobmsg_add_u64(buffer, "invoke_tunnel_streams",
                       (uint64_t)listener_status.invoke_tunnel_streams);
        blobmsg_add_u64(buffer, "invoke_tunnel_frames_sent",
                       listener_status.invoke_tunnel_frames_sent);
        blobmsg_add_u64(buffer, "invoke_tunnel_frames_received",
                       listener_status.invoke_tunnel_frames_received);
        blobmsg_add_u64(buffer, "invoke_tunnel_protocol_errors",
                       listener_status.invoke_tunnel_protocol_errors);
        if (listener_status.last_error[0] != '\0') {
            blobmsg_add_string(buffer, "last_transport_error",
                               listener_status.last_error);
        }
    } else if (has_transport_status) {
        blobmsg_add_u64(buffer, "local_sequence",
                        transport_status.local_sequence);
        blobmsg_add_u64(buffer, "tcp_connects",
                        transport_status.tcp_connects);
        blobmsg_add_u64(buffer, "inbound_accepts", 0U);
        blobmsg_add_u64(buffer, "tls_handshakes",
                        transport_status.tls_handshakes);
        blobmsg_add_u64(buffer, "h2_sessions", transport_status.h2_sessions);
        blobmsg_add_u64(buffer, "messages_sent",
                        transport_status.messages_sent);
        blobmsg_add_u64(buffer, "messages_received",
                        transport_status.messages_received);
        blobmsg_add_u64(buffer, "protocol_errors",
                        transport_status.protocol_errors);
        blobmsg_add_u64(buffer, "reconnects", transport_status.reconnects);
        blobmsg_add_u8(buffer, "relay_tunnel_enabled",
                       transport_status.relay_tunnel_enabled);
        blobmsg_add_u8(buffer, "relay_tunnel_up",
                       transport_status.relay_tunnel_up);
        blobmsg_add_u64(buffer, "relay_tunnel_streams",
                        (uint64_t)transport_status.relay_tunnel_streams);
        blobmsg_add_u64(buffer, "relay_tunnel_frames_sent",
                        transport_status.relay_tunnel_frames_sent);
        blobmsg_add_u64(buffer, "relay_tunnel_frames_received",
                        transport_status.relay_tunnel_frames_received);
        blobmsg_add_u64(buffer, "relay_tunnel_protocol_errors",
                        transport_status.relay_tunnel_protocol_errors);
        blobmsg_add_u8(buffer, "invoke_tunnel_enabled",
                       transport_status.relay_tunnel_enabled);
        blobmsg_add_u8(buffer, "invoke_tunnel_up",
                       transport_status.relay_tunnel_up);
        blobmsg_add_u64(buffer, "invoke_tunnel_streams",
                        (uint64_t)transport_status.relay_tunnel_streams);
        blobmsg_add_u64(buffer, "invoke_tunnel_frames_sent",
                        transport_status.relay_tunnel_frames_sent);
        blobmsg_add_u64(buffer, "invoke_tunnel_frames_received",
                        transport_status.relay_tunnel_frames_received);
        blobmsg_add_u64(buffer, "invoke_tunnel_protocol_errors",
                        transport_status.relay_tunnel_protocol_errors);
        if (transport_status.last_error[0] != '\0') {
            blobmsg_add_string(buffer, "last_transport_error",
                               transport_status.last_error);
        }
    }
}

enum register_field {
    REGISTER_ROUTE_ID,
    REGISTER_INTENT,
    REGISTER_VERSION,
    REGISTER_ORIGIN,
    REGISTER_ENDPOINT,
    REGISTER_TENANT,
    REGISTER_REGION,
    REGISTER_COST,
    REGISTER_LATENCY,
    REGISTER_TRUST,
    REGISTER_LOAD,
    REGISTER_HOPS,
    REGISTER_LEASE,
    __REGISTER_MAX
};

static const struct blobmsg_policy register_policy[__REGISTER_MAX] = {
    [REGISTER_ROUTE_ID] = { .name = "route_id", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_INTENT] = { .name = "intent", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_VERSION] = { .name = "version", .type = BLOBMSG_TYPE_INT32 },
    [REGISTER_ORIGIN] = { .name = "origin", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_ENDPOINT] = { .name = "endpoint", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_TENANT] = { .name = "tenant", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_REGION] = { .name = "region", .type = BLOBMSG_TYPE_STRING },
    [REGISTER_COST] = {
        .name = "cost_microunits", .type = BLOBMSG_TYPE_UNSPEC
    },
    [REGISTER_LATENCY] = {
        .name = "latency_ms", .type = BLOBMSG_TYPE_INT32
    },
    [REGISTER_TRUST] = { .name = "trust", .type = BLOBMSG_TYPE_INT32 },
    [REGISTER_LOAD] = {
        .name = "load_permille", .type = BLOBMSG_TYPE_INT32
    },
    [REGISTER_HOPS] = { .name = "hop_count", .type = BLOBMSG_TYPE_INT32 },
    [REGISTER_LEASE] = {
        .name = "lease_seconds", .type = BLOBMSG_TYPE_INT32
    }
};

static int agent_register(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__REGISTER_MAX];
    struct agent_route route;
    enum route_table_result result;
    const struct agent_route *existing;
    uint32_t lease_seconds;
    uint32_t value;
    const char *text;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(register_policy, __REGISTER_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));

    if (attributes[REGISTER_INTENT] == NULL ||
        attributes[REGISTER_VERSION] == NULL ||
        attributes[REGISTER_ORIGIN] == NULL ||
        attributes[REGISTER_ENDPOINT] == NULL ||
        attributes[REGISTER_TENANT] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    memset(&route, 0, sizeof(route));
    if (attributes[REGISTER_ROUTE_ID] != NULL) {
        text = blobmsg_get_string(attributes[REGISTER_ROUTE_ID]);
        if (!valid_route_id(text) ||
            !copy_text(route.route_id, sizeof(route.route_id), text)) {
            return UBUS_STATUS_INVALID_ARGUMENT;
        }
    } else if (!random_route_id(route.route_id)) {
        return UBUS_STATUS_UNKNOWN_ERROR;
    }

    if (!copy_text(route.intent, sizeof(route.intent),
                   blobmsg_get_string(attributes[REGISTER_INTENT])) ||
        !copy_text(route.origin, sizeof(route.origin),
                   blobmsg_get_string(attributes[REGISTER_ORIGIN])) ||
        !copy_text(route.endpoint, sizeof(route.endpoint),
                   blobmsg_get_string(attributes[REGISTER_ENDPOINT]))) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    route.version = blobmsg_get_u32(attributes[REGISTER_VERSION]);
    if (route.version == 0U) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    text = blobmsg_get_string(attributes[REGISTER_TENANT]);
    if (!copy_text(route.tenant, sizeof(route.tenant), text)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    text = attributes[REGISTER_REGION] == NULL
        ? "local"
        : blobmsg_get_string(attributes[REGISTER_REGION]);
    if (!copy_text(route.region, sizeof(route.region), text)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    if (attributes[REGISTER_COST] == NULL) {
        route.cost_microunits = 0U;
    } else if (!blobmsg_get_compatible_u64(
                   attributes[REGISTER_COST], &route.cost_microunits)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route.latency_ms = attributes[REGISTER_LATENCY] == NULL
        ? 0U
        : blobmsg_get_u32(attributes[REGISTER_LATENCY]);

    value = attributes[REGISTER_TRUST] == NULL
        ? 50U
        : blobmsg_get_u32(attributes[REGISTER_TRUST]);
    if (value > 100U) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route.trust_level = (uint8_t)value;

    value = attributes[REGISTER_LOAD] == NULL
        ? 0U
        : blobmsg_get_u32(attributes[REGISTER_LOAD]);
    if (value > 1000U) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route.load_permille = (uint16_t)value;

    value = attributes[REGISTER_HOPS] == NULL
        ? 0U
        : blobmsg_get_u32(attributes[REGISTER_HOPS]);
    if (value > UINT8_MAX) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route.hop_count = (uint8_t)value;

    lease_seconds = requested_lease_seconds(attributes[REGISTER_LEASE]);
    if (!lease_is_valid(lease_seconds)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    route.healthy = true;
    route.lease_expires_ms = monotonic_ms() +
                             ((uint64_t)lease_seconds * 1000U);
    route.sequence = 1U;
    route.source = AGENT_ROUTE_SOURCE_LOCAL;
    existing = route_table_find(&routes, route.route_id);
    if (existing != NULL &&
        existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        return UBUS_STATUS_PERMISSION_DENIED;
    }
    result = route_table_upsert(&routes, &route);
    if (result != ROUTE_TABLE_OK) {
        return route_result_to_ubus(result);
    }
    (void)agent_dynamic_manifest_remove(&dynamic_manifests, route.route_id);
    (void)write_dynamic_manifest_snapshot();
    broadcast_route_update(route_table_find(&routes, route.route_id));

    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "route_id", route.route_id);
    blobmsg_add_u64(&response, "generation", routes.generation);
    blobmsg_add_u32(&response, "lease_seconds", lease_seconds);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

enum renew_field {
    RENEW_ROUTE_ID,
    RENEW_LEASE,
    RENEW_LATENCY,
    RENEW_LOAD,
    RENEW_HEALTHY,
    __RENEW_MAX
};

static const struct blobmsg_policy renew_policy[__RENEW_MAX] = {
    [RENEW_ROUTE_ID] = { .name = "route_id", .type = BLOBMSG_TYPE_STRING },
    [RENEW_LEASE] = {
        .name = "lease_seconds", .type = BLOBMSG_TYPE_INT32
    },
    [RENEW_LATENCY] = {
        .name = "latency_ms", .type = BLOBMSG_TYPE_INT32
    },
    [RENEW_LOAD] = {
        .name = "load_permille", .type = BLOBMSG_TYPE_INT32
    },
    [RENEW_HEALTHY] = { .name = "healthy", .type = BLOBMSG_TYPE_BOOL }
};

static int agent_renew(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__RENEW_MAX];
    struct route_renewal renewal;
    enum route_table_result result;
    const struct agent_route *existing;
    uint32_t lease_seconds;
    uint32_t load;
    const char *route_id;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(renew_policy, __RENEW_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));

    if (attributes[RENEW_ROUTE_ID] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route_id = blobmsg_get_string(attributes[RENEW_ROUTE_ID]);
    if (!valid_route_id(route_id)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    existing = route_table_find(&routes, route_id);
    if (existing == NULL) {
        return UBUS_STATUS_NOT_FOUND;
    }
    if (existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        return UBUS_STATUS_PERMISSION_DENIED;
    }

    lease_seconds = requested_lease_seconds(attributes[RENEW_LEASE]);
    if (!lease_is_valid(lease_seconds)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    memset(&renewal, 0, sizeof(renewal));
    renewal.lease_expires_ms = monotonic_ms() +
                               ((uint64_t)lease_seconds * 1000U);
    if (attributes[RENEW_LATENCY] != NULL) {
        renewal.update_latency = true;
        renewal.latency_ms = blobmsg_get_u32(attributes[RENEW_LATENCY]);
    }
    if (attributes[RENEW_LOAD] != NULL) {
        load = blobmsg_get_u32(attributes[RENEW_LOAD]);
        if (load > 1000U) {
            return UBUS_STATUS_INVALID_ARGUMENT;
        }
        renewal.update_load = true;
        renewal.load_permille = (uint16_t)load;
    }
    if (attributes[RENEW_HEALTHY] != NULL) {
        renewal.update_health = true;
        renewal.healthy = blobmsg_get_bool(attributes[RENEW_HEALTHY]);
    }

    result = route_table_renew(&routes, route_id, &renewal);
    if (result != ROUTE_TABLE_OK) {
        return route_result_to_ubus(result);
    }
    existing = route_table_find(&routes, route_id);
    if (existing != NULL && existing->healthy) {
        broadcast_route_update(existing);
    } else {
        broadcast_route_withdraw(route_id);
    }

    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "route_id", route_id);
    blobmsg_add_u64(&response, "generation", routes.generation);
    blobmsg_add_u32(&response, "lease_seconds", lease_seconds);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

enum id_field {
    ID_ROUTE_ID,
    __ID_MAX
};

static const struct blobmsg_policy id_policy[__ID_MAX] = {
    [ID_ROUTE_ID] = { .name = "route_id", .type = BLOBMSG_TYPE_STRING }
};

static int agent_unregister(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__ID_MAX];
    enum route_table_result result;
    const struct agent_route *existing;
    const char *route_id;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(id_policy, __ID_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));

    if (attributes[ID_ROUTE_ID] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    route_id = blobmsg_get_string(attributes[ID_ROUTE_ID]);
    if (!valid_route_id(route_id)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    existing = route_table_find(&routes, route_id);
    if (existing == NULL) {
        return UBUS_STATUS_NOT_FOUND;
    }
    if (existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        return UBUS_STATUS_PERMISSION_DENIED;
    }

    result = route_table_remove(&routes, route_id);
    if (result != ROUTE_TABLE_OK) {
        return route_result_to_ubus(result);
    }
    (void)agent_dynamic_manifest_remove(&dynamic_manifests, route_id);
    (void)write_dynamic_manifest_snapshot();
    broadcast_route_withdraw(route_id);

    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "route_id", route_id);
    blobmsg_add_u64(&response, "generation", routes.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static void ipc_registration_error(
    uint32_t *error_code,
    char *error,
    size_t error_capacity,
    uint32_t code,
    const char *message
)
{
    if (error_code != NULL) *error_code = code;
    if (error != NULL && error_capacity > 0U)
        (void)snprintf(error, error_capacity, "%s", message);
}

static bool public_ipv6_netd_apply(
    const char *method,
    const struct agent_public_ipv6_lease *lease
)
{
    struct blob_buf request = {0};
    uint32_t object_id;
    int status;

    if (ubus_ctx == NULL || method == NULL || lease == NULL) return false;
    status = ubus_lookup_id(ubus_ctx, AGENTD_NETD_UBUS_OBJECT, &object_id);
    if (status != UBUS_STATUS_OK) return false;
    blob_buf_init(&request, 0);
    blobmsg_add_string(&request, "route_id", lease->route_id);
    blobmsg_add_string(&request, "address", lease->address);
    blobmsg_add_string(&request, "origin", lease->origin);
    blobmsg_add_u64(&request, "expires_ms", lease->expires_ms);
    status = ubus_invoke(ubus_ctx, object_id, method, request.head, NULL,
                         NULL, AGENTD_NETD_TIMEOUT_MS);
    blob_buf_free(&request);
    return status == UBUS_STATUS_OK;
}

static bool public_ipv6_netd_release_cb(
    const struct agent_public_ipv6_lease *lease,
    void *context
)
{
    (void)context;
    return public_ipv6_netd_apply("release", lease);
}

static bool ipc_register_local_route(
    void *context,
    const struct agent_ipc_register_request *request,
    struct agent_ipc_lease_response *lease_response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
)
{
    struct agent_route route;
    struct agent_dynamic_manifest_entry manifest;
    const struct agent_route *existing;
    enum route_table_result result;
    uint32_t lease_seconds;
    bool had_public_ipv6 = false;
    struct agent_public_ipv6_lease public_lease;
    struct agent_public_ipv6_lease old_public_lease;
    const struct agent_public_ipv6_lease *active_public_lease;
    enum agent_public_ipv6_result address_result;
    enum agent_dynamic_manifest_result manifest_result =
        AGENT_DYNAMIC_MANIFEST_OK;
    bool manifest_valid = false;

    (void)context;
    if (request == NULL || lease_response == NULL) return false;
    lease_seconds = request->lease_seconds == 0U
        ? config.default_lease_seconds : request->lease_seconds;
    if (!lease_is_valid(lease_seconds)) {
        ipc_registration_error(error_code, error, error_capacity, 400U,
                               "lease_seconds is outside agentd bounds");
        return false;
    }
    memset(&route, 0, sizeof(route));
    if (request->route_id[0] != '\0') {
        if (!valid_route_id(request->route_id) ||
            !copy_text(route.route_id, sizeof(route.route_id),
                       request->route_id)) {
            ipc_registration_error(error_code, error, error_capacity, 400U,
                                   "route_id must be 32 lowercase hex digits");
            return false;
        }
    } else if (!random_route_id(route.route_id)) {
        ipc_registration_error(error_code, error, error_capacity, 500U,
                               "failed to generate route_id");
        return false;
    }
    if (!copy_text(route.intent, sizeof(route.intent), request->intent) ||
        !copy_text(route.origin, sizeof(route.origin), request->origin) ||
        !copy_text(route.endpoint, sizeof(route.endpoint), request->endpoint) ||
        !copy_text(route.tenant, sizeof(route.tenant), request->tenant) ||
        !copy_text(route.region, sizeof(route.region),
                   request->region[0] != '\0' ? request->region : "local")) {
        ipc_registration_error(error_code, error, error_capacity, 400U,
                               "registration field exceeds route bounds");
        return false;
    }
    route.version = request->version;
    route.cost_microunits = request->cost_microunits;
    route.latency_ms = request->latency_ms;
    route.trust_level = request->trust_level;
    route.load_permille = request->load_permille;
    route.hop_count = request->hop_count;
    route.healthy = true;
    route.lease_expires_ms = monotonic_ms() +
                             ((uint64_t)lease_seconds * 1000U);
    route.sequence = 1U;
    route.source = AGENT_ROUTE_SOURCE_LOCAL;
    memset(&manifest, 0, sizeof(manifest));
    if (request->cloud.present && request->cloud.tool_present) {
        int title_written;
        int description_written;

        manifest.publish = request->cloud.publish;
        manifest.computer_present = request->cloud.computer_present;
        manifest.computer_requirement = request->cloud.computer_requirement;
        manifest.workspace_capabilities =
            request->cloud.workspace_capabilities;
        manifest.mobile_present = request->cloud.mobile_present;
        manifest.mobile_requirement = request->cloud.mobile_requirement;
        manifest.mobile_capabilities = request->cloud.mobile_capabilities;
        manifest.mobile_scopes = request->cloud.mobile_scopes;
        manifest.mapping.enabled = true;
        manifest.mapping.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
        manifest.mapping.intent_version = request->version;
        manifest.mapping.task = request->cloud.task;
        manifest.mapping.resumable = request->cloud.resumable;
        manifest.mapping.continuable = request->cloud.continuable;
        manifest.mapping.recovery_protocol = request->cloud.recovery_protocol;
        manifest.mapping.demo = request->cloud.demo;
        manifest.mapping.chat = request->cloud.chat;
        manifest.mapping.interactive = request->cloud.interactive;
        title_written = snprintf(
            manifest.mapping.title, sizeof(manifest.mapping.title), "%s",
            request->cloud.tool_title);
        description_written = snprintf(
            manifest.mapping.description,
            sizeof(manifest.mapping.description), "%s",
            request->cloud.tool_description);
        manifest_valid =
            copy_text(manifest.route_id, sizeof(manifest.route_id),
                      route.route_id) &&
            copy_text(manifest.tenant, sizeof(manifest.tenant),
                      route.tenant) &&
            copy_text(manifest.origin, sizeof(manifest.origin),
                      route.origin) &&
            copy_text(manifest.agent_name, sizeof(manifest.agent_name),
                      request->cloud.agent_name) &&
            copy_text(manifest.manifest_digest,
                      sizeof(manifest.manifest_digest),
                      request->cloud.manifest_digest) &&
            copy_text(manifest.mapping.authority,
                      sizeof(manifest.mapping.authority), route.origin) &&
            copy_text(manifest.mapping.selector,
                      sizeof(manifest.mapping.selector),
                      request->cloud.tool_name) &&
            copy_text(manifest.mapping.intent,
                      sizeof(manifest.mapping.intent), route.intent) &&
            copy_text(manifest.mapping.input_schema_json,
                      sizeof(manifest.mapping.input_schema_json),
                      request->cloud.tool_input_schema) &&
            title_written >= 0 &&
            (size_t)title_written < sizeof(manifest.mapping.title) &&
            description_written >= 0 &&
            (size_t)description_written <
                sizeof(manifest.mapping.description);
        if (manifest_valid) {
            manifest_result = agent_dynamic_manifest_validate_upsert(
                &dynamic_manifests, &manifest);
            manifest_valid = manifest_result == AGENT_DYNAMIC_MANIFEST_OK;
        } else {
            manifest_result = AGENT_DYNAMIC_MANIFEST_INVALID;
        }
    }
    existing = route_table_find(&routes, route.route_id);
    if (existing != NULL && existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        ipc_registration_error(error_code, error, error_capacity, 403U,
                               "route_id belongs to a non-local route");
        return false;
    }
    memset(&public_lease, 0, sizeof(public_lease));
    memset(&old_public_lease, 0, sizeof(old_public_lease));
    active_public_lease = agent_public_ipv6_find(
        &public_ipv6_pool, route.route_id);
    had_public_ipv6 = active_public_lease != NULL;
    if (active_public_lease != NULL) old_public_lease = *active_public_lease;
    if (request->request_public_ipv6 || had_public_ipv6) {
        if (!public_ipv6_pool.configured) {
            ipc_registration_error(error_code, error, error_capacity, 409U,
                                   "router public IPv6 pool is disabled");
            return false;
        }
        address_result = agent_public_ipv6_allocate(
            &public_ipv6_pool, route.route_id, route.origin,
            route.lease_expires_ms, &public_lease);
        if (address_result != AGENT_PUBLIC_IPV6_OK) {
            ipc_registration_error(
                error_code, error, error_capacity,
                address_result == AGENT_PUBLIC_IPV6_FULL ? 503U : 409U,
                address_result == AGENT_PUBLIC_IPV6_FULL ?
                    "router public IPv6 pool is full" :
                    "public IPv6 lease conflicts with this route");
            return false;
        }
        if (!public_ipv6_netd_apply("allocate", &public_lease)) {
            if (had_public_ipv6) {
                (void)agent_public_ipv6_renew(
                    &public_ipv6_pool, old_public_lease.route_id,
                    old_public_lease.origin, old_public_lease.expires_ms,
                    NULL);
            } else {
                (void)agent_public_ipv6_release(
                    &public_ipv6_pool, route.route_id, NULL);
            }
            ipc_registration_error(error_code, error, error_capacity, 503U,
                                   "agent-netd could not apply public IPv6");
            return false;
        }
    }
    result = route_table_upsert(&routes, &route);
    if (result != ROUTE_TABLE_OK) {
        if (!had_public_ipv6 && public_lease.address[0] != '\0') {
            (void)public_ipv6_netd_apply("release", &public_lease);
            (void)agent_public_ipv6_release(
                &public_ipv6_pool, route.route_id, NULL);
        } else if (had_public_ipv6) {
            (void)agent_public_ipv6_renew(
                &public_ipv6_pool, old_public_lease.route_id,
                old_public_lease.origin, old_public_lease.expires_ms, NULL);
            (void)public_ipv6_netd_apply("renew", &old_public_lease);
        }
        ipc_registration_error(
            error_code, error, error_capacity,
            result == ROUTE_TABLE_INVALID ? 400U : 503U,
            result == ROUTE_TABLE_INVALID ?
                "route registration is invalid" : "route table is full");
        return false;
    }
    if (manifest_valid) {
        manifest_result = agent_dynamic_manifest_upsert(
            &dynamic_manifests, &manifest);
        if (manifest_result != AGENT_DYNAMIC_MANIFEST_OK) {
            dynamic_manifest_rejected++;
            fprintf(stderr,
                    "agentd: dynamic manifest rejected origin=%s reason=%s\n",
                    route.origin,
                    agent_dynamic_manifest_result_name(manifest_result));
        }
    } else {
        (void)agent_dynamic_manifest_remove(
            &dynamic_manifests, route.route_id);
        if (request->cloud.present && request->cloud.tool_present) {
            dynamic_manifest_rejected++;
            fprintf(stderr,
                    "agentd: dynamic manifest rejected origin=%s reason=%s\n",
                    route.origin,
                    agent_dynamic_manifest_result_name(manifest_result));
        }
    }
    (void)write_dynamic_manifest_snapshot();
    broadcast_route_update(route_table_find(&routes, route.route_id));
    memset(lease_response, 0, sizeof(*lease_response));
    lease_response->request_id = request->request_id;
    lease_response->generation = routes.generation;
    lease_response->lease_seconds = lease_seconds;
    (void)copy_text(lease_response->route_id,
                    sizeof(lease_response->route_id), route.route_id);
    if (public_lease.address[0] != '\0')
        (void)copy_text(lease_response->public_ipv6,
                        sizeof(lease_response->public_ipv6),
                        public_lease.address);
    return true;
}

static bool ipc_renew_local_route(
    void *context,
    const struct agent_ipc_renew_request *request,
    struct agent_ipc_lease_response *lease_response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
)
{
    const struct agent_route *existing;
    struct route_renewal renewal;
    enum route_table_result result;
    uint32_t lease_seconds;
    struct agent_public_ipv6_lease public_lease;
    struct agent_public_ipv6_lease old_public_lease;
    const struct agent_public_ipv6_lease *active_public_lease;

    (void)context;
    if (request == NULL || lease_response == NULL ||
        !valid_route_id(request->route_id)) {
        ipc_registration_error(error_code, error, error_capacity, 400U,
                               "invalid route_id");
        return false;
    }
    existing = route_table_find(&routes, request->route_id);
    if (existing == NULL) {
        ipc_registration_error(error_code, error, error_capacity, 404U,
                               "local route was not found");
        return false;
    }
    if (existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        ipc_registration_error(error_code, error, error_capacity, 403U,
                               "only local routes can be renewed");
        return false;
    }
    if (request->requester_agent[0] != '\0' &&
        strcmp(existing->origin, request->requester_agent) != 0) {
        ipc_registration_error(error_code, error, error_capacity, 403U,
                               "route lease belongs to another Agent");
        return false;
    }
    lease_seconds = request->lease_seconds == 0U
        ? config.default_lease_seconds : request->lease_seconds;
    if (!lease_is_valid(lease_seconds)) {
        ipc_registration_error(error_code, error, error_capacity, 400U,
                               "lease_seconds is outside agentd bounds");
        return false;
    }
    memset(&renewal, 0, sizeof(renewal));
    renewal.lease_expires_ms = monotonic_ms() +
                               ((uint64_t)lease_seconds * 1000U);
    renewal.update_latency =
        (request->update_flags & AGENT_IPC_RENEW_LATENCY) != 0U;
    renewal.latency_ms = request->latency_ms;
    renewal.update_load =
        (request->update_flags & AGENT_IPC_RENEW_LOAD) != 0U;
    renewal.load_permille = request->load_permille;
    renewal.update_health =
        (request->update_flags & AGENT_IPC_RENEW_HEALTH) != 0U;
    renewal.healthy = request->healthy;
    memset(&public_lease, 0, sizeof(public_lease));
    memset(&old_public_lease, 0, sizeof(old_public_lease));
    active_public_lease = agent_public_ipv6_find(
        &public_ipv6_pool, request->route_id);
    if (active_public_lease != NULL) old_public_lease = *active_public_lease;
    if (active_public_lease != NULL &&
        agent_public_ipv6_renew(
            &public_ipv6_pool, request->route_id, existing->origin,
            renewal.lease_expires_ms, &public_lease) != AGENT_PUBLIC_IPV6_OK) {
        ipc_registration_error(error_code, error, error_capacity, 503U,
                               "failed to renew public IPv6 lease");
        return false;
    }
    if (public_lease.address[0] != '\0' &&
        !public_ipv6_netd_apply("renew", &public_lease)) {
        (void)agent_public_ipv6_renew(
            &public_ipv6_pool, old_public_lease.route_id,
            old_public_lease.origin, old_public_lease.expires_ms, NULL);
        ipc_registration_error(error_code, error, error_capacity, 503U,
                               "agent-netd could not renew public IPv6");
        return false;
    }
    result = route_table_renew(&routes, request->route_id, &renewal);
    if (result != ROUTE_TABLE_OK) {
        if (old_public_lease.address[0] != '\0') {
            (void)agent_public_ipv6_renew(
                &public_ipv6_pool, old_public_lease.route_id,
                old_public_lease.origin, old_public_lease.expires_ms, NULL);
            (void)public_ipv6_netd_apply("renew", &old_public_lease);
        }
        ipc_registration_error(error_code, error, error_capacity, 503U,
                               "failed to renew local route");
        return false;
    }
    existing = route_table_find(&routes, request->route_id);
    if (existing != NULL && existing->healthy)
        broadcast_route_update(existing);
    else
        broadcast_route_withdraw(request->route_id);
    memset(lease_response, 0, sizeof(*lease_response));
    lease_response->request_id = request->request_id;
    lease_response->generation = routes.generation;
    lease_response->lease_seconds = lease_seconds;
    (void)copy_text(lease_response->route_id,
                    sizeof(lease_response->route_id), request->route_id);
    if (public_lease.address[0] != '\0')
        (void)copy_text(lease_response->public_ipv6,
                        sizeof(lease_response->public_ipv6),
                        public_lease.address);
    return true;
}

static bool ipc_unregister_local_route(
    void *context,
    const struct agent_ipc_unregister_request *request,
    struct agent_ipc_lease_response *lease_response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
)
{
    const struct agent_route *existing;
    enum route_table_result result;
    const struct agent_public_ipv6_lease *public_lease;
    struct agent_public_ipv6_lease released_lease;

    (void)context;
    if (request == NULL || lease_response == NULL ||
        !valid_route_id(request->route_id)) {
        ipc_registration_error(error_code, error, error_capacity, 400U,
                               "invalid route_id");
        return false;
    }
    existing = route_table_find(&routes, request->route_id);
    if (existing == NULL) {
        ipc_registration_error(error_code, error, error_capacity, 404U,
                               "local route was not found");
        return false;
    }
    if (existing->source != AGENT_ROUTE_SOURCE_LOCAL) {
        ipc_registration_error(error_code, error, error_capacity, 403U,
                               "only local routes can be removed");
        return false;
    }
    if (request->requester_agent[0] != '\0' &&
        strcmp(existing->origin, request->requester_agent) != 0) {
        ipc_registration_error(error_code, error, error_capacity, 403U,
                               "route lease belongs to another Agent");
        return false;
    }
    public_lease = agent_public_ipv6_find(&public_ipv6_pool,
                                          request->route_id);
    memset(&released_lease, 0, sizeof(released_lease));
    if (public_lease != NULL) {
        released_lease = *public_lease;
        if (!public_ipv6_netd_apply("release", public_lease)) {
            ipc_registration_error(error_code, error, error_capacity, 503U,
                                   "agent-netd could not release public IPv6");
            return false;
        }
        (void)agent_public_ipv6_release(&public_ipv6_pool,
                                        request->route_id, NULL);
    }
    result = route_table_remove(&routes, request->route_id);
    if (result != ROUTE_TABLE_OK) {
        if (released_lease.address[0] != '\0') {
            (void)agent_public_ipv6_allocate(
                &public_ipv6_pool, released_lease.route_id,
                released_lease.origin, released_lease.expires_ms, NULL);
            (void)public_ipv6_netd_apply("allocate", &released_lease);
        }
        ipc_registration_error(error_code, error, error_capacity, 503U,
                               "failed to remove local route");
        return false;
    }
    (void)agent_dynamic_manifest_remove(
        &dynamic_manifests, request->route_id);
    (void)write_dynamic_manifest_snapshot();
    broadcast_route_withdraw(request->route_id);
    memset(lease_response, 0, sizeof(*lease_response));
    lease_response->request_id = request->request_id;
    lease_response->generation = routes.generation;
    lease_response->removed = true;
    (void)copy_text(lease_response->route_id,
                    sizeof(lease_response->route_id), request->route_id);
    return true;
}

enum lookup_field {
    LOOKUP_INTENT,
    LOOKUP_VERSION,
    LOOKUP_TENANT,
    LOOKUP_REGION,
    LOOKUP_MAX_COST,
    LOOKUP_MAX_LATENCY,
    LOOKUP_MIN_TRUST,
    LOOKUP_SOURCE_AGENT,
    LOOKUP_TARGET_AGENT,
    __LOOKUP_MAX
};

static const struct blobmsg_policy lookup_policy[__LOOKUP_MAX] = {
    [LOOKUP_INTENT] = { .name = "intent", .type = BLOBMSG_TYPE_STRING },
    [LOOKUP_VERSION] = { .name = "version", .type = BLOBMSG_TYPE_INT32 },
    [LOOKUP_TENANT] = { .name = "tenant", .type = BLOBMSG_TYPE_STRING },
    [LOOKUP_REGION] = { .name = "region", .type = BLOBMSG_TYPE_STRING },
    [LOOKUP_MAX_COST] = {
        .name = "max_cost_microunits", .type = BLOBMSG_TYPE_UNSPEC
    },
    [LOOKUP_MAX_LATENCY] = {
        .name = "max_latency_ms", .type = BLOBMSG_TYPE_INT32
    },
    [LOOKUP_MIN_TRUST] = {
        .name = "min_trust", .type = BLOBMSG_TYPE_INT32
    },
    [LOOKUP_SOURCE_AGENT] = {
        .name = "source_agent", .type = BLOBMSG_TYPE_STRING
    },
    [LOOKUP_TARGET_AGENT] = {
        .name = "target_agent", .type = BLOBMSG_TYPE_STRING
    }
};

static int do_lookup(
    struct ubus_context *ctx,
    struct ubus_request_data *request,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LOOKUP_MAX];
    struct route_query query;
    struct route_selection selection;
    struct route_diagnostics diagnostics;
    void *route_blob;
    void *diagnostics_blob;
    uint32_t trust;
    bool found;

    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(lookup_policy, __LOOKUP_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));

    if (attributes[LOOKUP_INTENT] == NULL ||
        attributes[LOOKUP_VERSION] == NULL ||
        attributes[LOOKUP_TENANT] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    memset(&query, 0, sizeof(query));
    if (!copy_text(query.intent, sizeof(query.intent),
                   blobmsg_get_string(attributes[LOOKUP_INTENT])) ||
        !copy_text(query.tenant, sizeof(query.tenant),
                   blobmsg_get_string(attributes[LOOKUP_TENANT]))) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    query.version = blobmsg_get_u32(attributes[LOOKUP_VERSION]);
    if (query.version == 0U) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    if (attributes[LOOKUP_REGION] != NULL &&
        !copy_text(query.region, sizeof(query.region),
                   blobmsg_get_string(attributes[LOOKUP_REGION]))) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    if (attributes[LOOKUP_MAX_COST] == NULL) {
        query.max_cost_microunits = 0U;
    } else if (!blobmsg_get_compatible_u64(
                   attributes[LOOKUP_MAX_COST],
                   &query.max_cost_microunits)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    query.max_latency_ms = attributes[LOOKUP_MAX_LATENCY] == NULL
        ? 0U
        : blobmsg_get_u32(attributes[LOOKUP_MAX_LATENCY]);

    trust = attributes[LOOKUP_MIN_TRUST] == NULL
        ? 0U
        : blobmsg_get_u32(attributes[LOOKUP_MIN_TRUST]);
    if (trust > 100U) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    query.min_trust_level = (uint8_t)trust;
    if (attributes[LOOKUP_SOURCE_AGENT] != NULL &&
        !copy_text(query.source_agent, sizeof(query.source_agent),
                   blobmsg_get_string(attributes[LOOKUP_SOURCE_AGENT]))) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    if (attributes[LOOKUP_TARGET_AGENT] != NULL &&
        !copy_text(query.target_agent, sizeof(query.target_agent),
                   blobmsg_get_string(attributes[LOOKUP_TARGET_AGENT]))) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    found = route_table_lookup_ex(&routes, &query, monotonic_ms(), &selection,
                                  &diagnostics);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "found", found);
    blobmsg_add_u64(&response, "generation", routes.generation);
    if (found) {
        route_blob = blobmsg_open_table(&response, "route");
        add_route_to_blob(&response, selection.route, true);
        blobmsg_close_table(&response, route_blob);
    }
    diagnostics_blob = blobmsg_open_table(&response, "diagnostics");
    blobmsg_add_u64(&response, "total", (uint64_t)diagnostics.total);
    blobmsg_add_u64(&response, "eligible", (uint64_t)diagnostics.eligible);
    blobmsg_add_u64(&response, "intent_mismatch",
                    (uint64_t)diagnostics.intent_mismatch);
    blobmsg_add_u64(&response, "target_agent_mismatch",
                    (uint64_t)diagnostics.target_agent_mismatch);
    blobmsg_add_u64(&response, "version_mismatch",
                    (uint64_t)diagnostics.version_mismatch);
    blobmsg_add_u64(&response, "tenant_denied",
                    (uint64_t)diagnostics.tenant_denied);
    blobmsg_add_u64(&response, "region_mismatch",
                    (uint64_t)diagnostics.region_mismatch);
    blobmsg_add_u64(&response, "unhealthy",
                    (uint64_t)diagnostics.unhealthy);
    blobmsg_add_u64(&response, "expired",
                    (uint64_t)diagnostics.expired);
    blobmsg_add_u64(&response, "cost_exceeded",
                    (uint64_t)diagnostics.cost_exceeded);
    blobmsg_add_u64(&response, "latency_exceeded",
                    (uint64_t)diagnostics.latency_exceeded);
    blobmsg_add_u64(&response, "trust_too_low",
                    (uint64_t)diagnostics.trust_too_low);
    blobmsg_add_u64(&response, "policy_denied",
                    (uint64_t)diagnostics.policy_denied);
    blobmsg_add_u64(&response, "policy_region_mismatch",
                    (uint64_t)diagnostics.policy_region_mismatch);
    blobmsg_add_u64(&response, "policy_cost_exceeded",
                    (uint64_t)diagnostics.policy_cost_exceeded);
    blobmsg_add_u64(&response, "policy_latency_exceeded",
                    (uint64_t)diagnostics.policy_latency_exceeded);
    blobmsg_add_u64(&response, "policy_trust_too_low",
                    (uint64_t)diagnostics.policy_trust_too_low);
    blobmsg_add_u64(&response, "policy_load_exceeded",
                    (uint64_t)diagnostics.policy_load_exceeded);
    blobmsg_add_u64(&response, "policy_hops_exceeded",
                    (uint64_t)diagnostics.policy_hops_exceeded);
    blobmsg_add_u64(&response, "policy_source_denied",
                    (uint64_t)diagnostics.policy_source_denied);
    blobmsg_add_u64(&response, "policy_peer_denied",
                    (uint64_t)diagnostics.policy_peer_denied);
    blobmsg_add_u8(&response, "policy_matched", diagnostics.policy_matched);
    blobmsg_add_u8(&response, "policy_allowed", diagnostics.policy_allowed);
    if (diagnostics.policy_id[0] != '\0')
        blobmsg_add_string(&response, "policy_id", diagnostics.policy_id);
    blobmsg_close_table(&response, diagnostics_blob);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_lookup(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    (void)object;
    (void)method;
    return do_lookup(ctx, request, message);
}

enum list_field {
    LIST_LIMIT,
    __LIST_MAX
};

static const struct blobmsg_policy list_policy[__LIST_MAX] = {
    [LIST_LIMIT] = { .name = "limit", .type = BLOBMSG_TYPE_INT32 }
};

enum promote_field {
    PROMOTE_SOURCE,
    PROMOTE_ROUTER_ID,
    PROMOTE_PEER_ID,
    PROMOTE_GENERATION,
    PROMOTE_GRACE_SECONDS,
    __PROMOTE_MAX
};

static const struct blobmsg_policy promote_policy[__PROMOTE_MAX] = {
    [PROMOTE_SOURCE] = {
        .name = "source", .type = BLOBMSG_TYPE_STRING
    },
    [PROMOTE_ROUTER_ID] = {
        .name = "router_id", .type = BLOBMSG_TYPE_STRING
    },
    [PROMOTE_PEER_ID] = {
        .name = "peer_id", .type = BLOBMSG_TYPE_STRING
    },
    [PROMOTE_GENERATION] = {
        .name = "generation", .type = BLOBMSG_TYPE_UNSPEC
    },
    [PROMOTE_GRACE_SECONDS] = {
        .name = "graceful_restart_seconds", .type = BLOBMSG_TYPE_INT32
    }
};

enum promotion_id_field {
    PROMOTION_ID_PEER_ID,
    __PROMOTION_ID_MAX
};

static const struct blobmsg_policy promotion_id_policy[__PROMOTION_ID_MAX] = {
    [PROMOTION_ID_PEER_ID] = {
        .name = "peer_id", .type = BLOBMSG_TYPE_STRING
    }
};

enum card_field {
    CARD_ROUTER_ID,
    CARD_DOMAIN_ID,
    CARD_URI,
    CARD_ARPX_ENDPOINT,
    CARD_ISSUER,
    CARD_KEY_ID,
    CARD_DIGEST,
    CARD_REVISION,
    CARD_ISSUED_AT_MS,
    CARD_EXPIRES_AT_MS,
    CARD_CAPABILITIES,
    __CARD_MAX
};

static const struct blobmsg_policy card_policy_blob[__CARD_MAX] = {
    [CARD_ROUTER_ID] = {.name = "router_id", .type = BLOBMSG_TYPE_STRING},
    [CARD_DOMAIN_ID] = {.name = "domain_id", .type = BLOBMSG_TYPE_STRING},
    [CARD_URI] = {.name = "card_uri", .type = BLOBMSG_TYPE_STRING},
    [CARD_ARPX_ENDPOINT] = {
        .name = "arpx_endpoint", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_ISSUER] = {.name = "issuer", .type = BLOBMSG_TYPE_STRING},
    [CARD_KEY_ID] = {.name = "key_id", .type = BLOBMSG_TYPE_STRING},
    [CARD_DIGEST] = {
        .name = "digest_sha256", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_REVISION] = {.name = "revision", .type = BLOBMSG_TYPE_UNSPEC},
    [CARD_ISSUED_AT_MS] = {
        .name = "issued_at_ms", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_EXPIRES_AT_MS] = {
        .name = "expires_at_ms", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_CAPABILITIES] = {
        .name = "capabilities", .type = BLOBMSG_TYPE_ARRAY
    }
};

enum card_capability_field {
    CARD_CAPABILITY_SKILL_ID,
    CARD_CAPABILITY_INTENT,
    CARD_CAPABILITY_VERSION,
    CARD_CAPABILITY_TENANT,
    __CARD_CAPABILITY_MAX
};

static const struct blobmsg_policy
card_capability_policy[__CARD_CAPABILITY_MAX] = {
    [CARD_CAPABILITY_SKILL_ID] = {
        .name = "skill_id", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_CAPABILITY_INTENT] = {
        .name = "intent", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_CAPABILITY_VERSION] = {
        .name = "version", .type = BLOBMSG_TYPE_INT32
    },
    [CARD_CAPABILITY_TENANT] = {
        .name = "tenant", .type = BLOBMSG_TYPE_STRING
    }
};

enum card_id_field {
    CARD_ID_ROUTER_ID,
    __CARD_ID_MAX
};

static const struct blobmsg_policy card_id_policy[__CARD_ID_MAX] = {
    [CARD_ID_ROUTER_ID] = {
        .name = "router_id", .type = BLOBMSG_TYPE_STRING
    }
};

enum card_trust_field {
    CARD_TRUST_DIRECTORY_ID,
    CARD_TRUST_DIGEST,
    CARD_TRUST_REVISION,
    CARD_TRUST_ISSUED_AT_MS,
    CARD_TRUST_EXPIRES_AT_MS,
    CARD_TRUST_KEYS,
    __CARD_TRUST_MAX
};

static const struct blobmsg_policy card_trust_policy_blob[__CARD_TRUST_MAX] = {
    [CARD_TRUST_DIRECTORY_ID] = {
        .name = "directory_id", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_DIGEST] = {
        .name = "digest_sha256", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_REVISION] = {
        .name = "revision", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_TRUST_ISSUED_AT_MS] = {
        .name = "issued_at_ms", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_TRUST_EXPIRES_AT_MS] = {
        .name = "expires_at_ms", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_TRUST_KEYS] = {
        .name = "card_keys", .type = BLOBMSG_TYPE_ARRAY
    }
};

enum card_trust_key_field {
    CARD_TRUST_KEY_ROUTER_ID,
    CARD_TRUST_KEY_DOMAIN_ID,
    CARD_TRUST_KEY_ISSUER,
    CARD_TRUST_KEY_ID,
    CARD_TRUST_KEY_URI,
    CARD_TRUST_KEY_SHA256,
    CARD_TRUST_KEY_STATUS,
    CARD_TRUST_KEY_NOT_BEFORE_MS,
    CARD_TRUST_KEY_NOT_AFTER_MS,
    __CARD_TRUST_KEY_MAX
};

static const struct blobmsg_policy
card_trust_key_policy[__CARD_TRUST_KEY_MAX] = {
    [CARD_TRUST_KEY_ROUTER_ID] = {
        .name = "router_id", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_DOMAIN_ID] = {
        .name = "domain_id", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_ISSUER] = {
        .name = "issuer", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_ID] = {
        .name = "key_id", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_URI] = {
        .name = "public_key_uri", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_SHA256] = {
        .name = "public_key_sha256", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_STATUS] = {
        .name = "status", .type = BLOBMSG_TYPE_STRING
    },
    [CARD_TRUST_KEY_NOT_BEFORE_MS] = {
        .name = "not_before_ms", .type = BLOBMSG_TYPE_UNSPEC
    },
    [CARD_TRUST_KEY_NOT_AFTER_MS] = {
        .name = "not_after_ms", .type = BLOBMSG_TYPE_UNSPEC
    }
};

static int agent_routes(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_route *route;
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));

    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    (void)agent_peer_routes_prune(&peer_routes, monotonic_ms());
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "generation", routes.generation);
    array = blobmsg_open_array(&response, "routes");
    for (route = route_table_first(&routes);
         route != NULL && emitted < limit;
         route = route->next) {
        item = blobmsg_open_table(&response, NULL);
        add_route_to_blob(&response, route, true);
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated", routes.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_local_agents(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    struct agent_local_summary *summaries;
    struct agent_local_collection collection;
    const struct agent_route *route;
    uint32_t limit = 100U;
    uint64_t now = monotonic_ms();
    size_t index;
    void *array;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    summaries = calloc((size_t)limit, sizeof(*summaries));
    if (summaries == NULL) {
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
    collection = agent_local_agents_collect(
        &routes, now, summaries, (size_t)limit);

    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "attachment_model", "capability_lease");
    blobmsg_add_u8(&response, "transport_connection_tracked", false);
    blobmsg_add_u64(&response, "generation", routes.generation);
    array = blobmsg_open_array(&response, "agents");
    for (index = 0U; index < collection.count; index++) {
        const struct agent_local_summary *summary = &summaries[index];
        size_t capabilities = 0U;
        void *item = blobmsg_open_table(&response, NULL);
        void *capability_array;

        blobmsg_add_string(&response, "agent_id", summary->agent_id);
        blobmsg_add_string(&response, "endpoint", summary->endpoint);
        blobmsg_add_string(&response, "tenant", summary->tenant);
        blobmsg_add_string(&response, "state",
                           summary->healthy_capability_count > 0U
                               ? "active" : "unhealthy");
        blobmsg_add_u8(&response, "active_lease", true);
        blobmsg_add_u8(&response, "healthy",
                       summary->healthy_capability_count > 0U);
        blobmsg_add_u32(&response, "capability_count",
                        (uint32_t)summary->capability_count);
        blobmsg_add_u32(&response, "healthy_capability_count",
                        (uint32_t)summary->healthy_capability_count);
        blobmsg_add_u64(&response, "earliest_remaining_ms",
                        summary->earliest_expires_ms - now);
        blobmsg_add_u64(&response, "latest_remaining_ms",
                        summary->latest_expires_ms - now);
        capability_array = blobmsg_open_array(&response, "capabilities");
        for (route = route_table_first(&routes);
             route != NULL && capabilities < AGENTD_AGENT_CAPABILITY_LIMIT;
             route = route->next) {
            void *capability;

            if (route->lease_expires_ms <= now ||
                !agent_local_summary_matches(summary, route)) {
                continue;
            }
            capability = blobmsg_open_table(&response, NULL);
            blobmsg_add_string(&response, "intent", route->intent);
            blobmsg_add_u32(&response, "version", route->version);
            blobmsg_add_string(&response, "route_id", route->route_id);
            blobmsg_add_u8(&response, "healthy", route->healthy);
            blobmsg_add_u64(&response, "remaining_ms",
                            route->lease_expires_ms - now);
            blobmsg_close_table(&response, capability);
            capabilities++;
        }
        blobmsg_close_array(&response, capability_array);
        blobmsg_add_u8(&response, "capabilities_truncated",
                       summary->capability_count > capabilities);
        blobmsg_close_table(&response, item);
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", (uint32_t)collection.count);
    blobmsg_add_u8(&response, "truncated", collection.truncated);
    free(summaries);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_neighbors(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_peer *peer;
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;
    struct agent_peer_transport_stats transport_stats;
    struct agent_peer_listener_stats listener_stats;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    agent_peer_transport_get_stats(peer_transport, &transport_stats);
    agent_peer_listener_get_stats(peer_listener, &listener_stats);
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "generation", peers.generation);
    blobmsg_add_u8(&response, "wire_protocol_implemented", true);
    blobmsg_add_u8(&response, "session_engine_implemented", true);
    blobmsg_add_u8(&response, "transport_implemented", true);
    blobmsg_add_u8(&response, "transport_enabled", transport_stats.enabled);
    blobmsg_add_u8(&response, "listener_enabled", listener_stats.enabled);
    blobmsg_add_u8(&response, "listener_listening", listener_stats.listening);
    array = blobmsg_open_array(&response, "neighbors");
    for (peer = peer_table_first(&peers);
         peer != NULL && emitted < limit;
         peer = peer->next) {
        item = blobmsg_open_table(&response, NULL);
        add_peer_to_blob(&response, peer);
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated", peers.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_public_ipv6_addresses(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    size_t index;
    uint64_t now = monotonic_ms();
    void *array;

    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "enabled", public_ipv6_pool.configured);
    blobmsg_add_string(&response, "mode",
        config.public_ipv6_upstream_relay ?
            "upstream-relay" : "routed-prefix");
    blobmsg_add_u8(&response, "ndp_proxy",
                   config.public_ipv6_upstream_relay);
    blobmsg_add_string(&response, "prefix",
        public_ipv6_pool.configured ? public_ipv6_pool.prefix_text : "");
    blobmsg_add_u32(&response, "capacity",
                    (uint32_t)public_ipv6_pool.capacity);
    blobmsg_add_u32(&response, "active",
                    (uint32_t)public_ipv6_pool.count);
    array = blobmsg_open_array(&response, "addresses");
    for (index = 0U; index < public_ipv6_pool.count; index++) {
        const struct agent_public_ipv6_lease *lease =
            &public_ipv6_pool.leases[index];
        void *item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "route_id", lease->route_id);
        blobmsg_add_string(&response, "origin", lease->origin);
        blobmsg_add_string(&response, "public_ipv6", lease->address);
        blobmsg_add_u64(&response, "remaining_ms",
            lease->expires_ms > now ? lease->expires_ms - now : 0U);
        blobmsg_close_table(&response, item);
    }
    blobmsg_close_array(&response, array);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

enum discovery_record_field {
    DISCOVERY_RECORD_IFACE,
    DISCOVERY_RECORD_HOST,
    DISCOVERY_RECORD_IPV4,
    DISCOVERY_RECORD_TXT,
    DISCOVERY_RECORD_PORT,
    DISCOVERY_RECORD_TTL,
    DISCOVERY_RECORD_LAST_UPDATE,
    __DISCOVERY_RECORD_MAX
};

static const struct blobmsg_policy
discovery_record_policy[__DISCOVERY_RECORD_MAX] = {
    [DISCOVERY_RECORD_IFACE] = {
        .name = "iface", .type = BLOBMSG_TYPE_STRING
    },
    [DISCOVERY_RECORD_HOST] = {
        .name = "host", .type = BLOBMSG_TYPE_STRING
    },
    [DISCOVERY_RECORD_IPV4] = {
        .name = "ipv4", .type = BLOBMSG_TYPE_ARRAY
    },
    [DISCOVERY_RECORD_TXT] = {
        .name = "txt", .type = BLOBMSG_TYPE_ARRAY
    },
    [DISCOVERY_RECORD_PORT] = {
        .name = "port", .type = BLOBMSG_TYPE_INT32
    },
    [DISCOVERY_RECORD_TTL] = {
        .name = "ttl", .type = BLOBMSG_TYPE_INT32
    },
    [DISCOVERY_RECORD_LAST_UPDATE] = {
        .name = "last_update", .type = BLOBMSG_TYPE_STRING
    }
};

static bool discovery_copy(
    char *target,
    size_t capacity,
    const char *source
)
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

static void discovery_consume_record(
    const char *instance_name,
    struct blob_attr *record,
    uint64_t now_ms
)
{
    struct blob_attr *attributes[__DISCOVERY_RECORD_MAX];
    struct blob_attr *item;
    struct agent_discovery_observation observation;
    uint32_t value;
    int remaining;
    bool valid = true;

    agent_discovery_observation_init(&observation);
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(discovery_record_policy, __DISCOVERY_RECORD_MAX,
                  attributes, blobmsg_data(record), blobmsg_len(record));
    if (!discovery_copy(observation.instance,
                        sizeof(observation.instance), instance_name) ||
        attributes[DISCOVERY_RECORD_IFACE] == NULL ||
        attributes[DISCOVERY_RECORD_HOST] == NULL ||
        attributes[DISCOVERY_RECORD_IPV4] == NULL ||
        attributes[DISCOVERY_RECORD_TXT] == NULL ||
        attributes[DISCOVERY_RECORD_PORT] == NULL ||
        attributes[DISCOVERY_RECORD_TTL] == NULL ||
        attributes[DISCOVERY_RECORD_LAST_UPDATE] == NULL ||
        !discovery_copy(
            observation.iface, sizeof(observation.iface),
            blobmsg_get_string(attributes[DISCOVERY_RECORD_IFACE])) ||
        !discovery_copy(
            observation.hostname, sizeof(observation.hostname),
            blobmsg_get_string(attributes[DISCOVERY_RECORD_HOST])) ||
        !discovery_copy(
            observation.last_update, sizeof(observation.last_update),
            blobmsg_get_string(attributes[DISCOVERY_RECORD_LAST_UPDATE]))) {
        valid = false;
    }

    if (valid) {
        valid = false;
        blobmsg_for_each_attr(
            item, attributes[DISCOVERY_RECORD_IPV4], remaining) {
            if (blobmsg_type(item) == BLOBMSG_TYPE_STRING &&
                discovery_copy(observation.ipv4,
                               sizeof(observation.ipv4),
                               blobmsg_get_string(item))) {
                valid = true;
                break;
            }
        }
    }
    if (valid) {
        blobmsg_for_each_attr(
            item, attributes[DISCOVERY_RECORD_TXT], remaining) {
            if (blobmsg_type(item) != BLOBMSG_TYPE_STRING ||
                !agent_discovery_observation_add_txt(
                    &observation, blobmsg_get_string(item))) {
                valid = false;
                break;
            }
        }
    }
    if (valid) {
        value = blobmsg_get_u32(attributes[DISCOVERY_RECORD_PORT]);
        if (value == 0U || value > UINT16_MAX) {
            valid = false;
        } else {
            observation.port = (uint16_t)value;
        }
    }
    if (valid) {
        value = blobmsg_get_u32(attributes[DISCOVERY_RECORD_TTL]);
        if (value < AGENT_DISCOVERY_MIN_TTL_SECONDS) {
            value = AGENT_DISCOVERY_MIN_TTL_SECONDS;
        } else if (value > config.discovery_ttl_cap_seconds) {
            value = config.discovery_ttl_cap_seconds;
        }
        observation.ttl_seconds = value;
    }
    if (!valid) {
        if (discoveries.rejected != UINT64_MAX) {
            discoveries.rejected++;
        }
        return;
    }
    (void)agent_discovery_table_observe(
        &discoveries, &observation,
        config.peer_transport.local_router_id, now_ms);
}

static void discovery_browse_callback(
    struct ubus_request *request,
    int type,
    struct blob_attr *message
)
{
    struct blob_attr *service;
    struct blob_attr *record;
    uint64_t now_ms = monotonic_ms();
    int service_remaining;
    int record_remaining;

    (void)request;
    (void)type;
    if (message == NULL) {
        return;
    }
    blobmsg_for_each_attr(service, message, service_remaining) {
        if (strcmp(blobmsg_name(service), AGENTD_DISCOVERY_SERVICE) != 0 ||
            blobmsg_type(service) != BLOBMSG_TYPE_TABLE) {
            continue;
        }
        blobmsg_for_each_attr(record, service, record_remaining) {
            if (blobmsg_type(record) == BLOBMSG_TYPE_TABLE) {
                discovery_consume_record(blobmsg_name(record), record,
                                         now_ms);
            }
        }
    }
}

static bool refresh_discoveries(void)
{
    struct blob_buf query = {0};
    uint32_t umdns_id;
    int status;

    if (!config.discovery_enabled || ubus_ctx == NULL) {
        return false;
    }
    if (discovery_polls != UINT64_MAX) {
        discovery_polls++;
    }
    if (ubus_lookup_id(ubus_ctx, "umdns", &umdns_id) != UBUS_STATUS_OK) {
        if (discovery_poll_failures != UINT64_MAX) {
            discovery_poll_failures++;
        }
        return false;
    }

    blob_buf_init(&query, 0);
    (void)ubus_invoke(ubus_ctx, umdns_id, "update", query.head, NULL,
                      NULL, AGENTD_DISCOVERY_UBUS_TIMEOUT_MS);
    blob_buf_free(&query);

    blob_buf_init(&query, 0);
    blobmsg_add_string(&query, "service", AGENTD_DISCOVERY_SERVICE);
    blobmsg_add_u8(&query, "array", true);
    blobmsg_add_u8(&query, "address", true);
    status = ubus_invoke(ubus_ctx, umdns_id, "browse", query.head,
                         discovery_browse_callback, NULL,
                         AGENTD_DISCOVERY_UBUS_TIMEOUT_MS);
    blob_buf_free(&query);
    (void)agent_discovery_table_prune(&discoveries, monotonic_ms());
    if (status != UBUS_STATUS_OK && discovery_poll_failures != UINT64_MAX) {
        discovery_poll_failures++;
    }
    return status == UBUS_STATUS_OK;
}

static const struct agent_peer *discovery_policy_peer(
    const struct agent_discovery_candidate *candidate
)
{
    const struct agent_peer *peer;
    const struct agent_discovery_observation *observation;
    const char *port_start;
    const char *path;
    char *end = NULL;
    unsigned long endpoint_port;

    if (candidate == NULL) {
        return NULL;
    }
    observation = &candidate->observation;
    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        if (peer->role == AGENT_PEER_ROLE_RELAY ||
            agent_discovery_promotion_find(&promotions, peer->peer_id) !=
                NULL) {
            continue;
        }
        if (strcmp(peer->router_id, observation->router_id) != 0 ||
            strcmp(peer->domain_id, observation->domain_id) != 0 ||
            peer->connect_ipv4[0] == '\0' ||
            strcmp(peer->connect_ipv4, observation->ipv4) != 0) {
            continue;
        }
        path = strstr(peer->endpoint, "/arpx/v1");
        port_start = path == NULL ? NULL : path;
        while (port_start != NULL && port_start > peer->endpoint &&
               port_start[-1] != ':') {
            port_start--;
        }
        if (port_start == NULL || port_start == peer->endpoint ||
            port_start[-1] != ':') {
            continue;
        }
        endpoint_port = strtoul(port_start, &end, 10);
        if (end == path && endpoint_port == observation->port) {
            return peer;
        }
    }
    return NULL;
}

static const struct agent_discovery_promotion *promotion_for_candidate(
    enum agent_promotion_source source,
    const char *router_id
)
{
    const struct agent_discovery_promotion *promotion;

    for (promotion = agent_discovery_promotion_first(&promotions);
         promotion != NULL; promotion = promotion->next) {
        if (promotion->source == source &&
            strcmp(promotion->router_id, router_id) == 0) {
            return promotion;
        }
    }
    return NULL;
}

static bool configured_peer_for_lan_candidate(
    const struct agent_discovery_candidate *candidate
)
{
    const struct agent_peer *peer;
    const struct agent_discovery_observation *observation;

    if (candidate == NULL) return false;
    observation = &candidate->observation;
    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        if (peer->role != AGENT_PEER_ROLE_RELAY &&
            agent_discovery_promotion_find(&promotions, peer->peer_id) ==
                NULL &&
            strcmp(peer->router_id, observation->router_id) == 0 &&
            strcmp(peer->domain_id, observation->domain_id) == 0) {
            return true;
        }
    }
    return false;
}

static size_t apply_auto_promotions(uint64_t now_ms)
{
    const struct agent_discovery_candidate *candidate;
    char added_ids[AGENTD_DEFAULT_MAX_PROMOTIONS][AGENT_PEER_ID_LEN];
    enum agent_promotion_result result;
    size_t added = 0U;
    size_t i;

    if (!config.discovery_enabled || !config.peer_transport.enabled ||
        auto_promotion_policy.mode == AGENT_AUTO_PROMOTION_OFF) return 0U;
    (void)agent_discovery_table_prune(&discoveries, now_ms);
    (void)reconcile_promotions(now_ms);
    for (candidate = agent_discovery_table_first(&discoveries);
         candidate != NULL; candidate = candidate->next) {
        const char *router_id = candidate->observation.router_id;

        if (!agent_auto_promotion_eligible(
                &auto_promotion_policy, candidate) ||
            promotion_for_candidate(AGENT_PROMOTION_LAN, router_id) != NULL ||
            configured_peer_for_lan_candidate(candidate)) continue;
        if (auto_promotion_attempts != UINT64_MAX) auto_promotion_attempts++;
        result = agent_discovery_promote_lan_auto(
            &promotions, &peers, &discoveries, router_id,
            discoveries.generation,
            auto_promotion_policy.graceful_restart_seconds, now_ms);
        if (result != AGENT_PROMOTION_OK) {
            if (auto_promotion_failures != UINT64_MAX) {
                auto_promotion_failures++;
            }
            continue;
        }
        if (added < AGENTD_DEFAULT_MAX_PROMOTIONS) {
            (void)copy_text(added_ids[added], sizeof(added_ids[added]),
                            router_id);
            added++;
        }
    }
    if (added == 0U) return 0U;
    if (reload_peer_runtime()) {
        if (UINT64_MAX - auto_promotion_succeeded < added) {
            auto_promotion_succeeded = UINT64_MAX;
        } else {
            auto_promotion_succeeded += added;
        }
        return added;
    }
    for (i = 0U; i < added; i++) {
        (void)agent_discovery_promotion_remove(
            &promotions, &peers, added_ids[i]);
    }
    (void)reload_peer_runtime();
    if (UINT64_MAX - auto_promotion_failures < added) {
        auto_promotion_failures = UINT64_MAX;
    } else {
        auto_promotion_failures += added;
    }
    return 0U;
}

static int agent_discoveries(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_discovery_candidate *candidate;
    const struct agent_peer *matched_peer;
    const struct agent_discovery_promotion *promotion;
    uint64_t now_ms = monotonic_ms();
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_discovery_table_prune(&discoveries, now_ms);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "enabled", config.discovery_enabled);
    blobmsg_add_string(&response, "service",
                       AGENTD_DISCOVERY_SERVICE ".local");
    blobmsg_add_u64(&response, "generation", discoveries.generation);
    array = blobmsg_open_array(&response, "discoveries");
    for (candidate = agent_discovery_table_first(&discoveries);
         candidate != NULL && emitted < limit;
         candidate = candidate->next) {
        const struct agent_discovery_observation *observation =
            &candidate->observation;
        uint64_t remaining_ms = candidate->expires_at_ms > now_ms
            ? candidate->expires_at_ms - now_ms : 0U;

        matched_peer = discovery_policy_peer(candidate);
        promotion = promotion_for_candidate(
            AGENT_PROMOTION_LAN, observation->router_id);
        item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "instance", observation->instance);
        blobmsg_add_string(&response, "router_id", observation->router_id);
        blobmsg_add_string(&response, "domain_id", observation->domain_id);
        blobmsg_add_string(&response, "hostname", observation->hostname);
        blobmsg_add_string(&response, "ipv4", observation->ipv4);
        blobmsg_add_string(&response, "interface", observation->iface);
        blobmsg_add_u32(&response, "port", observation->port);
        blobmsg_add_u32(&response, "protocol_version",
                        observation->protocol_version);
        blobmsg_add_string(&response, "registration_path",
                           observation->registration_path);
        blobmsg_add_u64(&response, "remaining_ms", remaining_ms);
        blobmsg_add_string(&response, "source_last_update",
                           observation->last_update);
        blobmsg_add_u8(&response, "policy_match", matched_peer != NULL);
        blobmsg_add_u8(&response, "auto_peer_created", false);
        blobmsg_add_u8(&response, "promoted", promotion != NULL);
        blobmsg_add_u8(&response, "auto_promotion_eligible",
                       agent_auto_promotion_eligible(
                           &auto_promotion_policy, candidate));
        if (promotion != NULL) {
            blobmsg_add_string(&response, "promoted_peer_id",
                               promotion->peer.peer_id);
            blobmsg_add_u8(&response, "promotion_automatic",
                           promotion->automatic);
        }
        if (matched_peer != NULL) {
            blobmsg_add_string(&response, "static_peer_id",
                               matched_peer->peer_id);
        }
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated", discoveries.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_discovery_refresh(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    bool refreshed;
    size_t promoted;

    (void)object;
    (void)method;
    (void)message;
    refreshed = refresh_discoveries();
    promoted = refreshed ? apply_auto_promotions(monotonic_ms()) : 0U;
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "enabled", config.discovery_enabled);
    blobmsg_add_u8(&response, "refreshed", refreshed);
    blobmsg_add_u64(&response, "generation", discoveries.generation);
    blobmsg_add_u64(&response, "candidates", (uint64_t)discoveries.count);
    blobmsg_add_u64(&response, "auto_promoted", (uint64_t)promoted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static void cross_query_close(void)
{
    uloop_timeout_cancel(&cross_query_timeout);
    if (cross_query_fd.fd >= 0) {
        uloop_fd_delete(&cross_query_fd);
        close(cross_query_fd.fd);
        cross_query_fd.fd = -1;
    }
    cross_query_pending = false;
}

static void cross_query_timeout_cb(struct uloop_timeout *timeout)
{
    (void)timeout;
    if (cross_query_pending) {
        if (cross_query_timeouts != UINT64_MAX) {
            cross_query_timeouts++;
        }
        cross_query_close();
    }
}

static size_t apply_open_mesh_cross_promotions(uint64_t now_ms)
{
    const struct agent_cross_candidate *candidate;
    char added_ids[AGENTD_DEFAULT_MAX_PROMOTIONS][AGENT_PEER_ID_LEN];
    size_t added = 0U;
    size_t index;

    if (!config.peer_transport.open_mesh ||
        !config.cross_discovery_enabled ||
        !config.peer_transport.enabled) return 0U;
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    (void)reconcile_promotions(now_ms);
    for (candidate = agent_cross_table_first(&cross_discoveries);
         candidate != NULL; candidate = candidate->next) {
        const struct agent_peer *peer;
        bool configured = false;
        enum agent_promotion_result result;

        for (peer = peer_table_first(&peers); peer != NULL;
             peer = peer->next) {
            if (peer->role != AGENT_PEER_ROLE_RELAY &&
                strcmp(peer->router_id, candidate->router_id) == 0) {
                configured = true;
                break;
            }
        }
        if (configured) continue;
        if (auto_promotion_attempts != UINT64_MAX) auto_promotion_attempts++;
        result = agent_discovery_promote_svcb_auto(
            &promotions, &peers, &cross_discoveries,
            candidate->router_id, cross_discoveries.generation,
            config.auto_promotion_grace_seconds, now_ms);
        if (result != AGENT_PROMOTION_OK) {
            if (auto_promotion_failures != UINT64_MAX) {
                auto_promotion_failures++;
            }
            continue;
        }
        if (added < AGENTD_DEFAULT_MAX_PROMOTIONS &&
            copy_text(added_ids[added], sizeof(added_ids[added]),
                      candidate->router_id)) {
            added++;
        }
    }
    if (added == 0U) return 0U;
    if (reload_peer_runtime()) {
        auto_promotion_succeeded = UINT64_MAX - auto_promotion_succeeded < added
            ? UINT64_MAX : auto_promotion_succeeded + added;
        return added;
    }
    for (index = 0U; index < added; index++) {
        (void)agent_discovery_promotion_remove(
            &promotions, &peers, added_ids[index]);
    }
    (void)reload_peer_runtime();
    auto_promotion_failures = UINT64_MAX - auto_promotion_failures < added
        ? UINT64_MAX : auto_promotion_failures + added;
    return 0U;
}

static void cross_query_read_cb(struct uloop_fd *descriptor,
                                unsigned int events)
{
    uint8_t packet[AGENT_CROSS_MAX_DNS_PACKET + 1U];
    ssize_t length;
    enum agent_cross_parse_result result;

    (void)events;
    if (descriptor == NULL || descriptor->fd < 0 || !cross_query_pending) {
        return;
    }
    length = recv(descriptor->fd, packet, sizeof(packet), 0);
    if (length <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        cross_query_close();
        return;
    }
    if ((size_t)length > AGENT_CROSS_MAX_DNS_PACKET) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        cross_query_close();
        return;
    }
    result = agent_cross_parse_response(
        &cross_discoveries, packet, (size_t)length, cross_query_id,
        config.cross_discovery_owner, monotonic_ms(),
        config.cross_ttl_cap_seconds);
    if (result == AGENT_CROSS_PARSE_OK ||
        result == AGENT_CROSS_PARSE_NO_DATA) {
        if (cross_responses_secure != UINT64_MAX) {
            cross_responses_secure++;
        }
        if (result == AGENT_CROSS_PARSE_OK) {
            (void)apply_open_mesh_cross_promotions(monotonic_ms());
        }
    } else if (cross_query_failures != UINT64_MAX) {
        cross_query_failures++;
    }
    cross_query_close();
}

static bool refresh_cross_discoveries(void)
{
    struct sockaddr_in resolver;
    uint8_t packet[512];
    size_t packet_length;
    ssize_t sent;
    int flags;

    if (!config.cross_discovery_enabled || cross_query_pending) {
        return false;
    }
    cross_query_id++;
    if (cross_query_id == 0U) {
        cross_query_id = 1U;
    }
    packet_length = agent_cross_build_query(
        packet, sizeof(packet), cross_query_id,
        config.cross_discovery_owner);
    if (packet_length == 0U) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        return false;
    }
    cross_query_fd.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (cross_query_fd.fd < 0) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        return false;
    }
    flags = fcntl(cross_query_fd.fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(cross_query_fd.fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        close(cross_query_fd.fd);
        cross_query_fd.fd = -1;
        return false;
    }
    memset(&resolver, 0, sizeof(resolver));
    resolver.sin_family = AF_INET;
    resolver.sin_port = htons(config.cross_resolver_port);
    if (inet_pton(AF_INET, config.cross_resolver_ipv4,
                  &resolver.sin_addr) != 1 ||
        connect(cross_query_fd.fd, (struct sockaddr *)&resolver,
                sizeof(resolver)) != 0) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        close(cross_query_fd.fd);
        cross_query_fd.fd = -1;
        return false;
    }
    sent = send(cross_query_fd.fd, packet, packet_length, 0);
    if (sent < 0 || (size_t)sent != packet_length) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        close(cross_query_fd.fd);
        cross_query_fd.fd = -1;
        return false;
    }
    cross_query_fd.cb = cross_query_read_cb;
    if (uloop_fd_add(&cross_query_fd, ULOOP_READ) != 0) {
        if (cross_query_failures != UINT64_MAX) {
            cross_query_failures++;
        }
        close(cross_query_fd.fd);
        cross_query_fd.fd = -1;
        return false;
    }
    cross_query_pending = true;
    if (cross_queries_sent != UINT64_MAX) {
        cross_queries_sent++;
    }
    cross_query_timeout.cb = cross_query_timeout_cb;
    uloop_timeout_set(&cross_query_timeout,
                      (int)config.cross_timeout_ms);
    return true;
}

static bool static_peer_endpoint_matches_cross(
    const struct agent_peer *peer,
    const struct agent_cross_candidate *candidate
)
{
    const char *host_start;
    const char *port_start;
    const char *path;
    size_t host_length;
    char host[AGENT_CROSS_HOSTNAME_LEN];
    char *end = NULL;
    unsigned long port;

    if (peer == NULL || candidate == NULL ||
        strcmp(peer->router_id, candidate->router_id) != 0 ||
        strcmp(peer->domain_id, candidate->domain_id) != 0 ||
        strncmp(peer->endpoint, "https://", 8U) != 0) {
        return false;
    }
    host_start = peer->endpoint + 8U;
    port_start = strchr(host_start, ':');
    path = port_start == NULL ? NULL : strchr(port_start, '/');
    if (port_start == NULL || path == NULL ||
        strcmp(path, "/arpx/v1") != 0) {
        return false;
    }
    host_length = (size_t)(port_start - host_start);
    if (host_length == 0U || host_length >= sizeof(host)) {
        return false;
    }
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    port = strtoul(port_start + 1U, &end, 10);
    if (end != path || port != candidate->port ||
        strcmp(host, candidate->target) != 0) {
        return false;
    }
    return candidate->ipv4_hint[0] == '\0' ||
           (peer->connect_ipv4[0] != '\0' &&
            strcmp(peer->connect_ipv4, candidate->ipv4_hint) == 0);
}

static const struct agent_peer *cross_policy_peer(
    const struct agent_cross_candidate *candidate
)
{
    const struct agent_peer *peer;

    for (peer = peer_table_first(&peers); peer != NULL; peer = peer->next) {
        if (peer->role == AGENT_PEER_ROLE_RELAY ||
            agent_discovery_promotion_find(&promotions, peer->peer_id) !=
                NULL) {
            continue;
        }
        if (static_peer_endpoint_matches_cross(peer, candidate)) {
            return peer;
        }
    }
    return NULL;
}

static int agent_cross_discoveries(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_cross_candidate *candidate;
    const struct agent_peer *matched_peer;
    const struct agent_discovery_promotion *promotion;
    uint64_t now_ms = monotonic_ms();
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "enabled", config.cross_discovery_enabled);
    blobmsg_add_u8(&response, "experimental", true);
    blobmsg_add_u8(&response, "dnssec_required", true);
    blobmsg_add_string(&response, "owner", config.cross_discovery_owner);
    blobmsg_add_u64(&response, "generation", cross_discoveries.generation);
    array = blobmsg_open_array(&response, "discoveries");
    for (candidate = agent_cross_table_first(&cross_discoveries);
         candidate != NULL && emitted < limit;
         candidate = candidate->next) {
        uint64_t remaining_ms = candidate->expires_at_ms > now_ms
            ? candidate->expires_at_ms - now_ms : 0U;

        matched_peer = cross_policy_peer(candidate);
        promotion = promotion_for_candidate(
            AGENT_PROMOTION_SVCB, candidate->router_id);
        item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "owner", candidate->owner);
        blobmsg_add_u32(&response, "priority", candidate->priority);
        blobmsg_add_string(&response, "target", candidate->target);
        blobmsg_add_u32(&response, "port", candidate->port);
        blobmsg_add_string(&response, "alpn", "h2");
        blobmsg_add_string(&response, "router_id", candidate->router_id);
        blobmsg_add_string(&response, "domain_id", candidate->domain_id);
        blobmsg_add_string(&response, "agent_card_uri",
                           candidate->agent_card_uri);
        if (candidate->ipv4_hint[0] != '\0') {
            blobmsg_add_string(&response, "ipv4_hint",
                               candidate->ipv4_hint);
        }
        blobmsg_add_u32(&response, "unknown_optional_keys",
                        candidate->unknown_optional_keys);
        blobmsg_add_u64(&response, "remaining_ms", remaining_ms);
        blobmsg_add_u8(&response, "dnssec_secure", true);
        blobmsg_add_u8(&response, "policy_match", matched_peer != NULL);
        blobmsg_add_u8(&response, "auto_peer_created", false);
        blobmsg_add_u8(&response, "promoted", promotion != NULL);
        blobmsg_add_u8(&response, "card_authorized",
                       agent_card_find(&cards, candidate->router_id) != NULL);
        if (promotion != NULL) {
            blobmsg_add_string(&response, "promoted_peer_id",
                               promotion->peer.peer_id);
        }
        if (matched_peer != NULL) {
            blobmsg_add_string(&response, "static_peer_id",
                               matched_peer->peer_id);
        }
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated",
                   cross_discoveries.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_cross_discovery_refresh(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    bool queued;

    (void)object;
    (void)method;
    (void)message;
    queued = refresh_cross_discoveries();
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "enabled", config.cross_discovery_enabled);
    blobmsg_add_u8(&response, "queued", queued);
    blobmsg_add_u8(&response, "query_pending", cross_query_pending);
    blobmsg_add_u32(&response, "query_id", cross_query_id);
    blobmsg_add_string(&response, "owner", config.cross_discovery_owner);
    blobmsg_add_u64(&response, "generation", cross_discoveries.generation);
    blobmsg_add_u64(&response, "candidates",
                    (uint64_t)cross_discoveries.count);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int promotion_result_to_ubus(enum agent_promotion_result result)
{
    switch (result) {
    case AGENT_PROMOTION_OK:
        return UBUS_STATUS_OK;
    case AGENT_PROMOTION_NOT_FOUND:
        return UBUS_STATUS_NOT_FOUND;
    case AGENT_PROMOTION_INVALID:
    case AGENT_PROMOTION_STALE_GENERATION:
    case AGENT_PROMOTION_ADDRESS_REQUIRED:
    case AGENT_PROMOTION_CONFLICT:
        return UBUS_STATUS_INVALID_ARGUMENT;
    case AGENT_PROMOTION_FULL:
    case AGENT_PROMOTION_NO_MEMORY:
    default:
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
}

static int agent_promotions(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_discovery_promotion *promotion;
    uint64_t now_ms = monotonic_ms();
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_discovery_table_prune(&discoveries, now_ms);
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    (void)agent_card_prune(&cards, now_ms, card_peer_removing, NULL);
    (void)reconcile_promotions(now_ms);
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "generation", promotions.generation);
    blobmsg_add_string(&response, "lan_auto_promotion_mode",
                       agent_auto_promotion_mode_name(
                           auto_promotion_policy.mode));
    blobmsg_add_u64(&response, "max_promotions",
                    (uint64_t)promotions.max_promotions);
    array = blobmsg_open_array(&response, "promotions");
    for (promotion = agent_discovery_promotion_first(&promotions);
         promotion != NULL && emitted < limit;
         promotion = promotion->next) {
        item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "source",
                           agent_promotion_source_name(promotion->source));
        blobmsg_add_u8(&response, "automatic", promotion->automatic);
        blobmsg_add_string(&response, "router_id", promotion->router_id);
        blobmsg_add_string(&response, "peer_id",
                           promotion->peer.peer_id);
        blobmsg_add_string(&response, "endpoint",
                           promotion->peer.endpoint);
        if (promotion->peer.connect_ipv4[0] != '\0') {
            blobmsg_add_string(&response, "connect_ipv4",
                               promotion->peer.connect_ipv4);
        }
        blobmsg_add_u64(&response, "candidate_generation",
                        promotion->candidate_generation);
        blobmsg_add_u64(&response, "approved_at_monotonic_ms",
                        promotion->approved_at_ms);
        blobmsg_add_u64(&response, "remaining_ms",
                        promotion->expires_at_ms > now_ms
                            ? promotion->expires_at_ms - now_ms : 0U);
        blobmsg_add_u32(&response, "graceful_restart_seconds",
                        promotion->peer.graceful_restart_seconds);
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated", promotions.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_discovery_promote(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__PROMOTE_MAX];
    const struct agent_discovery_promotion *existing;
    enum agent_promotion_result result;
    const char *source;
    const char *router_id;
    const char *peer_id;
    uint64_t generation;
    uint64_t now_ms = monotonic_ms();
    uint32_t grace = AGENTD_DEFAULT_PROMOTION_GRACE_SECONDS;
    bool renewed;

    (void)object;
    (void)method;
    if (!config.peer_transport.enabled) {
        return UBUS_STATUS_NOT_SUPPORTED;
    }
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(promote_policy, __PROMOTE_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[PROMOTE_SOURCE] == NULL ||
        attributes[PROMOTE_ROUTER_ID] == NULL ||
        !blobmsg_get_compatible_u64(attributes[PROMOTE_GENERATION],
                                    &generation)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    source = blobmsg_get_string(attributes[PROMOTE_SOURCE]);
    router_id = blobmsg_get_string(attributes[PROMOTE_ROUTER_ID]);
    peer_id = attributes[PROMOTE_PEER_ID] == NULL
        ? router_id : blobmsg_get_string(attributes[PROMOTE_PEER_ID]);
    if (attributes[PROMOTE_GRACE_SECONDS] != NULL) {
        grace = blobmsg_get_u32(attributes[PROMOTE_GRACE_SECONDS]);
    }
    if (grace < AGENTD_MIN_LEASE_SECONDS ||
        grace > AGENTD_MAX_PROMOTION_GRACE_SECONDS) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_discovery_table_prune(&discoveries, now_ms);
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    (void)reconcile_promotions(now_ms);
    existing = agent_discovery_promotion_find(&promotions, peer_id);
    renewed = existing != NULL;
    if (strcmp(source, "lan") == 0 && config.discovery_enabled) {
        result = agent_discovery_promote_lan(
            &promotions, &peers, &discoveries, router_id, peer_id,
            generation, grace, now_ms);
    } else if (strcmp(source, "svcb") == 0 &&
               config.cross_discovery_enabled) {
        result = agent_discovery_promote_svcb(
            &promotions, &peers, &cross_discoveries, router_id, peer_id,
            generation, grace, now_ms);
    } else {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    if (result != AGENT_PROMOTION_OK) {
        return promotion_result_to_ubus(result);
    }
    if (!renewed && !reload_peer_runtime()) {
        (void)agent_discovery_promotion_remove(
            &promotions, &peers, peer_id);
        (void)reload_peer_runtime();
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "promoted", true);
    blobmsg_add_u8(&response, "renewed", renewed);
    blobmsg_add_string(&response, "source", source);
    blobmsg_add_string(&response, "router_id", router_id);
    blobmsg_add_string(&response, "peer_id", peer_id);
    blobmsg_add_u64(&response, "candidate_generation", generation);
    blobmsg_add_u64(&response, "promotion_generation",
                    promotions.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_discovery_revoke(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__PROMOTION_ID_MAX];
    enum agent_promotion_result result;
    const char *peer_id;
    bool runtime_reloaded;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(promotion_id_policy, __PROMOTION_ID_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[PROMOTION_ID_PEER_ID] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    peer_id = blobmsg_get_string(attributes[PROMOTION_ID_PEER_ID]);
    if (agent_discovery_promotion_find(&promotions, peer_id) == NULL) {
        return UBUS_STATUS_NOT_FOUND;
    }
    promotion_peer_removing(NULL, peer_id);
    result = agent_discovery_promotion_remove(&promotions, &peers, peer_id);
    if (result != AGENT_PROMOTION_OK) {
        return promotion_result_to_ubus(result);
    }
    runtime_reloaded = reload_peer_runtime();
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "revoked", true);
    blobmsg_add_string(&response, "peer_id", peer_id);
    blobmsg_add_u8(&response, "runtime_reloaded", runtime_reloaded);
    blobmsg_add_string(&response, "route_withdrawal_state",
                       "stale_until_grace_expiry");
    blobmsg_add_u64(&response, "promotion_generation",
                    promotions.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static void add_relay_status(
    struct blob_buf *buffer,
    const struct agent_relay_plane *plane,
    const char *applied_generation
)
{
    struct agent_relay_bootstrap_status status;

    agent_relay_bootstrap_get_status(
        plane == NULL ? NULL : plane->bootstrap, &status);
    blobmsg_add_u8(buffer, "implemented", true);
    blobmsg_add_string(buffer, "plane",
                       plane == NULL ? "unknown" : plane->name);
    blobmsg_add_string(buffer, "applied_generation",
                       applied_generation == NULL ? "" : applied_generation);
    blobmsg_add_u8(buffer, "enabled", status.enabled);
    blobmsg_add_u8(buffer, "query_active", status.query_active);
    blobmsg_add_u8(buffer, "assignment_active", status.assignment_active);
    struct agent_peer_transport_status mesh_transport_status;
    bool tunnel_connected = status.assignment_active &&
        agent_peer_transport_get_status(peer_transport, status.assignment.relay_id, &mesh_transport_status) &&
        mesh_transport_status.relay_tunnel_up;
    blobmsg_add_u8(buffer, "tunnel_connected", tunnel_connected);
    blobmsg_add_string(buffer, "phase",
                       status.phase == NULL ? "unavailable" : status.phase);
    blobmsg_add_u64(buffer, "assignment_remaining_ms",
                    status.assignment_remaining_ms);
    blobmsg_add_u64(buffer, "queries", status.queries);
    blobmsg_add_u64(buffer, "assignments", status.assignments);
    blobmsg_add_u64(buffer, "renewals", status.renewals);
    blobmsg_add_u64(buffer, "failures", status.failures);
    blobmsg_add_u64(buffer, "expirations", status.expirations);
    blobmsg_add_u64(buffer, "directory_failovers",
                    status.directory_failovers);
    blobmsg_add_u64(buffer, "relay_failover_requests",
                    status.relay_failover_requests);
    blobmsg_add_u64(buffer, "tls_handshakes", status.tls_handshakes);
    blobmsg_add_u32(buffer, "tls_last_error_code",
                    status.tls_last_error_code);
    blobmsg_add_u32(buffer, "tls_last_verify_flags",
                    status.tls_last_verify_flags);
    blobmsg_add_string(buffer, "tls_last_alpn", status.tls_last_alpn);
    blobmsg_add_u64(buffer, "directory_count",
                    (uint64_t)status.directory_count);
    blobmsg_add_u64(buffer, "directory_index",
                    (uint64_t)status.directory_index);
    blobmsg_add_u8(buffer, "directory_dns", status.directory_dns);
    blobmsg_add_string(buffer, "directory_server_identity",
                       status.directory_server_identity);
    blobmsg_add_string(buffer, "directory_resolved_ipv4",
                       status.directory_resolved_ipv4);
    if (status.assignment_active) {
        blobmsg_add_string(buffer, "assignment_id",
                           status.assignment.assignment_id);
        blobmsg_add_string(buffer, "relay_id", status.assignment.relay_id);
        blobmsg_add_string(buffer, "relay_router_id",
                           status.assignment.relay_router_id);
        blobmsg_add_string(buffer, "relay_domain_id",
                           status.assignment.relay_domain_id);
        blobmsg_add_string(buffer, "relay_endpoint",
                           status.assignment.relay_endpoint);
        blobmsg_add_string(buffer, "connect_ipv4",
                           status.assignment.connect_ipv4);
        blobmsg_add_u32(buffer, "lease_seconds",
                        status.assignment.lease_seconds);
        blobmsg_add_u8(buffer, "ticket_present",
                       status.assignment.session_ticket[0] != '\0');
    }
    if (status.last_error[0] != '\0') {
        blobmsg_add_string(buffer, "last_error", status.last_error);
    }
}

static int agent_relay_status(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    add_relay_status(&response, &cloud_relay,
                     config.relay_config_generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_open_mesh_relay_status(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    add_relay_status(&response, &open_mesh_relay, "");
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_relay_refresh(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    bool queued;

    (void)object;
    (void)method;
    (void)message;
    queued = agent_relay_bootstrap_refresh(cloud_relay.bootstrap);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "queued", queued);
    add_relay_status(&response, &cloud_relay,
                     config.relay_config_generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_open_mesh_relay_refresh(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    bool queued;

    (void)object;
    (void)method;
    (void)message;
    queued = agent_relay_bootstrap_refresh(open_mesh_relay.bootstrap);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "queued", queued);
    add_relay_status(&response, &open_mesh_relay, "");
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}
static void add_recovery_domain(
    struct blob_buf *buffer,
    enum agent_recovery_domain domain
)
{
    const struct agent_recovery_domain_state *status;
    void *item;

    status = agent_recovery_domain_status(&recovery_state, domain);
    if (status == NULL) return;
    item = blobmsg_open_table(buffer, agent_recovery_domain_name(domain));
    blobmsg_add_u8(buffer, "configuration_ok", status->configuration_ok);
    blobmsg_add_u64(buffer, "startup_attempts", status->startup_attempts);
    blobmsg_add_u64(buffer, "reload_attempts", status->reload_attempts);
    blobmsg_add_u64(buffer, "successes", status->successes);
    blobmsg_add_u64(buffer, "failures", status->failures);
    blobmsg_add_string(buffer, "last_error", status->last_error);
    blobmsg_close_table(buffer, item);
}

static int agent_recovery_status_method(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    void *domains;

    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "degraded",
                   agent_recovery_degraded(&recovery_state));
    blobmsg_add_u64(&response, "generation", recovery_state.generation);
    blobmsg_add_u64(
        &response, "process_started_monotonic_ms",
        recovery_state.process_started_ms);
    blobmsg_add_u64(
        &response, "process_uptime_ms",
        agent_recovery_uptime_ms(&recovery_state, monotonic_ms()));
    blobmsg_add_u64(
        &response, "configuration_failures",
        agent_recovery_failures(&recovery_state));
    blobmsg_add_string(
        &response, "startup_failure_mode",
        "fail-closed-empty-afib-with-live-management");
    blobmsg_add_u8(&response, "dynamic_arib_persisted", false);
    blobmsg_add_string(
        &response, "dynamic_arib_recovery",
        "lease-reregister-or-peer-snapshot");
    domains = blobmsg_open_table(&response, "configuration_domains");
    add_recovery_domain(&response, AGENT_RECOVERY_STATIC_ROUTES);
    add_recovery_domain(&response, AGENT_RECOVERY_POLICY_RIB);
    add_recovery_domain(&response, AGENT_RECOVERY_STATIC_PEERS);
    blobmsg_close_table(&response, domains);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static bool dynamic_manifest_route_active(
    const char *route_id,
    void *context
)
{
    const struct route_table *table = context;
    const struct agent_route *route = route_table_find(table, route_id);

    return route != NULL && route->source == AGENT_ROUTE_SOURCE_LOCAL;
}

static size_t prune_dynamic_manifests(void)
{
    size_t removed = agent_dynamic_manifest_prune(
        &dynamic_manifests, dynamic_manifest_route_active, &routes);

    if (removed > 0U) (void)write_dynamic_manifest_snapshot();
    return removed;
}

static int agent_manifests(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    void *array;
    size_t index;

    (void)object;
    (void)method;
    (void)message;
    (void)prune_dynamic_manifests();
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "generation", dynamic_manifests.generation);
    blobmsg_add_u64(&response, "route_generation", routes.generation);
    array = blobmsg_open_array(&response, "manifests");
    for (index = 0U; index < dynamic_manifests.count; index++) {
        const struct agent_dynamic_manifest_entry *entry =
            &dynamic_manifests.entries[index];
        const struct agent_adapter_mapping *mapping = &entry->mapping;
        void *item = blobmsg_open_table(&response, NULL);
        void *tool;

        blobmsg_add_string(&response, "route_id", entry->route_id);
        blobmsg_add_string(&response, "tenant", entry->tenant);
        blobmsg_add_string(&response, "origin", entry->origin);
        blobmsg_add_string(&response, "agent_name", entry->agent_name);
        blobmsg_add_string(&response, "manifest_digest",
                           entry->manifest_digest);
        blobmsg_add_u8(&response, "publish", entry->publish);
        tool = blobmsg_open_table(&response, "tool");
        blobmsg_add_string(&response, "protocol", "mcp");
        blobmsg_add_string(&response, "authority", mapping->authority);
        blobmsg_add_string(&response, "selector", mapping->selector);
        blobmsg_add_string(&response, "intent", mapping->intent);
        blobmsg_add_u32(&response, "intent_version",
                        mapping->intent_version);
        if (mapping->title[0] != '\0')
            blobmsg_add_string(&response, "title", mapping->title);
        if (mapping->description[0] != '\0')
            blobmsg_add_string(&response, "description",
                               mapping->description);
        blobmsg_add_string(&response, "input_schema_json",
                           mapping->input_schema_json);
        blobmsg_add_u8(&response, "task", mapping->task);
        blobmsg_add_u8(&response, "continuable", mapping->continuable);
        blobmsg_add_u8(&response, "recovery_protocol",
                       mapping->recovery_protocol);
        blobmsg_add_u8(&response, "demo", mapping->demo);
        blobmsg_add_u8(&response, "chat", mapping->chat);
        blobmsg_add_u8(&response, "interactive", mapping->interactive);
        blobmsg_close_table(&response, tool);
        blobmsg_close_table(&response, item);
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u64(&response, "count", (uint64_t)dynamic_manifests.count);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_stats(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct agent_peer_transport_stats transport_stats;
    struct agent_peer_listener_stats listener_stats;
    struct agent_relay_bootstrap_status relay_status;
    struct agent_relay_bootstrap_status open_mesh_relay_status;
    struct agent_relay_invoke_stats invoke_stats;

    (void)object;
    (void)method;
    (void)message;
    (void)agent_peer_routes_prune(&peer_routes, monotonic_ms());
    (void)prune_dynamic_manifests();
    agent_peer_transport_get_stats(peer_transport, &transport_stats);
    agent_peer_listener_get_stats(peer_listener, &listener_stats);
    agent_relay_bootstrap_get_status(cloud_relay.bootstrap, &relay_status);
    agent_relay_bootstrap_get_status(
        open_mesh_relay.bootstrap, &open_mesh_relay_status);
    agent_relay_invoke_get_stats(relay_invoke, &invoke_stats);
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "routes", (uint64_t)routes.count);
    blobmsg_add_u64(&response, "generation", routes.generation);
    blobmsg_add_u64(&response, "dynamic_manifest_generation",
                    dynamic_manifests.generation);
    blobmsg_add_u64(&response, "dynamic_manifest_tools",
                    (uint64_t)dynamic_manifests.count);
    blobmsg_add_u64(&response, "dynamic_manifest_rejected",
                    dynamic_manifest_rejected);
    blobmsg_add_u64(&response, "dynamic_manifest_snapshot_failures",
                    dynamic_manifest_snapshot_failures);
    blobmsg_add_u64(&response, "max_routes",
                    (uint64_t)routes.max_routes);
    blobmsg_add_u8(&response, "route_index_enabled",
                   route_table_index_enabled(&routes));
    blobmsg_add_u64(&response, "route_index_buckets",
                    (uint64_t)route_table_index_buckets(&routes));
    blobmsg_add_u64(&response, "route_memory_bytes",
                    (uint64_t)route_table_memory_bytes(&routes));
    blobmsg_add_u64(&response, "static_routes",
                    (uint64_t)static_routes_loaded);
    blobmsg_add_u64(&response, "configured_peers",
                    (uint64_t)static_peers_loaded);
    blobmsg_add_u64(&response, "peer_generation", peers.generation);
    blobmsg_add_u64(&response, "peer_sessions_up",
                    (uint64_t)(transport_stats.sessions_up +
                               listener_stats.sessions_up));
    blobmsg_add_u8(&response, "peer_wire_protocol_implemented", true);
    blobmsg_add_u8(&response, "peer_session_engine_implemented", true);
    blobmsg_add_u8(&response, "peer_transport_implemented", true);
    blobmsg_add_u8(&response, "peer_transport_enabled",
                   transport_stats.enabled);
    blobmsg_add_u64(&response, "peer_transport_eligible",
                    (uint64_t)(transport_stats.eligible +
                               listener_stats.eligible));
    blobmsg_add_u64(&response, "peer_tcp_connects",
                    transport_stats.tcp_connects);
    blobmsg_add_u64(&response, "peer_tls_handshakes",
                    transport_stats.tls_handshakes +
                    listener_stats.tls_handshakes);
    blobmsg_add_u64(&response, "peer_h2_sessions",
                    transport_stats.h2_sessions + listener_stats.h2_sessions);
    blobmsg_add_u64(&response, "peer_messages_sent",
                    transport_stats.messages_sent +
                    listener_stats.messages_sent);
    blobmsg_add_u64(&response, "peer_messages_received",
                    transport_stats.messages_received +
                    listener_stats.messages_received);
    blobmsg_add_u64(&response, "peer_transport_failures",
                    transport_stats.failures + listener_stats.failures);
    blobmsg_add_u64(&response, "peer_updates_sent", peer_updates_sent);
    blobmsg_add_u64(&response, "peer_withdrawals_sent",
                    peer_withdrawals_sent);
    blobmsg_add_u64(&response, "peer_send_failures", peer_send_failures);
    blobmsg_add_u64(&response, "peer_updates_accepted",
                    peer_routes.updates_accepted);
    blobmsg_add_u64(&response, "peer_withdrawals_accepted",
                    peer_routes.withdrawals_accepted);
    blobmsg_add_u64(&response, "peer_route_loops_rejected",
                    peer_routes.loops_rejected);
    blobmsg_add_u64(&response, "peer_route_ownership_rejected",
                    peer_routes.ownership_rejected);
    blobmsg_add_u64(&response, "peer_route_authorization_rejected",
                    peer_routes.authorization_rejected);
    blobmsg_add_string(&response, "card_authorization_mode",
                       agent_card_authorization_mode_name(card_policy.mode));
    blobmsg_add_u64(&response, "agent_cards_active", (uint64_t)cards.count);
    blobmsg_add_u64(&response, "agent_cards_accepted", cards.accepted);
    blobmsg_add_u64(&response, "agent_cards_rejected", cards.rejected);
    blobmsg_add_u64(&response, "agent_card_signature_rejected",
                    cards.signature_rejected);
    blobmsg_add_u64(&response, "agent_card_identity_rejected",
                    cards.identity_rejected);
    blobmsg_add_u64(&response, "agent_card_policy_rejected",
                    cards.policy_rejected);
    blobmsg_add_u64(&response, "agent_card_rollback_rejected",
                    cards.rollback_rejected);
    blobmsg_add_u64(&response, "agent_card_route_updates_rejected",
                    cards.route_updates_rejected);
    blobmsg_add_u8(&response, "agent_card_directory_trust_loaded",
                   card_trust.loaded);
    blobmsg_add_u64(&response, "agent_card_directory_trust_generation",
                    card_trust.generation);
    blobmsg_add_u64(&response, "agent_card_directory_trust_accepted",
                    card_trust.accepted);
    blobmsg_add_u64(&response, "agent_card_directory_trust_rejected",
                    card_trust.rejected);
    blobmsg_add_u64(&response, "peer_routes_staled",
                    peer_routes.routes_staled);
    blobmsg_add_u64(&response, "peer_routes_removed",
                    peer_routes.routes_removed);
    blobmsg_add_u8(&response, "reflector_enabled",
                   config.reflector_enabled);
    blobmsg_add_u64(&response, "snapshot_requests_sent",
                    snapshot_requests_sent);
    blobmsg_add_u64(&response, "snapshot_requests_received",
                    snapshot_requests_received);
    blobmsg_add_u64(&response, "snapshot_ends_sent",
                    snapshot_ends_sent);
    blobmsg_add_u64(&response, "snapshots_started",
                    peer_routes.snapshots_started);
    blobmsg_add_u64(&response, "snapshots_completed",
                    peer_routes.snapshots_completed);
    blobmsg_add_u64(&response, "snapshot_routes_exported",
                    snapshot_exported_routes);
    blobmsg_add_u64(&response, "snapshot_routes_removed",
                    peer_routes.snapshot_routes_removed);
    blobmsg_add_u64(&response, "snapshot_export_restarts",
                    snapshot_export_restarts);
    blobmsg_add_u64(&response, "reflected_updates_sent",
                    reflected_updates_sent);
    blobmsg_add_u64(&response, "reflected_withdrawals_sent",
                    reflected_withdrawals_sent);
    blobmsg_add_u64(&response, "split_horizon_suppressed",
                    split_horizon_suppressed);
    blobmsg_add_u8(&response, "peer_listener_enabled",
                   listener_stats.enabled);
    blobmsg_add_u8(&response, "peer_listener_listening",
                   listener_stats.listening);
    blobmsg_add_string(&response, "peer_listen_ipv4",
                       listener_stats.listen_ipv4);
    blobmsg_add_u32(&response, "peer_listen_port",
                    listener_stats.listen_port);
    blobmsg_add_u64(&response, "peer_inbound_active",
                    (uint64_t)listener_stats.active_connections);
    blobmsg_add_u64(&response, "peer_inbound_accepts",
                    listener_stats.accepts);
    blobmsg_add_u64(&response, "peer_inbound_rejected",
                    listener_stats.rejected_connections);
    blobmsg_add_u8(&response, "lan_discovery_implemented", true);
    blobmsg_add_u8(&response, "lan_discovery_enabled",
                   config.discovery_enabled);
    blobmsg_add_u64(&response, "discovery_candidates",
                    (uint64_t)discoveries.count);
    blobmsg_add_u64(&response, "discovery_generation",
                    discoveries.generation);
    blobmsg_add_u64(&response, "discovery_observations_accepted",
                    discoveries.accepted);
    blobmsg_add_u64(&response, "discovery_observations_rejected",
                    discoveries.rejected);
    blobmsg_add_u64(&response, "discovery_candidates_expired",
                    discoveries.expired);
    blobmsg_add_u64(&response, "discovery_self_suppressed",
                    discoveries.self_suppressed);
    blobmsg_add_u64(&response, "discovery_polls", discovery_polls);
    blobmsg_add_u64(&response, "discovery_poll_failures",
                    discovery_poll_failures);
    blobmsg_add_u8(&response, "cross_discovery_implemented", true);
    blobmsg_add_u8(&response, "cross_discovery_enabled",
                   config.cross_discovery_enabled);
    blobmsg_add_u8(&response, "cross_discovery_experimental", true);
    blobmsg_add_u8(&response, "cross_discovery_dnssec_required", true);
    blobmsg_add_string(&response, "cross_discovery_resolver_ipv4",
                       config.cross_resolver_ipv4);
    blobmsg_add_u32(&response, "cross_discovery_resolver_port",
                    config.cross_resolver_port);
    blobmsg_add_u8(&response, "cross_discovery_resolver_detected",
                   config.cross_resolver_detected);
    blobmsg_add_u64(&response, "cross_discovery_candidates",
                    (uint64_t)cross_discoveries.count);
    blobmsg_add_u64(&response, "cross_discovery_generation",
                    cross_discoveries.generation);
    blobmsg_add_u64(&response, "cross_discovery_queries_sent",
                    cross_queries_sent);
    blobmsg_add_u64(&response, "cross_discovery_responses_secure",
                    cross_responses_secure);
    blobmsg_add_u64(&response, "cross_discovery_query_failures",
                    cross_query_failures);
    blobmsg_add_u64(&response, "cross_discovery_query_timeouts",
                    cross_query_timeouts);
    blobmsg_add_u64(&response, "cross_discovery_records_accepted",
                    cross_discoveries.records_accepted);
    blobmsg_add_u64(&response, "cross_discovery_records_rejected",
                    cross_discoveries.records_rejected);
    blobmsg_add_u64(&response, "cross_discovery_records_incompatible",
                    cross_discoveries.records_incompatible);
    blobmsg_add_u64(&response, "cross_discovery_dnssec_rejected",
                    cross_discoveries.dnssec_rejected);
    blobmsg_add_u64(&response, "cross_discovery_candidates_expired",
                    cross_discoveries.candidates_expired);
    blobmsg_add_u8(&response, "discovery_promotion_implemented", true);
    blobmsg_add_u64(&response, "discovery_promotions",
                    (uint64_t)promotions.count);
    blobmsg_add_u64(&response, "discovery_promotion_max",
                    (uint64_t)promotions.max_promotions);
    blobmsg_add_u64(&response, "discovery_promotion_generation",
                    promotions.generation);
    blobmsg_add_u64(&response, "discovery_candidates_promoted",
                    promotions.promoted);
    blobmsg_add_u64(&response, "discovery_promotions_renewed",
                    promotions.renewed);
    blobmsg_add_u64(&response, "discovery_promotions_revoked",
                    promotions.revoked);
    blobmsg_add_u64(&response, "discovery_promotions_expired",
                    promotions.expired);
    blobmsg_add_u64(&response, "discovery_promotion_conflicts",
                    promotions.conflicts);
    blobmsg_add_u8(&response, "lan_auto_promotion_implemented", true);
    blobmsg_add_u8(&response, "open_mesh_enabled",
                   config.peer_transport.open_mesh);
    blobmsg_add_string(&response, "lan_auto_promotion_mode",
                       agent_auto_promotion_mode_name(
                           auto_promotion_policy.mode));
    blobmsg_add_u32(&response, "lan_auto_promotion_grace_seconds",
                    auto_promotion_policy.graceful_restart_seconds);
    blobmsg_add_u64(&response, "lan_auto_promotion_attempts",
                    auto_promotion_attempts);
    blobmsg_add_u64(&response, "lan_auto_promotion_succeeded",
                    auto_promotion_succeeded);
    blobmsg_add_u64(&response, "lan_auto_promotion_failures",
                    auto_promotion_failures);
    blobmsg_add_u8(&response, "relay_bootstrap_implemented", true);
    blobmsg_add_u8(&response, "relay_bootstrap_enabled",
                   relay_status.enabled);
    blobmsg_add_u8(&response, "relay_assignment_active",
                   relay_status.assignment_active);
    blobmsg_add_u64(&response, "relay_directory_queries",
                    relay_status.queries);
    blobmsg_add_u64(&response, "relay_directory_failures",
                    relay_status.failures);
    blobmsg_add_u64(&response, "relay_assignment_renewals",
                    relay_status.renewals);
    blobmsg_add_u64(&response, "relay_assignment_expirations",
                    relay_status.expirations);
    blobmsg_add_u64(&response, "relay_directory_failovers",
                    relay_status.directory_failovers);
    blobmsg_add_u64(&response, "relay_failover_requests",
                    relay_status.relay_failover_requests);
    blobmsg_add_u8(&response, "open_mesh_relay_bootstrap_implemented", true);
    blobmsg_add_u8(&response, "open_mesh_relay_bootstrap_enabled",
                   open_mesh_relay_status.enabled);
    blobmsg_add_u8(&response, "open_mesh_relay_assignment_active",
                   open_mesh_relay_status.assignment_active);
    blobmsg_add_u64(&response, "open_mesh_relay_directory_queries",
                    open_mesh_relay_status.queries);
    blobmsg_add_u64(&response, "open_mesh_relay_directory_failures",
                    open_mesh_relay_status.failures);
    blobmsg_add_u64(&response, "open_mesh_relay_assignment_renewals",
                    open_mesh_relay_status.renewals);
    blobmsg_add_u64(&response, "open_mesh_relay_assignment_expirations",
                    open_mesh_relay_status.expirations);
    blobmsg_add_u8(&response, "relay_tunnel_implemented", true);
    blobmsg_add_u8(&response, "relay_tunnel_enabled",
                   transport_stats.relay_tunnel_enabled);
    blobmsg_add_u64(&response, "relay_tunnels_up",
                    (uint64_t)transport_stats.relay_tunnels_up);
    blobmsg_add_u64(&response, "relay_tunnel_streams",
                    (uint64_t)transport_stats.relay_tunnel_streams);
    blobmsg_add_u64(&response, "relay_tunnel_frames_sent",
                    transport_stats.relay_tunnel_frames_sent);
    blobmsg_add_u64(&response, "relay_tunnel_frames_received",
                    transport_stats.relay_tunnel_frames_received);
    blobmsg_add_u64(&response, "relay_tunnel_protocol_errors",
                    transport_stats.relay_tunnel_protocol_errors);
    blobmsg_add_u8(&response, "peer_invoke_tunnel_implemented", true);
    blobmsg_add_u8(&response, "peer_invoke_tunnel_enabled",
                   transport_stats.relay_tunnel_enabled ||
                   listener_stats.invoke_tunnel_enabled);
    blobmsg_add_u64(&response, "peer_invoke_tunnels_up",
                   (uint64_t)(transport_stats.relay_tunnels_up +
                              listener_stats.invoke_tunnels_up));
    blobmsg_add_u64(&response, "peer_invoke_tunnel_streams",
                   (uint64_t)(transport_stats.relay_tunnel_streams +
                              listener_stats.invoke_tunnel_streams));
    blobmsg_add_u64(&response, "peer_invoke_tunnel_frames_sent",
                   transport_stats.relay_tunnel_frames_sent +
                   listener_stats.invoke_tunnel_frames_sent);
    blobmsg_add_u64(&response, "peer_invoke_tunnel_frames_received",
                   transport_stats.relay_tunnel_frames_received +
                   listener_stats.invoke_tunnel_frames_received);
    blobmsg_add_u8(&response, "relay_invoke_implemented", true);
    blobmsg_add_u64(&response, "relay_invokes_active",
                    (uint64_t)invoke_stats.active);
    blobmsg_add_u64(&response, "relay_invokes_source_started",
                    invoke_stats.source_started);
    blobmsg_add_u64(&response, "relay_invokes_target_started",
                    invoke_stats.target_started);
    blobmsg_add_u64(&response, "relay_invokes_transit_started",
                    invoke_stats.transit_started);
    blobmsg_add_u64(&response, "relay_invokes_transit_completed",
                    invoke_stats.transit_completed);
    blobmsg_add_u64(&response, "relay_invokes_transit_failed",
                    invoke_stats.transit_failed);
    blobmsg_add_u64(&response, "relay_invokes_transit_no_route",
                    invoke_stats.transit_no_route);
    blobmsg_add_u64(&response, "relay_invokes_transit_hop_limit_rejected",
                    invoke_stats.transit_hop_limit_rejected);
    blobmsg_add_u64(&response, "relay_invoke_transit_bytes_forwarded",
                    invoke_stats.transit_bytes_forwarded);
    blobmsg_add_u64(&response, "relay_invokes_completed",
                    invoke_stats.completed);
    blobmsg_add_u64(&response, "relay_invokes_failed",
                    invoke_stats.failed);
    blobmsg_add_u64(&response, "relay_invokes_timed_out",
                    invoke_stats.timed_out);
    blobmsg_add_u64(&response, "relay_invoke_bytes_sent",
                    invoke_stats.bytes_sent);
    blobmsg_add_u64(&response, "relay_invoke_bytes_received",
                    invoke_stats.bytes_received);
    blobmsg_add_u64(&response, "relay_streams_started",
                    invoke_stats.streams_started);
    blobmsg_add_u64(&response, "relay_streams_completed",
                    invoke_stats.streams_completed);
    blobmsg_add_u64(&response, "relay_stream_events",
                    invoke_stats.stream_events);
    blobmsg_add_u64(&response, "relay_stream_bytes",
                    invoke_stats.stream_bytes);
    blobmsg_add_u64(&response, "relay_stream_backpressure_resets",
                    invoke_stats.stream_backpressure_resets);
    blobmsg_add_u8(&response, "relay_internal_gateway_enabled",
                   config.relay_gateway_internal_enabled);
    blobmsg_add_u64(&response, "relay_internal_gateway_started",
                    invoke_stats.internal_gateway_started);
    blobmsg_add_u64(&response, "relay_internal_gateway_completed",
                    invoke_stats.internal_gateway_completed);
    blobmsg_add_u64(&response, "relay_internal_gateway_failed",
                    invoke_stats.internal_gateway_failed);
    blobmsg_add_u64(&response, "relay_internal_gateway_unavailable",
                    invoke_stats.internal_gateway_unavailable);
    blobmsg_add_u8(&response, "forwarding_assertion_required",
                   config.forwarding_assertion_required);
    blobmsg_add_u64(&response, "forwarding_assertions_verified",
                    invoke_stats.forwarding_assertions_verified);
    blobmsg_add_u64(&response, "forwarding_assertions_rejected",
                    invoke_stats.forwarding_assertions_rejected);
    blobmsg_add_u64(&response, "forwarding_assertion_replays",
                    invoke_stats.forwarding_assertion_replays);
    blobmsg_add_u64(&response, "forwarding_assertion_body_mismatches",
                    invoke_stats.forwarding_assertion_body_mismatches);
    blobmsg_add_u64(&response, "relay_cloud_tenant_aliases",
                    invoke_stats.cloud_tenant_aliases);
    blobmsg_add_u64(&response, "relay_cloud_tenant_alias_rejections",
                    invoke_stats.cloud_tenant_alias_rejections);
    blobmsg_add_string(&response, "ipc_socket", config.ipc_socket);
    blobmsg_add_u32(&response, "ipc_allowed_uid",
                    (uint32_t)config.ipc_allowed_uid);
    blobmsg_add_u64(&response, "ipc_accepted_connections",
                    ipc_server.accepted_connections);
    blobmsg_add_u64(&response, "ipc_rejected_credentials",
                    ipc_server.rejected_credentials);
    blobmsg_add_u64(&response, "ipc_invoke_requests",
                    ipc_server.invoke_requests);
    blobmsg_add_u64(&response, "ipc_invoke_completions",
                    ipc_server.invoke_completions);
    blobmsg_add_u64(&response, "ipc_stream_starts",
                    ipc_server.stream_starts);
    blobmsg_add_u64(&response, "ipc_stream_data_frames",
                    ipc_server.stream_data_frames);
    blobmsg_add_u64(&response, "ipc_stream_completions",
                    ipc_server.stream_completions);
    blobmsg_add_u64(&response, "policy_generation", policies.generation);
    blobmsg_add_u64(&response, "policy_rules", (uint64_t)policies.count);
    blobmsg_add_u8(&response, "recovery_degraded",
                   agent_recovery_degraded(&recovery_state));
    blobmsg_add_u64(&response, "recovery_generation",
                    recovery_state.generation);
    blobmsg_add_u64(&response, "configuration_failures",
                    agent_recovery_failures(&recovery_state));
    blobmsg_add_u64(
        &response, "process_uptime_ms",
        agent_recovery_uptime_ms(&recovery_state, monotonic_ms()));
    blobmsg_add_string(&response, "implemented_phase", "P0-P7.2");
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_policy_status(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    size_t index;
    void *array;
    void *item;
    const struct agent_policy_rule *rule;

    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    blobmsg_add_u64(&response, "generation", policies.generation);
    blobmsg_add_string(&response, "default_action",
                       policies.default_allow ? "allow" : "deny");
    blobmsg_add_u32(&response, "health_failure_threshold",
                    policies.health_failure_threshold);
    blobmsg_add_u32(&response, "health_recovery_threshold",
                    policies.health_recovery_threshold);
    blobmsg_add_u64(&response, "rule_count", (uint64_t)policies.count);
    array = blobmsg_open_array(&response, "rules");
    for (index = 0U; index < policies.count; index++) {
        rule = &policies.rules[index];
        item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "policy_id", rule->policy_id);
        blobmsg_add_u32(&response, "priority", rule->priority);
        blobmsg_add_string(&response, "action",
                           agent_policy_action_name(rule->action));
        blobmsg_add_string(&response, "tenant", rule->match_tenant);
        blobmsg_add_string(&response, "source_agent",
                           rule->match_source_agent);
        blobmsg_add_string(&response, "intent", rule->match_intent);
        blobmsg_add_string(&response, "required_region",
                           rule->required_region);
        blobmsg_add_u64(&response, "max_cost_microunits",
                        rule->max_cost_microunits);
        blobmsg_add_u32(&response, "max_latency_ms",
                        rule->max_latency_ms);
        blobmsg_add_u32(&response, "min_trust", rule->min_trust_level);
        blobmsg_add_u32(&response, "max_load_permille",
                        rule->max_load_permille);
        blobmsg_add_u32(&response, "max_hops", rule->max_hops);
        blobmsg_add_u32(&response, "route_source_mask", rule->source_mask);
        blobmsg_add_string(&response, "required_peer", rule->required_peer);
        blobmsg_add_string(&response, "denied_peer", rule->denied_peer);
        blobmsg_add_string(&response, "preferred_peer", rule->preferred_peer);
        blobmsg_add_string(&response, "allowed_endpoint_prefix",
                           rule->allowed_endpoint_prefix);
        blobmsg_add_u64(&response, "nonpreferred_peer_penalty",
                        rule->nonpreferred_peer_penalty);
        blobmsg_add_u32(&response, "latency_weight", rule->latency_weight);
        blobmsg_add_u32(&response, "cost_divisor", rule->cost_divisor);
        blobmsg_add_u32(&response, "load_weight", rule->load_weight);
        blobmsg_add_u32(&response, "trust_weight", rule->trust_weight);
        blobmsg_add_u32(&response, "hop_weight", rule->hop_weight);
        blobmsg_add_u32(&response, "tenant_max_inflight",
                        rule->tenant_max_inflight);
        blobmsg_add_u32(&response, "tenant_rate_per_second",
                        rule->tenant_rate_per_second);
        blobmsg_add_u32(&response, "tenant_rate_burst",
                        rule->tenant_rate_burst);
        blobmsg_close_table(&response, item);
    }
    blobmsg_close_array(&response, array);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_reload_policy(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct static_policy_load_result load_result;
    bool loaded;

    (void)object;
    (void)method;
    (void)message;
    loaded = static_policies_reload(&policies, "agent_policy", &load_result);
    agent_recovery_record_reload(
        &recovery_state, AGENT_RECOVERY_POLICY_RIB, loaded,
        loaded ? NULL : load_result.error);
    if (loaded) {
        static_policies_loaded = load_result.loaded;
        route_table_set_policy(&routes, &policies);
    }
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "configuration_reloaded", loaded);
    blobmsg_add_u8(&response, "policy_rib_compiler_implemented", true);
    if (loaded) {
        blobmsg_add_u64(&response, "policies",
                        (uint64_t)static_policies_loaded);
    } else {
        blobmsg_add_string(&response, "error", load_result.error);
    }
    blobmsg_add_u64(&response, "policy_generation", policies.generation);
    blobmsg_add_u64(&response, "afib_generation", routes.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_reload(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct static_route_load_result load_result;
    enum route_table_result result;

    (void)object;
    (void)method;
    (void)message;
    result = static_routes_reload(&routes, "agent", &load_result);
    agent_recovery_record_reload(
        &recovery_state, AGENT_RECOVERY_STATIC_ROUTES,
        result == ROUTE_TABLE_OK,
        result == ROUTE_TABLE_OK ? NULL : load_result.error);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "configuration_reloaded",
                   result == ROUTE_TABLE_OK);
    blobmsg_add_u8(&response, "static_route_compiler_implemented", true);
    if (result == ROUTE_TABLE_OK) {
        static_routes_loaded = load_result.loaded;
        blobmsg_add_u64(&response, "static_routes",
                        (uint64_t)static_routes_loaded);
    } else {
        blobmsg_add_string(&response, "error", load_result.error);
    }
    blobmsg_add_u64(&response, "generation", routes.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_reload_peers(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct static_peer_load_result load_result;
    struct peer_table candidate_peers;
    enum peer_table_result result;
    char transport_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};
    char listener_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};
    bool transport_reloaded = false;
    bool listener_reloaded = false;
    const struct agent_discovery_promotion *promotion;
    const struct agent_discovery_promotion *next_promotion;

    (void)object;
    (void)method;
    (void)message;
    peer_table_init(&candidate_peers, peers.max_peers);
    result = static_peers_reload(
        &candidate_peers, "agent_peers", &load_result);
    if (result == PEER_TABLE_OK) {
        for (promotion = agent_discovery_promotion_first(&promotions);
             promotion != NULL; promotion = next_promotion) {
            const struct agent_peer *static_peer;
            bool shadowed = false;

            next_promotion = promotion->next;
            if (!promotion->automatic) continue;
            for (static_peer = peer_table_first(&candidate_peers);
                 static_peer != NULL; static_peer = static_peer->next) {
                if (strcmp(static_peer->peer_id,
                           promotion->peer.peer_id) == 0 ||
                    (strcmp(static_peer->router_id,
                            promotion->peer.router_id) == 0 &&
                     strcmp(static_peer->domain_id,
                            promotion->peer.domain_id) == 0)) {
                    shadowed = true;
                    break;
                }
            }
            if (!shadowed) continue;
            promotion_peer_removing(NULL, promotion->peer.peer_id);
            (void)agent_discovery_promotion_remove(
                &promotions, &peers, promotion->peer.peer_id);
        }
    }
    if (result == PEER_TABLE_OK &&
        agent_discovery_promotion_reapply(&promotions, &candidate_peers) !=
            AGENT_PROMOTION_OK) {
        result = PEER_TABLE_INVALID;
        (void)snprintf(load_result.error, sizeof(load_result.error),
                       "discovery promotion conflicts with static peers");
    }
    if (result == PEER_TABLE_OK) {
        struct agent_relay_plane *planes[] = {
            &cloud_relay, &open_mesh_relay
        };
        size_t plane_index;

        for (plane_index = 0U;
             plane_index < sizeof(planes) / sizeof(planes[0]);
             plane_index++) {
            struct agent_peer relay_peer;
            struct agent_relay_plane *plane = planes[plane_index];

            if (!plane->assignment_active) continue;
            relay_peer_from_assignment(&plane->assignment, &relay_peer);
            result = peer_table_upsert(&candidate_peers, &relay_peer);
            if (result != PEER_TABLE_OK) {
                (void)snprintf(
                    load_result.error, sizeof(load_result.error),
                    "active %s Directory relay conflicts with static peers",
                    plane->name);
                break;
            }
        }
    }
    agent_recovery_record_reload(
        &recovery_state, AGENT_RECOVERY_STATIC_PEERS,
        result == PEER_TABLE_OK,
        result == PEER_TABLE_OK ? NULL : load_result.error);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "configuration_reloaded",
                   result == PEER_TABLE_OK);
    blobmsg_add_u8(&response, "peer_session_transport_implemented", true);
    if (result == PEER_TABLE_OK) {
        peer_table_swap(&peers, &candidate_peers);
        (void)agent_peer_routes_flush(&peer_routes);
        memset(snapshot_jobs, 0, sizeof(snapshot_jobs));
        static_peers_loaded = load_result.loaded;
        transport_reloaded = agent_peer_transport_reload(
            peer_transport, &peers, transport_error,
            sizeof(transport_error));
        listener_reloaded = agent_peer_listener_reload(
            peer_listener, &peers, listener_error,
            sizeof(listener_error));
        blobmsg_add_u64(&response, "configured_peers",
                        (uint64_t)static_peers_loaded);
    } else {
        blobmsg_add_string(&response, "error", load_result.error);
    }
    peer_table_destroy(&candidate_peers);
    blobmsg_add_u8(&response, "transport_reloaded", transport_reloaded);
    blobmsg_add_u8(&response, "listener_reloaded", listener_reloaded);
    if (result == PEER_TABLE_OK && !transport_reloaded) {
        blobmsg_add_string(&response, "transport_error", transport_error);
    }
    if (result == PEER_TABLE_OK && !listener_reloaded) {
        blobmsg_add_string(&response, "listener_error", listener_error);
    }
    blobmsg_add_u64(&response, "generation", peers.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static const struct agent_cross_candidate *cross_candidate_for_router(
    const char *router_id
)
{
    const struct agent_cross_candidate *candidate;

    for (candidate = agent_cross_table_first(&cross_discoveries);
         candidate != NULL; candidate = candidate->next) {
        if (strcmp(candidate->router_id, router_id) == 0) return candidate;
    }
    return NULL;
}

static int card_result_to_ubus(enum agent_card_result result)
{
    switch (result) {
    case AGENT_CARD_OK: return UBUS_STATUS_OK;
    case AGENT_CARD_NOT_FOUND: return UBUS_STATUS_NOT_FOUND;
    case AGENT_CARD_INVALID:
    case AGENT_CARD_SIGNATURE_REQUIRED:
    case AGENT_CARD_IDENTITY_MISMATCH:
    case AGENT_CARD_POLICY_REJECTED:
    case AGENT_CARD_EXPIRED:
    case AGENT_CARD_ROLLBACK:
    case AGENT_CARD_CONFLICT:
        return UBUS_STATUS_INVALID_ARGUMENT;
    case AGENT_CARD_FULL:
    case AGENT_CARD_NO_MEMORY:
    default:
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
}

static int card_trust_result_to_ubus(enum agent_card_trust_result result)
{
    switch (result) {
    case AGENT_CARD_TRUST_OK: return UBUS_STATUS_OK;
    case AGENT_CARD_TRUST_INVALID:
    case AGENT_CARD_TRUST_SIGNATURE_REQUIRED:
    case AGENT_CARD_TRUST_EXPIRED:
    case AGENT_CARD_TRUST_ROLLBACK:
    case AGENT_CARD_TRUST_CONFLICT:
    default:
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
}

static void reconcile_directory_trusted_cards(
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms
)
{
    const struct agent_card_record *record;
    char revoke[AGENT_CARD_TRUST_MAX_ENTRIES][AGENT_CROSS_ROUTER_ID_LEN];
    size_t count = 0U;
    size_t index;

    for (record = cards.head; record != NULL; record = record->next) {
        if (!record->document.directory_trusted) continue;
        if (agent_card_trust_authorize(
                &card_trust, &record->document, now_monotonic_ms,
                now_unix_ms, NULL)) continue;
        if (count >= AGENT_CARD_TRUST_MAX_ENTRIES) break;
        if (copy_text(revoke[count], sizeof(revoke[count]),
                      record->document.router_id)) count++;
    }
    for (index = 0U; index < count; index++) {
        (void)agent_card_revoke(&cards, revoke[index], card_peer_removing,
                                NULL);
    }
}

static bool parse_card_trust_document(
    struct blob_attr *message,
    struct agent_card_trust_document *document
)
{
    struct blob_attr *attributes[__CARD_TRUST_MAX];
    struct blob_attr *key_blob;
    uint64_t revision;
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;
    int remaining;

    memset(document, 0, sizeof(*document));
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(card_trust_policy_blob, __CARD_TRUST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[CARD_TRUST_DIRECTORY_ID] == NULL ||
        attributes[CARD_TRUST_DIGEST] == NULL ||
        !blobmsg_get_compatible_u64(attributes[CARD_TRUST_REVISION],
                                    &revision) ||
        !blobmsg_get_compatible_u64(attributes[CARD_TRUST_ISSUED_AT_MS],
                                    &issued_at_ms) ||
        !blobmsg_get_compatible_u64(attributes[CARD_TRUST_EXPIRES_AT_MS],
                                    &expires_at_ms) ||
        attributes[CARD_TRUST_KEYS] == NULL || revision == 0U ||
        !copy_text(document->directory_id, sizeof(document->directory_id),
                   blobmsg_get_string(attributes[CARD_TRUST_DIRECTORY_ID])) ||
        !copy_text(document->digest_sha256,
                   sizeof(document->digest_sha256),
                   blobmsg_get_string(attributes[CARD_TRUST_DIGEST]))) {
        return false;
    }
    document->revision = revision;
    document->issued_at_ms = issued_at_ms;
    document->expires_at_ms = expires_at_ms;
    document->signature_verified = true;
    blobmsg_for_each_attr(key_blob, attributes[CARD_TRUST_KEYS], remaining) {
        struct blob_attr *fields[__CARD_TRUST_KEY_MAX];
        struct agent_card_trust_entry *entry;
        uint64_t not_before_ms;
        uint64_t not_after_ms;

        if (blobmsg_type(key_blob) != BLOBMSG_TYPE_TABLE ||
            document->entry_count >= AGENT_CARD_TRUST_MAX_ENTRIES) {
            return false;
        }
        memset(fields, 0, sizeof(fields));
        blobmsg_parse(card_trust_key_policy, __CARD_TRUST_KEY_MAX, fields,
                      blobmsg_data(key_blob), blobmsg_len(key_blob));
        if (fields[CARD_TRUST_KEY_ROUTER_ID] == NULL ||
            fields[CARD_TRUST_KEY_DOMAIN_ID] == NULL ||
            fields[CARD_TRUST_KEY_ISSUER] == NULL ||
            fields[CARD_TRUST_KEY_ID] == NULL ||
            fields[CARD_TRUST_KEY_URI] == NULL ||
            fields[CARD_TRUST_KEY_SHA256] == NULL ||
            fields[CARD_TRUST_KEY_STATUS] == NULL ||
            !blobmsg_get_compatible_u64(
                fields[CARD_TRUST_KEY_NOT_BEFORE_MS], &not_before_ms) ||
            !blobmsg_get_compatible_u64(
                fields[CARD_TRUST_KEY_NOT_AFTER_MS], &not_after_ms)) {
            return false;
        }
        entry = &document->entries[document->entry_count];
        if (!copy_text(entry->router_id, sizeof(entry->router_id),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_ROUTER_ID])) ||
            !copy_text(entry->domain_id, sizeof(entry->domain_id),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_DOMAIN_ID])) ||
            !copy_text(entry->issuer, sizeof(entry->issuer),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_ISSUER])) ||
            !copy_text(entry->key_id, sizeof(entry->key_id),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_ID])) ||
            !copy_text(entry->public_key_uri,
                       sizeof(entry->public_key_uri),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_URI])) ||
            !copy_text(entry->public_key_sha256,
                       sizeof(entry->public_key_sha256),
                       blobmsg_get_string(fields[CARD_TRUST_KEY_SHA256])) ||
            !agent_card_trust_key_status_parse(
                blobmsg_get_string(fields[CARD_TRUST_KEY_STATUS]),
                &entry->status)) return false;
        entry->not_before_ms = not_before_ms;
        entry->not_after_ms = not_after_ms;
        document->entry_count++;
    }
    return document->entry_count > 0U;
}

static int agent_card_trust_ingest_verified(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct agent_card_trust_document document;
    enum agent_card_trust_result result;
    uint64_t now_ms = monotonic_ms();
    uint64_t wall_ms = unix_ms();

    (void)object;
    (void)method;
    if (card_policy.mode != AGENT_CARD_AUTH_DIRECTORY_TRUSTED) {
        return UBUS_STATUS_NOT_SUPPORTED;
    }
    if (!parse_card_trust_document(message, &document)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    result = agent_card_trust_replace(&card_trust, &document, now_ms,
                                      wall_ms);
    if (result != AGENT_CARD_TRUST_OK) {
        return card_trust_result_to_ubus(result);
    }
    reconcile_directory_trusted_cards(now_ms, wall_ms);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "accepted", true);
    blobmsg_add_u8(&response, "signature_verified", true);
    blobmsg_add_string(&response, "directory_id", document.directory_id);
    blobmsg_add_u64(&response, "revision", document.revision);
    blobmsg_add_u64(&response, "trust_generation", card_trust.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_card_trust_status(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    uint64_t now_ms = monotonic_ms();
    void *array;
    size_t index;

    (void)object;
    (void)method;
    (void)message;
    if (agent_card_trust_prune(&card_trust, now_ms)) {
        reconcile_directory_trusted_cards(now_ms, unix_ms());
    }
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "loaded", card_trust.loaded);
    blobmsg_add_u64(&response, "generation", card_trust.generation);
    if (card_trust.loaded) {
        blobmsg_add_string(&response, "directory_id",
                           card_trust.document.directory_id);
        blobmsg_add_string(&response, "digest_sha256",
                           card_trust.document.digest_sha256);
        blobmsg_add_u64(&response, "revision",
                        card_trust.document.revision);
        blobmsg_add_u64(&response, "remaining_ms",
                        card_trust.authorized_until_ms > now_ms
                            ? card_trust.authorized_until_ms - now_ms : 0U);
    }
    array = blobmsg_open_array(&response, "card_keys");
    for (index = 0U; card_trust.loaded &&
         index < card_trust.document.entry_count; index++) {
        const struct agent_card_trust_entry *entry =
            &card_trust.document.entries[index];
        void *item = blobmsg_open_table(&response, NULL);

        blobmsg_add_string(&response, "router_id", entry->router_id);
        blobmsg_add_string(&response, "domain_id", entry->domain_id);
        blobmsg_add_string(&response, "issuer", entry->issuer);
        blobmsg_add_string(&response, "key_id", entry->key_id);
        blobmsg_add_string(&response, "public_key_uri",
                           entry->public_key_uri);
        blobmsg_add_string(&response, "public_key_sha256",
                           entry->public_key_sha256);
        blobmsg_add_string(&response, "status",
                           agent_card_trust_key_status_name(entry->status));
        blobmsg_add_u64(&response, "not_before_ms", entry->not_before_ms);
        blobmsg_add_u64(&response, "not_after_ms", entry->not_after_ms);
        blobmsg_close_table(&response, item);
    }
    blobmsg_close_array(&response, array);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_cards(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__LIST_MAX];
    const struct agent_card_record *record;
    uint64_t now_ms = monotonic_ms();
    uint32_t limit = 100U;
    uint32_t emitted = 0U;
    void *array;
    void *item;
    void *capabilities;
    size_t index;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(list_policy, __LIST_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[LIST_LIMIT] != NULL) {
        limit = blobmsg_get_u32(attributes[LIST_LIMIT]);
    }
    if (limit == 0U || limit > AGENTD_MAX_LIST_LIMIT) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_card_prune(&cards, now_ms, card_peer_removing, NULL);
    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "authorization_mode",
                       agent_card_authorization_mode_name(card_policy.mode));
    blobmsg_add_u64(&response, "generation", cards.generation);
    array = blobmsg_open_array(&response, "cards");
    for (record = cards.head; record != NULL && emitted < limit;
         record = record->next) {
        item = blobmsg_open_table(&response, NULL);
        blobmsg_add_string(&response, "router_id",
                           record->document.router_id);
        blobmsg_add_string(&response, "domain_id",
                           record->document.domain_id);
        blobmsg_add_string(&response, "card_uri",
                           record->document.card_uri);
        blobmsg_add_string(&response, "issuer", record->document.issuer);
        blobmsg_add_string(&response, "key_id", record->document.key_id);
        blobmsg_add_u8(&response, "directory_trusted",
                       record->document.directory_trusted);
        blobmsg_add_string(&response, "digest_sha256",
                           record->document.digest_sha256);
        blobmsg_add_u64(&response, "revision", record->document.revision);
        blobmsg_add_u64(&response, "candidate_generation",
                        record->candidate_generation);
        blobmsg_add_u64(&response, "remaining_ms",
                        record->authorized_until_ms > now_ms
                            ? record->authorized_until_ms - now_ms : 0U);
        capabilities = blobmsg_open_array(&response, "capabilities");
        for (index = 0U; index < record->document.capability_count; index++) {
            const struct agent_card_capability *capability =
                &record->document.capabilities[index];
            void *capability_item = blobmsg_open_table(&response, NULL);

            blobmsg_add_string(&response, "skill_id", capability->skill_id);
            blobmsg_add_string(&response, "intent", capability->intent);
            blobmsg_add_u32(&response, "version", capability->version);
            blobmsg_add_string(&response, "tenant", capability->tenant);
            blobmsg_close_table(&response, capability_item);
        }
        blobmsg_close_array(&response, capabilities);
        blobmsg_close_table(&response, item);
        emitted++;
    }
    blobmsg_close_array(&response, array);
    blobmsg_add_u32(&response, "returned", emitted);
    blobmsg_add_u8(&response, "truncated", cards.count > emitted);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static bool parse_card_document(
    struct blob_attr *message,
    struct agent_card_document *document
)
{
    struct blob_attr *attributes[__CARD_MAX];
    struct blob_attr *capability_blob;
    int remaining;
    uint64_t revision;
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;

    memset(document, 0, sizeof(*document));
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(card_policy_blob, __CARD_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[CARD_ROUTER_ID] == NULL ||
        attributes[CARD_DOMAIN_ID] == NULL || attributes[CARD_URI] == NULL ||
        attributes[CARD_ARPX_ENDPOINT] == NULL ||
        attributes[CARD_ISSUER] == NULL || attributes[CARD_KEY_ID] == NULL ||
        attributes[CARD_DIGEST] == NULL ||
        !blobmsg_get_compatible_u64(attributes[CARD_REVISION], &revision) ||
        !blobmsg_get_compatible_u64(attributes[CARD_ISSUED_AT_MS],
                                    &issued_at_ms) ||
        !blobmsg_get_compatible_u64(attributes[CARD_EXPIRES_AT_MS],
                                    &expires_at_ms) ||
        attributes[CARD_CAPABILITIES] == NULL || revision == 0U) {
        return false;
    }
    if (!copy_text(document->router_id, sizeof(document->router_id),
                   blobmsg_get_string(attributes[CARD_ROUTER_ID])) ||
        !copy_text(document->domain_id, sizeof(document->domain_id),
                   blobmsg_get_string(attributes[CARD_DOMAIN_ID])) ||
        !copy_text(document->card_uri, sizeof(document->card_uri),
                   blobmsg_get_string(attributes[CARD_URI])) ||
        !copy_text(document->arpx_endpoint,
                   sizeof(document->arpx_endpoint),
                   blobmsg_get_string(attributes[CARD_ARPX_ENDPOINT])) ||
        !copy_text(document->issuer, sizeof(document->issuer),
                   blobmsg_get_string(attributes[CARD_ISSUER])) ||
        !copy_text(document->key_id, sizeof(document->key_id),
                   blobmsg_get_string(attributes[CARD_KEY_ID])) ||
        !copy_text(document->digest_sha256,
                   sizeof(document->digest_sha256),
                   blobmsg_get_string(attributes[CARD_DIGEST]))) {
        return false;
    }
    document->revision = revision;
    document->issued_at_ms = issued_at_ms;
    document->expires_at_ms = expires_at_ms;
    document->signature_verified = true;
    document->directory_trusted = false;
    document->directory_authorized_until_ms = 0U;
    blobmsg_for_each_attr(capability_blob, attributes[CARD_CAPABILITIES],
                          remaining) {
        struct blob_attr *fields[__CARD_CAPABILITY_MAX];
        struct agent_card_capability *capability;

        if (blobmsg_type(capability_blob) != BLOBMSG_TYPE_TABLE ||
            document->capability_count >= AGENT_CARD_MAX_CAPABILITIES) {
            return false;
        }
        memset(fields, 0, sizeof(fields));
        blobmsg_parse(card_capability_policy, __CARD_CAPABILITY_MAX, fields,
                      blobmsg_data(capability_blob),
                      blobmsg_len(capability_blob));
        if (fields[CARD_CAPABILITY_SKILL_ID] == NULL ||
            fields[CARD_CAPABILITY_INTENT] == NULL ||
            fields[CARD_CAPABILITY_VERSION] == NULL ||
            fields[CARD_CAPABILITY_TENANT] == NULL) return false;
        capability = &document->capabilities[document->capability_count];
        if (!copy_text(capability->skill_id, sizeof(capability->skill_id),
                       blobmsg_get_string(fields[CARD_CAPABILITY_SKILL_ID])) ||
            !copy_text(capability->intent, sizeof(capability->intent),
                       blobmsg_get_string(fields[CARD_CAPABILITY_INTENT])) ||
            !copy_text(capability->tenant, sizeof(capability->tenant),
                       blobmsg_get_string(fields[CARD_CAPABILITY_TENANT]))) {
            return false;
        }
        capability->version = blobmsg_get_u32(
            fields[CARD_CAPABILITY_VERSION]);
        document->capability_count++;
    }
    return document->capability_count > 0U;
}

static int agent_card_ingest_verified(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct agent_card_document document;
    const struct agent_cross_candidate *candidate;
    enum agent_card_result result;
    enum agent_promotion_result promotion_result;
    uint64_t now_ms = monotonic_ms();

    (void)object;
    (void)method;
    if (card_policy.mode == AGENT_CARD_AUTH_OFF) {
        return UBUS_STATUS_NOT_SUPPORTED;
    }
    if (!parse_card_document(message, &document)) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    (void)agent_card_prune(&cards, now_ms, card_peer_removing, NULL);
    candidate = cross_candidate_for_router(document.router_id);
    if (candidate == NULL) return UBUS_STATUS_NOT_FOUND;
    if (card_policy.mode == AGENT_CARD_AUTH_DIRECTORY_TRUSTED) {
        if (!agent_card_trust_authorize(
                &card_trust, &document, now_ms, unix_ms(),
                &document.directory_authorized_until_ms)) {
            return UBUS_STATUS_PERMISSION_DENIED;
        }
        document.directory_trusted = true;
    }
    result = agent_card_ingest(
        &cards, &card_policy, candidate, &document,
        cross_discoveries.generation, now_ms, unix_ms());
    if (result != AGENT_CARD_OK) return card_result_to_ubus(result);
    promotion_result = agent_discovery_promote_svcb_auto(
        &promotions, &peers, &cross_discoveries, document.router_id,
        cross_discoveries.generation, config.auto_promotion_grace_seconds,
        now_ms);
    if (promotion_result != AGENT_PROMOTION_OK) {
        (void)agent_card_revoke(&cards, document.router_id, NULL, NULL);
        return promotion_result_to_ubus(promotion_result);
    }
    if (!reload_peer_runtime()) {
        promotion_peer_removing(NULL, document.router_id);
        (void)agent_discovery_promotion_remove(
            &promotions, &peers, document.router_id);
        (void)agent_card_revoke(&cards, document.router_id, NULL, NULL);
        (void)reload_peer_runtime();
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "accepted", true);
    blobmsg_add_u8(&response, "signature_verified", true);
    blobmsg_add_u8(&response, "directory_trusted",
                   document.directory_trusted);
    blobmsg_add_u8(&response, "automatic_peer_created", true);
    blobmsg_add_string(&response, "router_id", document.router_id);
    blobmsg_add_u64(&response, "card_generation", cards.generation);
    blobmsg_add_u64(&response, "promotion_generation",
                    promotions.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int agent_card_revoke_method(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    struct blob_attr *attributes[__CARD_ID_MAX];
    enum agent_card_result result;
    const char *router_id;

    (void)object;
    (void)method;
    memset(attributes, 0, sizeof(attributes));
    blobmsg_parse(card_id_policy, __CARD_ID_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[CARD_ID_ROUTER_ID] == NULL) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    router_id = blobmsg_get_string(attributes[CARD_ID_ROUTER_ID]);
    result = agent_card_revoke(
        &cards, router_id, card_peer_removing, NULL);
    if (result != AGENT_CARD_OK) return card_result_to_ubus(result);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "revoked", true);
    blobmsg_add_string(&response, "router_id", router_id);
    blobmsg_add_u64(&response, "card_generation", cards.generation);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static const struct ubus_method agent_methods[] = {
    UBUS_METHOD("register", agent_register, register_policy),
    UBUS_METHOD("renew", agent_renew, renew_policy),
    UBUS_METHOD("unregister", agent_unregister, id_policy),
    UBUS_METHOD("lookup", agent_lookup, lookup_policy),
    UBUS_METHOD("route", agent_lookup, lookup_policy),
    UBUS_METHOD_NOARG("policy", agent_policy_status),
    UBUS_METHOD("routes", agent_routes, list_policy),
    UBUS_METHOD("agents", agent_local_agents, list_policy),
    UBUS_METHOD_NOARG("manifests", agent_manifests),
    UBUS_METHOD_NOARG("addresses", agent_public_ipv6_addresses),
    UBUS_METHOD("neighbors", agent_neighbors, list_policy),
    UBUS_METHOD("discoveries", agent_discoveries, list_policy),
    UBUS_METHOD("cross_discoveries", agent_cross_discoveries, list_policy),
    UBUS_METHOD("promotions", agent_promotions, list_policy),
    UBUS_METHOD("cards", agent_cards, list_policy),
    UBUS_METHOD_NOARG("card_trust", agent_card_trust_status),
    UBUS_METHOD("card_trust_ingest_verified",
                agent_card_trust_ingest_verified, card_trust_policy_blob),
    UBUS_METHOD("card_ingest_verified", agent_card_ingest_verified,
                card_policy_blob),
    UBUS_METHOD("card_revoke", agent_card_revoke_method, card_id_policy),
    UBUS_METHOD_NOARG("relay", agent_relay_status),
    UBUS_METHOD_NOARG("open_mesh_relay", agent_open_mesh_relay_status),
    UBUS_METHOD_NOARG("stats", agent_stats),
    UBUS_METHOD_NOARG("recovery", agent_recovery_status_method),
    UBUS_METHOD_NOARG("reload", agent_reload),
    UBUS_METHOD_NOARG("reload_policy", agent_reload_policy),
    UBUS_METHOD_NOARG("reload_peers", agent_reload_peers),
    UBUS_METHOD_NOARG("discovery_refresh", agent_discovery_refresh),
    UBUS_METHOD_NOARG("cross_discovery_refresh",
                      agent_cross_discovery_refresh),
    UBUS_METHOD("discovery_promote", agent_discovery_promote,
                promote_policy),
    UBUS_METHOD("discovery_revoke", agent_discovery_revoke,
                promotion_id_policy),
    UBUS_METHOD_NOARG("relay_refresh", agent_relay_refresh),
    UBUS_METHOD_NOARG("open_mesh_relay_refresh",
                      agent_open_mesh_relay_refresh)
};

static struct ubus_object_type agent_object_type =
    UBUS_OBJECT_TYPE("agent", agent_methods);

static struct ubus_object agent_object = {
    .name = "agent",
    .type = &agent_object_type,
    .methods = agent_methods,
    .n_methods = ARRAY_SIZE(agent_methods)
};

static void prune_expired_routes(struct uloop_timeout *timeout)
{
    uint64_t now_ms = monotonic_ms();

    (void)timeout;
    (void)agent_discovery_table_prune(&discoveries, now_ms);
    (void)agent_cross_table_prune(&cross_discoveries, now_ms);
    (void)agent_card_prune(&cards, now_ms, card_peer_removing, NULL);
    if (agent_card_trust_prune(&card_trust, now_ms)) {
        reconcile_directory_trusted_cards(now_ms, unix_ms());
    }
    (void)reconcile_promotions(now_ms);
    (void)agent_public_ipv6_prune(
        &public_ipv6_pool, now_ms, public_ipv6_netd_release_cb, NULL);
    (void)agent_peer_routes_prune(&peer_routes, now_ms);
    (void)prune_dynamic_manifests();
    uloop_timeout_set(&prune_timer, (int)config.prune_interval_ms);
}

static void pump_snapshot_timer(struct uloop_timeout *timeout)
{
    (void)timeout;
    pump_snapshot_exports();
    uloop_timeout_set(&snapshot_timer,
                      (int)AGENTD_SNAPSHOT_PUMP_INTERVAL_MS);
}

static void poll_discovery_timer(struct uloop_timeout *timeout)
{
    (void)timeout;
    if (refresh_discoveries()) {
        (void)apply_auto_promotions(monotonic_ms());
    }
    uloop_timeout_set(&discovery_timer, (int)config.discovery_poll_ms);
}

static void poll_cross_discovery_timer(struct uloop_timeout *timeout)
{
    (void)timeout;
    (void)agent_cross_table_prune(&cross_discoveries, monotonic_ms());
    (void)refresh_cross_discoveries();
    uloop_timeout_set(&cross_discovery_timer,
                      (int)config.cross_poll_ms);
}

static void stop_signal(int signal_number)
{
    (void)signal_number;
    uloop_end();
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [-m max_routes] [-d default_lease_seconds] "
            "[-l min_lease_seconds] [-L max_lease_seconds] "
            "[-p prune_interval_ms] [-s ipc_socket] "
            "[-u ipc_allowed_uid] [-T peer_transport_enabled] "
            "[-0 open_mesh_enabled] "
            "[-R router_id] [-D domain_id] [-C peer_ca_file] "
            "[-X peer_client_cert] [-K peer_client_key] "
            "[-H heartbeat_ms] [-c connect_timeout_ms] "
            "[-o open_timeout_ms] [-M miss_limit] "
            "[-b initial_backoff_ms] [-B max_backoff_ms] "
            "[-t peer_tick_ms] [-I peer_listener_enabled] "
            "[-F reflector_enabled] "
            "[-A peer_listen_ipv4] [-P peer_listen_port] "
            "[-N peer_max_inbound] [-Q discovery_enabled] "
            "[-q discovery_poll_ms] [-n max_discoveries] "
            "[-e discovery_ttl_cap_seconds] "
            "[-O lan_auto_promotion_mode] "
            "[-W lan_auto_promotion_allowlist] "
            "[-k lan_auto_promotion_grace_seconds] "
            "[-G cross_discovery_enabled] [-g discovery_domain] "
            "[-V resolver_ipv4] [-v resolver_port] "
            "[-J cross_poll_ms] [-j cross_timeout_ms] "
            "[-Y max_cross_discoveries] "
            "[-y cross_ttl_cap_seconds] "
            "[-S card_authorization_mode] "
            "[-r relay_enabled] [-E directory_endpoint] "
            "[-a optional_directory_connect_ipv4] [-w relay_poll_ms] "
            "[-x relay_timeout_ms] [-z relay_tunnel_enabled] "
            "[-f forwarding_assertion_required] "
            "[-U forwarding_public_key] [-Z forwarding_kid] "
            "[-i forwarding_issuer] [-9 forwarding_source_router_id] "
            "[-6 public_ipv6_prefix] "
            "[-7 max_public_ipv6] [-8 upstream_ndp_relay]\n",
            program);
}

static bool read_options(int argc, char **argv)
{
    int option;
    uint32_t value;
    struct in_addr resolver_address;
    struct agent_invoke_endpoint internal_gateway_endpoint;
    const char *resolver_detected;
    const char *relay_device_token_file;
    const char *relay_config_generation;
    const char *relay_directory_ca_file;
    const char *relay_gateway_enabled;
    const char *relay_gateway_endpoint;
    const char *relay_gateway_token_file;
    const char *open_mesh_relay_enabled;
    const char *open_mesh_directory_endpoints;
    const char *open_mesh_directory_connect_ipv4s;
    const char *open_mesh_directory_poll_ms;
    const char *open_mesh_directory_timeout_ms;

    config.max_routes = AGENTD_DEFAULT_MAX_ROUTES;
    config.default_lease_seconds = AGENTD_DEFAULT_LEASE_SECONDS;
    config.min_lease_seconds = AGENTD_MIN_LEASE_SECONDS;
    config.max_lease_seconds = AGENTD_MAX_LEASE_SECONDS;
    config.prune_interval_ms = AGENTD_DEFAULT_PRUNE_INTERVAL_MS;
    config.ipc_allowed_uid = (uid_t)AGENTD_DEFAULT_IPC_ALLOWED_UID;
    snprintf(config.ipc_socket, sizeof(config.ipc_socket), "%s",
             AGENTD_DEFAULT_IPC_SOCKET);
    config.peer_transport.enabled = false;
    config.peer_transport.open_mesh = false;
    config.peer_transport.deterministic_roles = false;
    config.peer_transport.relay_tunnel_enabled = false;
    config.peer_listener.enabled = false;
    config.reflector_enabled = false;
    config.discovery_enabled = false;
    config.discovery_poll_ms = AGENTD_DEFAULT_DISCOVERY_POLL_MS;
    config.discovery_ttl_cap_seconds =
        AGENTD_DEFAULT_DISCOVERY_TTL_CAP_SECONDS;
    config.max_discoveries = AGENTD_DEFAULT_MAX_DISCOVERIES;
    snprintf(config.auto_promotion_mode,
             sizeof(config.auto_promotion_mode), "%s", "off");
    config.auto_promotion_allowlist[0] = '\0';
    config.auto_promotion_grace_seconds =
        AGENTD_DEFAULT_PROMOTION_GRACE_SECONDS;
    config.cross_discovery_enabled = false;
    snprintf(config.cross_discovery_domain,
             sizeof(config.cross_discovery_domain), "%s",
             AGENTD_DEFAULT_CROSS_DOMAIN);
    snprintf(config.cross_resolver_ipv4,
             sizeof(config.cross_resolver_ipv4), "%s",
             AGENTD_DEFAULT_CROSS_RESOLVER_IPV4);
    config.cross_resolver_port = AGENTD_DEFAULT_CROSS_RESOLVER_PORT;
    resolver_detected = getenv("NEXUS_CROSS_RESOLVER_DETECTED");
    config.cross_resolver_detected = resolver_detected == NULL ||
        strcmp(resolver_detected, "0") != 0;
    config.cross_poll_ms = AGENTD_DEFAULT_CROSS_POLL_MS;
    config.cross_timeout_ms = AGENTD_DEFAULT_CROSS_TIMEOUT_MS;
    config.cross_ttl_cap_seconds =
        AGENTD_DEFAULT_CROSS_TTL_CAP_SECONDS;
    config.max_cross_discoveries =
        AGENTD_DEFAULT_MAX_CROSS_DISCOVERIES;
    snprintf(config.card_authorization_mode,
             sizeof(config.card_authorization_mode), "%s", "off");
    config.card_authorization_allowlist[0] = '\0';
    config.relay.enabled = false;
    snprintf(config.relay.directory_endpoint,
             sizeof(config.relay.directory_endpoint), "%s",
             AGENTD_DEFAULT_RELAY_DIRECTORY_ENDPOINT);
    snprintf(config.relay.directory_connect_ipv4,
             sizeof(config.relay.directory_connect_ipv4), "%s",
             AGENTD_DEFAULT_RELAY_DIRECTORY_IPV4);
    config.relay.poll_ms = AGENTD_DEFAULT_RELAY_POLL_MS;
    config.relay.timeout_ms = AGENTD_DEFAULT_RELAY_TIMEOUT_MS;
    config.relay.device_token_file[0] = '\0';
    config.relay.ca_file[0] = '\0';
    config.open_mesh_relay.enabled = false;
    config.open_mesh_relay.directory_endpoint[0] = '\0';
    config.open_mesh_relay.directory_connect_ipv4[0] = '\0';
    config.open_mesh_relay.poll_ms = AGENTD_DEFAULT_RELAY_POLL_MS;
    config.open_mesh_relay.timeout_ms = AGENTD_DEFAULT_RELAY_TIMEOUT_MS;
    config.open_mesh_relay.device_token_file[0] = '\0';
    config.open_mesh_relay.ca_file[0] = '\0';
    config.relay_config_generation[0] = '\0';
    config.relay_gateway_internal_enabled = false;
    snprintf(config.relay_gateway_internal_endpoint,
             sizeof(config.relay_gateway_internal_endpoint), "%s",
             "http://127.0.0.1:7788/agent/v1/internal-invoke");
    snprintf(config.relay_gateway_token_file,
             sizeof(config.relay_gateway_token_file), "%s",
             "/etc/agentd/nexus-cloud-gateway.token");
    relay_config_generation = getenv("NEXUS_RELAY_CONFIG_GENERATION");
    if (relay_config_generation != NULL &&
        relay_config_generation[0] != '\0') {
        size_t index;

        if (strlen(relay_config_generation) != 64U) {
            fprintf(stderr,
                    "agentd: Relay configuration generation is invalid\n");
            return false;
        }
        for (index = 0U; index < 64U; index++) {
            if (!isxdigit((unsigned char)relay_config_generation[index]) ||
                isupper((unsigned char)relay_config_generation[index])) {
                fprintf(stderr,
                        "agentd: Relay configuration generation is invalid\n");
                return false;
            }
        }
        if (!copy_text(config.relay_config_generation,
                       sizeof(config.relay_config_generation),
                       relay_config_generation)) return false;
    }
    relay_device_token_file = getenv("NEXUS_RELAY_DEVICE_TOKEN_FILE");
    if (relay_device_token_file != NULL &&
        relay_device_token_file[0] != '\0' &&
        !copy_text(config.relay.device_token_file,
                   sizeof(config.relay.device_token_file),
                   relay_device_token_file)) {
        fprintf(stderr,
                "agentd: Relay device token file path is invalid\n");
        return false;
    }
    relay_directory_ca_file = getenv("NEXUS_RELAY_DIRECTORY_CA_FILE");
    if (relay_directory_ca_file != NULL &&
        relay_directory_ca_file[0] != '\0' &&
        !copy_text(config.relay.ca_file,
                   sizeof(config.relay.ca_file),
                   relay_directory_ca_file)) {
        fprintf(stderr,
                "agentd: Relay Directory CA file path is invalid\n");
        return false;
    }
    relay_gateway_enabled = getenv("NEXUS_RELAY_GATEWAY_INTERNAL_ENABLED");
    if (relay_gateway_enabled != NULL &&
        relay_gateway_enabled[0] != '\0') {
        if (strcmp(relay_gateway_enabled, "1") == 0) {
            config.relay_gateway_internal_enabled = true;
        } else if (strcmp(relay_gateway_enabled, "0") != 0) {
            fprintf(stderr,
                    "agentd: internal Gateway enable flag is invalid\n");
            return false;
        }
    }
    relay_gateway_endpoint = getenv("NEXUS_RELAY_GATEWAY_INTERNAL_ENDPOINT");
    if (relay_gateway_endpoint != NULL &&
        relay_gateway_endpoint[0] != '\0' &&
        !copy_text(config.relay_gateway_internal_endpoint,
                   sizeof(config.relay_gateway_internal_endpoint),
                   relay_gateway_endpoint)) {
        fprintf(stderr,
                "agentd: internal Gateway endpoint is invalid\n");
        return false;
    }
    relay_gateway_token_file = getenv("NEXUS_RELAY_GATEWAY_TOKEN_FILE");
    if (relay_gateway_token_file != NULL &&
        relay_gateway_token_file[0] != '\0' &&
        !copy_text(config.relay_gateway_token_file,
                   sizeof(config.relay_gateway_token_file),
                   relay_gateway_token_file)) {
        fprintf(stderr,
                "agentd: internal Gateway credential path is invalid\n");
        return false;
    }
    open_mesh_relay_enabled =
        getenv("NEXUS_OPEN_MESH_RELAY_ENABLED");
    if (open_mesh_relay_enabled != NULL &&
        open_mesh_relay_enabled[0] != '\0') {
        if (strcmp(open_mesh_relay_enabled, "1") == 0) {
            config.open_mesh_relay.enabled = true;
        } else if (strcmp(open_mesh_relay_enabled, "0") != 0) {
            fprintf(stderr,
                    "agentd: Open Mesh Relay enable flag is invalid\n");
            return false;
        }
    }
    open_mesh_directory_endpoints =
        getenv("NEXUS_OPEN_MESH_DIRECTORY_ENDPOINTS");
    const char *mesh_profile_json = getenv("NEXUS_OPEN_MESH_JOIN_PROFILE");
    if (mesh_profile_json && mesh_profile_json[0] && !copy_text(config.open_mesh_relay.mesh_profile_json,
            sizeof(config.open_mesh_relay.mesh_profile_json), mesh_profile_json)) return false;
    if (open_mesh_directory_endpoints != NULL &&
        open_mesh_directory_endpoints[0] != '\0' &&
        !copy_text(config.open_mesh_relay.directory_endpoint,
                   sizeof(config.open_mesh_relay.directory_endpoint),
                   open_mesh_directory_endpoints)) {
        fprintf(stderr,
                "agentd: Open Mesh Directory endpoint set is invalid\n");
        return false;
    }
    open_mesh_directory_connect_ipv4s =
        getenv("NEXUS_OPEN_MESH_DIRECTORY_CONNECT_IPV4S");
    if (open_mesh_directory_connect_ipv4s != NULL &&
        open_mesh_directory_connect_ipv4s[0] != '\0' &&
        !copy_text(config.open_mesh_relay.directory_connect_ipv4,
                   sizeof(config.open_mesh_relay.directory_connect_ipv4),
                   open_mesh_directory_connect_ipv4s)) {
        fprintf(stderr,
                "agentd: Open Mesh Directory IPv4 set is invalid\n");
        return false;
    }
    open_mesh_directory_poll_ms =
        getenv("NEXUS_OPEN_MESH_DIRECTORY_POLL_MS");
    if (open_mesh_directory_poll_ms != NULL &&
        open_mesh_directory_poll_ms[0] != '\0' &&
        !parse_u32(open_mesh_directory_poll_ms,
                   &config.open_mesh_relay.poll_ms)) {
        fprintf(stderr, "agentd: Open Mesh poll interval is invalid\n");
        return false;
    }
    open_mesh_directory_timeout_ms =
        getenv("NEXUS_OPEN_MESH_DIRECTORY_TIMEOUT_MS");
    if (open_mesh_directory_timeout_ms != NULL &&
        open_mesh_directory_timeout_ms[0] != '\0' &&
        !parse_u32(open_mesh_directory_timeout_ms,
                   &config.open_mesh_relay.timeout_ms)) {
        fprintf(stderr, "agentd: Open Mesh timeout is invalid\n");
        return false;
    }
    snprintf(config.peer_listener.listen_ipv4,
             sizeof(config.peer_listener.listen_ipv4), "%s", "0.0.0.0");
    config.peer_listener.listen_port = 7444U;
    config.peer_listener.max_connections = 8U;
    snprintf(config.peer_transport.local_router_id,
             sizeof(config.peer_transport.local_router_id), "%s",
             AGENTD_DEFAULT_ROUTER_ID);
    snprintf(config.peer_transport.local_domain_id,
             sizeof(config.peer_transport.local_domain_id), "%s",
             AGENTD_DEFAULT_DOMAIN_ID);
    snprintf(config.peer_transport.ca_file,
             sizeof(config.peer_transport.ca_file), "%s",
             AGENTD_DEFAULT_PEER_CA_FILE);
    snprintf(config.peer_transport.client_cert_file,
             sizeof(config.peer_transport.client_cert_file), "%s",
             AGENTD_DEFAULT_PEER_CERT_FILE);
    snprintf(config.peer_transport.client_key_file,
             sizeof(config.peer_transport.client_key_file), "%s",
             AGENTD_DEFAULT_PEER_KEY_FILE);
    config.peer_transport.heartbeat_ms = 5000U;
    config.peer_transport.connect_timeout_ms = 5000U;
    config.peer_transport.open_timeout_ms = 5000U;
    config.peer_transport.heartbeat_miss_limit = 3U;
    config.peer_transport.initial_backoff_ms = 250U;
    config.peer_transport.max_backoff_ms = 30000U;
    config.peer_transport.tick_ms = 100U;
    config.forwarding_assertion_required = false;
    config.public_ipv6_prefix[0] = '\0';
    config.max_public_ipv6 = AGENTD_DEFAULT_MAX_PUBLIC_IPV6;
    config.public_ipv6_upstream_relay = false;
    snprintf(config.forwarding_public_key,
             sizeof(config.forwarding_public_key), "%s",
             "/etc/agentd/forwarding-public.pem");
    snprintf(config.forwarding_key_id,
             sizeof(config.forwarding_key_id), "%s", "router-default");
    snprintf(config.forwarding_issuer,
             sizeof(config.forwarding_issuer), "%s", "router-source");
    config.forwarding_source_router_id[0] = '\0';

    while ((option = getopt(argc, argv,
                            "m:d:l:L:p:s:u:0:T:R:D:C:X:K:H:c:o:M:b:B:t:I:F:A:P:N:Q:q:n:e:O:W:k:G:g:V:v:J:j:Y:y:S:r:E:a:w:x:z:f:U:Z:i:9:6:7:8:h")) != -1) {
        if (option == '?' || option == ':') {
            fprintf(stderr,
                    "agentd: rejected unknown or incomplete option near argv[%d]\n",
                    optind > 0 ? optind - 1 : 0);
            return false;
        }
        if (option == 'h') {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (option == 's') {
            if (!copy_text(config.ipc_socket, sizeof(config.ipc_socket),
                           optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'g') {
            if (!copy_text(config.cross_discovery_domain,
                           sizeof(config.cross_discovery_domain), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'O') {
            if (!copy_text(config.auto_promotion_mode,
                           sizeof(config.auto_promotion_mode), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'S') {
            const char *mode = optarg;

            config.card_authorization_allowlist[0] = '\0';
            if (strncmp(optarg, "allowlist=", 10U) == 0) {
                mode = "allowlist";
                if (!copy_text(config.card_authorization_allowlist,
                               sizeof(config.card_authorization_allowlist),
                               optarg + 10U)) return false;
            }
            if (!copy_text(config.card_authorization_mode,
                           sizeof(config.card_authorization_mode), mode)) {
                return false;
            }
            continue;
        }
        if (option == 'W') {
            int written = snprintf(
                config.auto_promotion_allowlist,
                sizeof(config.auto_promotion_allowlist), "%s", optarg);

            if (written < 0 || (size_t)written >=
                    sizeof(config.auto_promotion_allowlist)) return false;
            continue;
        }
        if (option == 'V') {
            if (!copy_text(config.cross_resolver_ipv4,
                           sizeof(config.cross_resolver_ipv4), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'E') {
            if (!copy_text(config.relay.directory_endpoint,
                           sizeof(config.relay.directory_endpoint), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'U') {
            if (!copy_text(config.forwarding_public_key,
                           sizeof(config.forwarding_public_key), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'Z') {
            if (!copy_text(config.forwarding_key_id,
                           sizeof(config.forwarding_key_id), optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'i') {
            if (!copy_text(config.forwarding_issuer,
                           sizeof(config.forwarding_issuer), optarg)) {
                return false;
            }
            continue;
        }
        if (option == '9') {
            if (!copy_text(config.forwarding_source_router_id,
                           sizeof(config.forwarding_source_router_id),
                           optarg)) {
                return false;
            }
            continue;
        }
        if (option == '6') {
            if (!copy_text(config.public_ipv6_prefix,
                           sizeof(config.public_ipv6_prefix), optarg))
                return false;
            continue;
        }
        if (option == 'a') {
            if (!copy_text(config.relay.directory_connect_ipv4,
                           sizeof(config.relay.directory_connect_ipv4),
                           optarg)) {
                return false;
            }
            continue;
        }
        if (option == 'R' || option == 'D' || option == 'C' ||
            option == 'X' || option == 'K' || option == 'A') {
            char *target = option == 'R'
                ? config.peer_transport.local_router_id
                : option == 'D' ? config.peer_transport.local_domain_id
                : option == 'C' ? config.peer_transport.ca_file
                : option == 'X' ? config.peer_transport.client_cert_file
                : option == 'K' ? config.peer_transport.client_key_file
                                : config.peer_listener.listen_ipv4;
            size_t capacity = option == 'R'
                ? sizeof(config.peer_transport.local_router_id)
                : option == 'D' ? sizeof(config.peer_transport.local_domain_id)
                : option == 'C' ? sizeof(config.peer_transport.ca_file)
                : option == 'X'
                    ? sizeof(config.peer_transport.client_cert_file)
                : option == 'K'
                    ? sizeof(config.peer_transport.client_key_file)
                    : sizeof(config.peer_listener.listen_ipv4);

            if (!copy_text(target, capacity, optarg)) {
                return false;
            }
            continue;
        }
        if (!parse_u32(optarg, &value)) {
            fprintf(stderr,
                    "agentd: option -%c requires an unsigned integer\n",
                    option);
            return false;
        }

        switch (option) {
        case '0':
            if (value > 1U) return false;
            config.peer_transport.open_mesh = value == 1U;
            break;
        case '8':
            if (value > 1U) return false;
            config.public_ipv6_upstream_relay = value == 1U;
            break;
        case '7':
            if (value == 0U || value > 65535U) return false;
            config.max_public_ipv6 = (size_t)value;
            break;
        case 'm':
            if (value == 0U || value > 1000000U) {
                return false;
            }
            config.max_routes = (size_t)value;
            break;
        case 'd':
            config.default_lease_seconds = value;
            break;
        case 'l':
            config.min_lease_seconds = value;
            break;
        case 'L':
            config.max_lease_seconds = value;
            break;
        case 'p':
            if (value < 100U || value > 60000U) {
                return false;
            }
            config.prune_interval_ms = value;
            break;
        case 'u':
            config.ipc_allowed_uid = (uid_t)value;
            if ((uint32_t)config.ipc_allowed_uid != value) {
                return false;
            }
            break;
        case 'T':
            if (value > 1U) {
                return false;
            }
            config.peer_transport.enabled = value == 1U;
            break;
        case 'H':
            config.peer_transport.heartbeat_ms = value;
            break;
        case 'c':
            config.peer_transport.connect_timeout_ms = value;
            break;
        case 'o':
            config.peer_transport.open_timeout_ms = value;
            break;
        case 'M':
            config.peer_transport.heartbeat_miss_limit = value;
            break;
        case 'b':
            config.peer_transport.initial_backoff_ms = value;
            break;
        case 'B':
            config.peer_transport.max_backoff_ms = value;
            break;
        case 't':
            config.peer_transport.tick_ms = value;
            break;
        case 'I':
            if (value > 1U) {
                return false;
            }
            config.peer_listener.enabled = value == 1U;
            break;
        case 'F':
            if (value > 1U) {
                return false;
            }
            config.reflector_enabled = value == 1U;
            break;
        case 'P':
            if (value == 0U || value > UINT16_MAX) {
                return false;
            }
            config.peer_listener.listen_port = (uint16_t)value;
            break;
        case 'N':
            if (value == 0U || value > 64U) {
                return false;
            }
            config.peer_listener.max_connections = value;
            break;
        case 'Q':
            if (value > 1U) {
                return false;
            }
            config.discovery_enabled = value == 1U;
            break;
        case 'q':
            if (value < 1000U || value > 60000U) {
                return false;
            }
            config.discovery_poll_ms = value;
            break;
        case 'n':
            if (value == 0U || value > 256U) {
                return false;
            }
            config.max_discoveries = (size_t)value;
            break;
        case 'e':
            if (value < AGENT_DISCOVERY_MIN_TTL_SECONDS ||
                value > AGENT_DISCOVERY_MAX_TTL_SECONDS) {
                return false;
            }
            config.discovery_ttl_cap_seconds = value;
            break;
        case 'k':
            if (value < AGENTD_MIN_LEASE_SECONDS ||
                value > AGENTD_MAX_PROMOTION_GRACE_SECONDS) return false;
            config.auto_promotion_grace_seconds = value;
            break;
        case 'G':
            if (value > 1U) {
                return false;
            }
            config.cross_discovery_enabled = value == 1U;
            break;
        case 'v':
            if (value == 0U || value > UINT16_MAX) {
                return false;
            }
            config.cross_resolver_port = (uint16_t)value;
            break;
        case 'J':
            if (value < 5000U || value > 3600000U) {
                return false;
            }
            config.cross_poll_ms = value;
            break;
        case 'j':
            if (value < 100U || value > 10000U) {
                return false;
            }
            config.cross_timeout_ms = value;
            break;
        case 'Y':
            if (value == 0U || value > 128U) {
                return false;
            }
            config.max_cross_discoveries = (size_t)value;
            break;
        case 'y':
            if (value < AGENT_CROSS_MIN_TTL_SECONDS ||
                value > AGENT_CROSS_MAX_TTL_SECONDS) {
                return false;
            }
            config.cross_ttl_cap_seconds = value;
            break;
        case 'r':
            if (value > 1U) {
                return false;
            }
            config.relay.enabled = value == 1U;
            break;
        case 'w':
            if (value < 5000U || value > 3600000U) {
                return false;
            }
            config.relay.poll_ms = value;
            break;
        case 'x':
            if (value < 100U || value > 30000U) {
                return false;
            }
            config.relay.timeout_ms = value;
            break;
        case 'z':
            if (value > 1U) {
                return false;
            }
            config.peer_transport.relay_tunnel_enabled = value == 1U;
            break;
        case 'f':
            if (value > 1U) return false;
            config.forwarding_assertion_required = value == 1U;
            break;
        default:
            return false;
        }
    }

    config.peer_transport.deterministic_roles =
        config.peer_listener.enabled;
    config.peer_listener.session = config.peer_transport;
    (void)copy_text(config.relay.router_id,
                    sizeof(config.relay.router_id),
                    config.peer_transport.local_router_id);
    (void)copy_text(config.relay.domain_id,
                    sizeof(config.relay.domain_id),
                    config.peer_transport.local_domain_id);
    if (config.forwarding_source_router_id[0] == '\0') {
        (void)copy_text(config.forwarding_source_router_id,
                        sizeof(config.forwarding_source_router_id),
                        config.forwarding_issuer);
    }
    if (config.relay.ca_file[0] == '\0') {
        (void)copy_text(config.relay.ca_file,
                        sizeof(config.relay.ca_file),
                        config.peer_transport.ca_file);
    }
    (void)copy_text(config.relay.client_cert_file,
                    sizeof(config.relay.client_cert_file),
                    config.peer_transport.client_cert_file);
    (void)copy_text(config.relay.client_key_file,
                    sizeof(config.relay.client_key_file),
                    config.peer_transport.client_key_file);
    (void)copy_text(config.open_mesh_relay.router_id,
                    sizeof(config.open_mesh_relay.router_id),
                    config.peer_transport.local_router_id);
    (void)copy_text(config.open_mesh_relay.domain_id,
                    sizeof(config.open_mesh_relay.domain_id),
                    config.peer_transport.local_domain_id);
    (void)copy_text(config.open_mesh_relay.ca_file,
                    sizeof(config.open_mesh_relay.ca_file),
                    config.peer_transport.ca_file);
    (void)copy_text(config.open_mesh_relay.client_cert_file,
                    sizeof(config.open_mesh_relay.client_cert_file),
                    config.peer_transport.client_cert_file);
    (void)copy_text(config.open_mesh_relay.client_key_file,
                    sizeof(config.open_mesh_relay.client_key_file),
                    config.peer_transport.client_key_file);
    if (!agent_cross_owner_from_domain(
            config.cross_discovery_domain,
            config.cross_discovery_owner,
            sizeof(config.cross_discovery_owner))) {
        fprintf(stderr, "agentd: invalid cross-discovery domain\n");
        return false;
    }
    if (inet_pton(AF_INET, config.cross_resolver_ipv4,
                  &resolver_address) != 1 ||
        (ntohl(resolver_address.s_addr) & 0xff000000U) != 0x7f000000U) {
        fprintf(stderr,
                "agentd: cross-discovery resolver must be loopback IPv4\n");
        return false;
    }
    if ((config.relay.enabled || config.open_mesh_relay.enabled) &&
        !config.peer_transport.enabled) {
        fprintf(stderr,
                "agentd: Relay requires peer transport to be enabled\n");
        return false;
    }
    if (config.open_mesh_relay.enabled) {
        struct agent_relay_directory_endpoint endpoints[
            AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS];
        size_t endpoint_count = 0U;
        size_t endpoint_index;

        if (!agent_relay_directory_endpoint_set_parse(
                config.open_mesh_relay.directory_endpoint,
                config.open_mesh_relay.directory_connect_ipv4,
                endpoints, AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS,
                &endpoint_count)) {
            fprintf(stderr,
                    "agentd: Open Mesh Directory endpoint set is invalid\n");
            return false;
        }
        for (endpoint_index = 0U; endpoint_index < endpoint_count;
             endpoint_index++) {
            if (!endpoints[endpoint_index].open_mesh) {
                fprintf(stderr,
                        "agentd: Open Mesh Relay accepts only the Open Mesh assignment path\n");
                return false;
            }
        }
    }
    if (config.peer_transport.relay_tunnel_enabled &&
        !config.peer_transport.enabled) {
        fprintf(stderr,
                "agentd: Relay tunnel requires peer transport to be enabled\n");
        return false;
    }
    if (config.forwarding_assertion_required &&
        (!config.peer_transport.relay_tunnel_enabled ||
         !forwarding_key_path_safe(config.forwarding_public_key) ||
         config.forwarding_key_id[0] == '\0' ||
         config.forwarding_issuer[0] == '\0' ||
         config.forwarding_source_router_id[0] == '\0')) {
        fprintf(stderr,
                "agentd: forwarding assertions require a Relay tunnel and complete trust metadata\n");
        return false;
    }
    if (config.relay_gateway_internal_enabled &&
        (!config.peer_transport.relay_tunnel_enabled ||
         !config.forwarding_assertion_required ||
         strcmp(config.relay_gateway_token_file,
                "/etc/agentd/nexus-cloud-gateway.token") != 0 ||
         !agent_invoke_parse_loopback_endpoint(
             config.relay_gateway_internal_endpoint,
             &internal_gateway_endpoint) ||
         strcmp(internal_gateway_endpoint.path,
                AGENT_INVOKE_INTERNAL_PATH) != 0)) {
        fprintf(stderr,
                "agentd: internal Gateway requires NFA-protected Relay, a fixed loopback endpoint and the managed credential path\n");
        return false;
    }
    if (!agent_auto_promotion_policy_init(
            &auto_promotion_policy, config.auto_promotion_mode,
            config.peer_transport.local_domain_id,
            config.auto_promotion_allowlist,
            config.auto_promotion_grace_seconds)) {
        fprintf(stderr, "agentd: invalid LAN auto-promotion policy\n");
        return false;
    }
    if (!agent_card_policy_init(
            &card_policy, config.card_authorization_mode,
            config.peer_transport.local_domain_id,
            config.card_authorization_allowlist)) {
        fprintf(stderr, "agentd: invalid Agent Card authorization policy\n");
        return false;
    }
    if (card_policy.mode != AGENT_CARD_AUTH_OFF &&
        (!config.cross_discovery_enabled ||
         !config.peer_transport.enabled)) {
        fprintf(stderr,
                "agentd: Agent Card authorization requires cross-discovery and peer transport\n");
        return false;
    }
    if (auto_promotion_policy.mode != AGENT_AUTO_PROMOTION_OFF &&
        (!config.discovery_enabled || !config.peer_transport.enabled)) {
        fprintf(stderr,
                "agentd: LAN auto-promotion requires discovery and peer transport\n");
        return false;
    }
    if (config.min_lease_seconds == 0U ||
        config.min_lease_seconds > config.default_lease_seconds ||
        config.default_lease_seconds > config.max_lease_seconds) {
        fprintf(stderr,
                "agentd: lease bounds must satisfy 0 < min <= default <= max\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    struct static_route_load_result load_result;
    struct static_policy_load_result policy_load_result;
    struct static_peer_load_result peer_load_result;
    enum route_table_result route_result;
    enum peer_table_result peer_result;
    int status;
    char transport_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};
    char listener_error[AGENT_PEER_TRANSPORT_ERROR_LEN] = {0};
    char relay_error[AGENT_RELAY_BOOTSTRAP_ERROR_LEN] = {0};
    struct agent_relay_invoke_auth_config invoke_auth;
    struct agent_relay_invoke_gateway_config invoke_gateway;
    struct agent_relay_invoke_alias_config invoke_alias;

    if (!read_options(argc, argv)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (config.public_ipv6_prefix[0] != '\0' &&
        !agent_public_ipv6_pool_init(
            &public_ipv6_pool, config.public_ipv6_prefix,
            config.max_public_ipv6)) {
        fprintf(stderr,
                "agentd: invalid public IPv6 prefix; require a routed /48.."
                "/64 prefix and a non-zero bounded pool\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    route_table_init(&routes, config.max_routes);
    agent_dynamic_manifest_init(&dynamic_manifests);
    (void)write_dynamic_manifest_snapshot();
    agent_policy_table_init(&policies);
    agent_recovery_init(&recovery_state, monotonic_ms());
    if (!static_policies_reload(&policies, "agent_policy",
                                &policy_load_result)) {
        fprintf(stderr,
                "agentd: policy configuration degraded; using fail-closed "
                "empty Policy RIB: %s\n", policy_load_result.error);
        policies.default_allow = false;
        static_policies_loaded = 0U;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_POLICY_RIB, false,
            policy_load_result.error);
    } else {
        static_policies_loaded = policy_load_result.loaded;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_POLICY_RIB, true, NULL);
    }
    route_table_set_policy(&routes, &policies);
    peer_table_init(&peers, AGENTD_DEFAULT_MAX_PEERS);
    agent_discovery_table_init(&discoveries, config.max_discoveries);
    agent_cross_table_init(&cross_discoveries,
                           config.max_cross_discoveries);
    agent_discovery_promotion_init(
        &promotions, AGENTD_DEFAULT_MAX_PROMOTIONS);
    agent_card_manager_init(&cards, config.max_cross_discoveries);
    agent_card_trust_manager_init(&card_trust);
    route_result = static_routes_reload(&routes, "agent", &load_result);
    if (route_result != ROUTE_TABLE_OK) {
        fprintf(stderr,
                "agentd: static route configuration degraded; using empty "
                "static AFIB: %s\n", load_result.error);
        static_routes_loaded = 0U;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_STATIC_ROUTES, false,
            load_result.error);
    } else {
        static_routes_loaded = load_result.loaded;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_STATIC_ROUTES, true, NULL);
    }
    peer_result = static_peers_reload(&peers, "agent_peers",
                                      &peer_load_result);
    if (peer_result != PEER_TABLE_OK) {
        fprintf(stderr,
                "agentd: static peer configuration degraded; using empty "
                "peer table: %s\n", peer_load_result.error);
        static_peers_loaded = 0U;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_STATIC_PEERS, false,
            peer_load_result.error);
    } else {
        static_peers_loaded = peer_load_result.loaded;
        agent_recovery_record_startup(
            &recovery_state, AGENT_RECOVERY_STATIC_PEERS, true, NULL);
    }
    next_snapshot_id = monotonic_ms();
    if (next_snapshot_id == 0U) {
        next_snapshot_id = 1U;
    }
    if (!agent_peer_routes_init(
            &peer_routes, config.peer_transport.local_router_id,
            &routes, &peers)) {
        fprintf(stderr, "agentd: peer route manager initialization failed\n");
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        return EXIT_FAILURE;
    }
    agent_peer_routes_set_authorizer(
        &peer_routes, card_authorize_peer_update, &cards);
    agent_peer_routes_set_open_mesh(
        &peer_routes, config.peer_transport.open_mesh);
    config.peer_transport.message_handler = peer_message_received;
    config.peer_transport.session_handler = peer_session_changed;
    config.peer_transport.tunnel_handler = relay_tunnel_received;
    config.peer_transport.event_context = &peer_routes;
    config.peer_listener.session = config.peer_transport;
    uloop_init();
    if (agent_ipc_server_start(
            &ipc_server, config.ipc_socket, &routes, &peers,
            &public_ipv6_pool,
            monotonic_ms, config.ipc_allowed_uid,
            relay_invoke_ipc_received, relay_invoke_ipc_cancelled,
            NULL, ipc_register_local_route, ipc_renew_local_route,
            ipc_unregister_local_route, NULL) != 0) {
        fprintf(stderr, "agentd: failed to start IPC server at %s: %s\n",
                config.ipc_socket, strerror(errno));
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }

    ubus_ctx = ubus_connect(NULL);
    if (ubus_ctx == NULL) {
        fprintf(stderr, "agentd: failed to connect to ubus\n");
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }

    ubus_add_uloop(ubus_ctx);
    status = ubus_add_object(ubus_ctx, &agent_object);
    if (status != UBUS_STATUS_OK) {
        fprintf(stderr, "agentd: failed to register ubus object: %s\n",
                ubus_strerror(status));
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }

    peer_transport = agent_peer_transport_create(
        &config.peer_transport, &peers, monotonic_ms,
        transport_error, sizeof(transport_error));
    if (peer_transport == NULL) {
        fprintf(stderr, "agentd: peer transport initialization failed: %s\n",
                transport_error);
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }
    memset(&invoke_auth, 0, sizeof(invoke_auth));
    invoke_auth.required = config.forwarding_assertion_required;
    invoke_auth.public_key_file = config.forwarding_public_key;
    invoke_auth.key_id = config.forwarding_key_id;
    invoke_auth.issuer = config.forwarding_issuer;
    invoke_auth.source_router_id = config.forwarding_source_router_id;
    invoke_auth.clock_skew_seconds = 30U;
    invoke_auth.max_ttl_seconds = 60U;
    invoke_auth.replay_capacity = 512U;
    memset(&invoke_gateway, 0, sizeof(invoke_gateway));
    invoke_gateway.enabled = config.relay_gateway_internal_enabled;
    invoke_gateway.endpoint = config.relay_gateway_internal_endpoint;
    invoke_gateway.token_file = config.relay_gateway_token_file;
    memset(&invoke_alias, 0, sizeof(invoke_alias));
    invoke_alias.resolve = resolve_relay_cloud_tenant_alias;
    invoke_alias.context = &dynamic_manifests;
    relay_invoke = agent_relay_invoke_create(
        &routes, peer_transport, config.peer_transport.local_router_id,
        monotonic_ms, &invoke_auth, &invoke_gateway, &invoke_alias);
    if (relay_invoke == NULL) {
        fprintf(stderr, "agentd: Relay invoke initialization failed\n");
        agent_peer_transport_destroy(peer_transport);
        peer_transport = NULL;
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }
    config.peer_listener.boot_epoch =
        agent_peer_transport_boot_epoch(peer_transport);
    peer_listener = agent_peer_listener_create(
        &config.peer_listener, &peers, monotonic_ms,
        listener_error, sizeof(listener_error));
    if (peer_listener == NULL) {
        fprintf(stderr, "agentd: peer listener initialization failed: %s\n",
                listener_error);
        agent_relay_invoke_destroy(relay_invoke);
        relay_invoke = NULL;
        agent_peer_transport_destroy(peer_transport);
        peer_transport = NULL;
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }
    agent_relay_invoke_set_listener(relay_invoke, peer_listener);
    agent_relay_invoke_set_open_mesh(
        relay_invoke, &peers, config.peer_transport.open_mesh);

    config.relay.assignment_handler = relay_assignment_changed;
    config.relay.event_context = &cloud_relay;
    cloud_relay.bootstrap = agent_relay_bootstrap_create(
        &config.relay, monotonic_ms, relay_error, sizeof(relay_error));
    if (cloud_relay.bootstrap == NULL) {
        fprintf(stderr,
                "agentd: Cloud Relay bootstrap initialization failed: %s\n",
                relay_error);
        agent_peer_listener_destroy(peer_listener);
        peer_listener = NULL;
        agent_relay_invoke_destroy(relay_invoke);
        relay_invoke = NULL;
        agent_peer_transport_destroy(peer_transport);
        peer_transport = NULL;
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }
    memset(relay_error, 0, sizeof(relay_error));
    config.open_mesh_relay.assignment_handler = relay_assignment_changed;
    config.open_mesh_relay.event_context = &open_mesh_relay;
    open_mesh_relay.bootstrap = agent_relay_bootstrap_create(
        &config.open_mesh_relay, monotonic_ms,
        relay_error, sizeof(relay_error));
    if (open_mesh_relay.bootstrap == NULL) {
        fprintf(stderr,
                "agentd: Open Mesh Relay bootstrap initialization failed: %s\n",
                relay_error);
        agent_relay_bootstrap_destroy(cloud_relay.bootstrap);
        cloud_relay.bootstrap = NULL;
        agent_peer_listener_destroy(peer_listener);
        peer_listener = NULL;
        agent_relay_invoke_destroy(relay_invoke);
        relay_invoke = NULL;
        agent_peer_transport_destroy(peer_transport);
        peer_transport = NULL;
        ubus_free(ubus_ctx);
        agent_ipc_server_stop(&ipc_server);
        agent_cross_table_destroy(&cross_discoveries);
        agent_discovery_table_destroy(&discoveries);
        peer_table_destroy(&peers);
        route_table_destroy(&routes);
        uloop_done();
        return EXIT_FAILURE;
    }
    prune_timer.cb = prune_expired_routes;
    uloop_timeout_set(&prune_timer, (int)config.prune_interval_ms);
    snapshot_timer.cb = pump_snapshot_timer;
    uloop_timeout_set(&snapshot_timer,
                      (int)AGENTD_SNAPSHOT_PUMP_INTERVAL_MS);
    if (config.discovery_enabled) {
        discovery_timer.cb = poll_discovery_timer;
        uloop_timeout_set(&discovery_timer, 500);
    }
    if (config.cross_discovery_enabled) {
        cross_discovery_timer.cb = poll_cross_discovery_timer;
        uloop_timeout_set(&cross_discovery_timer, 750);
    }
    uloop_run();

    cross_query_close();
    uloop_timeout_cancel(&cross_discovery_timer);
    uloop_timeout_cancel(&discovery_timer);
    uloop_timeout_cancel(&snapshot_timer);
    uloop_timeout_cancel(&prune_timer);
    agent_relay_bootstrap_destroy(open_mesh_relay.bootstrap);
    open_mesh_relay.bootstrap = NULL;
    agent_relay_bootstrap_destroy(cloud_relay.bootstrap);
    cloud_relay.bootstrap = NULL;
    agent_peer_listener_destroy(peer_listener);
    peer_listener = NULL;
    agent_relay_invoke_destroy(relay_invoke);
    relay_invoke = NULL;
    agent_peer_transport_destroy(peer_transport);
    peer_transport = NULL;
    ubus_free(ubus_ctx);
    agent_ipc_server_stop(&ipc_server);
    blob_buf_free(&response);
    agent_discovery_promotion_destroy(&promotions);
    agent_card_manager_destroy(&cards);
    agent_cross_table_destroy(&cross_discoveries);
    agent_discovery_table_destroy(&discoveries);
    peer_table_destroy(&peers);
    route_table_destroy(&routes);
    agent_public_ipv6_pool_free(&public_ipv6_pool);
    uloop_done();
    return EXIT_SUCCESS;
}
