#include "agent_lan_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char secret[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char other_secret[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const uint8_t nonce[AGENT_LAN_SESSION_NONCE_LEN] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    struct agent_lan_session_claims claims;
    char token[AGENT_LAN_SESSION_TOKEN_MAX];
    char tampered[AGENT_LAN_SESSION_TOKEN_MAX];
    char *signature;

    assert(agent_lan_session_secret_is_valid(secret));
    assert(!agent_lan_session_secret_is_valid(
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(agent_lan_session_issue(
               secret, "192.168.250.164", "demo", "agent://demo/echo-agent",
               1800000000U, 300U, nonce, token, sizeof(token)) ==
           AGENT_LAN_SESSION_OK);
    assert(strncmp(token, "nls1.", 5U) == 0);
    assert(agent_lan_session_verify(
               secret, token, "192.168.250.164", 1800000001U, &claims) ==
           AGENT_LAN_SESSION_OK);
    assert(strcmp(claims.tenant, "demo") == 0);
    assert(strcmp(claims.origin, "agent://demo/echo-agent") == 0);
    assert(claims.expires_at == 1800000300U);
    assert(agent_lan_session_verify(
               secret, token, "192.168.250.165", 1800000001U, &claims) ==
           AGENT_LAN_SESSION_SOURCE_MISMATCH);
    assert(agent_lan_session_verify(
               secret, token, "192.168.250.164", 1800000300U, &claims) ==
           AGENT_LAN_SESSION_EXPIRED);
    assert(agent_lan_session_verify(
               other_secret, token, "192.168.250.164", 1800000001U, &claims) ==
           AGENT_LAN_SESSION_SIGNATURE_INVALID);

    snprintf(tampered, sizeof(tampered), "%s", token);
    tampered[8] = tampered[8] == 'A' ? 'B' : 'A';
    assert(agent_lan_session_verify(
               secret, tampered, "192.168.250.164", 1800000001U, &claims) ==
           AGENT_LAN_SESSION_SIGNATURE_INVALID);

    snprintf(tampered, sizeof(tampered), "%s", token);
    signature = strrchr(tampered, '.');
    assert(signature != NULL && strlen(signature) > 12U);
    signature[10] = signature[10] == 'A' ? 'B' : 'A';
    assert(agent_lan_session_verify(
               secret, tampered, "192.168.250.164", 1800000001U, &claims) ==
           AGENT_LAN_SESSION_SIGNATURE_INVALID);
    puts("LAN Session token tests passed");
    return 0;
}
