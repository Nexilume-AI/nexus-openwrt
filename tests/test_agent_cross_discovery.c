#include "agent_cross_discovery.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_u16(uint8_t *buffer, size_t *offset, uint16_t value)
{
    buffer[(*offset)++] = (uint8_t)(value >> 8U);
    buffer[(*offset)++] = (uint8_t)value;
}

static void put_u32(uint8_t *buffer, size_t *offset, uint32_t value)
{
    buffer[(*offset)++] = (uint8_t)(value >> 24U);
    buffer[(*offset)++] = (uint8_t)(value >> 16U);
    buffer[(*offset)++] = (uint8_t)(value >> 8U);
    buffer[(*offset)++] = (uint8_t)value;
}

static void put_name(uint8_t *buffer, size_t *offset, const char *name)
{
    const char *label = name;
    const char *dot;

    while (*label != '\0') {
        size_t length;

        dot = strchr(label, '.');
        length = dot == NULL ? strlen(label) : (size_t)(dot - label);
        buffer[(*offset)++] = (uint8_t)length;
        memcpy(buffer + *offset, label, length);
        *offset += length;
        if (dot == NULL) {
            break;
        }
        label = dot + 1;
    }
    buffer[(*offset)++] = 0U;
}

static void put_param(
    uint8_t *buffer,
    size_t *offset,
    uint16_t key,
    const uint8_t *value,
    uint16_t length
)
{
    put_u16(buffer, offset, key);
    put_u16(buffer, offset, length);
    if (length > 0U) {
        memcpy(buffer + *offset, value, length);
        *offset += length;
    }
}

static size_t build_response(
    uint8_t *buffer,
    uint16_t query_id,
    bool authenticated,
    bool unknown_mandatory,
    bool unknown_optional
)
{
    const char *owner = "_agents.remote.example";
    const char *target = "router-b.remote.example";
    const char *router = "router-b";
    const char *domain = "remote.example";
    const char *card =
        "https://router-b.remote.example:8443/.well-known/agent-card.json";
    uint8_t mandatory[12];
    uint8_t alpn[] = {2U, 'h', '2'};
    uint8_t port[] = {0x1dU, 0x14U};
    uint8_t ipv4[] = {192U, 0U, 2U, 10U};
    size_t offset = 0U;
    size_t rdlength_offset;
    size_t rdata_start;
    size_t mandatory_offset = 0U;

    memset(buffer, 0, AGENT_CROSS_MAX_DNS_PACKET);
    put_u16(buffer, &offset, query_id);
    put_u16(buffer, &offset,
            (uint16_t)(0x8180U | (authenticated ? 0x0020U : 0U)));
    put_u16(buffer, &offset, 1U);
    put_u16(buffer, &offset, 1U);
    put_u16(buffer, &offset, 0U);
    put_u16(buffer, &offset, 0U);
    put_name(buffer, &offset, owner);
    put_u16(buffer, &offset, AGENT_CROSS_SVCB_TYPE);
    put_u16(buffer, &offset, 1U);

    buffer[offset++] = 0xc0U;
    buffer[offset++] = 0x0cU;
    put_u16(buffer, &offset, AGENT_CROSS_SVCB_TYPE);
    put_u16(buffer, &offset, 1U);
    put_u32(buffer, &offset, 120U);
    rdlength_offset = offset;
    put_u16(buffer, &offset, 0U);
    rdata_start = offset;
    put_u16(buffer, &offset, 1U);
    put_name(buffer, &offset, target);

    put_u16(mandatory, &mandatory_offset, 1U);
    put_u16(mandatory, &mandatory_offset, 3U);
    if (unknown_mandatory) {
        put_u16(mandatory, &mandatory_offset, 65000U);
    }
    put_u16(mandatory, &mandatory_offset, AGENT_CROSS_KEY_ROUTER_ID);
    put_u16(mandatory, &mandatory_offset, AGENT_CROSS_KEY_DOMAIN_ID);
    put_u16(mandatory, &mandatory_offset, AGENT_CROSS_KEY_AGENT_CARD);
    put_param(buffer, &offset, 0U, mandatory, (uint16_t)mandatory_offset);
    put_param(buffer, &offset, 1U, alpn, sizeof(alpn));
    put_param(buffer, &offset, 3U, port, sizeof(port));
    put_param(buffer, &offset, 4U, ipv4, sizeof(ipv4));
    if (unknown_mandatory || unknown_optional) {
        put_param(buffer, &offset, 65000U, NULL, 0U);
    }
    put_param(buffer, &offset, AGENT_CROSS_KEY_ROUTER_ID,
              (const uint8_t *)router, (uint16_t)strlen(router));
    put_param(buffer, &offset, AGENT_CROSS_KEY_DOMAIN_ID,
              (const uint8_t *)domain, (uint16_t)strlen(domain));
    put_param(buffer, &offset, AGENT_CROSS_KEY_AGENT_CARD,
              (const uint8_t *)card, (uint16_t)strlen(card));
    buffer[rdlength_offset] = (uint8_t)((offset - rdata_start) >> 8U);
    buffer[rdlength_offset + 1U] = (uint8_t)(offset - rdata_start);
    return offset;
}

