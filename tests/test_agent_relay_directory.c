#include "agent_relay_directory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char valid_assignment[] =
    "{\"version\":1,\"assignment_id\":\"assign-001\","
    "\"relay_id\":\"relay-east-1\","
    "\"relay_router_id\":\"router-relay-east-1\","
    "\"relay_domain_id\":\"relay.nexus.example\","
    "\"relay_endpoint\":\"https://relay-east-1.nexus.example:7444/arpx/v1\","
    "\"connect_ipv4\":\"192.0.2.40\","
    "\"session_ticket\":\"nrt1.current.eyJ2IjoxLCJhaWQiOiJhc3NpZ24tMDAxIn0."
    "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY\","
    "\"lease_seconds\":300}";

static void endpoint_and_request(void)
{
    struct agent_relay_directory_endpoint endpoint;
    struct agent_relay_directory_endpoint endpoints[
        AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS];
    char request[AGENT_RELAY_DIRECTORY_REQUEST_MAX];
    size_t request_length = 0U;
    size_t endpoint_count = 0U;

    assert(agent_relay_directory_endpoint_parse(
        "https://directory.nexus.example:8443/v1/relay-assignment",
        "192.0.2.30", &endpoint));
    assert(strcmp(endpoint.server_identity, "directory.nexus.example") == 0);
    assert(strcmp(endpoint.authority, "directory.nexus.example:8443") == 0);
    assert(strcmp(endpoint.path, "/v1/relay-assignment") == 0);
    assert(!endpoint.open_mesh);
    assert(endpoint.port == 8443U);
    assert(agent_relay_directory_endpoint_parse(
        "https://seed.example:8443/v1/open-mesh/assignment",
        "192.0.2.50", &endpoint));
    assert(endpoint.open_mesh);
    assert(strcmp(endpoint.path, "/v1/open-mesh/assignment") == 0);
    assert(agent_relay_directory_endpoint_parse(
        "https://directory.nexus.example:8443/v1/relay-assignment",
        "", &endpoint));
    assert(endpoint.connect_ipv4[0] == '\0');
    assert(!agent_relay_directory_endpoint_parse(
        "http://directory.nexus.example:8443/v1/relay-assignment",
        "192.0.2.30", &endpoint));
    assert(!agent_relay_directory_endpoint_parse(
        "https://directory.nexus.example:8443/other", "192.0.2.30",
        &endpoint));
    assert(agent_relay_directory_endpoint_parse(
        "https://directory.nexus.example/api/v1/edge/v1/relay-assignment/",
        "", &endpoint));
    assert(endpoint.port == 443U);
    assert(agent_relay_directory_endpoint_set_parse(
        "https://directory-a.example:8443/v1/relay-assignment,"
        "https://directory-b.example:8443/v1/relay-assignment",
        "192.0.2.30,192.0.2.31", endpoints,
        AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS, &endpoint_count));
    assert(endpoint_count == 2U);
    assert(strcmp(endpoints[1].server_identity,
                  "directory-b.example") == 0);
    assert(agent_relay_directory_endpoint_set_parse(
        "https://directory-a.example:8443/v1/relay-assignment,"
        "https://directory-b.example:8443/v1/relay-assignment",
        "", endpoints, AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS,
        &endpoint_count));
    assert(endpoint_count == 2U);
    assert(endpoints[0].connect_ipv4[0] == '\0');
    assert(endpoints[1].connect_ipv4[0] == '\0');
    assert(!agent_relay_directory_endpoint_set_parse(
        "https://directory-a.example:8443/v1/relay-assignment,"
        "https://directory-b.example:8443/v1/relay-assignment",
        "192.0.2.30", endpoints, AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS,
        &endpoint_count));
    assert(!agent_relay_directory_endpoint_set_parse(
        "https://directory-a.example:8443/v1/relay-assignment,,"
        "https://directory-b.example:8443/v1/relay-assignment",
        "192.0.2.30,,192.0.2.31", endpoints,
        AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS, &endpoint_count));

    assert(agent_relay_directory_endpoint_parse(
        "https://directory.nexus.example:8443/v1/relay-assignment",
        "192.0.2.30", &endpoint));
    assert(agent_relay_directory_build_request(
        &endpoint, "router-node-1", "tenant-a.example", request,
        sizeof(request), &request_length) == AGENT_RELAY_DIRECTORY_OK);
    assert(request_length == strlen(request));
    assert(strstr(request, "POST /v1/relay-assignment HTTP/1.1\r\n") != NULL);
    assert(strstr(request, "Host: directory.nexus.example:8443\r\n") != NULL);
    assert(strstr(request, "\"router_id\":\"router-node-1\"") != NULL);
    assert(strstr(request, "\"current_relay_id\":\"\"") != NULL);
    assert(agent_relay_directory_build_ha_request(
        &endpoint, "router-node-1", "tenant-a.example", "relay-east-1",
        "relay-west-1", request, sizeof(request), &request_length) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(strstr(request,
                  "\"current_relay_id\":\"relay-east-1\"") != NULL);
    assert(strstr(request,
                  "\"failed_relay_id\":\"relay-west-1\"") != NULL);
    assert(agent_relay_directory_build_authenticated_ha_request(
        &endpoint, "router-node-1", "tenant-a.example", "", "",
        "edge_abcDEF123_-", request, sizeof(request), &request_length) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(strstr(request, "Authorization: Edge edge_abcDEF123_-\r\n") != NULL);
    assert(agent_relay_directory_endpoint_parse(
        "https://seed.example:8443/v1/open-mesh/assignment",
        "192.0.2.50", &endpoint));
    assert(agent_relay_directory_build_authenticated_ha_request(
        &endpoint, "router-node-1", "tenant-a.example", "", "", "",
        request, sizeof(request), &request_length) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(strstr(request, "POST /v1/open-mesh/assignment HTTP/1.1\r\n") != NULL);
    assert(strstr(request, "Authorization:") == NULL);
    assert(agent_relay_directory_build_ha_request(
        &endpoint, "router-node-1", "tenant-a.example", "bad relay", "",
        request, sizeof(request), &request_length) ==
        AGENT_RELAY_DIRECTORY_INVALID);
}

static void assignment_contract(void)
{
    struct agent_relay_assignment assignment;
    struct agent_relay_assignment renewed;
    char changed[sizeof(valid_assignment)];

    assert(agent_relay_directory_parse_assignment(
        valid_assignment, strlen(valid_assignment), 1000U, &assignment) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(strcmp(assignment.relay_id, "relay-east-1") == 0);
    assert(strcmp(assignment.connect_ipv4, "192.0.2.40") == 0);
    assert(assignment.lease_seconds == 300U);
    assert(strncmp(assignment.session_ticket, "nrt1.current.", 13U) == 0);
    assert(assignment.expires_at_ms == 301000U);
    assert(agent_relay_directory_parse_assignment(
        valid_assignment, strlen(valid_assignment), 2000U, &renewed) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(agent_relay_assignment_equal(&assignment, &renewed));
    assert(agent_relay_assignment_ticket_equal(&assignment, &renewed));

    snprintf(changed, sizeof(changed), "%s", valid_assignment);
    strstr(changed, "eyJ2Ijox")[0] = 'f';
    assert(agent_relay_directory_parse_assignment(
        changed, strlen(changed), 1000U, &renewed) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(agent_relay_assignment_equal(&assignment, &renewed));
    assert(!agent_relay_assignment_ticket_equal(&assignment, &renewed));

    snprintf(changed, sizeof(changed), "%s", valid_assignment);
    memcpy(strstr(changed, "192.0.2.40"), "192.0.2.41", 10U);
    assert(agent_relay_directory_parse_assignment(
        changed, strlen(changed), 1000U, &renewed) ==
        AGENT_RELAY_DIRECTORY_OK);
    assert(!agent_relay_assignment_equal(&assignment, &renewed));
}

static void fail_closed(void)
{
    struct agent_relay_assignment assignment;
    char duplicate[1024];
    char unknown[1024];
    char bad_endpoint[1024];
    char bad_ticket[1024];
    char too_large[AGENT_RELAY_DIRECTORY_BODY_MAX + 2U];

    snprintf(duplicate, sizeof(duplicate),
             "{\"version\":1,\"version\":1,%s",
             strchr(valid_assignment, ',') + 1);
    assert(agent_relay_directory_parse_assignment(
        duplicate, strlen(duplicate), 0U, &assignment) ==
        AGENT_RELAY_DIRECTORY_DUPLICATE);

    snprintf(unknown, sizeof(unknown),
             "{\"extra\":1,%s", valid_assignment + 1);
    assert(agent_relay_directory_parse_assignment(
        unknown, strlen(unknown), 0U, &assignment) ==
        AGENT_RELAY_DIRECTORY_UNSUPPORTED);

    snprintf(bad_endpoint, sizeof(bad_endpoint), "%s", valid_assignment);
    memcpy(strstr(bad_endpoint, "https://"), "http://x", 8U);
    assert(agent_relay_directory_parse_assignment(
        bad_endpoint, strlen(bad_endpoint), 0U, &assignment) ==
        AGENT_RELAY_DIRECTORY_INVALID);

    snprintf(bad_ticket, sizeof(bad_ticket), "%s", valid_assignment);
    memcpy(strstr(bad_ticket, "nrt1.current"), "bad!.current", 12U);
    assert(agent_relay_directory_parse_assignment(
        bad_ticket, strlen(bad_ticket), 0U, &assignment) ==
        AGENT_RELAY_DIRECTORY_INVALID);

    memset(too_large, 'x', sizeof(too_large));
    too_large[sizeof(too_large) - 1U] = '\0';
    assert(agent_relay_directory_parse_assignment(
        too_large, sizeof(too_large) - 1U, 0U, &assignment) ==
        AGENT_RELAY_DIRECTORY_TOO_LARGE);
}

int main(void)
{
    endpoint_and_request();
    assignment_contract();
    fail_closed();
    puts("agent relay directory tests passed");
    return 0;
}
