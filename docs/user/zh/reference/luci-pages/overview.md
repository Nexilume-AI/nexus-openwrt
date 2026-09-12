---
sidebar_position: 2
title: Overview
---

# Overview

只读总览每 5 秒刷新有界元数据，不读取 prompt、工具参数、模型输出、token 或任务正文。

| 卡片/区域 | 含义 | 异常时检查 |
| --- | --- | --- |
| AFIB routes | 当前可用于调用的最佳能力路由数/容量 | Agent 租约、策略、静态路由 |
| ARPX sessions | 已建立 Router Peer 会话/已配置 Peer 数 | TLS、端点、Static Peers、Peer Trust |
| LAN candidates | 当前 LAN 候选数及发现开关 | DNS-SD、接口、多播、防火墙 |
| Relay tunnels | 已建立隧道数及 Directory assignment 状态 | URL、证书、出站网络、租约 |
| Public Agent IPv6 | 活跃 `/128` 租约数及容量 | 前缀路由、NDP、`agent-netd` |
| Recovery | 配置域是否成功加载 | 下方 Last error 与系统日志 |
| Route index | 路由索引是否启用及 bucket 数 | `agentd` 启动和内存边界 |
| Policy RIB | 已编译规则数与默认动作 | Policy RIB 页面、原子 reload 错误 |
| Route memory | AFIB 有界内存用量 | 路由容量和异常增长 |
| Recovery domains | 每个配置域的 reload、失败次数和最后错误 | 先修复具体域，不要反复重启 |

绿色表示该控制面状态可用，不等于所有业务调用都成功。端到端结果还要结合 [Capability Routes](capability-routes.md) 和真实调用验证。
