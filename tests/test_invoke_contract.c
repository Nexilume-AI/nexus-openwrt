#include "agent_invoke_contract.h"
#include "agent_ipc_protocol.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_loopback_endpoints(void)
{
    struct agent_invoke_endpoint endpoint;

    assert(agent_invoke_parse_loopback_endpoint(
        "http://127.0.0.1:9001/invoke?mode=fast", &endpoint));
    assert(endpoint.family == AGENT_INVOKE_IPV4);
    assert(strcmp(endpoint.host, "127.0.0.1") == 0);
    assert(endpoint.port == 9001U);
    assert(strcmp(endpoint.path, "/invoke?mode=fast") == 0);
    assert(agent_invoke_parse_loopback_endpoint(
        "http://[::1]:8080/agent", &endpoint));
    assert(endpoint.family == AGENT_INVOKE_IPV6);
    assert(strcmp(endpoint.host, "::1") == 0);
    assert(!agent_invoke_parse_loopback_endpoint(
        "https://127.0.0.1:9001/invoke", &endpoint));
    assert(!agent_invoke_parse_loopback_endpoint(
        "http://192.168.1.5:9001/invoke", &endpoint));
    assert(!agent_invoke_parse_loopback_endpoint(
        "http://localhost:9001/invoke", &endpoint));
    assert(!agent_invoke_parse_loopback_endpoint(
        "http://127.0.0.1:9001/a#fragment", &endpoint));
    assert(!agent_invoke_parse_loopback_endpoint(
        "http://user@127.0.0.1:9001/invoke", &endpoint));
}

static void test_automatic_lan_endpoints(void)
{
    struct agent_invoke_endpoint endpoint;

    assert(agent_invoke_parse_lan_endpoint(
        "http://192.168.10.20:9443/invoke", &endpoint));
    assert(endpoint.family == AGENT_INVOKE_IPV4);
    assert(strcmp(endpoint.host, "192.168.10.20") == 0);
    assert(endpoint.port == 9443U);
    assert(strcmp(endpoint.path, "/invoke") == 0);
    assert(agent_invoke_parse_lan_endpoint(
        "http://10.20.30.40:8080/", &endpoint));
    assert(agent_invoke_parse_lan_endpoint(
        "http://172.31.255.254:8080/invoke?stream=1", &endpoint));
    assert(agent_invoke_parse_lan_endpoint(
        "http://[fd42:10::20]:9443/invoke", &endpoint));
    assert(endpoint.family == AGENT_INVOKE_IPV6);
    assert(strcmp(endpoint.host, "fd42:10::20") == 0);
    assert(agent_invoke_parse_lan_endpoint(
        "http://[FC00::20]:9443", &endpoint));
    assert(strcmp(endpoint.path, "/") == 0);

    assert(!agent_invoke_parse_lan_endpoint(
        "https://192.168.10.20:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://127.0.0.1:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://192.168.001.20:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://192.0.2.20:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://agent.lan:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://[2001:db8::20]:9443/invoke", &endpoint));
    assert(!agent_invoke_parse_lan_endpoint(
        "http://[fe80::20]:9443/invoke", &endpoint));
}

