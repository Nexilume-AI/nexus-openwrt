---
sidebar_position: 2
title: 一 Agent 一 IP：地址归属模型
description: 比较 SDK Host Alias、OpenWrt 托管 IPv6 和单地址多端口三种模型。
---

# 一 Agent 一 IP：地址归属模型

Nexus 的核心网络模型是：**可以给每个 Agent 一个独立 IPv6 地址**。调用方在网络层直接命中目标 Agent，不必先用“主机 IP + 端口”猜测进程身份。

```text
[Agent A 的 IPv6]:9443  → agent://demo/agent-a
[Agent B 的 IPv6]:9443  → agent://demo/agent-b
```

一 Agent 一 IP 有两条实现路径：Agent 主机用 SDK Host Alias 持有地址，或 OpenWrt 为 Agent 入口托管地址。只有在无法获得足够 IPv6 地址时，才退回单地址多端口。

## 路径一：SDK Host Alias

主机从真实可用的 `/64` 中为每个 Agent 租用独立 `/128`。不同 Agent 可以使用同一端口：

```text
[2001:db8:1::101]:9443 → Agent A
[2001:db8:1::102]:9443 → Agent B
```

上面的 `2001:db8::/32` 只用于说明拓扑，实际部署必须使用真实全球前缀。

`nexus-agent-addressd` 负责特权地址操作，普通 Agent 通过本地 IPC 申请、确认、续租和释放地址。该模式要求 `/64` 确实在链路上可用或已路由到主机；只有单个运营商 `/128` 时不能凭空生成更多地址。

这条路径**只用 Python SDK 即可完成 Agent 互调**，不使用 Router 的 ARIB/AFIB、Directory、Relay、能力注册或回调控制面。先运行 [两个 IPv6 Agent 互相调用](https://nexilume-ai.github.io/nexus-docs/sdk/tutorials/ipv6-agents-call-each-other)查看完整示例。

## 路径二：每 Agent 一个 OpenWrt 托管 IPv6

Python Agent 以 `public_ipv6="auto"` 注册后，OpenWrt 从 `/48` 到 `/64` 的可用前缀分配 `/128`。远端连接到该 `/128` 的统一入口端口（默认 `7443`），路由器读取原始目标地址并精确查找对应路由；查找失败不会回退到其他 Agent。

从用户视角，这是“一 Agent 一个由 Router 托管的 IPv6”。从当前实现细节看，地址绑定对象是**本地能力路由租约**：同一个 Agent 暴露多个能力时，不应假设它们天然共享一个永久地址。这个差异在做审计、续租和故障恢复时很重要。

地址来源有两种：

- **routed-prefix**：上游把委派或静态前缀路由到 OpenWrt，推荐使用；
- **upstream-relay**：没有前缀委派时，从 WAN 链路上 `/64` 分配 `/128`，由 `agent-netd` 只为有效租约代理 NDP。这里是 NDP 代理，不是跨 NAT 的 Nexus Relay。

选择这条路径的原因不是“SDK 不能直连”，而是你需要 OpenWrt 统一执行 TLS/JWT、连接上限、能力路由、协议适配、发现和跨站点策略。

## 兼容路径：单 IPv6、多 Agent 分端口

主机只有一个稳定公网 IPv6 时，每个 Agent 监听不同端口：

```text
[2001:db8::20]:9441  → Agent A
[2001:db8::20]:9442  → Agent B
[2001:db8::20]:9443  → Agent C
```

地址属于 Agent 主机，OpenWrt 只做普通 IPv6 转发。部署简单，但端口成为服务身份的一部分，防火墙、DNS 和运维记录都必须保留端口信息。

## 如何选择

| 属性 | SDK Host Alias | OpenWrt 托管 `/128` | 单地址分端口 |
| --- | --- | --- | --- |
| 是否一 Agent 一地址 | 是 | 是（当前按能力路由租约绑定） | 否 |
| 地址拥有者 | Agent 主机 | OpenWrt | Agent 主机 |
| 调用路径 | 数值 IPv6 直连 | Router 精确入口和 AFIB | 主机地址 + 端口 |
| 多 Agent 同端口 | 是 | 是 | 否 |
| 前缀要求 | 主机可用 `/64` | 路由到 OpenWrt 的 `/48`–`/64`，或 WAN 链路 `/64` | 一个可达地址 |
| 自动发现 | 无；另配 SVCB/Card/Directory | 可组合 LAN、SVCB、Peer、Directory | 无；另配端口记录 |
| 统一入口策略 | 主机自行实现 | Router TLS/JWT、连接上限和协议适配 | 主机自行实现 |
| 适合场景 | SDK-only 实验、边缘主机、直接 mesh | Agent 私有云、跨站点治理 | 地址资源受限的兼容部署 |

## 地址不是授权

独立 IPv6 解决“请求发给哪个 Agent”，不解决“调用者是否可信”。公网入口优先选择 HTTPS 和受验证的调用身份。明文 HTTP 只适合隔离实验网络；它会暴露 token、Envelope、请求和结果，而且 SDK 不会在 TLS 失败后自动降级为 HTTP。

接下来可按[通信模式选择器](model.md)决定使用 SDK 直连、LAN、DNS SVCB、Router 托管入口还是 NAT Relay。
