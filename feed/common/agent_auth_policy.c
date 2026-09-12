#include "agent_auth_policy.h"

#include <stdio.h>
#include <string.h>

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

bool agent_auth_scope_contains(const char *scope, const char *required)
{
    const char *cursor;
    const char *end;
    size_t required_length;

    if (scope == NULL || required == NULL || required[0] == '\0') {
        return false;
    }
    required_length = strlen(required);
    cursor = scope;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        end = strchr(cursor, ' ');
        if (end == NULL) {
            end = cursor + strlen(cursor);
        }
        if ((size_t)(end - cursor) == required_length &&
            memcmp(cursor, required, required_length) == 0) {
            return true;
        }
        cursor = end;
    }
    return false;
}

enum agent_auth_result agent_auth_validate_claims(
    const struct agent_auth_policy *policy,
    const struct agent_auth_claims *claims,
    uint64_t now_seconds
)
{
    uint64_t skew;

    if (policy == NULL || claims == NULL ||
        policy->max_token_lifetime_seconds == 0U) {
        return AGENT_AUTH_INVALID_ARGUMENT;
    }
    if (policy->issuer[0] == '\0' ||
        policy->audience[0] == '\0' ||
        policy->required_scope[0] == '\0' ||
        claims->issuer[0] == '\0' ||
        claims->audience[0] == '\0' ||
        claims->subject[0] == '\0' ||
        claims->tenant[0] == '\0' ||
        claims->scope[0] == '\0' ||
        (claims->transaction_token ?
             claims->transaction_id[0] == '\0' :
             claims->jti[0] == '\0') ||
        claims->issued_at == 0U ||
        claims->expires_at == 0U) {
        return AGENT_AUTH_MISSING_CLAIM;
    }
    if (strcmp(policy->issuer, claims->issuer) != 0) {
        return AGENT_AUTH_ISSUER_MISMATCH;
    }
    if (strcmp(policy->audience, claims->audience) != 0) {
        return AGENT_AUTH_AUDIENCE_MISMATCH;
    }
    if (!agent_auth_scope_contains(claims->scope,
                                   policy->required_scope)) {
        return AGENT_AUTH_SCOPE_DENIED;
    }

    skew = policy->clock_skew_seconds;
    if (claims->issued_at > now_seconds &&
        claims->issued_at - now_seconds > skew) {
        return AGENT_AUTH_NOT_YET_VALID;
    }
    if (claims->not_before > now_seconds &&
        claims->not_before - now_seconds > skew) {
        return AGENT_AUTH_NOT_YET_VALID;
    }
    if (now_seconds > claims->expires_at &&
        now_seconds - claims->expires_at > skew) {
        return AGENT_AUTH_EXPIRED;
    }
    if (claims->expires_at <= claims->issued_at ||
        claims->expires_at - claims->issued_at >
            policy->max_token_lifetime_seconds) {
        return AGENT_AUTH_LIFETIME_EXCEEDED;
    }
    return AGENT_AUTH_OK;
}

bool agent_auth_identity_from_claims(
    const struct agent_auth_claims *claims,
    struct gateway_authenticated_identity *identity
)
{
    if (claims == NULL || identity == NULL) {
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    if (!copy_text(identity->tenant, sizeof(identity->tenant),
                   claims->tenant) ||
        !copy_text(identity->source_agent, sizeof(identity->source_agent),
                   claims->subject)) {
        return false;
    }
    identity->verified = true;
    return true;
}

const char *agent_auth_result_name(enum agent_auth_result result)
{
    switch (result) {
    case AGENT_AUTH_OK:
        return "ok";
    case AGENT_AUTH_INVALID_ARGUMENT:
        return "invalid_argument";
    case AGENT_AUTH_MISSING_CLAIM:
        return "missing_claim";
    case AGENT_AUTH_ISSUER_MISMATCH:
        return "issuer_mismatch";
    case AGENT_AUTH_AUDIENCE_MISMATCH:
        return "audience_mismatch";
    case AGENT_AUTH_SCOPE_DENIED:
        return "scope_denied";
    case AGENT_AUTH_NOT_YET_VALID:
        return "not_yet_valid";
    case AGENT_AUTH_EXPIRED:
        return "expired";
    case AGENT_AUTH_LIFETIME_EXCEEDED:
        return "lifetime_exceeded";
    default:
        return "unknown";
    }
}
