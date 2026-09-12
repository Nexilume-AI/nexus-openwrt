---
sidebar_position: 4
title: 连接 Cloud 与 Cloud Relay
---

# 连接 Cloud 与 Cloud Relay

Cloud Relay 用于 Router 主动向 Nexus Cloud 建立中继通道，与 OpenWrt 自托管 Open Mesh seed 分开管理。普通 Router 无需安装服务端 Relay 包，保持 **Router Roles → Hosted services → Node only**。

## Cloud 侧准备

企业版集成启动器和当前社区版启动流程均已包含 Cloud Relay。社区版的操作步骤见[本地启动社区版](https://nexilume-ai.github.io/nexus-docs/server/getting-started/quickstart)。默认 Relay 仅供本机访问；运营方需在首次启动时选定 Router 可达的 IP，开放预定的隧道入口。社区版默认隧道端口为 `27444`，内部调用端口 `27445` 不应对外发布。

Cloud 服务“Relay available”不代表 Router 已连接。还需要可验证的 Cloud HTTPS 入口、设备 mTLS 入口、Owner/管理员生成的一次性配对码，以及 Router 到服务端的出站连通性。社区版测试用的 `http://127.0.0.1:18090` 不能直接作为远端 Router 的 Cloud URL。

## Router 配对

1. 打开 **状态 → Agent Routing → User mode**，可输入配对码并选择 **Pair with Nexus Cloud**。
2. 需要指定服务器地址或连接模式时，进入 **Developer mode → Nexus Cloud**。
3. 填写 **Nexus Cloud URL**（HTTPS）及 **One-time pairing code**。已配对设备输入新码会替换原注册。
4. **Certificate management** 保持 **Managed by Nexus Cloud**：私钥在 Router 本地生成，只发送 CSR，成功后移除配对码。
5. **Cloud connectivity** 选择 `Auto: Direct IPv6, then Relay`；无公网 IPv6 时也可明确选择 `Relay only (NAT / no IPv6)`。
6. Save & Apply 后使用 **Check Device TLS**，检查证书、密钥匹配、有效期及 Cloud TLS/JWT/JWKS 状态。

Auto 只有在公网 TLS 描述符健康时优先 Direct IPv6；Relay 使用主动出站 mTLS 隧道，不要求给 Router 开放 WAN 入站端口。不要因此关闭服务端身份校验。

## 验证与边界

在 Nexus Cloud 页确认配对、Cloud Relay 和 Agent 同步状态，并在 Console 验证已同步能力的实际调用。保存成功或本地 LuCI 可访问都不能替代这一步。

Quick Setup 的 **Open Mesh Directory URLs** 以及 Router Roles 页面给出的 `/v1/open-mesh/assignment` 是另一套自托管连接流程。不要把 Cloud URL 或 Cloud Relay 隧道 URL 填进该字段，也不要用切换自托管角色来修复 Cloud 配对失败。