static void test_remote_tls_endpoints(void)
{
    struct agent_invoke_endpoint endpoint;
    struct agent_invoke_remote_map mapping;
    char request[1024];
    size_t request_length = 0U;

    assert(agent_invoke_parse_remote_tls_endpoint(
        "https://agent.p274.test:19444/invoke?mode=fast", &endpoint));
    assert(endpoint.transport == AGENT_INVOKE_HTTPS);
    assert(endpoint.family == AGENT_INVOKE_DNS);
    assert(strcmp(endpoint.host, "agent.p274.test") == 0);
    assert(endpoint.port == 19444U);
    assert(strcmp(endpoint.path, "/invoke?mode=fast") == 0);
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "http://agent.p274.test:19444/invoke", &endpoint));
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "https://192.168.250.2:19444/invoke", &endpoint));
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "https://Agent.p274.test:19444/invoke", &endpoint));
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "https://agent..test:19444/invoke", &endpoint));
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "https://agent.p274.test/invoke", &endpoint));
    assert(!agent_invoke_parse_remote_tls_endpoint(
        "https://agent.p274.test:19444/a#fragment", &endpoint));

    assert(agent_invoke_parse_remote_map(
        "agent.p274.test:19444=192.168.250.164", &mapping));
    assert(strcmp(mapping.identity, "agent.p274.test") == 0);
    assert(mapping.port == 19444U);
    assert(strcmp(mapping.address, "192.168.250.164") == 0);
    assert(mapping.family == AGENT_INVOKE_IPV4);
    assert(agent_invoke_parse_remote_tls_endpoint(
        "https://agent.p274.test:19444/invoke", &endpoint));
    assert(agent_invoke_remote_map_matches(&endpoint, &mapping));
    assert(agent_invoke_build_http_request(
        &endpoint, "0123456789abcdef0123456789abcdef",
        "{}", 2U, request, sizeof(request), &request_length));
    assert(strstr(request,
                  "Host: agent.p274.test:19444\r\n") != NULL);
    assert(strstr(request, "Authorization:") == NULL);
    assert(agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=[2001:db8:813::20]", &mapping));
    assert(mapping.family == AGENT_INVOKE_IPV6);
    assert(strcmp(mapping.identity, "agent-v6.p813.test") == 0);
    assert(strcmp(mapping.address, "2001:db8:813::20") == 0);
    assert(!agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=2001:db8:813::20", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=[::]", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=[::1]", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=[fe80::1]", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent-v6.p813.test:19444=[ff02::1]", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent.p274.test:19444=127.0.0.1", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent.p274.test:19444=169.254.1.2", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent.p274.test:19444=224.0.0.1", &mapping));
    assert(!agent_invoke_parse_remote_map(
        "agent.p274.test:19444=192.168.001.2", &mapping));
}

