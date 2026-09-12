---
sidebar_position: 5
title: Local Agents
---

# Local Agents

该页把有效本地能力租约按 Agent identity、endpoint 和 tenant 分组。这里的“连接”通常表示可续租的能力租约，不表示永久应用 socket。

| 列/操作 | 含义 |
| --- | --- |
| Agent | Agent URI/身份 |
| Tenant / endpoint | 租户和 SDK 注册的回调端点 |
| Lease state | 健康状态与最早到期时间 |
| Capabilities | intent、版本、路由 ID、健康状态和剩余租期 |
| Search / state filter | 按 Agent、tenant、endpoint、capability 和健康状态筛选 |
| Show native Agent lease metadata | 查看原生有界 JSON，供排障使用 |
| Copy IPv6 connection | 为 Router-managed `/128` 复制直接连接描述符 |

“Copy IPv6 connection”只在公网 descriptor 已启用且信息完整时出现。HTTPS 还要求 TLS certificate identity 和 CA bundle label；HTTP 只发布明确的明文 scheme。地址按 capability route ID 绑定，不能据此假设一个 Agent 永久只有一个 `/128`。
