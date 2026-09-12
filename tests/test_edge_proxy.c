#include "agent_edge_proxy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct agent_edge_proxy_info info;
    char output[4096];
    size_t output_length = 0U;
    const char proxy[] =
        "PROXY TCP6 2001:db8::20 2001:db8:1::42 53000 7443\r\n";
    const char request[] =
        "POST /mcp/demo/tools/call HTTP/1.1\r\n"
        "Host: [2001:db8:1::42]:7443\r\n"
        "X-Nexus-Public-IPv6: 2001:db8::evil\r\n"
        "X-Nexus-Edge-Token: attacker\r\nConnection: keep-alive\r\n"
        "Content-Length: 2\r\n\r\n";

    assert(agent_edge_proxy_parse_v1(proxy, sizeof(proxy) - 1U, &info));
    assert(strcmp(info.destination_ipv6, "2001:db8:1::42") == 0);
    assert(info.destination_port == 7443U);
    assert(agent_edge_proxy_rewrite_http(
               request, sizeof(request) - 1U, info.destination_ipv6,
               "router-secret", output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_ADAPTER);
    output[output_length] = '\0';
    assert(strstr(output, "X-Nexus-Public-IPv6: 2001:db8:1::42\r\n") != NULL);
    assert(strstr(output, "X-Nexus-Edge-Token: router-secret\r\n") != NULL);
    assert(strstr(output, "attacker") == NULL);
    assert(strstr(output, "Connection: close\r\n") != NULL);
    assert(strstr(output, "keep-alive") == NULL);
    assert(agent_edge_proxy_rewrite_http(
               "POST /agent/v1/register HTTP/1.1\r\n\r\n",
               strlen("POST /agent/v1/register HTTP/1.1\r\n\r\n"),
               info.destination_ipv6, "router-secret", output,
               sizeof(output), &output_length) == AGENT_EDGE_BACKEND_INVALID);
    assert(agent_edge_proxy_rewrite_http(
               "GET /agent/v1/authentication HTTP/1.1\r\n"
               "Host: [2001:db8:1::42]:7443\r\n\r\n",
               strlen(
                   "GET /agent/v1/authentication HTTP/1.1\r\n"
                   "Host: [2001:db8:1::42]:7443\r\n\r\n"),
               info.destination_ipv6, "router-secret", output,
               sizeof(output), &output_length) == AGENT_EDGE_BACKEND_GATEWAY);
    output[output_length] = '\0';
    assert(strstr(output,
                  "GET /agent/v1/authentication HTTP/1.1\r\n") != NULL);
    assert(strstr(output, "X-Nexus-Edge-Token: router-secret\r\n") != NULL);
    assert(agent_edge_proxy_rewrite_lan_http(
               "POST /agent/v1/register HTTP/1.1\r\n"
               "Authorization: Bearer should-be-optional\r\n"
               "X-Nexus-Public-IPv6: spoofed\r\n"
               "X-Nexus-Edge-Token: spoofed\r\n"
               "X-Nexus-Lan-Source: 203.0.113.10\r\n"
               "X-Nexus-Lan-Bridge-Token: attacker\r\n"
               "Connection: keep-alive\r\n\r\n",
               strlen(
                   "POST /agent/v1/register HTTP/1.1\r\n"
                   "Authorization: Bearer should-be-optional\r\n"
                   "X-Nexus-Public-IPv6: spoofed\r\n"
                   "X-Nexus-Edge-Token: spoofed\r\n"
                   "X-Nexus-Lan-Source: 203.0.113.10\r\n"
                   "X-Nexus-Lan-Bridge-Token: attacker\r\n"
                   "Connection: keep-alive\r\n\r\n"),
               "192.168.250.164", "lan-bridge-secret",
               output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_GATEWAY);
    output[output_length] = '\0';
    assert(strstr(output, "POST /agent/v1/register HTTP/1.1\r\n") != NULL);
    assert(strstr(output, "Authorization: Bearer should-be-optional\r\n") != NULL);
    assert(strstr(output, "X-Nexus-Public-IPv6:") == NULL);
    assert(strstr(output, "X-Nexus-Edge-Token:") == NULL);
    assert(strstr(output, "203.0.113.10") == NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Source: 192.168.250.164\r\n") != NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Bridge-Token: lan-bridge-secret\r\n") != NULL);
    assert(strstr(output, "Connection: close\r\n") != NULL);
    assert(agent_edge_proxy_rewrite_lan_http(
               "POST /agent/v1/bootstrap HTTP/1.1\r\n"
               "X-Nexus-Lan-Source: 203.0.113.10\r\n"
               "X-Nexus-Lan-Bridge-Token: attacker\r\n\r\n",
               strlen(
                   "POST /agent/v1/bootstrap HTTP/1.1\r\n"
                   "X-Nexus-Lan-Source: 203.0.113.10\r\n"
                   "X-Nexus-Lan-Bridge-Token: attacker\r\n\r\n"),
               "fd13:c49e:d9d1::5e3", "lan-bridge-secret",
               output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_GATEWAY);
    output[output_length] = '\0';
    assert(strstr(output, "POST /agent/v1/bootstrap HTTP/1.1\r\n") != NULL);
    assert(strstr(output, "203.0.113.10") == NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Source: fd13:c49e:d9d1::5e3\r\n") != NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Bridge-Token: lan-bridge-secret\r\n") != NULL);
    assert(agent_edge_proxy_rewrite_lan_http(
               "GET /agent/v1/cloud-registration HTTP/1.1\r\n"
               "Authorization: Bearer lan-session\r\n"
               "X-Nexus-Lan-Source: 203.0.113.10\r\n"
               "X-Nexus-Lan-Bridge-Token: attacker\r\n\r\n",
               strlen(
                   "GET /agent/v1/cloud-registration HTTP/1.1\r\n"
                   "Authorization: Bearer lan-session\r\n"
                   "X-Nexus-Lan-Source: 203.0.113.10\r\n"
                   "X-Nexus-Lan-Bridge-Token: attacker\r\n\r\n"),
               "192.168.250.164", "lan-bridge-secret",
               output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_GATEWAY);
    output[output_length] = '\0';
    assert(strstr(output,
                  "GET /agent/v1/cloud-registration HTTP/1.1\r\n") != NULL);
    assert(strstr(output, "Authorization: Bearer lan-session\r\n") != NULL);
    assert(strstr(output, "203.0.113.10") == NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Source: 192.168.250.164\r\n") != NULL);
    assert(strstr(output,
                  "X-Nexus-Lan-Bridge-Token: lan-bridge-secret\r\n") != NULL);
    assert(agent_edge_proxy_rewrite_http(
               "GET /agent/v1/cloud-registration HTTP/1.1\r\n\r\n",
               strlen("GET /agent/v1/cloud-registration HTTP/1.1\r\n\r\n"),
               info.destination_ipv6, "router-secret", output,
               sizeof(output), &output_length) == AGENT_EDGE_BACKEND_INVALID);
    assert(agent_edge_proxy_rewrite_lan_http(
               "POST /agent/v1/unknown HTTP/1.1\r\n\r\n",
               strlen("POST /agent/v1/unknown HTTP/1.1\r\n\r\n"),
               NULL, NULL,
               output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_INVALID);
    assert(agent_edge_proxy_rewrite_http(
               "POST /agent/v1/internal-invoke HTTP/1.1\r\n\r\n",
               strlen("POST /agent/v1/internal-invoke HTTP/1.1\r\n\r\n"),
               info.destination_ipv6, "router-secret", output,
               sizeof(output), &output_length) == AGENT_EDGE_BACKEND_INVALID);
    assert(agent_edge_proxy_rewrite_lan_http(
               "POST /agent/v1/internal-invoke HTTP/1.1\r\n\r\n",
               strlen("POST /agent/v1/internal-invoke HTTP/1.1\r\n\r\n"),
               NULL, NULL,
               output, sizeof(output), &output_length) ==
           AGENT_EDGE_BACKEND_INVALID);
    puts("edge proxy tests passed");
    return 0;
}
