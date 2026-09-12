---
sidebar_position: 7
title: Reliability, recovery, and observability
---

# Reliability, recovery, and observability

The goal is not to pretend failure never happens. Failures need boundaries, state must converge, operators must be able to explain the result, and recovery must not duplicate side-effecting work.

| Failure | Convergence mechanism |
| --- | --- |
| clean Agent exit | SDK unregister |
| crash or host power loss | lease expiration |
| transient health noise | consecutive failure/recovery thresholds |
| short peer break | stale plus graceful timeout |
| Router restart loses lease | healthy SDK re-registers after renewal 404 |
| short stream disconnect | stable task/route plus SSE cursor |
| underlay failure | AFIB recompilation or explicit invoke error |

These mechanisms are not interchangeable. Re-registration can restore lost control state, but must not hide an unhealthy local handler.

## Bounded recovery

Routes, peers, tenant quotas, request/response sizes, SSE events, and resume history all have capacities. Full capacity fails closed instead of exhausting memory. Resume history is in memory and covers short disconnects, not Agent or Router restart.

A resumed stream keeps the same task ID, request fingerprint, and first route ID. It replays only events after the cursor and does not re-execute the handler. Mismatches produce explicit conflict or history-expired errors.

## Idempotency and fallback retry

Idempotency means repeating an operation has the same external effect. Fallback is considered only when the Envelope says the operation is idempotent, permits retry, carries an idempotency key, policy allows it, and no backend bytes have arrived. Payment, mutation, and device-control calls cannot be retried merely because a connection closed.

## Observe without reading business content

LuCI and ubus expose ARIB/AFIB counts and generations, exclusion reasons, peers, Relay tunnels, lease and recovery counters, policy IDs, health streaks, route/task IDs, latency, load, and byte counts. They should not record Prompts, tool arguments, model outputs, tokens, or full payloads.

## Recommended diagnostic order

1. Check the IP underlay: interface, DNS, time, and endpoint reachability.
2. Check control state: services, registration, lease, peer, and AFIB generation.
3. Explain policy: identity, matching rule, constraints, and exclusions.
4. Check data plane: authentication, deadline, backend TLS, response limits, and stream cursor.

Use the [diagnostics center](../troubleshooting/diagnostics.md) and [common problems](../troubleshooting/common.md) for commands and recovery actions.
