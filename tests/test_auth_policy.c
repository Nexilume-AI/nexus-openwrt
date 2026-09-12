#include "agent_auth_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written >= 0);
    assert((size_t)written < capacity);
}

static void valid_policy(
    struct agent_auth_policy *policy,
    struct agent_auth_claims *claims
)
{
    memset(policy, 0, sizeof(*policy));
    memset(claims, 0, sizeof(*claims));
    set_text(policy->issuer, sizeof(policy->issuer),
             "https://issuer.example");
    set_text(policy->audience, sizeof(policy->audience), "nexus-router");
    set_text(policy->required_scope, sizeof(policy->required_scope),
             "agent.route");
    policy->clock_skew_seconds = 30U;
    policy->max_token_lifetime_seconds = 300U;

    set_text(claims->issuer, sizeof(claims->issuer),
             "https://issuer.example");
    set_text(claims->audience, sizeof(claims->audience), "nexus-router");
    set_text(claims->subject, sizeof(claims->subject),
             "agent://tenant-a/client-1");
    set_text(claims->tenant, sizeof(claims->tenant), "tenant-a");
    set_text(claims->scope, sizeof(claims->scope),
             "openid agent.route agent.invoke");
    set_text(claims->jti, sizeof(claims->jti), "txn-001");
    claims->issued_at = 1000U;
    claims->not_before = 1000U;
    claims->expires_at = 1200U;
}

static void test_scope_matching(void)
{
    assert(agent_auth_scope_contains("a b c", "b"));
    assert(agent_auth_scope_contains("agent.route", "agent.route"));
    assert(!agent_auth_scope_contains("agent.route.extra", "agent.route"));
    assert(!agent_auth_scope_contains("", "agent.route"));
}

static void test_valid_claims_and_identity(void)
{
    struct agent_auth_policy policy;
    struct agent_auth_claims claims;
    struct gateway_authenticated_identity identity;

    valid_policy(&policy, &claims);
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) == AGENT_AUTH_OK);
    assert(agent_auth_identity_from_claims(&claims, &identity));
    assert(identity.verified);
    assert(strcmp(identity.tenant, "tenant-a") == 0);
    assert(strcmp(identity.source_agent,
                  "agent://tenant-a/client-1") == 0);
}

static void test_policy_rejections(void)
{
    struct agent_auth_policy policy;
    struct agent_auth_claims claims;

    valid_policy(&policy, &claims);
    set_text(claims.audience, sizeof(claims.audience), "other");
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) ==
           AGENT_AUTH_AUDIENCE_MISMATCH);

    valid_policy(&policy, &claims);
    set_text(claims.scope, sizeof(claims.scope), "agent.invoke");
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) == AGENT_AUTH_SCOPE_DENIED);

    valid_policy(&policy, &claims);
    claims.not_before = 1140U;
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) == AGENT_AUTH_NOT_YET_VALID);

    valid_policy(&policy, &claims);
    assert(agent_auth_validate_claims(
               &policy, &claims, 1231U) == AGENT_AUTH_EXPIRED);

    valid_policy(&policy, &claims);
    claims.expires_at = 1400U;
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) ==
           AGENT_AUTH_LIFETIME_EXCEEDED);

    valid_policy(&policy, &claims);
    claims.issued_at = 1140U;
    claims.not_before = 1000U;
    claims.expires_at = 1200U;
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) == AGENT_AUTH_NOT_YET_VALID);

    valid_policy(&policy, &claims);
    policy.max_token_lifetime_seconds = 0U;
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) ==
           AGENT_AUTH_INVALID_ARGUMENT);

    valid_policy(&policy, &claims);
    claims.transaction_token = true;
    claims.jti[0] = '\0';
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) ==
           AGENT_AUTH_MISSING_CLAIM);
    set_text(claims.transaction_id,
             sizeof(claims.transaction_id), "txn-route-001");
    assert(agent_auth_validate_claims(
               &policy, &claims, 1100U) == AGENT_AUTH_OK);
}

int main(void)
{
    test_scope_matching();
    test_valid_claims_and_identity();
    test_policy_rejections();
    puts("agent auth policy tests passed");
    return 0;
}
