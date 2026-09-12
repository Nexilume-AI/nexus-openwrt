---
sidebar_position: 1
title: 安装与构建
---

# 安装与构建

本项目以 OpenWrt feed 的形式提供路由器端软件包。下面的命令在 OpenWrt 源码树或匹配设备架构的 SDK 根目录中执行。

## 前置条件

- Linux 构建环境和可正常编译的 OpenWrt 源码树或 SDK。
- 已克隆本仓库，并能从 OpenWrt 构建目录访问本仓库路径。
- 目标设备有持久化存储空间，并能通过 LuCI 或 SSH 管理。

## 接入 feed

把本项目 feed 写入 OpenWrt 的 `feeds.conf.default`，然后更新并安装索引：

```bash
./scripts/feeds update nexus_agent_router
./scripts/feeds install agentd agent-gw agent-adapter
```

运行 `make menuconfig`，选择需要的软件包；典型部署至少包含：

- `agentd`：核心配置与路由控制面。
- `agent-gw`：Agent HTTP 网关。
- `agent-adapter`：协议适配层。
- `luci-app-agent-router`：Web 管理界面。

## 编译

```bash
make package/agentd/compile \
     package/agent-gw/compile \
     package/agent-adapter/compile V=s
```

也可以从本仓库使用可重复构建脚本：

```bash
sh scripts/build-openwrt-sdk.sh /absolute/path/to/openwrt-sdk
```

当前 OpenWrt 25.12 基线使用 `.apk` 包。编译完成后，从 `bin/packages/` 取出匹配架构的软件包，配置可信签名软件源及依赖，再使用 `apk add` 安装。不要把旧版 `opkg` 流程用于此基线。

## 验证

安装 LuCI 应用后，刷新浏览器并进入 **状态 → Agent Routing → User mode**。如果页面未出现，重新登录 LuCI，并检查：

```bash
apk info | grep -E 'agentd|agent-gw|agent-adapter|luci-app-agent-router'
logread | grep -E 'agentd|agent-gw|agent-adapter'
```

下一步：[使用快速配置向导](quick-setup.md)。
