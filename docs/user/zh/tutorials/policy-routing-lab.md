---
sidebar_position: 3
title: 实验：用策略控制能力选路
---

# 实验：用策略控制能力选路

本实验为 `demo.echo` 添加一条临时 Policy RIB 规则，观察允许、拒绝与约束如何改变 lookup。实验会修改 `/etc/config/agent_policy`，开始前请备份，并在最后清理。

## 你需要

- 已完成[路由生命周期实验](route-lifecycle-lab.md)，`demo.echo` 正在运行；
- Router SSH 管理权限；
- 当前固件包含 `agent.policy` 与 `reload_policy`。

## 第 1 步：备份并记录基线

```sh
cp /etc/config/agent_policy /tmp/agent_policy.before-lab
ubus call agent policy '{}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

先确认基线 lookup 能选到路由。备份位于 RAM 中，重启会消失，所以应在同一实验会话完成清理。

## 第 2 步：添加精确 allow 规则

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

重新查看 policy 与 lookup。输出应显示命中 `lab-demo-local`；只有 region/source/trust 合格的候选能进入排序。

## 第 3 步：证明身份匹配是硬条件

用另一个 source 查询：

```sh
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/other-caller"}'
```

它不会命中这条精确规则，而会使用其他匹配规则或 `default_action`。在真实 Gateway 调用中，source Agent 来自已验证身份，不能靠修改业务 JSON 冒充。

## 第 4 步：临时切换为 deny

```sh
uci set agent_policy.lab_demo.action='deny'
uci commit agent_policy
ubus call agent reload_policy '{}'
ubus call agent lookup '{"intent":"demo.echo","version":1,"tenant":"demo","source_agent":"agent://demo/caller-1"}'
```

最高优先级 deny 应拒绝整个查询，不会回退到低优先级 allow。这证明授权不是可被低分“补偿”的排序项。

## 第 5 步：清理并验证恢复

```sh
uci delete agent_policy.lab_demo
uci commit agent_policy
ubus call agent reload_policy '{}'
ubus call agent policy '{}'
```

若实验中配置损坏，可恢复备份：

```sh
cp /tmp/agent_policy.before-lab /etc/config/agent_policy
ubus call agent reload_policy '{}'
```

## 你学到了什么

Policy RIB 先按 priority 和 ID 确定规则，再执行 deny/allow 与硬约束，最后才对合格候选排序。继续阅读[从 ARIB 到 AFIB](../concepts/routing-selection.md)和[UCI、ubus 与诊断](../reference/config-api.md)。

## 常见问题

- `reload_policy` 失败：检查 `uci changes agent_policy` 和系统日志；原子重载失败时 live policy 应保持不变。
- lookup 仍命中其他规则：比较 priority、tenant/source/intent 与 policy ID。
- 实际调用与手工 lookup 不同：Gateway 使用认证后的身份，确认测试 token 的 claims 与手工参数一致。
