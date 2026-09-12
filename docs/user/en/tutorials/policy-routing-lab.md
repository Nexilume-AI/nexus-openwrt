---
sidebar_position: 3
title: "Lab: control capability routing with policy"
---

# Lab: control capability routing with policy

Add a temporary Policy RIB rule for `demo.echo` and observe how allow, deny, and constraints change lookup. This modifies `/etc/config/agent_policy`; back it up and clean up at the end.

## What you need

- A running `demo.echo` from the [route lifecycle lab](route-lifecycle-lab.md).
- Router SSH administrator access.
- Firmware with `agent.policy` and `reload_policy`.

## Step 1: back up and record the baseline

```sh
cp /etc/config/agent_policy /tmp/agent_policy.before-lab
ubus call agent policy '{}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

Confirm the baseline lookup succeeds. The backup is in RAM and disappears after reboot.

## Step 2: add an exact allow rule

```sh
uci set agent_policy.lab_demo='policy'
uci set agent_policy.lab_demo.enabled='1'
uci set agent_policy.lab_demo.policy_id='lab-demo-local'
uci set agent_policy.lab_demo.priority='900'
uci set agent_policy.lab_demo.action='allow'
uci set agent_policy.lab_demo.tenant='demo'
uci set agent_policy.lab_demo.source_agent='agent://demo/caller-1'
uci set agent_policy.lab_demo.intent='demo.echo'
uci set agent_policy.lab_demo.required_region='local'
uci set agent_policy.lab_demo.route_sources='local'
uci set agent_policy.lab_demo.min_trust='50'
uci commit agent_policy
ubus call agent reload_policy '{}'
```

Policy and lookup output should now identify `lab-demo-local`; only matching region/source/trust candidates remain eligible.

## Step 3: prove identity is a hard condition

```sh
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/other-caller"}'
```

This source does not match the exact rule and uses another rule or the default action. Real Gateway calls use authenticated source identity, not an editable business JSON field.

## Step 4: temporarily deny

```sh
uci set agent_policy.lab_demo.action='deny'
uci commit agent_policy
ubus call agent reload_policy '{}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

The selected high-priority deny ends the query instead of falling through to a lower allow.

## Step 5: clean up

```sh
uci delete agent_policy.lab_demo
uci commit agent_policy
ubus call agent reload_policy '{}'
ubus call agent policy '{}'
```

If necessary, restore the backup and reload:

```sh
cp /tmp/agent_policy.before-lab /etc/config/agent_policy
ubus call agent reload_policy '{}'
```

## What you learned

Policy first selects one rule deterministically, then applies deny/allow and hard constraints, and only then orders eligible candidates. See [From ARIB to AFIB](../concepts/routing-selection.md).

## Troubleshooting

- Reload fails: inspect `uci changes agent_policy` and logs; failed atomic reload should preserve live policy.
- Another rule wins: compare priority, match fields, and policy ID.
- Real invoke differs from manual lookup: align manual tenant/source parameters with the token's verified claims.
