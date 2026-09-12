---
slug: /
sidebar_position: 1
title: OpenWrt 用户指南
description: 从配置 Agent 私有云网络开始，系统学习通信模式、LuCI 配置、网络原理和部署实验。
---

# OpenWrt 用户指南

Nexus Agent Router 把 OpenWrt 变成 Agent 私有云的网络节点：Agent 发布带租约的能力，调用方按能力发现并访问，Router 负责身份、信任、策略、选路和转发。这套文档既可按任务查阅，也可像教材一样从原理学到实验。

## 配置 Agent 私有云网络

电脑体验入口见[完整 OpenWrt 虚拟机](getting-started/desktop-vm.md)。使用前需准备包含匹配 VHDX 的体验包；只有源码时请先完成镜像制作。真实路由器使用设备安装路径。

1. [选择部署路径](getting-started/choose-path.md)，再[创建信任域和第一个节点](getting-started/quick-setup.md)。
2. 用[通信模式选择器](communication/model.md)决定地址、路径和发现机制。
3. 在 [LuCI 逐页参考](reference/luci-pages/index.md)核对每个界面和字段。
4. 发布[第一个 Python Agent](https://nexilume-ai.github.io/nexus-docs/sdk/quickstart/first-agent)，观察 [Local Agents](reference/luci-pages/local-agents.md) 与 [Capability Routes](reference/luci-pages/capability-routes.md)。

单节点已经是可工作的最小私有云；只有需要扩展边界时才增加 LAN Peer、SVCB、Relay、Directory 或公网 IPv6。

## 教材式学习路线

| 单元 | 你会回答的问题 |
| --- | --- |
| [双平面心智模型](concepts/mental-model.md) | IP 路由与 Agent 路由为什么不能混成一张表？ |
| [能力、路由与租约](concepts/capability-lifecycle.md) | Agent 启动后，一条路由如何出现、续命和消失？ |
| [从 ARIB 到 AFIB](concepts/routing-selection.md) | 授权、硬约束和评分在何时发生？ |
| [调用数据路径](concepts/invoke-data-path.md) | HTTP/MCP/A2A 调用经过哪些安全与进程边界？ |
| [通信模式选择](communication/model.md) | 单地址分端口、每 Agent `/128`、Router 托管地址如何选择？ |
| [发现、信任与 Relay](communication/discovery.md) | LAN、SVCB、Directory 和 Relay 分别解决什么？ |
| [可靠性与可观测性](concepts/reliability-observability.md) | 故障如何收敛，为什么恢复不会盲目重做任务？ |

完成三项实验巩固概念：[观察路由生命周期](tutorials/route-lifecycle-lab.md)、[用策略控制选路](tutorials/policy-routing-lab.md)和[双路由器组网](tutorials/two-router.md)。

## 按任务进入

- LAN、多节点、公网 IPv6、SVCB 或 NAT：从[按场景配置通信](communication/scenarios.md)开始。
- SDK、HTTP/SSE、MCP 和 A2A：阅读[发布 Agent API](guides/publish-api.md)与 [Agent APIs & Protocols](reference/luci-pages/protocols.md)。
- 公网 `/128` 和入口认证：阅读[地址归属模型](communication/addressing.md)与 [Advanced Settings](reference/luci-pages/advanced-settings.md)。
- Relay/Directory 服务端：[配置私有云节点角色](guides/router-roles.md)。
- 状态异常：从[诊断中心](troubleshooting/diagnostics.md)开始。

当前 OpenWrt 软件包主版本为 **3.1**，具体组件版本见[软件包参考](reference/packages.md)。

当前 LuCI 默认首页为 **状态 → Agent Routing → User mode**；详细设置位于 Developer mode。Cloud 社区版也可提供 Relay，接入步骤见[Cloud Relay](guides/cloud-relay.md)。
