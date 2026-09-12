---
sidebar_position: 12
title: Policy RIB
---

# Policy RIB

策略按身份过滤 ARIB 候选并确定 AFIB 选择。规则原子编译；新配置失败时仍保留上一份有效策略。

## 默认与健康抑制

| 字段 | 作用 | 默认 |
| --- | --- | --- |
| Default action | 没有规则匹配时 allow 或 deny | allow |
| Failure threshold | 连续多少次失败后标记路由不健康 | 3 |
| Recovery threshold | 连续多少次成功后恢复健康 | 2 |

## 有序规则字段

| 字段 | 作用 |
| --- | --- |
| Enabled / Policy ID / Priority | 开关、稳定 ID 和优先级；规则可排序 |
| Action | 匹配后 allow 或 deny |
| Tenant / Source agent / Intent class | 身份选择器；`*` 表示任意 |
| Required region | 只接受指定区域 |
| Route sources | 逗号分隔，如 `local,static,peer` |
| Maximum cost / latency | 成本与延迟硬上限 |
| Minimum trust | 0–100 的最低信任分 |
| Maximum load | 0–1000‰ 的负载上限 |
| Maximum hops | 0–32 的路径长度上限 |
| Preferred peer | 在合格路由中偏好某 Peer |
| Allowed endpoint prefix | 限制回调 URL 前缀，常用 `https://` |

高优先级 deny 应先于宽泛 allow。建议先用窄范围 tenant/intent 规则验证，再逐步收紧默认动作；保存后在 Overview 的 Policy RIB 和 Capability Routes 中确认结果。
