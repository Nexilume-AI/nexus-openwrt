---
sidebar_position: 3
title: Quick Setup
---

# Quick Setup

面向常见单域部署；高级 ARPX、SVCB、公网 IPv6和容量参数在 [Advanced Settings](advanced-settings.md)。

| 分组/字段 | 作用 | 建议 |
| --- | --- | --- |
| Enable Agent routing | 启停 Agent 路由控制面 | 正常运行时开启 |
| Router ID | 1–64 位稳定标识，只能用小写字母、数字、`.`、`_`、`-`，首尾为字母或数字 | 每台 Router 唯一，保存后不要随意改 |
| Agent domain | Router 所属管理域 | 同域自动准入只在确有共同信任边界时使用 |
| Discover Agent routers on LAN | 消费 `_agent-router._tcp.local` DNS-SD | 只有需要发现其他 Router 时开启 |
| Publish this router on LAN | 发布本 Router DNS-SD 记录 | 希望被同 LAN Router 发现时开启 |
| LAN admission | `off` 手工；`same-domain` 同域；`allowlist` 列表 | 手工流程需先切换 Managed peer trust |
| Router allowlist | allowlist 模式允许自动准入的 Router ID | 一项一个稳定 ID |
| Connect to an OpenWrt Open Mesh seed | 开启 NAT 后的 Directory/Relay bootstrap | 普通 LAN 不需要 |
| Open Mesh Directory URLs | Directory 的 HTTPS 分配地址，最多四个，顺序故障切换 | 使用运营方提供的 URL；主机名参与 TLS 校验 |

LAN discovery 只产生候选。除自动准入策略命中外，仍需到 [Peer Trust](peer-trust.md) 审核。

本页配置 `open_mesh_relay_enabled` 和 `open_mesh_directory_endpoints`，不修改 Cloud Relay。Open Mesh 端点使用 `/v1/open-mesh/assignment`，最多四个；Cloud 连接使用 Developer mode → Nexus Cloud。默认 Router Mesh 为 Open，手动审核流程需先切换 Managed peer trust。
