---
sidebar_position: 0
title: 软件包获取与发布批次
---

# 软件包获取与发布批次

当前仓库定义了源码和 OpenWrt SDK 构建流程，但没有在配置中声明公共二进制下载地址。外部发布前，应由发布方提供带 SHA-256 校验值的同批次软件包仓库或下载页。

## 构建产物位置

OpenWrt 25.12 SDK 构建完成后，项目软件包通常位于：

```text
bin/packages/<architecture>/nexus_agent_router/
```

参考验收目标的架构目录是 `x86_64`。不要把 x86/64 软件包安装到 ARM 或 MIPS 设备。

## 推荐安装集合

| 使用方式 | 软件包 |
| --- | --- |
| 基础 Node | `agent-netd`, `agentd`, `agent-gw`, `agent-adapter`, `nexus-agent-roles`, `luci-app-agent-router` |
| Node + Relay | 基础 Node + `nexus-agent-relayd` |
| Node + Directory | 基础 Node + `nexus-agent-directoryd` |
| 仅命令行、无 LuCI | 去掉 `luci-app-agent-router`，保留其余运行时组件 |

安装前校验每个文件：

```sh
sha256sum *.apk
```

校验值必须来自与软件包分离、受信任的发布渠道。

## 版本匹配

同一发布批次中的组件可能使用不同语义版本，例如 `agentd 3.1.0` 与 `agent-gw 0.22.0`。不要仅按主版本号自行混装。以发布清单、构建时间和校验值作为批次依据。