static void test_lease_bound_https_maps(void)
{
    struct agent_invoke_dynamic_map_table table;
    struct agent_invoke_endpoint endpoint;
    struct agent_invoke_endpoint other_path;
    struct agent_invoke_remote_map mapping;
    struct agent_invoke_dynamic_map snapshot;
    const struct agent_invoke_dynamic_map *found;
    const char fingerprint[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    agent_invoke_dynamic_map_table_init(&table);
    assert(agent_invoke_parse_remote_tls_endpoint(
        "https://linter.example.test:9443/invoke", &endpoint));
    assert(agent_invoke_parse_remote_map(
        "linter.example.test:9443=192.168.10.20", &mapping));
    assert(agent_invoke_parse_remote_tls_endpoint(
        "https://linter.example.test:9443/other", &other_path));
    assert(agent_invoke_dynamic_map_upsert(
        &table, "0123456789abcdef0123456789abcdef", &endpoint, &mapping,
        "system", fingerprint, 5000U, 1000U));
    assert(table.count == 1U);
    assert(table.accepted == 1U);
    found = agent_invoke_dynamic_map_find(
        &table, "0123456789abcdef0123456789abcdef", &endpoint, 2000U);
    assert(found != NULL);
    assert(strcmp(found->mapping.address, "192.168.10.20") == 0);
    assert(strcmp(found->endpoint.path, "/invoke") == 0);
    assert(strcmp(found->ca_bundle_id, "system") == 0);
    assert(strcmp(found->certificate_sha256, fingerprint) == 0);
    assert(agent_invoke_dynamic_map_find(
        &table, found->route_id, &other_path, 2000U) == NULL);
    assert(agent_invoke_dynamic_map_renew(
        &table, found->route_id, 9000U, 3000U));
    assert(agent_invoke_dynamic_map_find(
        &table, found->route_id, &endpoint, 6000U) != NULL);
    snapshot = *found;
    assert(agent_invoke_dynamic_map_remove(
        &table, "0123456789abcdef0123456789abcdef"));
    assert(table.count == 0U);
    assert(agent_invoke_dynamic_map_matches(
        &snapshot, snapshot.route_id, &endpoint, 6000U));
    assert(!agent_invoke_dynamic_map_matches(
        &snapshot, snapshot.route_id, &endpoint, 9000U));
    assert(!agent_invoke_dynamic_map_matches(
        &snapshot, snapshot.route_id, &other_path, 6000U));

    assert(!agent_invoke_dynamic_map_upsert(
        &table, "route-2", &endpoint, &mapping, "bad bundle!", fingerprint,
        5000U, 1000U));
    assert(!agent_invoke_dynamic_map_upsert(
        &table, "route-2", &endpoint, &mapping, "system", "abcd",
        5000U, 1000U));
    assert(agent_invoke_dynamic_map_upsert(
        &table, "route-2", &endpoint, &mapping, "system", "",
        5000U, 1000U));
    assert(agent_invoke_dynamic_map_find(
        &table, "route-2", &endpoint, 5000U) == NULL);
    assert(table.expired == 1U);
}

static void test_hop_and_retry_contract(void)
{
    uint8_t forwarded = 0U;

    assert(agent_invoke_decrement_hop_limit(8U, &forwarded));
    assert(forwarded == 7U);
    assert(!agent_invoke_decrement_hop_limit(1U, &forwarded));
    assert(!agent_invoke_decrement_hop_limit(256U, &forwarded));
    assert(agent_invoke_retry_declaration_valid(false, false, NULL));
    assert(agent_invoke_retry_declaration_valid(
        true, true, "sha256:1234"));
    assert(!agent_invoke_retry_declaration_valid(false, true, "key"));
    assert(!agent_invoke_retry_declaration_valid(true, true, ""));
}

static void test_deadline(void)
{
    uint32_t timeout = 0U;
    const uint64_t base = 1785506400000ULL; /* 2026-07-31T14:00:00Z */

    assert(agent_invoke_effective_timeout_ms(
        base, 5000U, NULL, &timeout));
    assert(timeout == 5000U);
    assert(agent_invoke_effective_timeout_ms(
        base, 5000U, "2026-07-31T14:00:02Z", &timeout));
    assert(timeout == 2000U);
    assert(!agent_invoke_effective_timeout_ms(
        base, 5000U, "2026-07-31T14:00:00Z", &timeout));
    assert(!agent_invoke_effective_timeout_ms(
        base, 5000U, "2026-02-29T14:00:02Z", &timeout));
    assert(!agent_invoke_effective_timeout_ms(
        base, 5000U, "2026-07-31T22:00:02+08:00", &timeout));
    assert(agent_invoke_resolve_timeout_ms(
        base, 5000U, "2026-07-31T14:00:00Z", &timeout) ==
        AGENT_INVOKE_DEADLINE_EXPIRED);
    assert(agent_invoke_resolve_timeout_ms(
        base, 5000U, "not-a-date", &timeout) ==
        AGENT_INVOKE_DEADLINE_INVALID);
}

static void test_backend_lifecycle(void)
{
    struct agent_invoke_backend_machine machine;

    agent_invoke_backend_machine_init(&machine);
    assert(agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_CONNECTED));
    assert(agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_REQUEST_SENT));
    assert(agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_RESPONSE_RECEIVED));
    assert(machine.outcome == AGENT_BACKEND_SUCCEEDED);
    assert(!agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_IO_FAILED));

    agent_invoke_backend_machine_init(&machine);
    assert(agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_DEADLINE_EXPIRED));
    assert(machine.outcome == AGENT_BACKEND_TIMED_OUT);

    agent_invoke_backend_machine_init(&machine);
    assert(agent_invoke_backend_machine_transition(
        &machine, AGENT_BACKEND_CLIENT_DISCONNECTED));
    assert(machine.outcome == AGENT_BACKEND_CANCELLED);
}

