#include "agent_public_ingress.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char address[AGENT_PUBLIC_INGRESS_IPV6_MAX];
    const char *token = "0123456789abcdef0123456789abcdef";

    assert(agent_public_ingress_validate(
               NULL, 0U, NULL, 0U, token, address) ==
           AGENT_PUBLIC_INGRESS_NONE);
    assert(agent_public_ingress_validate(
               "2001:0db8::1", strlen("2001:0db8::1"), token,
               strlen(token), token, address) == AGENT_PUBLIC_INGRESS_TRUSTED);
    assert(strcmp(address, "2001:db8::1") == 0);
    assert(agent_public_ingress_validate(
               "2001:db8::1", strlen("2001:db8::1"), "wrong", 5U,
               token, address) == AGENT_PUBLIC_INGRESS_INVALID);
    assert(agent_public_ingress_validate(
               "192.0.2.1", strlen("192.0.2.1"), token, strlen(token),
               token, address) == AGENT_PUBLIC_INGRESS_INVALID);
    puts("public ingress tests passed");
    return 0;
}
