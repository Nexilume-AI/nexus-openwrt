#include "agent_adapter_ingress_contract.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void test_paths(void)
{
    struct agent_adapter_ingress_route route;

    assert(agent_adapter_ingress_parse_path(
        "/mcp/eda-local", strlen("/mcp/eda-local"), &route));
    assert(route.protocol == AGENT_ADAPTER_PROTOCOL_MCP);
    assert(strcmp(route.authority, "eda-local") == 0);
    assert(route.selector_from_request);

    assert(agent_adapter_ingress_parse_path(
        "/a2a/agent-card%3Aeda.example/lint/message:send",
        strlen("/a2a/agent-card%3Aeda.example/lint/message:send"), &route));
    assert(route.protocol == AGENT_ADAPTER_PROTOCOL_A2A);
    assert(strcmp(route.authority, "agent-card:eda.example") == 0);
    assert(strcmp(route.selector, "lint") == 0);
    assert(!route.selector_from_request);
    assert(!route.streaming);

    assert(agent_adapter_ingress_parse_path(
        "/a2a/card/lint/message:stream",
        strlen("/a2a/card/lint/message:stream"), &route));
    assert(route.protocol == AGENT_ADAPTER_PROTOCOL_A2A);
    assert(route.streaming);

    assert(!agent_adapter_ingress_parse_path(
        "/mcp/eda/local", strlen("/mcp/eda/local"), &route));
    assert(!agent_adapter_ingress_parse_path(
        "/a2a/card/lint/message:other",
        strlen("/a2a/card/lint/message:other"), &route));
    assert(!agent_adapter_ingress_parse_path(
        "/a2a/card/%0d/message:send",
        strlen("/a2a/card/%0d/message:send"), &route));
}

static void test_gateway_request(void)
{
    struct agent_invoke_endpoint endpoint;
    struct agent_adapter_credentials credentials = {0};
    char output[1024];
    char expected_length[64];
    size_t output_length = 0U;
    const char envelope[] = "{\"version\":\"1.0\"}";
    const char unicode_envelope[] =
        "{\"message\":\""
        "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x99\x82"
        "\"}";

    assert(agent_invoke_parse_loopback_endpoint(
        "http://127.0.0.1:7788/agent/v1/invoke", &endpoint));
    credentials.authorization = "Bearer compact.jwt";
    credentials.authorization_length = strlen(credentials.authorization);
    credentials.public_ipv6 = "2001:db8:1::42";
    credentials.public_ipv6_length = strlen(credentials.public_ipv6);
    credentials.edge_token = "0123456789abcdef0123456789abcdef";
    credentials.edge_token_length = strlen(credentials.edge_token);
    assert(agent_adapter_build_gateway_request(
        &endpoint, &credentials, false, envelope, sizeof(envelope) - 1U,
        output, sizeof(output), &output_length));
    assert(output_length == strlen(output));
    assert(strstr(output, "POST /agent/v1/invoke HTTP/1.1\r\n") != NULL);
    assert(strstr(output,
                  "Content-Type: application/vnd.nexus.agent-envelope+json; charset=utf-8\r\n") != NULL);
    assert(strstr(output, "Authorization: Bearer compact.jwt\r\n") != NULL);
    assert(strstr(output,
                  "X-Nexus-Public-IPv6: 2001:db8:1::42\r\n") != NULL);
    assert(strstr(output,
                  "X-Nexus-Edge-Token: 0123456789abcdef0123456789abcdef\r\n") != NULL);
    assert(strstr(output, "\r\n\r\n{\"version\":\"1.0\"}") != NULL);

    assert(agent_adapter_build_gateway_request(
        &endpoint, &credentials, false,
        unicode_envelope, sizeof(unicode_envelope) - 1U,
        output, sizeof(output), &output_length));
    snprintf(expected_length, sizeof(expected_length),
             "Content-Length: %" PRIu64 "\r\n",
             (uint64_t)(sizeof(unicode_envelope) - 1U));
    assert(strstr(output, expected_length) != NULL);
    assert(memcmp(output + output_length - (sizeof(unicode_envelope) - 1U),
                  unicode_envelope, sizeof(unicode_envelope) - 1U) == 0);

    credentials.transaction_token = "second-token";
    credentials.transaction_token_length = strlen(credentials.transaction_token);
    assert(!agent_adapter_build_gateway_request(
        &endpoint, &credentials, false, envelope, sizeof(envelope) - 1U,
        output, sizeof(output), &output_length));
    credentials.authorization = NULL;
    credentials.authorization_length = 0U;
    assert(agent_adapter_build_gateway_request(
        &endpoint, &credentials, true, envelope, sizeof(envelope) - 1U,
        output, sizeof(output), &output_length));
    assert(strstr(output, "Txn-Token: second-token\r\n") != NULL);
    assert(strstr(output, "Accept: text/event-stream\r\n") != NULL);

    credentials.transaction_token = "bad\r\ntoken";
    credentials.transaction_token_length = strlen(credentials.transaction_token);
    assert(!agent_adapter_build_gateway_request(
        &endpoint, &credentials, false, envelope, sizeof(envelope) - 1U,
        output, sizeof(output), &output_length));
}

int main(void)
{
    test_paths();
    test_gateway_request();
    puts("adapter ingress contract tests passed");
    return 0;
}