static void test_query_contract(void)
{
    uint8_t packet[512];
    char owner[AGENT_CROSS_OWNER_LEN];
    size_t length;

    assert(agent_cross_owner_from_domain("remote.example", owner,
                                         sizeof(owner)));
    assert(strcmp(owner, "_agents.remote.example") == 0);
    assert(!agent_cross_owner_from_domain("Bad Domain", owner,
                                          sizeof(owner)));
    length = agent_cross_build_query(packet, sizeof(packet), 0x1234U, owner);
    assert(length > 20U);
    assert(packet[0] == 0x12U && packet[1] == 0x34U);
    assert(packet[2] == 0x01U && packet[3] == 0x00U);
    assert(packet[10] == 0U && packet[11] == 1U);
    assert(packet[length - 4U] == 0x80U);
}

static void test_secure_candidate_and_expiry(void)
{
    struct agent_cross_table table;
    const struct agent_cross_candidate *candidate;
    uint8_t packet[AGENT_CROSS_MAX_DNS_PACKET];
    size_t length = build_response(packet, 0x2233U, true, false, true);

    agent_cross_table_init(&table, 4U);
    assert(agent_cross_parse_response(
               &table, packet, length, 0x2233U,
               "_agents.remote.example", 1000U, 30U) ==
           AGENT_CROSS_PARSE_OK);
    assert(table.count == 1U);
    assert(table.records_accepted == 1U);
    candidate = agent_cross_table_first(&table);
    assert(candidate != NULL);
    assert(strcmp(candidate->router_id, "router-b") == 0);
    assert(strcmp(candidate->target, "router-b.remote.example") == 0);
    assert(strcmp(candidate->ipv4_hint, "192.0.2.10") == 0);
    assert(candidate->port == 7444U);
    assert(candidate->ttl_seconds == 30U);
    assert(candidate->unknown_optional_keys == 1U);
    assert(agent_cross_table_prune(&table, 30999U) == 0U);
    assert(agent_cross_table_prune(&table, 31000U) == 1U);
    assert(table.candidates_expired == 1U);
    agent_cross_table_destroy(&table);
}

static void test_dnssec_and_mandatory_fail_closed(void)
{
    struct agent_cross_table table;
    uint8_t packet[AGENT_CROSS_MAX_DNS_PACKET];
    size_t length;

    agent_cross_table_init(&table, 4U);
    length = build_response(packet, 0x3344U, false, false, false);
    assert(agent_cross_parse_response(
               &table, packet, length, 0x3344U,
               "_agents.remote.example", 0U, 120U) ==
           AGENT_CROSS_PARSE_INSECURE);
    assert(table.count == 0U);
    assert(table.dnssec_rejected == 1U);

    length = build_response(packet, 0x4455U, true, true, false);
    assert(agent_cross_parse_response(
               &table, packet, length, 0x4455U,
               "_agents.remote.example", 0U, 120U) ==
           AGENT_CROSS_PARSE_OK);
    assert(table.count == 0U);
    assert(table.records_incompatible == 1U);
    agent_cross_table_destroy(&table);
}

static void test_oversized_packet_rejected(void)
{
    struct agent_cross_table table;
    uint8_t packet[AGENT_CROSS_MAX_DNS_PACKET + 1U] = {0};

    agent_cross_table_init(&table, 1U);
    assert(agent_cross_parse_response(
               &table, packet, sizeof(packet), 0U,
               "_agents.remote.example", 0U, 120U) ==
           AGENT_CROSS_PARSE_MALFORMED);
    assert(table.count == 0U);
    agent_cross_table_destroy(&table);
}

int main(void)
{
    test_query_contract();
    test_secure_candidate_and_expiry();
    test_dnssec_and_mandatory_fail_closed();
    test_oversized_packet_rejected();
    puts("agent cross-domain discovery tests passed");
    return 0;
}
