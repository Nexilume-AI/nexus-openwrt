---
sidebar_position: 3
title: LuCI 界面导览
---

# LuCI 界面导览

当前菜单是 **状态 → Agent Routing**，默认进入 **User mode**。旧的 Overview、Quick Setup 等直达路径会重定向到用户首页；需要详细设置时展开 **Developer mode**。

## User mode

首页显示 Nexus Cloud、This Router、Neighbor Routers 与 Agents 的状态，并提供三个开关：

| 功能 | 作用 |
| --- | --- |
| Cloud connection | 配对 Cloud 并同步符合条件的 Agent |
| Router network | 发现邻居、建立 Peer 并交换能力路由 |
| Agent services | SDK 注册和 Agent 调用 |

配对入口为 **Pair with Nexus Cloud**。已有注册时输入新配对码会替换旧注册。只读账号可查看状态，但不能修改配置。

## Developer mode

身份与发现使用 Quick Setup；Cloud 地址、配对及 Direct/Relay 模式使用 Nexus Cloud；自托管服务使用 Router Roles。SDK、协议和认证在 Agent APIs & Protocols，详细租约与 AFIB 分别在 Local Agents 和 Capability Routes。

Overview、Neighbors & Discovery、Peer Trust 用于定位服务、会话和准入问题。Advanced Settings、Static Peers、Policy RIB 用于精细配置。完整索引见[逐页参考](../reference/luci-pages/index.md)。

保存后检查实际运行状态，并执行一次真实调用。状态卡绿色不意味着远端调用、公网 IPv6 或 Cloud 配对都已经成功。
