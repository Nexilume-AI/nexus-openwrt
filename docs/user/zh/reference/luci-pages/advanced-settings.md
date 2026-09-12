---
sidebar_position: 10
title: Advanced Settings
---

# Advanced Settings

该页同时修改 `agent`、`agent_gateway` 和 `agent_adapter`。无效候选由 `agentd` 原子拒绝，不替换当前有效配置。常规部署优先使用 Quick Setup。

## Source defaults

下表来自当前 `agentd.config`。UCI-defaults、User mode 操作及升级保留配置可能改变有效值；用 `uci show agent` 核对目标设备。`router_mesh_mode` 默认 `open`，`off` 表示 Managed peer trust。

## Identity & capacity

| 字段 | 作用 | 默认/范围 |
| --- | --- | --- |
| Enable agentd | 启停核心控制面 | 开启 |
| Router ID / Agent domain | Router 稳定身份和管理域 | `router-local` / `local.invalid` |
| Maximum ARIB routes | 候选路由容量上限 | 10000；1–100000 |
| Default lease | 未显式指定时的租约秒数 | 30；5–3600 |

## ARPX transport

| 字段 | 作用 | 默认/风险 |
| --- | --- | --- |
| Enable outbound peer transport | 主动连接 Peer/Relay | 开启 |
| Enable inbound peer listener | 接受其他 Router 的 ARPX | 开启；同时配置 TLS 和防火墙 |
| Reflect learned routes | 把学习路由传播给其他 Peer | 关闭；普通边缘 Router 不开 |
| Listen IPv4 / port | 入站 ARPX 监听 | `0.0.0.0:7444` |
| Maximum inbound sessions | 入站连接上限 | 8；1–128 |
| Enable shared Invoke tunnel | 在 ARPX/Relay 会话上承载 Invoke | 开启；两端协议必须匹配 |

## LAN discovery

| 字段 | 作用 | 默认/范围 |
| --- | --- | --- |
| Consume LAN DNS-SD | 发现其他 Router | 开启 |
| Publish router DNS-SD record | 让其他 Router 发现本机 | 开启 |
| Zero-configuration admission | off、same-domain、allowlist、all | all；Open Mesh 默认自动准入，受管模式另行配置 |
| Router allowlist | allowlist 模式的 Router ID | 每项一个 |
| Graceful restart | 自动准入 Peer 的重启宽限 | 30 秒；5–300 |

## Cross-domain discovery

| 字段 | 作用 | 默认/条件 |
| --- | --- | --- |
| Enable DNS SVCB discovery | 查询 `_agents.<domain>` | 关闭 |
| Remote domain | 远端 Router 管理域，不是 Agent ID/URL | `remote.invalid` |
| Local DNSSEC resolver | 只读显示有效解析器 | 必须提供 authenticated-data |
| Resolver configuration | auto 自动检测 Unbound/dnsmasq；manual 覆盖 | auto |
| DNSSEC resolver IPv4 / port | manual 时的本地 loopback 解析器 | `127.0.0.1:1053`；IPv4 只允许 `127/8` |
| Agent Card authorization | off、same-domain、allowlist、all-signed、directory-trusted | off |
| Agent Card router allowlist | Card allowlist 的 Router ID | 仅 allowlist 显示 |

`all-signed` 只证明 Card 签名满足本地策略；`directory-trusted` 还要求已验证、未过期的 Directory 信任包。

## Open Mesh Relay

| 字段 | 作用 | 默认/范围 |
| --- | --- | --- |
| Enable self-hosted Open Mesh Relay | 从 Directory 获取 Relay 租约 | 关闭 |
| Open Mesh Directory URLs | 有序 HTTPS URL，最多四个 | 主机名是 TLS 身份 |
| Optional fixed Directory IPv4 | 与 URL 一一对应的底层地址 | 留空走 DNS |
| Directory timeout | 单次请求超时 | 5000 ms；100–60000 |
| Require forwarding assertions | 远端 Invoke 必须带可信源 Router 断言 | 默认关闭；跨域高信任部署建议启用 |

## Public Agent IPv6

| 字段 | 作用 | 默认/条件 |
| --- | --- | --- |
| IPv6 address source | auto、routed-prefix 或 upstream-relay | auto |
| Assign public IPv6 addresses | 允许 `public_ipv6="auto"` | 开启（仍需有效上游） |
| Detected routed/PD prefix | 只读显示 `/48`–`/64` | 未检测时需人工路由或选 no-PD 模式 |
| Detected upstream on-link `/64` | 只读显示 WAN `/64` | upstream-relay 前提 |
| Agent IPv6 source prefix | 实际分配前缀 | `/48`–`/64`；upstream-relay 必须 `/64` |
| Upstream IPv6 interface | NDP 代理所在逻辑接口 | `wan6`，仅 upstream-relay |
| Maximum active Agent addresses | `/128` 租约容量 | 256；1–65535 |
| Dedicated virtual interface | 地址落载接口 | `nexus-agent0`；不改默认路由/WAN 地址 |

## Static capability routes

| 字段 | 作用 |
| --- | --- |
| Enabled / Route ID | 开关；Route ID 为十六进制稳定标识 |
| Intent / Version | 能力名称和无符号版本 |
| Origin agent / Invoke endpoint | Agent URI 和回调 URL |
| Tenant / Region | 策略与评分元数据 |
| Cost / Latency / Trust / Load / Hop count | 分别为成本、毫秒、0–100、0–1000‰、0–32 |

静态路由不续租，直到配置删除；不要用它代替 SDK 动态注册。

## Public IPv6 ingress readiness

| 字段 | 作用 | 建议 |
| --- | --- | --- |
| Direct IPv6 transport | disabled、HTTPS mTLS、plain HTTP、legacy auto | 公网用 mTLS；HTTP 只用于隔离实验 |
| Allow Agents to call other Agents | 开启 Invoke 数据面 | 公网调用必需 |
| Agent call authentication | No JWT 或 JWT | 公网优先 JWT；mTLS 与 JWT可叠加 |
| Allow streaming and reconnect | SSE 及同一路由恢复 | 长任务开启 |
| Public listener address | mTLS listener，如 `[::]:7443` | 与证书、fw4 一致 |
| Destination port on every `/128` | 所有托管地址的统一端口 | 7443 |
| Publish direct descriptor | 注册响应带 scheme/port/TLS 元数据 | 需要外部直连时开启 |
| TLS certificate identity | 证书 DNS SAN 中的精确名称 | 只用于 HTTPS descriptor |
| Caller CA bundle label | 调用方映射到本地 CA 文件的标签，不是路径 | 1–63 位安全字符 |
| Maximum concurrent public connections | 公网连接上限 | 32；1–128 |

明文 HTTP 不加密 JWT 和业务数据，且不会作为 TLS 失败回退。目标 `/128` 始终精确绑定一条本地路由。

## MCP and A2A public ingress

| 字段 | 作用 |
| --- | --- |
| Enable MCP and A2A adapter | 开启协议适配；普通 HTTP/SSE Invoke 不需要 |
| Allow MCP and A2A streaming | 允许适配器流式输出；还需 gateway streaming |

协议到能力的映射在 [Agent APIs & Protocols](protocols.md) 配置。

本页配置 `open_mesh_relay_enabled` 和 `open_mesh_directory_endpoints`，不修改 Cloud Relay。Open Mesh 端点使用 `/v1/open-mesh/assignment`，最多四个；Cloud 连接使用 Developer mode → Nexus Cloud。默认 Router Mesh 为 Open，手动审核流程需先切换 Managed peer trust。
