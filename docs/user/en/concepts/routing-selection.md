---
sidebar_position: 4
title: From ARIB to AFIB
---

# From ARIB to AFIB: filtering, policy, and ordering

Capability routing is not “give everything a score and pick the winner.” Authorization, placement, lease, and trust are non-negotiable. Only candidates that pass every hard condition are compared by preference.

## 1. Define the query

A query contains at least intent, major version, tenant, and source Agent. It may also include an exact `target_agent`, region, maximum cost or latency, minimum trust, and hop limit. A target Agent is a route constraint, not caller identity.

## 2. Select one policy rule

Policy RIB rules match tenant, source Agent, and intent using exact values or `*`:

1. choose the matching rule with the highest priority;
2. break equal priority by lexicographically smaller `policy_id`;
3. use `default_action` when no rule matches;
4. a selected deny ends the query instead of falling through to a lower allow.

Verified tenant and source identity override Envelope declarations, so changing JSON cannot bypass policy.

## 3. Apply hard filters

Typical filters include exact intent/version; identity and tenant authorization; target Agent, region, classification, and egress; cost, latency, load, hops, and trust; lease and health; path-vector loop prevention; underlay reachability; and protocol/schema/endpoint compatibility.

Envelope constraints and policy constraints use the stricter intersection. A preferred peer can order eligible candidates, but cannot make an ineligible candidate valid.

## 4. Order eligible candidates

Configurable fixed-point weights compare latency, cost, load, trust penalty, hop count, and non-preferred-peer penalty. Integer and saturating arithmetic make the result deterministic on small devices. Equal scores use a stable route-ID tie-breaker.

```text
score = latency + cost + load + trust_penalty + hops + peer_penalty
```

Lower is better. Use the current Policy RIB and lookup explanation for actual weights rather than hard-coding this conceptual formula.

## Stickiness and retry

A long-running or resumed task stays on the route selected for its first execution. A reconnect must not move the task to a newly better-scoring Agent. Bounded fallback retry is also strict: the request must be explicitly idempotent, permit retry, carry an idempotency key, and have received no backend bytes.

## Explain a decision

Inspect the selected policy and action, exclusion counts for every hard condition, route metrics/source/peer, and both policy and AFIB generations. The useful part is not only who won, but why every other candidate was ineligible.

Continue with the [policy-routing lab](../tutorials/policy-routing-lab.md) and [invoke data path](invoke-data-path.md).
