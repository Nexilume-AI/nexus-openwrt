---
sidebar_position: 1
title: 页面与使用顺序
---

# LuCI 页面与使用顺序

菜单位于 **状态 → Agent Routing**，默认进入 **User mode**。先用 Cloud connection、Router network、Agent services 控制所需功能；下表详细页面均位于 **Developer mode**。Cloud 地址、配对和 Direct/Relay 模式在 **Nexus Cloud** 页，见[连接指南](../../guides/cloud-relay.md)。

| 类型 | 页面 | 何时使用 |
| --- | --- | --- |
| 私有云初始化 | [Quick Setup](quick-setup.md)、[Router Roles](roles.md) | 创建信任域和首个节点，或承载 Relay/Directory |
| 功能配置 | [Agent APIs & Protocols](protocols.md)、[Advanced Settings](advanced-settings.md)、[Static Peers](static-peers.md)、[Policy RIB](policy-rib.md) | 接入 Agent，配置公网 IPv6、发现、Peer 和策略 |
| 状态与信任 | [Overview](overview.md)、[Local Agents](local-agents.md)、[Capability Routes](capability-routes.md)、[Neighbors & Discovery](neighbors.md)、[Peer Trust](peer-trust.md) | 验证节点、租约、路由、邻居和信任状态 |

推荐顺序：**User mode → Developer mode（按需）→ Local Agents → Capability Routes → 实际调用**。需要修改身份或使用受管信任时再进入 Quick Setup / Advanced Settings。

:::tip 保存与运行状态
配置页修改后点击 **Save & Apply**。保存成功只表示候选配置通过校验；还应在状态页确认服务、会话、租约和路由已经生效。
:::
