---
sidebar_position: 9
title: Agent APIs & Protocols
---

# Agent APIs & Protocols

该页配置 SDK 入口、认证、Agent 回调和 MCP/A2A 显式映射。Router 从不读取 prompt 或工具参数来猜测路由。

## Common setup

| 字段 | 作用 | 默认/关系 |
| --- | --- | --- |
| Allow Agents to call other Agents | 开启 Envelope route-and-invoke | 默认关闭；调用必需 |
| Allow Python SDK registration | 开启 register/renew/unregister | 默认关闭；只暴露在受保护入口 |
| Enable MCP/A2A compatibility | 开启 MCP `tools/call` 与 A2A `message:*` 适配 | 默认关闭 |
| Enable streaming responses | 同步开启 adapter/gateway streaming | 默认关闭 |
| Resume long-running calls | 保存 task-to-route 绑定，按 Last-Event-ID 恢复 | streaming 开启时显示；默认开启 |
| Default tenant | 入口未提供租户时使用 | `local` |
| Protocol adapter identity | adapter 的 source Agent URI | `agent://local/adapterd` |

恢复调用始终回到原 Agent，只重放窗口内事件，不在其他 Agent 上重启任务。

## Authentication & task credentials

| 字段 | 作用 | 条件/建议 |
| --- | --- | --- |
| Agent call authentication | No JWT、Remote JWKS、Local public key | 公网优先 Remote JWKS |
| Enable No JWT LAN access | 启动专用可信 LAN listener | 仅 No JWT 模式；用防火墙限制区域 |
| No JWT LAN listener | SDK 使用的 `NEXUS_ROUTER_URL` 地址 | 默认 `0.0.0.0:7445` |
| Token issuer URL | 必须精确匹配 JWT `iss` | JWKS/local 模式 |
| Router audience | 必须匹配 JWT `aud` | 默认 `nexus-agent-router` |
| JWKS URL | 远程公钥集合；空时从 issuer 推导 | JWKS 模式，HTTPS |
| Configuration status | 检查已保存配置、gateway 和 JWKS refresher | 先 Save & Apply |

### Authentication advanced fields

| 字段 | 作用 | 默认/范围 |
| --- | --- | --- |
| Local signing key ID | 本地公钥模式匹配 token `kid` | `default` |
| Local public key file | Router 本地 PEM | `/etc/agent-gw/jwt-public.pem` |
| JWKS cache file | refresher 管理的缓存 | `/etc/agent-gw/jwks.json` |
| JWKS HTTPS CA file | 获取 JWKS 的信任根 | 系统 CA bundle |
| Allowed clock skew | 时钟偏差 | 30 秒；0–300 |
| Maximum token lifetime | token 最大有效期 | 300 秒；1–3600 |
| Transaction-token replay cache | 一次性 transaction ID 容量 | 512；1–65536 |

## Python Agent Servers

| 字段/按钮 | 作用 | 建议 |
| --- | --- | --- |
| Automatically call registered LAN Agents | 根据 SDK 租约自动维护私网 IPv4/ULA 回调 | 保持开启 |
| Show automatic Agent example | 生成最小 Python called-Agent 示例 | 首次接入使用 |
| Automatically call registered HTTPS Agents | 自动维护 address、TLS name、CA label、leaf fingerprint 映射 | 远端 HTTPS Agent 开启 |
| HTTPS Agent certificate trust | 系统公共 CA 或私有/企业 CA | 由管理员预先信任，Agent 无权安装 CA |
| Show automatic HTTPS example | 生成 HTTPS Agent 示例 | 保存 CA 选择后使用 |
| Trusted CA label | 必须与 Agent 的 `server_ca_bundle_id` 匹配 | 只是标签，不是证书 |
| Trusted CA bundle file | Router 本地管理员批准的 PEM | Agent 不能覆盖 |
| Show legacy fixed mappings | 显示兼容/恢复字段 | 正常 SDK 注册无需开启 |
| Fixed HTTPS endpoint mappings | `TLS-name:port=IPv4/[IPv6]`，动态租约优先 | 仅应急手工映射 |

## Advanced limits

| 字段 | 作用 | 默认/范围 |
| --- | --- | --- |
| Default hop limit | Envelope 最大跳数 | 8；2–255 |
| Call timeout | adapter 到 gateway 超时 | 5500 ms；100–60000 |
| Concurrent protocol calls | 并发上限 | 32；1–1024 |
| Maximum request bytes | 单请求上限 | 65536；1024–1048576 |
| Maximum response bytes | 单响应上限 | 262144；1024–4194304 |
| Stream idle timeout | 无事件流超时 | 15000 ms；1000–300000 |
| Maximum SSE event bytes | 单事件上限 | 65536；256–1048576 |
| Retained resumable tasks | task-route 绑定容量 | 512；1–4096 |
| Resume window | 断开/完成后可重放时间 | 300 秒；1–86400 |

被调用 Agent 的事件历史窗口应不短于 Router 的 Resume window。

## MCP/A2A capability mappings

| 字段 | 作用 |
| --- | --- |
| Enabled | 是否加载映射 |
| Protocol | MCP `tools/call` 或 A2A `message:send/message:stream` |
| Server / Agent Card ID | URL 中的稳定 authority/Card ID |
| Tool name / A2A Skill ID | 精确操作选择器；A2A 必须与 `agent.expose()` 的 skill 一致 |
| Capability intent / version | 进入 AFIB 查找的 Nexus 能力 |
| A2A/MCP caller URL | 保存后生成的只读入口 URL |
| Show Python example | 为当前映射生成 caller/called-Agent 示例 |

映射是显式的 `protocol + authority + selector → intent.vN`。请求正文不参与路由推断，避免同一 prompt 被错误发送到不同能力。

## User mode 与 LAN SDK

当前 User mode 的 **Agent services** 同时启用注册、调用、LAN SDK listener、LAN 回调和 adapter。LAN SDK listener 默认地址为 `0.0.0.0:7446`；原生软件包中的功能开关默认值不等于用户启用该功能后的状态。旧的 No JWT LAN listener `7445` 是另一独立入口，不能与 `7446` 混称。调用方应使用已启用入口及其认证流程；确认主机回调地址可从 Router 访问。