static void test_http_contract(void)
{
    struct agent_invoke_endpoint endpoint;
    struct agent_invoke_http_response response;
    char request[2048];
    size_t request_length = 0U;
    const char body[] = "{\"payload\":{\"secret\":\"not-logged\"}}";
    const char reply[] =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 11\r\nConnection: close\r\n\r\n"
        "{\"ok\":true}";
    const char redirect[] =
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n";
    const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char duplicate_length[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\na";
    const char oversized[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2048\r\n\r\n";
    const char malformed_status[] =
        "HTTP/1.1 200junk\r\nContent-Length: 0\r\n\r\n";

    assert(agent_invoke_parse_loopback_endpoint(
        "http://127.0.0.1:9001/invoke", &endpoint));
    assert(agent_invoke_build_http_request(
        &endpoint, "0123456789abcdef0123456789abcdef",
        body, strlen(body), request, sizeof(request), &request_length));
    assert(request_length > strlen(body));
    assert(strstr(request, "POST /invoke HTTP/1.1\r\n") != NULL);
    assert(strstr(request, "Authorization:") == NULL);
    assert(strstr(request, "Accept: application/json\r\n") != NULL);
    assert(agent_invoke_build_stream_http_request(
        &endpoint, "0123456789abcdef0123456789abcdef",
        body, strlen(body), request, sizeof(request), &request_length));
    assert(strstr(request, "Accept: text/event-stream\r\n") != NULL);
    assert(strstr(request, "Txn-Token:") == NULL);
    assert(agent_invoke_parse_http_response(
        reply, strlen(reply), 1024U, &response) == AGENT_INVOKE_HTTP_OK);
    assert(response.status == 201);
    assert(response.body_length == 11U);
    assert(strcmp(response.content_type, "application/json") == 0);
    assert(agent_invoke_parse_http_response(
        reply, strlen(reply) - 1U, 1024U,
        &response) == AGENT_INVOKE_HTTP_INCOMPLETE);
    assert(agent_invoke_parse_http_response(
        redirect, strlen(redirect), 1024U,
        &response) == AGENT_INVOKE_HTTP_REDIRECT);
    assert(agent_invoke_parse_http_response(
        chunked, strlen(chunked), 1024U,
        &response) == AGENT_INVOKE_HTTP_INVALID);
    assert(agent_invoke_parse_http_response(
        duplicate_length, strlen(duplicate_length), 1024U,
        &response) == AGENT_INVOKE_HTTP_INVALID);
    assert(agent_invoke_parse_http_response(
        oversized, strlen(oversized), 1024U,
        &response) == AGENT_INVOKE_HTTP_TOO_LARGE);
    assert(agent_invoke_parse_http_response(
        malformed_status, strlen(malformed_status), 1024U,
        &response) == AGENT_INVOKE_HTTP_INVALID);
    assert(agent_invoke_parse_http_response(
        reply, strlen(reply), SIZE_MAX,
        &response) == AGENT_INVOKE_HTTP_INVALID);
}

static void test_internal_gateway_contract(void)
{
    struct agent_invoke_endpoint gateway;
    struct agent_invoke_endpoint selected;
    struct agent_invoke_endpoint parsed;
    struct agent_invoke_remote_map mapping;
    struct agent_invoke_dynamic_map_table table;
    struct agent_invoke_dynamic_map lease_snapshot;
    char request[4096];
    size_t request_length = 0U;
    const char token[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char wrong[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char generation[] = "0123456789abcdef0123456789abcdef";
    const char body[] =
        "{\"version\":\"1.0\",\"target_agent\":\"agent://tenant/probe\","
        "\"payload\":{\"nonce\":\"abc\"}}";
    const char *body_start;
    char content_length[64];
    const char certificate[] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

    assert(agent_invoke_internal_token_is_valid(token, strlen(token)));
    assert(!agent_invoke_internal_token_is_valid(token, strlen(token) - 1U));
    assert(!agent_invoke_internal_token_is_valid(wrong, 0U));
    assert(agent_invoke_internal_token_matches(token, token, strlen(token)));
    assert(!agent_invoke_internal_token_matches(token, wrong, strlen(wrong)));
    assert(!agent_invoke_internal_token_matches(
        token, token, strlen(token) - 1U));
    assert(!agent_invoke_internal_token_matches(token, NULL, 0U));
    assert(agent_invoke_internal_generation_is_valid(generation));
    assert(!agent_invoke_internal_generation_is_valid(""));
    assert(!agent_invoke_internal_generation_is_valid(
        "0123456789abcdef0123456789abcdeF"));
    assert(agent_invoke_backend_response_limit(false, 262144U) == 262144U);
    assert(agent_invoke_backend_response_limit(true, 262144U) ==
           AGENT_IPC_MAX_INVOKE_BODY);
    assert(agent_invoke_backend_response_limit(true, 1024U) == 1024U);

    agent_invoke_dynamic_map_table_init(&table);
    assert(agent_invoke_parse_remote_tls_endpoint(
        "https://probe.edge.test:9443/invoke", &selected));
    assert(agent_invoke_parse_remote_map(
        "probe.edge.test:9443=192.168.10.20", &mapping));
    assert(agent_invoke_dynamic_map_upsert(
        &table, "0123456789abcdef0123456789abcdef", &selected, &mapping,
        "system", certificate, 5000U, 1000U));
    assert(agent_invoke_internal_selection_validate(
        &table, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke", "agent://tenant/probe",
        "agent://tenant/probe", "agent://tenant/probe", 2000U,
        &parsed, &lease_snapshot) == AGENT_INVOKE_INTERNAL_SELECTION_OK);
    assert(strcmp(lease_snapshot.endpoint.path, "/invoke") == 0);
    assert(agent_invoke_internal_selection_validate(
        &table, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke", "agent://tenant/probe",
        "agent://tenant/other", "agent://tenant/other", 2000U,
        &parsed, &lease_snapshot) ==
            AGENT_INVOKE_INTERNAL_SELECTION_TARGET_MISMATCH);
    assert(agent_invoke_internal_selection_validate(
        &table, "1123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke", "agent://tenant/probe",
        "agent://tenant/probe", "agent://tenant/probe", 2000U,
        &parsed, &lease_snapshot) ==
            AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED);
    assert(agent_invoke_internal_selection_validate(
        &table, "0123456789abcdef0123456789abcdef",
        "https://other.edge.test:9443/invoke", "agent://tenant/probe",
        "agent://tenant/probe", "agent://tenant/probe", 2000U,
        &parsed, &lease_snapshot) ==
            AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED);
    assert(agent_invoke_internal_selection_validate(
        &table, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/other", "agent://tenant/probe",
        "agent://tenant/probe", "agent://tenant/probe", 2000U,
        &parsed, &lease_snapshot) ==
            AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED);
    assert(agent_invoke_internal_selection_validate(
        &table, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke", "agent://tenant/probe",
        "agent://tenant/probe", "agent://tenant/probe", 5000U,
        &parsed, &lease_snapshot) ==
            AGENT_INVOKE_INTERNAL_SELECTION_NOT_LEASED);

    assert(agent_invoke_parse_loopback_endpoint(
        "http://127.0.0.1:7788/agent/v1/internal-invoke", &gateway));
    assert(agent_invoke_build_internal_http_request(
        &gateway, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke",
        "agent://tenant/probe", "agent://tenant/probe", token, 4321U,
        body, strlen(body), request, sizeof(request), &request_length));
    assert(request_length > strlen(body));
    assert(strstr(request,
                  "POST /agent/v1/internal-invoke HTTP/1.1\r\n") != NULL);
    assert(strstr(request,
                  "X-Nexus-Selected-Endpoint: "
                  "https://probe.edge.test:9443/invoke\r\n") != NULL);
    assert(strstr(request,
                  "X-Nexus-Selected-Origin: agent://tenant/probe\r\n") != NULL);
    assert(strstr(request,
                  "X-Nexus-Target-Agent: agent://tenant/probe\r\n") != NULL);
    assert(strstr(request, "Authorization:") == NULL);
    assert(strstr(request, token) != NULL);
    assert(strstr(request, "X-Nexus-Timeout-Ms: 4321\r\n") != NULL);
    body_start = strstr(request, "\r\n\r\n");
    assert(body_start != NULL);
    body_start += 4U;
    assert((size_t)(body_start - request) + strlen(body) == request_length);
    assert(memcmp(body_start, body, strlen(body)) == 0);
    assert(body_start[strlen(body)] == '\0');
    (void)snprintf(content_length, sizeof(content_length),
                   "Content-Length: %" PRIu64 "\r\n",
                   (uint64_t)strlen(body));
    assert(strstr(request, content_length) != NULL);

    assert(!agent_invoke_build_internal_http_request(
        &gateway, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke",
        "agent://tenant/probe", "", token, 4321U,
        body, strlen(body), request,
        sizeof(request), &request_length));

    assert(!agent_invoke_build_internal_http_request(
        &gateway, "0123456789abcdef0123456789abcdef",
        "http://192.168.10.2:9443/invoke", "agent://tenant/probe",
        "agent://tenant/probe", token, 4321U, body, strlen(body), request,
        sizeof(request), &request_length));
    assert(!agent_invoke_build_internal_http_request(
        &gateway, "0123456789abcdef0123456789abcdef",
        "https://probe.edge.test:9443/invoke", "agent://tenant/probe\r\nX: 1",
        "", token, 4321U, body, strlen(body), request, sizeof(request),
        &request_length));
}

int main(void)
{
    test_loopback_endpoints();
    test_automatic_lan_endpoints();
    test_remote_tls_endpoints();
    test_lease_bound_https_maps();
    test_hop_and_retry_contract();
    test_deadline();
    test_backend_lifecycle();
    test_http_contract();
    test_internal_gateway_contract();
    return 0;
}
