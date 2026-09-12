---
sidebar_position: 4
title: 如何升级或卸载
---

# 如何升级或卸载 Nexus Agent Router

本指南适用于 OpenWrt 25.12 的 `apk` 包管理器。升级会保留声明为 conffile 的 UCI 配置，但仍应先备份。

## 升级前

1. 记录当前版本和状态：

   ```sh
   apk info | grep -E 'agentd|agent-gw|agent-adapter|luci-app-agent-router|nexus-agent'
   /etc/init.d/agentd status
   /etc/init.d/agent-gw status
   ```

2. 创建系统备份：

   ```sh
   sysupgrade -b /tmp/nexus-before-upgrade.tar.gz
   ```

3. 下载备份文件到管理电脑。备份可能包含凭据，不要上传到工单或公开仓库。

## 升级

把同一构建批次的 `.apk` 上传到路由器，然后安装依赖和核心组件，最后安装 LuCI：

```sh
apk add --allow-untrusted --upgrade ./agent-netd-*.apk
apk add --allow-untrusted --upgrade ./agentd-*.apk ./agent-gw-*.apk ./agent-adapter-*.apk
apk add --allow-untrusted --upgrade ./nexus-agent-roles-*.apk ./luci-app-agent-router-*.apk
```

通配符必须只匹配准备升级的一个版本。如果目录中有多个版本，改用完整文件名。

## 验证升级

```sh
/etc/init.d/agentd restart
/etc/init.d/agent-gw restart
/etc/init.d/agent-adapter restart
logread | grep -E 'agentd|agent-gw|agent-adapter' | tail -n 80
```

在 LuCI **Agent Router → Overview** 确认 Recovery 为 Healthy，再调用一个已知能力。若包管理器生成 `.apk-new` 配置，先比较差异，不要直接覆盖现有密钥和身份。

## 卸载

先停止服务：

```sh
/etc/init.d/agent-adapter stop
/etc/init.d/agent-gw stop
/etc/init.d/agentd stop
apk del luci-app-agent-router nexus-agent-roles agent-adapter agent-gw agentd agent-netd
```

卸载后检查 `/etc/config/agent*` 和 `/etc/agent-gw/`。包管理器可能为了防止数据丢失而保留配置。只有在确认备份可用且不再需要身份、策略和密钥时，才手动删除这些文件。

## 回退

优先重新安装上一个已验证的整套软件包，再恢复备份。不要只回退 `agentd` 或 `agent-gw` 中的一个组件，因为 IPC 和配置契约可能不匹配。
