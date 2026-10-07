---
sidebar_position: 0
title: 软件包获取与发布批次
---

# 软件包获取与发布批次

从 [GitHub Releases](https://github.com/Nexilume-AI/nexus-openwrt/releases) 下载已签名的 x86_64 Beta 安装包。每批包含适用目标、软件包清单、签名公钥和 SHA-256 校验值。请遵循[安装指南](../../../package-install.md)，先核对发布说明中的公钥指纹，再信任软件包索引。

首批二进制包仅适用于 **OpenWrt 25.12.4 x86/64**，不是整机固件或离线安装器；系统依赖仍来自匹配的官方软件源。

## 构建产物位置

OpenWrt 25.12 SDK 构建完成后，项目软件包通常位于：

```text
bin/packages/<architecture>/nexus_agent_router/
```

参考验收目标的架构目录是 `x86_64`。不要把 x86/64 软件包安装到 ARM 或 MIPS 设备。

## 推荐安装集合

| 使用方式 | 软件包 |
| --- | --- |
| Router | `nexus-agent-router`：路由、LuCI、Cloud connector，不安装 Node.js |
| Router + Relay | `nexus-agent-router-relay`：Router、自建 Relay 和 Node.js |
| Seed | `nexus-agent-router-seed`：Router、自建 Relay、Directory 和 Node.js |

安装前校验每个文件：

```sh
sha256sum -c SHA256SUMS
```

通过受信任渠道核对公钥指纹后，验证 APK 索引签名。校验值本身不证明发布者身份，不要绕过 APK 签名校验。

## 版本匹配

同一发布批次中的组件可能使用不同语义版本，例如 `agentd 3.1.0` 与 `agent-gw 0.22.0`。不要仅按主版本号自行混装。以发布清单、构建时间和校验值作为批次依据。
