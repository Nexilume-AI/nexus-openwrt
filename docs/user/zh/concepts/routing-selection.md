---
sidebar_position: 4
title: 从 ARIB 到 AFIB：过滤、策略与排序
---

# 从 ARIB 到 AFIB：过滤、策略与排序

能力路由不是“给每条候选算一个分，最高就赢”。授权、数据位置、租约和信任属于不可妥协的条件；只有通过全部硬条件的候选才能比较偏好。

## 第一步：确定要查什么

查询至少包含 intent、major version、tenant 与 source Agent。还可以给出精确 `target_agent`、region、最高成本/延迟、最低信任和 hop limit。`target_agent` 是路由约束，不是调用者身份。

## 第二步：选择策略规则

Policy RIB 的规则匹配 tenant、source Agent 和 intent，每项支持精确值或 `*`：

1. 选择匹配规则中 priority 最大者；
2. priority 相同时，选择 `policy_id` 字典序更小者；
3. 没有规则时使用 `default_action`；
4. 最高优先级规则若为 deny，整个查询结束，不回退到更低优先级 allow。

这种确定性避免了“同一配置偶尔选到不同规则”。认证后的 tenant/source 会覆盖 Envelope 自报值，因此伪造 JSON 不能绕过策略。

## 第三步：硬过滤

典型过滤顺序包括：

- intent 与 major version 精确匹配；
- tenant、调用主体和策略授权；
- target Agent、region、数据分级和出口要求；
- 最大成本、延迟、负载、跳数与最低信任；
- lease、健康与熔断状态；
- path vector 防环；
- endpoint 或下一跳的 IP underlay 可达；
- 协议、schema 和允许的 endpoint 前缀兼容。

Envelope 约束与 Policy RIB 约束取更严格的交集。`preferred_peer` 只能影响合格候选的排序，不能把不合格候选变成合格。

## 第四步：排序

合格候选使用可配置的固定点整数权重比较延迟、成本、负载、信任惩罚、跳数和非首选 peer 惩罚。整数和饱和运算便于在小型设备上得到确定、可测试的结果。同分时使用稳定的 route ID 次序，避免随机抖动。

```text
score = latency + cost + load + trust_penalty + hops + peer_penalty
```

分数越小越优。实际权重以当前 Policy RIB 和 lookup 解释为准，不应从本文复制后硬编码到业务程序。

## 粘性与重试

长任务与恢复流必须保持第一次选择的路由，不能在重连时因为分数变化换到另一 Agent。普通同步调用的候补重试也默认受限：只有请求明确幂等、允许重试、带幂等键，且后端尚未返回任何字节时才可能尝试候补。非幂等操作不能靠“网络失败”猜测是否应重做。

## 如何解释一次选择

在 LuCI 的路由解释或允许的 ubus lookup 中同时查看：

- 命中的 policy ID 与 action；
- 每类硬约束排除了多少候选；
- 最终候选的指标、来源和 peer；
- 当前 policy generation 与 AFIB generation。

不要只看“最终选了谁”。可解释性最有价值的部分是知道其他候选为什么没有资格。

下一步完成[策略选路实验](../tutorials/policy-routing-lab.md)，再阅读[调用数据路径](invoke-data-path.md)。
