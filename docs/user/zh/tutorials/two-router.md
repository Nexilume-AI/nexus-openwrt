---
sidebar_position: 1
title: 连接两台局域网路由器
---

# 连接两台局域网路由器

本实验把 Router A 的 Agent 能力发布给 Router B。先完成同一二层 LAN 的直连验证，不需要 Cloud、Relay 或 Directory。

## 准备网络与身份

- 两台 OpenWrt 使用兼容的 Nexus 软件包，Router ID 分别为 `router-a`、`router-b`。
- LAN 允许 DNS-SD/umDNS 发现和 ARPX 连接；检查 TCP `7444` 与两端 Peer TLS 配置。
- 两台设备使用不冲突的管理地址。不要直接把两台都提供 DHCP 的默认 LAN 接在一起；实验 LAN 只保留一个预定 DHCP 服务，或使用静态地址。
- Router A 至少有一个已注册、可在本机成功调用的 Agent。

桌面启动器目前使用固定 VM 名称、交换机和 IPv4/ULA 网段，只支持一个体验实例。不能运行两次默认脚本就得到这个双路由器拓扑。使用独立设备或另行配置好地址、身份和交换机的实验 VM；见[桌面虚拟机](../getting-started/desktop-vm.md)。

## 路径 A：默认 Open Mesh

1. 在两端 **状态 → Agent Routing → User mode** 开启 **Router network**；Router A 开启 **Agent services**。
2. 在 **Developer mode → Quick Setup** 核对唯一 Router ID。
3. 在 **Advanced Settings → Router Mesh** 确认两端使用 **Open distributed mesh (zero configuration)**。
4. 等待 Neighbor Routers 出现对端；默认自动准入不要求同域，也不要求逐个点击批准。
5. 在 Router B 的 **Developer mode → Neighbors & Discovery** 确认 ARPX 会话，再到 **Capability Routes** 检查 A 的能力。

## 路径 B：手动批准实验

如果要练习受管信任，先在两端 **Advanced Settings → Router Mesh** 选择 **Managed peer trust**，再把 **Quick Setup → LAN admission** 设为 **Manual approval**。确保两端 Peer 证书链和 TLS 身份满足各自的受管信任配置；仅改开关不会建立共同 CA。

两端分别打开 **Peer Trust**，核对对方 Router ID、域和端点后批准。在 A 上批准 B，在 B 上批准 A；不要仅批准一端就假定双向发布已获准。自动准入仍开启时，撤销的 Peer 可能在刷新后重新出现。

## 验证调用与撤销

先确认 B 上有 A 的有效能力租约，再通过 B 的已配置调用入口发起请求。SDK 地址应指向 B，Agent 回调需能从 A 访问；不要使用只在宿主机可见的 `127.0.0.1`。

成功响应才证明发现、会话、路由、策略和数据转发共同工作。随后停止 A 的 Agent，确认租约到期或撤销后，B 不再把它作为可调用目标。

## 排错

- **看不到邻居**：检查二层隔离、umDNS、发现/发布开关及地址冲突。
- **有候选无会话**：检查 Mesh 模式、双边准入、TCP 7444、证书与时间；不是所有失败都由“未批准”造成。
- **有会话无能力**：检查 A 的注册租约、导出/导入策略和 B 的 AFIB。
- **跨 NAT/站点**：选[自托管 Open Mesh seed](../guides/router-roles.md)或[Cloud Relay](../guides/cloud-relay.md)，不要将两种 assignment 配置混用。
