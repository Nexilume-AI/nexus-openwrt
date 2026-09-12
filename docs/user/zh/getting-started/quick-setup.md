---
sidebar_position: 2
title: 配置 Agent 私有云网络
---

# 配置 Agent 私有云网络

当前入口是 **状态 → Agent Routing → User mode**。普通用户可以在同一页控制 Cloud connection、Router network 和 Agent services；身份、信任和协议参数位于 **Developer mode**。

## 1. 检查节点身份

在 **Developer mode → Quick Setup** 检查 Router ID 和 Agent domain。每台设备的 Router ID 必须唯一且稳定：1–64 位小写字母、数字、点、下划线或连字符，首尾为字母或数字。不要把克隆镜像中的相同身份用于两台设备。

Agent domain 是管理域标识。当前默认 **Router Mesh = Open distributed mesh (zero configuration)**，可以跨域交换路由，因此相同域名不是默认的授权边界。

## 2. 开启需要的功能

回到 **User mode**：

- **Agent services**：启用 SDK 注册与 Agent 调用。
- **Router network**：启用 Peer 收发以及 LAN 发现和发布。单节点实验不需要邻居时可关闭。
- **Cloud connection**：有 Cloud 账号、HTTPS 入口和配对码后再接入；不连接 Cloud 也可以使用本地 Agent 网络。

状态应与实际服务一致；开启开关不代表已有 Agent 或远端能力。需要手动调整 SDK 监听和认证时，使用 **Developer mode → Agent APIs & Protocols**。

## 3. 选择邻居信任方式

默认 Open Mesh 自动准入经过发现校验的 LAN、静态种子及 DNSSEC/SVCB Router，不要求逐个批准。发现本身不产生 Agent 能力；建立 Peer 后还需远端发布租约，并由路由策略筛选。

如果需要逐个批准，在 **Developer mode → Advanced Settings → Router Mesh** 选择 **Managed peer trust**，再到 Quick Setup 将 **LAN admission** 设为 **Manual approval**。双方都按同一管理意图配置，并在 **Peer Trust** 中核对候选。仅把 LAN admission 改为 Manual approval，不能把 Open Mesh 当作受管信任模式。

## 4. 分清两种 Relay

- 连接 Nexus Cloud（包括已加入 Relay 的社区版）：使用 **Developer mode → Nexus Cloud** 的配对和 **Cloud connectivity**。见[连接 Cloud 与 Cloud Relay](../guides/cloud-relay.md)。
- 使用自托管 OpenWrt seed：在 Quick Setup 启用 **Connect to an OpenWrt Open Mesh seed**，填写 **Open Mesh Directory URLs**，最多四个。端点路径为 `/v1/open-mesh/assignment`，应复制 seed 页面给出的完整地址。

这两组设置独立；Quick Setup 不修改 Cloud Relay。普通客户端保持 **Router Roles → Hosted services → Node only**。

## 5. 验证注册和调用

在 User mode 的 Agents、Neighbor Routers 和路由列表检查结果。详细状态位于 **Developer mode → Local Agents / Capability Routes / Overview**。至少验证一个真实 Agent 注册、租约与实际调用；单节点没有 Peer 或 Relay 时，会话数为 0 正常。

本地实验使用 Router 可访问的 IPv4/ULA 回调地址。公网 Agent 与私网 Agent 的地址条件不同，参见[IPv6 配置](../guides/ipv6.md)和[发布 Agent API](../guides/publish-api.md)。
