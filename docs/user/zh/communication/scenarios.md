---
sidebar_position: 4
title: 按场景扩展 Agent 私有云
---

# 按场景扩展 Agent 私有云

## 场景一：单节点 Agent 私有云

1. 按[配置 Agent 私有云网络](../getting-started/quick-setup.md)设置 Router ID 和 Agent domain。
2. 在 **Agent APIs & Protocols** 启用 Python SDK 注册和 Agent 调用。
3. 保持 **Automatically call registered LAN Agents** 开启。
4. 无 JWT 模式只绑定可信 LAN，并在防火墙中限制网段；互联网入口使用 JWKS。
5. Agent 注册私网 IPv4 或 ULA 回调地址，Router 按租约自动创建和删除映射。

单节点已经具备完整的注册、路由和调用能力。只有同一 LAN 确实存在第二个 Nexus Router 时，才需要 Router 间 LAN discovery。

## 场景二：扩展到同一 LAN 的多个节点

1. 每个节点使用唯一 Router ID；需要同域自动准入时使用相同 Agent domain。
2. 开启发布和消费 LAN DNS-SD。
3. 默认 Open Mesh 自动准入有效邻居。如需手动批准，先在 Advanced Settings 将 Router Mesh 切换为 Managed peer trust，再选 Manual approval 并核对 Peer Trust。
4. Managed peer trust 可按管理边界选 same-domain 或 allowlist；这些规则不能替代 Mesh 模式选择。
5. 在 **Neighbors & Discovery** 确认 ARPX session，再到 **Capability Routes** 检查远端能力是否进入 AFIB。

## 场景三：接入一台公网 Agent 主机

- 只有一个公网 IPv6：每个 Agent 使用不同端口。
- 有可用 `/64`：使用 Host Alias，让每个 Agent 获得独立 `/128`，可共享端口。
- 两种方式都要求主机自行配置 TLS、认证和防火墙；Router 只做普通 IPv6 转发。

## 场景四：由 OpenWrt 托管公网 Agent 地址

1. 确认上游把 `/48`–`/64` 路由到 OpenWrt；没有 PD 时确认 WAN 获得链路上 `/64`。
2. 在 **Advanced Settings → Public Agent IPv6** 选择地址来源并启用分配。
3. 在同页的 **Public IPv6 ingress readiness** 选择 HTTPS mTLS，开启调用，设置 `7443` 入口和连接上限。
4. 如需 MCP/A2A，启用协议适配器并配置显式能力映射。
5. Agent 用 `public_ipv6="auto"` 注册，从 **Local Agents** 复制直接连接描述符。

不要把文档地址 `2001:db8::/32` 用于生产。必须从外部网络验证分配的 `/128` 可达，而不只是 Router 本机能看到地址。

## 场景五：通过 SVCB 连接跨域节点

1. 远端域发布 `_agents.<domain>` SVCB 和 DNSSEC 链。
2. 本地启用 DNSSEC 验证解析器；优先让 Router 自动检测。
3. 在 **Advanced Settings → Cross-domain discovery** 填写远端管理域。
4. 在 **Peer Trust** 核对 DNSSEC 和 Agent Card/Directory 证据后准入。
5. 在 **Neighbors & Discovery** 与 **Capability Routes** 分别验证 Peer 会话和能力路由。

## 场景六：跨 NAT 扩展私有云

1. 自托管路径在 **Developer mode → Quick Setup** 或 **Advanced Settings → Open Mesh Relay** 开启 OpenWrt Open Mesh seed 连接。
2. 填写 seed 提供的 HTTPS `/v1/open-mesh/assignment` URL；最多四个，按顺序故障切换。
3. 主机名用于 SNI 和证书校验。固定 IPv4 只是高级覆盖，不改变 TLS 身份。
4. 确认 **Overview** 中 assignment active，随后检查 Relay tunnel 和远端能力路由。

普通 NAT 节点保持 **Node only**。只有对外提供 Relay 或 Directory 服务时，才在 **Router Roles** 选择相应服务端角色。

## 场景七：接入 Cloud 社区版

社区版启动流程已包含 Cloud Relay。Router 在 **Developer mode → Nexus Cloud** 完成配对，并使用 Auto 或 Relay only；不需要在 Quick Setup 填自托管 Directory URL。服务器 IP、设备 mTLS 和验证步骤见[Cloud Relay](../guides/cloud-relay.md)。
